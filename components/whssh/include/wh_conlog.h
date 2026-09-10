/* wh_conlog – pure helper for the single-writer console adapter.
 *
 * The console redirects ESP_LOG output into a queue and drains it between
 * keystrokes (see console.c / debug.c). Queued lines are raw log text; this
 * formats one for a raw VT terminal. Pure (only <stddef.h>), so it is
 * host-testable. */
#ifndef WH_CONLOG_H
#define WH_CONLOG_H

#include <stddef.h>

/* Format a captured ESP_LOG line for a raw VT terminal:
 *   - wrap it in a colour chosen from the leading level letter (E=red, W=yellow, I=green,
 *     D=cyan; verbose/unknown reset to default), always ending with a reset, so every line
 *     is self-contained and can't inherit leaked terminal colour (CONFIG_LOG_COLORS is off,
 *     so ESP-IDF itself emits none);
 *   - strip any embedded/leading ANSI escapes from the source;
 *   - translate each embedded '\n' to "\r\n", drop stray '\r', end with exactly one CRLF;
 *   - return 0 (empty output) for an empty / whitespace-only record, so stray blank records
 *     don't render as blank lines.
 * Writes into out[0..cap), NUL-terminates if room, never overflows. Returns bytes written
 * (excluding the NUL). Pure (only <stddef.h>), so it is host-testable. */
size_t wh_log_line_fmt(const char *in, char *out, size_t cap);

#endif /* WH_CONLOG_H */
