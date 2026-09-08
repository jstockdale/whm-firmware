/* http_svc.c - WHM P2c file service.
 *
 * Server (port 80, every node):
 *   GET /            tiny status stub (future web-UI front door)
 *   GET /manifest    JSON [{"n":name,"s":size,"h":sha256hex}, ...]
 *   GET /media/<f>   the file; honors a single Range: bytes=a[-b]
 *
 * Client:
 *   whm_media_sync(host) - diff remote manifest against local media and
 *   pull what's missing or different, with .part resume.
 *
 * The sha256 manifest is CACHED: hashing tens of MB of MP3s per request
 * would be seconds of SD churn. Built lazily, invalidated on card events
 * via whm_http_media_dirty().
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "whm_board.h"
#include "settings.h"
#include "storage_test.h"
#include "mp3_player.h"
#include "esp_ota_ops.h"
#include "http_svc.h"

static const char *TAG = "whm_http";

static void url_enc(const char *in, char *out, size_t olen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < olen; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void url_dec(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) &&
            isxdigit((unsigned char)s[2])) {
            int hi = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
            int lo = s[2] <= '9' ? s[2] - '0' : (s[2] | 32) - 'a' + 10;
            *w++ = (char)((hi << 4) | lo);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

static const char *host_clean(const char *host)
{
    while (*host == '@' || *host == ' ') host++;   /* fleet-@ habit */
    return host;
}

#define MEDIA_MAX 24
typedef struct {
    char name[64];
    long size;
    char sha[65];
} media_ent_t;

static media_ent_t s_ents[MEDIA_MAX];
static int s_ent_n = 0;
static bool s_dirty = true;
static httpd_handle_t s_srv = NULL;
static char s_dir[80];

void whm_http_media_dirty(void)
{
    s_dirty = true;
}

static const char *media_dir(void)
{
    struct stat st;
    snprintf(s_dir, sizeof(s_dir), "%s/media", WHM_SD_MOUNT);
    if (stat(s_dir, &st) == 0 && S_ISDIR(st.st_mode)) return s_dir;
    if (mkdir(s_dir, 0777) == 0) {
        ESP_LOGI(TAG, "created %s", s_dir);
        /* adopt strays: any .mp3 living at card root moves in */
        DIR *d = opendir(WHM_SD_MOUNT);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".mp3") != 0) continue;
                char a[176], b[176];
                snprintf(a, sizeof(a), "%s/%.90s", WHM_SD_MOUNT,
                         e->d_name);
                snprintf(b, sizeof(b), "%s/%.90s", s_dir, e->d_name);
                if (rename(a, b) == 0) {
                    ESP_LOGI(TAG, "migrated %s -> /media", e->d_name);
                }
            }
            closedir(d);
            whm_mp3_invalidate();
        }
        return s_dir;
    }
    ESP_LOGW(TAG, "mkdir %s failed - using card root", s_dir);
    snprintf(s_dir, sizeof(s_dir), "%s", WHM_SD_MOUNT);
    return s_dir;
}

static bool sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *buf = malloc(8192);
    if (!buf) { fclose(f); return false; }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    size_t n;
    while ((n = fread(buf, 1, 8192, f)) > 0) {
        mbedtls_sha256_update(&c, buf, n);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    free(buf);
    fclose(f);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[64] = 0;
    return true;
}

static void manifest_build(void)
{
    if (!s_dirty) return;
    s_ent_n = 0;
    const char *dir = media_dir();
    DIR *d = opendir(dir);
    if (!d) { s_dirty = false; return; }
    struct dirent *e;
    while ((e = readdir(d)) && s_ent_n < MEDIA_MAX) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".mp3") != 0) continue;
        if (strlen(e->d_name) >= 64) continue;
        media_ent_t pre;
        strlcpy(pre.name, e->d_name, sizeof(pre.name));
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, pre.name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        media_ent_t *m = &s_ents[s_ent_n];
        *m = pre;
        m->size = (long)st.st_size;
        ESP_LOGI(TAG, "hashing %s (%ld bytes)...", m->name, m->size);
        if (!sha256_file(path, m->sha)) m->sha[0] = 0;
        s_ent_n++;
    }
    closedir(d);
    s_dirty = false;
    ESP_LOGI(TAG, "manifest: %d file(s)", s_ent_n);
}

/* ---------------------------------------------------------- handlers */

static esp_err_t h_root(httpd_req_t *r)
{
    char node[17] = "whm";
    whm_settings_get_str("node", node, sizeof(node));
    char out[160];
    manifest_build();
    snprintf(out, sizeof(out),
             "WHM node '%s'\nfw %s\nmedia files: %d\n"
             "GET /manifest, GET /media/<file>\n",
             node, WHM_VERSION_STR, s_ent_n);
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_send(r, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_manifest(httpd_req_t *r)
{
    manifest_build();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_ent_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", s_ents[i].name);
        cJSON_AddNumberToObject(o, "s", (double)s_ents[i].size);
        cJSON_AddStringToObject(o, "h", s_ents[i].sha);
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    esp_err_t err = httpd_resp_send(r, js, HTTPD_RESP_USE_STRLEN);
    free(js);
    return err;
}

static esp_err_t h_media(httpd_req_t *r)
{
    char fnbuf[96];
    strlcpy(fnbuf, r->uri + strlen("/media/"), sizeof(fnbuf));
    url_dec(fnbuf);
    const char *fn = fnbuf;
    if (!fn[0] || strchr(fn, '/') || strstr(fn, "..")) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "bad name");
        return ESP_OK;
    }
    char path[168];
    snprintf(path, sizeof(path), "%.80s/%.80s", media_dir(), fn);
    struct stat st;
    if (stat(path, &st) != 0) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_OK;
    }
    long total = (long)st.st_size, from = 0, to = total - 1;
    char rng[48];
    bool partial = false;
    if (httpd_req_get_hdr_value_str(r, "Range", rng, sizeof(rng)) ==
        ESP_OK) {
        long a = 0, b = -1;
        if (sscanf(rng, "bytes=%ld-%ld", &a, &b) >= 1) {
            from = a;
            if (b >= a) to = b;
            if (from < 0 || from >= total) {
                httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "range");
                return ESP_OK;
            }
            if (to >= total) to = total - 1;
            partial = true;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                  "open"); return ESP_OK; }
    fseek(f, from, SEEK_SET);
    httpd_resp_set_type(r, "audio/mpeg");
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%ld", to - from + 1);
    if (partial) {
        httpd_resp_set_status(r, "206 Partial Content");
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", from, to, total);
        httpd_resp_set_hdr(r, "Content-Range", cr);
    }
    char *buf = malloc(4096);
    long left = to - from + 1;
    esp_err_t err = ESP_OK;
    while (left > 0 && buf) {
        size_t want = left > 4096 ? 4096 : (size_t)left;
        size_t got = fread(buf, 1, want, f);
        if (!got) break;
        if (httpd_resp_send_chunk(r, buf, got) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        left -= (long)got;
    }
    free(buf);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_fw(httpd_req_t *r)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "X-WHM-FW", WHM_VERSION_STR);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(4096);
    if (!buf) return httpd_resp_send_500(r);
    size_t left = run->size;
    size_t off = 0;
    esp_err_t err = ESP_OK;
    while (left > 0) {
        size_t want = left > 4096 ? 4096 : left;
        if (esp_partition_read(run, off, buf, want) != ESP_OK) {
            printf("fw: partition_read FAILED at %u - aborting "
                   "serve\n", (unsigned)off);
            err = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(r, buf, want) != ESP_OK) {
            printf("fw: send_chunk FAILED at %u (client gone / "
                   "socket timeout / no mem) - aborting serve\n",
                   (unsigned)off);
            err = ESP_FAIL;
            break;
        }
        off += want;
        left -= want;
    }
    free(buf);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(r, NULL, 0);
        printf("fw: served %u KB\n", (unsigned)(off / 1024));
    }
    return ESP_OK;
}

esp_err_t whm_http_start(void)
{
    if (s_srv) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_srv, &cfg), TAG, "httpd");
    httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET, .handler = h_root };
    httpd_uri_t u2 = { .uri = "/manifest", .method = HTTP_GET,
                       .handler = h_manifest };
    httpd_uri_t u3 = { .uri = "/media/*", .method = HTTP_GET,
                       .handler = h_media };
    httpd_uri_t u4 = { .uri = "/fw", .method = HTTP_GET,
                       .handler = h_fw };
    httpd_register_uri_handler(s_srv, &u4);
    httpd_register_uri_handler(s_srv, &u1);
    httpd_register_uri_handler(s_srv, &u2);
    httpd_register_uri_handler(s_srv, &u3);
    ESP_LOGI(TAG, "file service on :80 (/, /manifest, /media/<f>, /fw)");
    return ESP_OK;
}

esp_err_t whm_http_media_lookup(const char *name, char sha_out[65],
                                long *size)
{
    manifest_build();
    for (int i = 0; i < s_ent_n; i++) {
        if (strcasecmp(s_ents[i].name, name) == 0) {
            if (sha_out) strlcpy(sha_out, s_ents[i].sha, 65);
            if (size) *size = s_ents[i].size;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------ client */

static long local_size(const char *name, char sha_out[65], bool want_sha)
{
    for (int i = 0; i < s_ent_n; i++) {
        if (strcmp(s_ents[i].name, name) == 0) {
            if (want_sha) strlcpy(sha_out, s_ents[i].sha, 65);
            return s_ents[i].size;
        }
    }
    return -1;
}

static bool pull_one(const char *host, const char *name, long rsize)
{
    char part[176], fin[168], url[248];
    snprintf(fin, sizeof(fin), "%s/%s", media_dir(), name);
    snprintf(part, sizeof(part), "%s.part", fin);
    struct stat st;
    long off = (stat(part, &st) == 0) ? (long)st.st_size : 0;
    if (off > rsize) { unlink(part); off = 0; }
    char enc[200];
    url_enc(name, enc, sizeof(enc));
    snprintf(url, sizeof(url), "http://%s/media/%s", host_clean(host),
             enc);
    esp_http_client_config_t cc = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cc);
    if (!h) return false;
    if (off > 0) {
        char rng[40];
        snprintf(rng, sizeof(rng), "bytes=%ld-", off);
        esp_http_client_set_header(h, "Range", rng);
        printf("  resuming %s at %ld\n", name, off);
    }
    bool ok = false;
    do {
        if (esp_http_client_open(h, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);
        if (status != 200 && status != 206) {
            printf("  %s: http %d\n", name, status);
            break;
        }
        if (status == 200 && off > 0) { off = 0; unlink(part); }
        FILE *f = fopen(part, off ? "ab" : "wb");
        if (!f) break;
        char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
        if (!buf) buf = malloc(16384);
        long got = off, mark = off;
        int64_t t0 = esp_timer_get_time();
        int n;
        while (buf && (n = esp_http_client_read(h, buf, 16384)) > 0) {
            if (fwrite(buf, 1, n, f) != (size_t)n) { n = -1; break; }
            got += n;
            if (got - mark >= 524288) {
                printf("  %s: %ld/%ld KB\n", name, got / 1024,
                       rsize / 1024);
                mark = got;
            }
        }
        int64_t dt = esp_timer_get_time() - t0;
        if (got > off && dt > 0) {
            printf("  %s: %ld KB at %ld KB/s\n", name,
                   (got - off) / 1024,
                   (long)(((got - off) * 1000000LL) / dt / 1024));
        }
        free(buf);
        fclose(f);
        if (got == rsize) {
            unlink(fin);
            ok = (rename(part, fin) == 0);
        } else {
            printf("  %s: partial %ld/%ld (kept .part for resume)\n",
                   name, got, rsize);
        }
    } while (0);
    esp_http_client_cleanup(h);
    return ok;
}

esp_err_t whm_media_sync(const char *host)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s/manifest", host_clean(host));
    esp_http_client_config_t cc = { .url = url, .timeout_ms = 6000 };
    esp_http_client_handle_t h = esp_http_client_init(&cc);
    if (!h) return ESP_FAIL;
    char *js = NULL;
    int len = 0;
    do {
        if (esp_http_client_open(h, 0) != ESP_OK) break;
        int cl = esp_http_client_fetch_headers(h);
        if (cl <= 0 || cl > 8192) cl = 8192;
        js = malloc(cl + 1);
        if (!js) break;
        len = esp_http_client_read(h, js, cl);
        if (len > 0) js[len] = 0;
    } while (0);
    esp_http_client_cleanup(h);
    if (!js || len <= 0) {
        free(js);
        printf("sync media: no manifest from %s\n", host);
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_Parse(js);
    free(js);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        printf("sync media: bad manifest\n");
        return ESP_FAIL;
    }
    manifest_build();                      /* fresh local view */
    int pulled = 0, skipped = 0, failed = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, "n"));
        cJSON *sj = cJSON_GetObjectItem(it, "s");
        const char *hh = cJSON_GetStringValue(cJSON_GetObjectItem(it, "h"));
        if (!n || !cJSON_IsNumber(sj)) continue;
        long rs = (long)sj->valuedouble;
        char lsha[65] = "";
        long ls = local_size(n, lsha, true);
        if (ls == rs && hh && lsha[0] && strcmp(hh, lsha) == 0) {
            skipped++;
            continue;
        }
        printf("pulling %s (%ld KB)...\n", n, rs / 1024);
        bool ok = pull_one(host, n, rs);
        if (!ok) {
            printf("  retrying %s once...\n", n);
            ok = pull_one(host, n, rs);      /* .part resume kicks in */
        }
        if (ok) pulled++;
        else failed++;
    }
    cJSON_Delete(arr);
    if (pulled) {
        whm_http_media_dirty();
        whm_mp3_invalidate();
    }
    printf("sync media: %d pulled, %d up-to-date, %d failed\n",
           pulled, skipped, failed);
    return failed ? ESP_FAIL : ESP_OK;
}
