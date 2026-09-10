/* whssh_log — the ESP_LOG tee. A vprintf hook copies each formatted log
 * line into a PSRAM ring when an SSH session is active, then forwards to
 * the original vprintf so the USB feed is untouched. Task-context only
 * (esp_log never calls the hook from an ISR), so PSRAM is safe; a mutex
 * guards the ring (many tasks log). Drop-oldest with a counter: a remote
 * viewer prefers fresh lines over blocking loggers. */
#include "whssh.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define WL_LINE 160
#define WL_N     48

EXT_RAM_BSS_ATTR static char s_ring[WL_N][WL_LINE];
static int s_head, s_tail;
static unsigned s_dropped;
static volatile bool s_active;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;

static int tee_vprintf(const char *fmt, va_list ap)
{
    if (s_active && s_lock) {
        va_list ap2;
        va_copy(ap2, ap);
        char line[WL_LINE];
        vsnprintf(line, sizeof line, fmt, ap2);
        va_end(ap2);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            int nxt = (s_head + 1) % WL_N;
            if (nxt == s_tail) {                 /* full: drop oldest */
                s_tail = (s_tail + 1) % WL_N;
                s_dropped++;
            }
            memcpy(s_ring[s_head], line, WL_LINE);
            s_head = nxt;
            xSemaphoreGive(s_lock);
        } else {
            s_dropped++;
        }
    }
    return s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
}

void whssh_log_hook_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_orig = esp_log_set_vprintf(tee_vprintf);
}

void whssh_log_set_active(bool on)
{
    s_active = on;
    if (!on && s_lock &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_head = s_tail = 0;                     /* stale lines die here */
        xSemaphoreGive(s_lock);
    }
}

bool whssh_log_take(char *out, size_t cap)
{
    bool got = false;
    if (!s_lock) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (s_tail != s_head) {
        snprintf(out, cap, "%s", s_ring[s_tail]);
        s_tail = (s_tail + 1) % WL_N;
        got = true;
    }
    xSemaphoreGive(s_lock);
    return got;
}

unsigned whssh_log_dropped_take(void)
{
    unsigned d = s_dropped;
    s_dropped = 0;
    return d;
}
