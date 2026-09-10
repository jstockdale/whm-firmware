/* wh_lineedit – pure, transport-agnostic line editor (Layer 1 of the serial
 * console, per SERIAL-CONSOLE-SPEC §3).
 *
 * It is a pure transducer: you feed it one received byte and it appends the
 * exact bytes to emit (echo, cursor motion, repaints) into a caller-provided
 * buffer, and tells you when a complete line is ready. It does no I/O and knows
 * nothing about the transport, the command set, or logs, so it is fully
 * host-testable: feed a byte string, assert on the emitted bytes and the line.
 *
 * Only <stddef.h>/<stdint.h>/<string.h>/<stdbool.h> – no ESP-IDF, no stdio.
 *
 * Beyond the node reference this editor adds a horizontal-scroll window so long
 * lines (e.g. `hid type <text>`) render on a single row without wrap artifacts.
 * For lines that fit the terminal width the emitted result is identical to a
 * naive single-line repaint, so parity (spec §1.1) holds. */
#ifndef WH_LINEEDIT_H
#define WH_LINEEDIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Compile-time caps – budget, not contract (spec §1.2/§10). The watch has RAM
 * to spend, so history is deeper than the node's 8. */
#ifndef WH_LE_LINE_MAX
#define WH_LE_LINE_MAX     160   /* max editable line incl. NUL (matches console LINE_CAP) */
#endif
#ifndef WH_LE_HIST_N
#define WH_LE_HIST_N        24   /* history ring depth */
#endif
#ifndef WH_LE_PROMPT_MAX
#define WH_LE_PROMPT_MAX    24   /* max prompt incl. NUL */
#endif
#ifndef WH_LE_WIDTH_DEFAULT
#define WH_LE_WIDTH_DEFAULT 80   /* assumed terminal width in columns */
#endif

/* Completion indexer supplied by the device (spec §3.6):
 *   verb == NULL : return the idx-th top-level command verb
 *   verb != NULL : return the idx-th completion for the FIRST argument of `verb`
 *   return NULL once idx is past the last candidate. */
typedef const char *(*wh_le_complete_fn)(void *ctx, const char *verb, int idx);

typedef enum { WH_LE_NORMAL, WH_LE_ESC, WH_LE_CSI, WH_LE_SS3 } wh_le_state_t;

typedef struct {
    char buf[WH_LE_LINE_MAX];    /* editable line; buf[len] is always '\0' */
    int  len;                    /* bytes in buf (excl. NUL) */
    int  cur;                    /* cursor index, 0..len */
    int  scroll;                 /* index of first visible char (horizontal scroll) */
    int  width;                  /* terminal width in columns */

    wh_le_state_t st;            /* escape-parser state, carried across feeds */
    bool skip_lf;                /* swallow the LF of a CRLF */
    int  csi_param;              /* accumulated CSI numeric parameter */

    char prompt[WH_LE_PROMPT_MAX];
    int  plen;

    char hist[WH_LE_HIST_N][WH_LE_LINE_MAX];
    int  hist_count;             /* entries stored, 0..WH_LE_HIST_N */
    int  hist_head;              /* ring: index of the next write slot */
    int  nav;                    /* 0 = live line; 1..hist_count = steps back */
    char stash[WH_LE_LINE_MAX];  /* live line saved while browsing history */
    int  stash_len;

    wh_le_complete_fn complete;
    void *complete_ctx;
} wh_lineedit_t;

/* Initialise. prompt is copied; NULL -> "> ". */
void   wh_le_init(wh_lineedit_t *l, const char *prompt);

/* Optional completion indexer (NULL disables Tab completion). */
void   wh_le_set_completer(wh_lineedit_t *l, wh_le_complete_fn fn, void *ctx);

/* Set the assumed terminal width (columns). Ignored if < 20. */
void   wh_le_set_width(wh_lineedit_t *l, int cols);

/* Emit a fresh prompt (call after a command's output, per the ordering contract). */
size_t wh_le_prompt(wh_lineedit_t *l, char *out, size_t cap);

/* Repaint prompt + current line + cursor (used to restore input after async logs). */
size_t wh_le_render(wh_lineedit_t *l, char *out, size_t cap);

/* Feed one received byte. Appends bytes-to-emit into out[0..out_cap) and sets
 * *out_len. Returns 1 when a line is complete, copying it (NUL-terminated) into
 * line[0..line_cap); returns 0 otherwise. On completion the editor emits CRLF
 * and resets but does NOT emit a new prompt. */
int    wh_le_feed(wh_lineedit_t *l, char c,
                  char *out, size_t out_cap, size_t *out_len,
                  char *line, size_t line_cap);

#endif /* WH_LINEEDIT_H */
