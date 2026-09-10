/* The Eye: atomic framebuffer capture + HTTP viewer.
 * GET /        tiny page with the live capture + reload
 * GET /fb.bmp  24-bit top-down BMP, converted from the RGB565 snap
 * Console 'snap' uses the same atomic path, base64 over serial. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mon_pins.h"
#include "mon_http.h"
static const char *TAG = "mon_http";
static uint16_t *s_snap;                    /* PSRAM copy */
static volatile int s_req;
static SemaphoreHandle_t s_done;
void mon_snap_service(const uint16_t *fb)
{
    if (!s_req) return;
    memcpy(s_snap, fb, LCD_W * LCD_H * 2);
    s_req = 0;
    xSemaphoreGive(s_done);
}
int mon_snap_take(void)
{
    xSemaphoreTake(s_done, 0);              /* drain stale */
    s_req = 1;
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) == pdTRUE
           ? 0 : -1;
}
const uint16_t *mon_snap_buf(void) { return s_snap; }
static esp_err_t h_root(httpd_req_t *r)
{
    static const char pg[] =
        "<!doctype html><meta name=viewport "
        "content='width=device-width'>"
        "<body style='background:#0b0e14;color:#c8d2e0;"
        "font-family:monospace;text-align:center'>"
        "<h3>whm-monitor - the eye</h3>"
        "<img id=i src=/fb.png style='width:96%;max-width:1072px;"
        "image-rendering:pixelated;border:1px solid #2a3446'>"
        "<p><button onclick=\"i.src='/fb.png?'+Date.now()\" "
        "style='padding:10px 18px'>capture</button></body>";
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, pg, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_bmp(httpd_req_t *r)
{
    if (mon_snap_take() != 0) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "snap timeout");
        return ESP_FAIL;
    }
    uint32_t rowb = LCD_W * 3;              /* 1608, 4-aligned */
    uint32_t datab = rowb * LCD_H;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + datab;
    memcpy(hdr + 2, &fsz, 4);
    uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t bi = 40;  memcpy(hdr + 14, &bi, 4);
    int32_t w = LCD_W; memcpy(hdr + 18, &w, 4);
    int32_t hh = -LCD_H; memcpy(hdr + 22, &hh, 4);  /* top-down */
    uint16_t pl = 1;  memcpy(hdr + 26, &pl, 2);
    uint16_t bpp = 24; memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &datab, 4);
    httpd_resp_set_type(r, "image/bmp");
    httpd_resp_send_chunk(r, (char *)hdr, 54);
    uint8_t *row = heap_caps_malloc(rowb, MALLOC_CAP_INTERNAL);
    for (int y = 0; y < LCD_H; y++) {
        const uint16_t *s = s_snap + y * LCD_W;
        for (int x = 0; x < LCD_W; x++) {
            uint16_t c = __builtin_bswap16(s[x]); /* fb pre-swapped */
            row[x * 3 + 2] = (uint8_t)((c >> 8) & 0xF8);  /* R */
            row[x * 3 + 1] = (uint8_t)((c >> 3) & 0xFC);  /* G */
            row[x * 3 + 0] = (uint8_t)((c << 3) & 0xF8);  /* B */
        }
        httpd_resp_send_chunk(r, (char *)row, rowb);
    }
    free(row);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}
extern const uint8_t viewer_html_start[]
    asm("_binary_whm_viewer_html_start");
extern const uint8_t viewer_html_end[]
    asm("_binary_whm_viewer_html_end");
static esp_err_t h_viewer(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, (const char *)viewer_html_start,
                           viewer_html_end - viewer_html_start - 1);
}
/* PNG, zero-dependency: stored-deflate blocks are a valid zlib
 * stream, so this is a real PNG every decoder accepts - same
 * bytes as raw, streamed. Upgrade path: espressif/zlib managed
 * component swaps stored blocks for real compression later. */
static uint32_t crc32b(uint32_t c, const uint8_t *b, int n)
{
    c = ~c;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (-(int32_t)(c & 1)));
    }
    return ~c;
}
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static esp_err_t h_png(httpd_req_t *r)
{
    if (mon_snap_take() != 0) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "snap timeout");
        return ESP_FAIL;
    }
    const uint32_t rowb = 1 + LCD_W * 3;         /* filter + RGB */
    const uint32_t raw = rowb * LCD_H;
    const uint32_t nblk = (raw + 65534) / 65535;
    const uint32_t zlen = 2 + nblk * 5 + raw + 4;
    httpd_resp_set_type(r, "image/png");
    static const uint8_t sig[8] =
        { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    httpd_resp_send_chunk(r, (char *)sig, 8);
    uint8_t ih[25];
    be32(ih, 13); memcpy(ih + 4, "IHDR", 4);
    be32(ih + 8, LCD_W); be32(ih + 12, LCD_H);
    ih[16] = 8; ih[17] = 2; ih[18] = 0; ih[19] = 0; ih[20] = 0;
    be32(ih + 21, crc32b(0, ih + 4, 17));
    httpd_resp_send_chunk(r, (char *)ih, 25);
    uint8_t ch[8];
    be32(ch, zlen); memcpy(ch + 4, "IDAT", 4);
    httpd_resp_send_chunk(r, (char *)ch, 8);
    uint32_t crc = crc32b(0, (uint8_t *)"IDAT", 4);
    uint32_t a1 = 1, a2 = 0, left = raw, srci = 0;
    uint8_t zh[2] = { 0x78, 0x01 };
    crc = crc32b(crc, zh, 2);
    httpd_resp_send_chunk(r, (char *)zh, 2);
    uint8_t *row = heap_caps_malloc(rowb, MALLOC_CAP_INTERNAL);
    uint32_t rowoff = 0; int y = 0;
    row[0] = 0;
    for (int x = 0; x < LCD_W; x++) {
        uint16_t c = mon_snap_buf()[x];
        row[1 + x * 3] = (c >> 8) & 0xF8;
        row[2 + x * 3] = (c >> 3) & 0xFC;
        row[3 + x * 3] = (c << 3) & 0xF8;
    }
    while (left) {
        uint32_t blk = left > 65535 ? 65535 : left;
        uint8_t bh[5] = { left <= 65535, (uint8_t)blk,
                          (uint8_t)(blk >> 8), (uint8_t)~blk,
                          (uint8_t)(~blk >> 8) };
        crc = crc32b(crc, bh, 5);
        httpd_resp_send_chunk(r, (char *)bh, 5);
        uint32_t done = 0;
        while (done < blk) {
            uint32_t take = rowb - rowoff;
            if (take > blk - done) take = blk - done;
            crc = crc32b(crc, row + rowoff, take);
            for (uint32_t i = 0; i < take; i++) {
                a1 = (a1 + row[rowoff + i]) % 65521;
                a2 = (a2 + a1) % 65521;
            }
            httpd_resp_send_chunk(r, (char *)row + rowoff, take);
            done += take; rowoff += take;
            if (rowoff == rowb) {
                rowoff = 0; y++;
                if (y < LCD_H) {
                    row[0] = 0;
                    const uint16_t *s =
                        mon_snap_buf() + y * LCD_W;
                    for (int x = 0; x < LCD_W; x++) {
                        uint16_t c = __builtin_bswap16(s[x]); /* fb pre-swapped */
                        row[1 + x * 3] = (c >> 8) & 0xF8;
                        row[2 + x * 3] = (c >> 3) & 0xFC;
                        row[3 + x * 3] = (c << 3) & 0xF8;
                    }
                }
            }
        }
        left -= blk; srci += blk;
    }
    free(row);
    uint8_t ad[8];
    be32(ad, (a2 << 16) | a1);
    crc = crc32b(crc, ad, 4);
    be32(ad + 4, crc);
    httpd_resp_send_chunk(r, (char *)ad, 8);
    static const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N',
        'D', 0xAE, 0x42, 0x60, 0x82 };
    httpd_resp_send_chunk(r, (char *)iend, 12);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}
void mon_http_start(void)
{
    s_snap = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    s_done = xSemaphoreCreateBinary();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    httpd_handle_t h;
    if (httpd_start(&h, &cfg) == ESP_OK) {
        httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET,
                           .handler = h_root };
        httpd_uri_t u2 = { .uri = "/fb.bmp", .method = HTTP_GET,
                           .handler = h_bmp };
        httpd_register_uri_handler(h, &u1);
        httpd_register_uri_handler(h, &u2);
        httpd_uri_t u4 = { .uri = "/fb.png", .method = HTTP_GET,
                           .handler = h_png };
        httpd_register_uri_handler(h, &u4);
        httpd_uri_t u3 = { .uri = "/viewer", .method = HTTP_GET,
                           .handler = h_viewer };
        httpd_register_uri_handler(h, &u3);
        ESP_LOGI(TAG, "the eye: http://<ip>/  (/fb.bmp raw)");
    }
}
