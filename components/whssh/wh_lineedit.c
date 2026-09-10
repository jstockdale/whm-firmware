/* wh_lineedit – see wh_lineedit.h. Pure line editor, spec §3.
 *
 * Rendering strategy: every visible change repaints the whole line in place
 * (\r, prompt, the visible window, ESC[K, then reposition the cursor). This is
 * simpler than incremental emits and, crucially, makes the horizontal-scroll
 * window fall out for free – the line is always drawn on one physical row, so
 * there are no wrap artifacts and ESC[K alone clears any stale tail. It emits a
 * few more bytes per keystroke than a minimal diff, which is irrelevant on a
 * serial console, and it produces the same on-screen result, so parity holds. */

#include "wh_lineedit.h"
#include <string.h>

/* ---- tiny output appender (never writes past cap) ---- */
typedef struct { char *p; size_t cap; size_t len; } emitbuf_t;

static void em(emitbuf_t *e, const char *s, size_t n)
{
    for (size_t i = 0; i < n && e->len < e->cap; i++) e->p[e->len++] = s[i];
}
static void ems(emitbuf_t *e, const char *s) { em(e, s, strlen(s)); }
static void emc(emitbuf_t *e, char c) { if (e->len < e->cap) e->p[e->len++] = c; }
static void emit_uint(emitbuf_t *e, unsigned v)
{
    char t[10];
    int k = 0;
    if (v == 0) { emc(e, '0'); return; }
    while (v > 0 && k < (int)sizeof t) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0) emc(e, t[--k]);
}

/* ---- small string helper (null-terminating, truncating, no strncpy warts) ---- */
static void scopy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) return;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- rendering ---- */
static int line_viewport(const wh_lineedit_t *l)
{
    int vp = l->width - l->plen;
    if (vp < 8) vp = 8;                 /* floor, in case of a wide prompt / tiny width */
    return vp;
}

static void recompute_scroll(wh_lineedit_t *l)
{
    int vp = line_viewport(l);
    if (l->cur < l->scroll) l->scroll = l->cur;
    if (l->cur - l->scroll >= vp) l->scroll = l->cur - vp + 1;
    if (l->scroll < 0) l->scroll = 0;
}

static void render_into(wh_lineedit_t *l, emitbuf_t *e)
{
    recompute_scroll(l);
    int vp = line_viewport(l);
    int vis_end = l->scroll + vp;
    if (vis_end > l->len) vis_end = l->len;
    int nvis = vis_end - l->scroll;

    emc(e, '\r');
    em(e, l->prompt, (size_t)l->plen);
    em(e, l->buf + l->scroll, (size_t)nvis);
    ems(e, "\x1b[K");                     /* erase from cursor to end of line */
    emc(e, '\r');
    int col = l->plen + (l->cur - l->scroll);
    if (col > 0) { ems(e, "\x1b["); emit_uint(e, (unsigned)col); emc(e, 'C'); }
}

/* ---- history ---- */
static void hist_push(wh_lineedit_t *l, const char *s)
{
    if (s[0] == '\0') return;             /* blank not stored */
    if (l->hist_count > 0) {
        int recent = (l->hist_head - 1 + WH_LE_HIST_N) % WH_LE_HIST_N;
        if (strcmp(l->hist[recent], s) == 0) return;   /* dup of most recent not stored */
    }
    scopy(l->hist[l->hist_head], WH_LE_LINE_MAX, s);
    l->hist_head = (l->hist_head + 1) % WH_LE_HIST_N;
    if (l->hist_count < WH_LE_HIST_N) l->hist_count++;
}

static void load_hist(wh_lineedit_t *l, int nav)   /* nav in 1..hist_count */
{
    int idx = (l->hist_head - nav + WH_LE_HIST_N) % WH_LE_HIST_N;
    scopy(l->buf, WH_LE_LINE_MAX, l->hist[idx]);
    l->len = (int)strlen(l->buf);
    l->cur = l->len;
    l->scroll = 0;
}

static void history_prev(wh_lineedit_t *l, emitbuf_t *e)   /* Up / ^P */
{
    if (l->nav >= l->hist_count) return;               /* nothing older */
    if (l->nav == 0) {                                 /* entering history: stash live line */
        l->buf[l->len] = '\0';
        scopy(l->stash, WH_LE_LINE_MAX, l->buf);
        l->stash_len = l->len;
    }
    l->nav++;
    load_hist(l, l->nav);
    render_into(l, e);
}

static void history_next(wh_lineedit_t *l, emitbuf_t *e)   /* Down / ^N */
{
    if (l->nav == 0) return;                            /* already at live line */
    l->nav--;
    if (l->nav == 0) {                                  /* restore stashed live line */
        scopy(l->buf, WH_LE_LINE_MAX, l->stash);
        l->len = l->stash_len;
        if (l->len > WH_LE_LINE_MAX - 1) l->len = WH_LE_LINE_MAX - 1;
        l->buf[l->len] = '\0';
        l->cur = l->len;
        l->scroll = 0;
    } else {
        load_hist(l, l->nav);
    }
    render_into(l, e);
}

/* ---- editing primitives ---- */
static void insert_char(wh_lineedit_t *l, emitbuf_t *e, char c)
{
    if (l->len >= WH_LE_LINE_MAX - 1) { emc(e, '\a'); return; }   /* full: bell, drop */
    memmove(l->buf + l->cur + 1, l->buf + l->cur, (size_t)(l->len - l->cur));
    l->buf[l->cur] = c;
    l->cur++;
    l->len++;
    l->buf[l->len] = '\0';
    render_into(l, e);
}

static void backspace(wh_lineedit_t *l, emitbuf_t *e)
{
    if (l->cur == 0) return;                            /* no-op at col 0, emits nothing */
    memmove(l->buf + l->cur - 1, l->buf + l->cur, (size_t)(l->len - l->cur));
    l->cur--;
    l->len--;
    l->buf[l->len] = '\0';
    render_into(l, e);
}

static void delete_at(wh_lineedit_t *l, emitbuf_t *e)   /* Delete / ^D */
{
    if (l->cur >= l->len) return;                      /* nothing under cursor */
    memmove(l->buf + l->cur, l->buf + l->cur + 1, (size_t)(l->len - l->cur - 1));
    l->len--;
    l->buf[l->len] = '\0';
    render_into(l, e);
}

static void move_left(wh_lineedit_t *l, emitbuf_t *e)  { if (l->cur > 0)      { l->cur--; render_into(l, e); } }
static void move_right(wh_lineedit_t *l, emitbuf_t *e) { if (l->cur < l->len) { l->cur++; render_into(l, e); } }
static void move_home(wh_lineedit_t *l, emitbuf_t *e)  { if (l->cur != 0)     { l->cur = 0; render_into(l, e); } }
static void move_end(wh_lineedit_t *l, emitbuf_t *e)   { if (l->cur != l->len){ l->cur = l->len; render_into(l, e); } }

static void kill_to_end(wh_lineedit_t *l, emitbuf_t *e)   /* ^K */
{
    if (l->cur >= l->len) return;
    l->len = l->cur;
    l->buf[l->len] = '\0';
    render_into(l, e);
}

static void kill_line(wh_lineedit_t *l, emitbuf_t *e)     /* ^U */
{
    if (l->len == 0 && l->cur == 0) return;
    l->len = 0;
    l->cur = 0;
    l->scroll = 0;
    l->buf[0] = '\0';
    render_into(l, e);
}

static void kill_word(wh_lineedit_t *l, emitbuf_t *e)     /* ^W */
{
    if (l->cur == 0) return;
    int i = l->cur;
    while (i > 0 && l->buf[i - 1] == ' ') i--;          /* skip trailing spaces */
    while (i > 0 && l->buf[i - 1] != ' ') i--;          /* skip the word */
    int removed = l->cur - i;
    if (removed <= 0) return;
    memmove(l->buf + i, l->buf + l->cur, (size_t)(l->len - l->cur));
    l->len -= removed;
    l->cur = i;
    l->buf[l->len] = '\0';
    render_into(l, e);
}

static void clear_screen(wh_lineedit_t *l, emitbuf_t *e) /* ^L */
{
    ems(e, "\x1b[2J\x1b[H");                            /* clear screen + home */
    render_into(l, e);
}

static void reset_line(wh_lineedit_t *l)
{
    l->len = 0;
    l->cur = 0;
    l->scroll = 0;
    l->nav = 0;
    l->stash_len = 0;
    l->buf[0] = '\0';
}

static void cancel_line(wh_lineedit_t *l, emitbuf_t *e)  /* ^C */
{
    ems(e, "^C\r\n");
    reset_line(l);
    em(e, l->prompt, (size_t)l->plen);                  /* fresh prompt */
}

static int accept_line(wh_lineedit_t *l, emitbuf_t *e, char *line, size_t line_cap)
{
    ems(e, "\r\n");
    l->buf[l->len] = '\0';
    if (line && line_cap) {
        size_t n = (size_t)l->len;
        if (n > line_cap - 1) n = line_cap - 1;
        memcpy(line, l->buf, n);
        line[n] = '\0';
    }
    hist_push(l, l->buf);
    reset_line(l);
    return 1;
}

/* ---- completion (spec §3.6) ---- */
static void set_token(wh_lineedit_t *l, int tok_start, const char *val, bool add_space)
{
    int i = tok_start;
    for (const char *p = val; *p && i < WH_LE_LINE_MAX - 1; p++) l->buf[i++] = *p;
    if (add_space && i < WH_LE_LINE_MAX - 1) l->buf[i++] = ' ';
    l->buf[i] = '\0';
    l->len = i;
    l->cur = i;
    l->scroll = 0;
}

static void complete_tab(wh_lineedit_t *l, emitbuf_t *e)
{
    if (!l->complete || l->cur != l->len) { emc(e, '\a'); return; }   /* end-of-line only */
    l->buf[l->len] = '\0';

    int spaces = 0, first_space = -1;
    for (int i = 0; i < l->len; i++) {
        if (l->buf[i] == ' ') { spaces++; if (first_space < 0) first_space = i; }
    }

    const char *verb;
    const char *token;
    int tok_start;
    char verbbuf[WH_LE_LINE_MAX];
    if (spaces == 0) {
        verb = NULL;
        token = l->buf;
        tok_start = 0;
    } else if (spaces == 1) {
        int vl = first_space;
        if (vl > WH_LE_LINE_MAX - 1) vl = WH_LE_LINE_MAX - 1;
        memcpy(verbbuf, l->buf, (size_t)vl);
        verbbuf[vl] = '\0';
        verb = verbbuf;
        token = l->buf + first_space + 1;
        tok_start = first_space + 1;
    } else {
        emc(e, '\a');                                   /* 2+ spaces: out of scope */
        return;
    }

    size_t tlen = strlen(token);
    const char *first_match = NULL;
    int nm = 0;
    char lcp[WH_LE_LINE_MAX];
    int lcp_len = -1;
    for (int idx = 0; ; idx++) {
        const char *cand = l->complete(l->complete_ctx, verb, idx);
        if (!cand) break;
        if (strncmp(cand, token, tlen) != 0) continue;
        if (nm == 0) first_match = cand;
        nm++;
        if (lcp_len < 0) { scopy(lcp, sizeof lcp, cand); lcp_len = (int)strlen(lcp); }
        else {
            int k = 0;
            while (k < lcp_len && cand[k] && lcp[k] == cand[k]) k++;
            lcp_len = k;
            lcp[k] = '\0';
        }
    }

    if (nm == 0) { emc(e, '\a'); return; }
    if (nm == 1) { set_token(l, tok_start, first_match, true); render_into(l, e); return; }
    if (lcp_len > (int)tlen) { set_token(l, tok_start, lcp, false); render_into(l, e); return; }

    /* several matches, no further common prefix: list them (6 per row) and repaint */
    ems(e, "\r\n");
    int shown = 0;
    for (int idx = 0; ; idx++) {
        const char *cand = l->complete(l->complete_ctx, verb, idx);
        if (!cand) break;
        if (strncmp(cand, token, tlen) != 0) continue;
        if (shown > 0) ems(e, (shown % 6 == 0) ? "\r\n" : "  ");
        ems(e, cand);
        shown++;
    }
    ems(e, "\r\n");
    render_into(l, e);
}

/* ---- escape finals ---- */
static void handle_arrow(wh_lineedit_t *l, unsigned char f, emitbuf_t *e)
{
    switch (f) {
    case 'A': history_prev(l, e); break;
    case 'B': history_next(l, e); break;
    case 'C': move_right(l, e); break;
    case 'D': move_left(l, e); break;
    case 'H': move_home(l, e); break;
    case 'F': move_end(l, e); break;
    default: break;
    }
}

static void handle_csi_final(wh_lineedit_t *l, unsigned char f, emitbuf_t *e)
{
    if (f == '~') {
        switch (l->csi_param) {
        case 1: case 7: move_home(l, e); break;
        case 4: case 8: move_end(l, e); break;
        case 3:         delete_at(l, e); break;
        default: break;                                 /* 2=Insert etc.: ignore */
        }
        return;
    }
    handle_arrow(l, f, e);
}

/* ---- public API ---- */
void wh_le_init(wh_lineedit_t *l, const char *prompt)
{
    memset(l, 0, sizeof *l);
    if (!prompt) prompt = "> ";
    scopy(l->prompt, WH_LE_PROMPT_MAX, prompt);
    l->plen = (int)strlen(l->prompt);
    l->width = WH_LE_WIDTH_DEFAULT;
    l->st = WH_LE_NORMAL;
}

void wh_le_set_completer(wh_lineedit_t *l, wh_le_complete_fn fn, void *ctx)
{
    l->complete = fn;
    l->complete_ctx = ctx;
}

void wh_le_set_width(wh_lineedit_t *l, int cols) { if (cols >= 20) l->width = cols; }

size_t wh_le_prompt(wh_lineedit_t *l, char *out, size_t cap)
{
    emitbuf_t e = { out, cap, 0 };
    em(&e, l->prompt, (size_t)l->plen);
    return e.len;
}

size_t wh_le_render(wh_lineedit_t *l, char *out, size_t cap)
{
    emitbuf_t e = { out, cap, 0 };
    l->buf[l->len] = '\0';
    render_into(l, &e);
    return e.len;
}

int wh_le_feed(wh_lineedit_t *l, char c,
               char *out, size_t out_cap, size_t *out_len,
               char *line, size_t line_cap)
{
    emitbuf_t e = { out, out_cap, 0 };
    int done = 0;
    unsigned char ch = (unsigned char)c;

    switch (l->st) {
    case WH_LE_ESC:
        if (ch == '[')      { l->st = WH_LE_CSI; l->csi_param = 0; }
        else if (ch == 'O') { l->st = WH_LE_SS3; }
        else                { l->st = WH_LE_NORMAL; }   /* unknown 2-byte seq consumed */
        goto done_out;

    case WH_LE_CSI:
        if (ch >= '0' && ch <= '9') { l->csi_param = l->csi_param * 10 + (ch - '0'); goto done_out; }
        if (ch == ';') { l->csi_param = 0; goto done_out; } /* we use only the first param */
        if (ch >= 0x40 && ch <= 0x7e) { l->st = WH_LE_NORMAL; handle_csi_final(l, ch, &e); }
        goto done_out;                                  /* intermediates (0x20-0x2f): stay, ignore */

    case WH_LE_SS3:
        l->st = WH_LE_NORMAL;
        handle_arrow(l, ch, &e);
        goto done_out;

    case WH_LE_NORMAL:
    default:
        break;
    }

    /* CRLF: swallow the LF that immediately follows a CR */
    if (l->skip_lf) {
        l->skip_lf = false;
        if (ch == '\n') goto done_out;
    }

    switch (ch) {
    case '\r': l->skip_lf = true; done = accept_line(l, &e, line, line_cap); break;
    case '\n': done = accept_line(l, &e, line, line_cap); break;
    case 0x1b: l->st = WH_LE_ESC; break;                /* ESC */
    case 0x08: case 0x7f: backspace(l, &e); break;      /* Backspace */
    case 0x02: move_left(l, &e); break;                 /* ^B */
    case 0x06: move_right(l, &e); break;                /* ^F */
    case 0x01: move_home(l, &e); break;                 /* ^A */
    case 0x05: move_end(l, &e); break;                  /* ^E */
    case 0x04: delete_at(l, &e); break;                 /* ^D */
    case 0x10: history_prev(l, &e); break;              /* ^P */
    case 0x0e: history_next(l, &e); break;              /* ^N */
    case 0x0b: kill_to_end(l, &e); break;               /* ^K */
    case 0x15: kill_line(l, &e); break;                 /* ^U */
    case 0x17: kill_word(l, &e); break;                 /* ^W */
    case 0x0c: clear_screen(l, &e); break;              /* ^L */
    case 0x09: complete_tab(l, &e); break;              /* Tab */
    case 0x03: cancel_line(l, &e); break;               /* ^C */
    default:
        if (ch >= 0x20 && ch < 0x7f) insert_char(l, &e, (char)ch);
        /* other control bytes < 0x20 are ignored */
        break;
    }

done_out:
    if (out_len) *out_len = e.len;
    return done;
}
