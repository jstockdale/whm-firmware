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
        "<img id=i src=/fb.bmp style='width:96%;max-width:1072px;"
        "image-rendering:pixelated;border:1px solid #2a3446'>"
        "<p><button onclick=\"i.src='/fb.bmp?'+Date.now()\" "
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
            uint16_t c = s[x];
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
        httpd_uri_t u3 = { .uri = "/viewer", .method = HTTP_GET,
                           .handler = h_viewer };
        httpd_register_uri_handler(h, &u3);
        ESP_LOGI(TAG, "the eye: http://<ip>/  (/fb.bmp raw)");
    }
}
