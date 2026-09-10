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

/* THE CHIME CONVICTION (owner's boot log): a vprintf hook taxes
 * EVERY logging task's stack - the 160 B line buffer hoisted into
 * this frame's prologue blew whm_chime's tightly-tuned 2816 during
 * its I2S init logs. The inactive path is now a bare tail-call
 * with ZERO locals; the active path formats into a STATIC scratch
 * under the existing mutex, so the only per-caller stack cost left
 * is vsnprintf's own internals - and that runs only while an SSH
 * session is live. */
static char s_fmt[WL_LINE];              /* guarded by s_lock */

static int tee_active_path(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        vsnprintf(s_fmt, sizeof s_fmt, fmt, ap2);
        int nxt = (s_head + 1) % WL_N;
        if (nxt == s_tail) {                 /* full: drop oldest */
            s_tail = (s_tail + 1) % WL_N;
            s_dropped++;
        }
        memcpy(s_ring[s_head], s_fmt, WL_LINE);
        s_head = nxt;
        xSemaphoreGive(s_lock);
    } else {
        s_dropped++;
    }
    va_end(ap2);
    return s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
}

static int tee_vprintf(const char *fmt, va_list ap)
{
    if (!s_active || !s_lock)
        return s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
    return tee_active_path(fmt, ap);
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
