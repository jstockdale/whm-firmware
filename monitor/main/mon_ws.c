/* The Relay: the monitor doubles as whm_bridge. A WebSocket server
 * on :8777 speaks the bridge's exact JSON ({t,ip,port,n,hex}), so
 * whm_viewer.html connects unchanged; the viewer itself is embedded
 * in flash and served at /viewer on port 80 (see mon_http.c). */
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mon_ws.h"
static const char *TAG = "mon_ws";
static httpd_handle_t s_ws;
#define MAXC 4
static int s_fd[MAXC];
static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {           /* handshake done */
        int fd = httpd_req_to_sockfd(r);
        for (int i = 0; i < MAXC; i++)
            if (s_fd[i] == 0 || s_fd[i] == fd) { s_fd[i] = fd; break; }
        ESP_LOGI(TAG, "viewer client fd=%d", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };            /* drain client frames */
    httpd_ws_recv_frame(r, &f, 0);
    return ESP_OK;
}
void mon_ws_feed(const uint8_t *b, int n, uint32_t ip)
{
    if (!s_ws || n <= 0 || n > 160) return;
    static char js[480];
    int o = snprintf(js, sizeof js,
        "{\"t\":%lld,\"ip\":\"%u.%u.%u.%u\",\"port\":7777,"
        "\"n\":%d,\"hex\":\"",
        (long long)(esp_timer_get_time() / 1000),
        (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
        (unsigned)((ip >> 16) & 0xff), (unsigned)(ip >> 24), n);
    for (int i = 0; i < n; i++)
        o += snprintf(js + o, sizeof js - o, "%02x", b[i]);
    o += snprintf(js + o, sizeof js - o, "\"}");
    httpd_ws_frame_t f = { .final = true,
                           .type = HTTPD_WS_TYPE_TEXT,
                           .payload = (uint8_t *)js,
                           .len = (size_t)o };
    for (int i = 0; i < MAXC; i++) {
        if (!s_fd[i]) continue;
        if (httpd_ws_send_frame_async(s_ws, s_fd[i], &f) != ESP_OK)
            s_fd[i] = 0;                   /* client gone */
    }
}
void mon_ws_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8777;
    cfg.ctrl_port = 32778;                 /* distinct from :80 */
    if (httpd_start(&s_ws, &cfg) == ESP_OK) {
        static const httpd_uri_t u = { .uri = "/",
            .method = HTTP_GET, .handler = h_ws,
            .is_websocket = true };
        httpd_register_uri_handler(s_ws, &u);
        ESP_LOGI(TAG, "relay: ws://<ip>:8777 (bridge dialect)");
    }
}
