/* wh_conlog – see wh_conlog.h. Pure; no ESP-IDF, no stdio. */

#include "wh_conlog.h"

/* Skip one ANSI escape at *pp if present (CSI "ESC [ ... final", or a lone ESC).
 * On a CSI, advances *pp to the final byte (or the terminating NUL). Returns 1 if an
 * escape was seen, else 0. For a lone ESC, *pp is left on the ESC (caller's loop step
 * moves past it). */
static int skip_esc(const char **pp)
{
    const char *q = *pp;
    if (*q != '\x1b') return 0;
    if (q[1] == '[') {                       /* CSI: ESC '[' params/intermediates final(@..~) */
        q += 2;
        while (*q && (*q < '@' || *q > '~')) q++;
        *pp = q;
    }
    return 1;
}

size_t wh_log_line_fmt(const char *in, char *out, size_t cap)
{
    if (cap == 0) return 0;

    /* Skip any leading escape(s) so a leaked/stray colour code can't hide the level letter. */
    const char *p = in;
    while (*p == '\x1b') { const char *q = p; skip_esc(&q); p = (*q ? q + 1 : q); }

    /* Colour by the ESP-IDF level letter (CONFIG_LOG_COLORS is off, so the letter is bare).
     * V/unknown normalise to a reset so the line still can't inherit leaked terminal colour. */
    const char *col;
    switch (*p) {
        case 'E': col = "\x1b[0;31m"; break;   /* red    */
        case 'W': col = "\x1b[0;33m"; break;   /* yellow */
        case 'I': col = "\x1b[0;32m"; break;   /* green  */
        case 'D': col = "\x1b[0;36m"; break;   /* cyan   */
        default:  col = "\x1b[0m";    break;   /* verbose / unknown */
    }

    /* Any visible content? Skip empty / whitespace-only entries entirely (return 0) so stray
     * empty log records don't render as blank lines. */
    int any = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '\x1b') { const char *e = q; skip_esc(&e); q = e; if (!*q) break; continue; }
        if (*q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') { any = 1; break; }
    }
    if (!any) { out[0] = '\0'; return 0; }

    size_t w = 0;
#define PUT(str) do { for (const char *s_ = (str); *s_ && w + 1 < cap; s_++) out[w++] = *s_; } while (0)
    PUT(col);
    /* Body: strip embedded escapes, drop stray CR, translate LF->CRLF (interior LFs keep the
     * line's colour), and hold the trailing LF (we emit the reset + one CRLF ourselves). */
    for (; *p && w + 8 < cap; p++) {
        if (*p == '\x1b') { const char *e = p; skip_esc(&e); p = e; if (!*p) break; continue; }
        if (*p == '\r') continue;
        if (*p == '\n') { if (p[1] == '\0') break; out[w++] = '\r'; out[w++] = '\n'; continue; }
        out[w++] = *p;
    }
    PUT("\x1b[0m\r\n");
#undef PUT
    if (w < cap) out[w] = '\0';
    return w;
}
