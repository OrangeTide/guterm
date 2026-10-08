/* torture.c : random input stress test for the buffer, encoder and VT */

/*
 * Feeds the VT layer random byte streams, escape-heavy garbage and UTF-8
 * fragments in random chunk sizes, interleaved with random resizes, and
 * checks the structural invariants after every step. Also hammers the
 * buffer API and the key encoder with random arguments, in-range and not.
 *
 * Every heap input is allocated at its exact size so that address
 * sanitizer catches a read past the end of a chunk.
 *
 * Usage: torture [iterations] [seed]
 */

#define GUTERM_IMPLEMENTATION
#define GUTERM_NO_WINDOW
#include "guterm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint32_t
rnd(void)
{
    /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

static int
rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static int failures;

#define FAIL(...) \
    do { \
        failures++; \
        fprintf(stderr, "torture: iteration %lu: ", (unsigned long)iter); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } while (0)

static unsigned long iter;

/* ---- invariants ---- */

static void
check_buf(const struct gut_buf *b)
{
    if (b->rows < 1 || b->cols < 1)
        FAIL("buffer %dx%d", b->rows, b->cols);
    if (b->cursor_row < 0 || b->cursor_row >= b->rows ||
        b->cursor_col < 0 || b->cursor_col >= b->cols)
        FAIL("cursor %d,%d outside %dx%d", b->cursor_row, b->cursor_col,
             b->rows, b->cols);
    for (int r = 0; r < b->rows; r++) {
        for (int c = 0; c < b->cols; c++) {
            const struct gut_cell *cell = &b->cells[r * b->cols + c];

            if (cell->width > 2)
                FAIL("cell %d,%d width %d", r, c, cell->width);
            if (cell->width == 2) {
                if (c + 1 >= b->cols)
                    FAIL("wide cell at last column %d,%d", r, c);
                else if (b->cells[r * b->cols + c + 1].width != 0)
                    FAIL("wide cell %d,%d without continuation", r, c);
            }
            if (cell->width == 0) {
                if (cell->cp != GUT_CELL_CONT)
                    FAIL("zero width cell %d,%d is not CONT", r, c);
                if (c == 0 || b->cells[r * b->cols + c - 1].width != 2)
                    FAIL("orphan continuation at %d,%d", r, c);
            }
            if (cell->fg.type > GUT_COLOR_RGB || cell->bg.type > GUT_COLOR_RGB)
                FAIL("bad color type at %d,%d", r, c);
        }
    }
}

static void
check_vt(const struct gut_vt *vt)
{
    const struct gut_buf *b = vt->buf;

    check_buf(b);
    if (vt->row < 0 || vt->row >= b->rows || vt->col < 0 ||
        vt->col >= b->cols)
        FAIL("vt cursor %d,%d outside %dx%d", vt->row, vt->col, b->rows,
             b->cols);
    if (vt->scroll_top < 0 || vt->scroll_bot > b->rows ||
        vt->scroll_top >= vt->scroll_bot)
        FAIL("scroll region %d..%d in %d rows", vt->scroll_top,
             vt->scroll_bot, b->rows);
    if (vt->state < GUT_ST_GROUND || vt->state > GUT_ST_DCS_PASSTHRU)
        FAIL("parser state %d", vt->state);
    if (vt->nparam < 0 || vt->nparam > GUT_VT_MAX_PARAMS)
        FAIL("nparam %d", vt->nparam);
    if (vt->osc_len >= GUT_VT_OSC_MAX || vt->osc_len > vt->osc_cap ||
        (vt->osc_len > 0 && !vt->osc))
        FAIL("osc_len %lu cap %lu", (unsigned long)vt->osc_len,
             (unsigned long)vt->osc_cap);
    if (vt->utf8_len < 0 || vt->utf8_len > 4 || vt->utf8_need > 4)
        FAIL("utf8 state %d/%d", vt->utf8_len, vt->utf8_need);
    if (vt->charset < 0 || vt->charset > 1)
        FAIL("charset %d", vt->charset);
    if (vt->saved.row < 0 || vt->saved.col < 0)
        FAIL("saved cursor %d,%d", vt->saved.row, vt->saved.col);
    if (vt->sb_len < 0 || vt->sb_len > vt->sb_cap)
        FAIL("scrollback holds %d of %d", vt->sb_len, vt->sb_cap);
    if (vt->view < 0 || vt->view > vt->sb_len)
        FAIL("view %d with %d lines", vt->view, vt->sb_len);
    if (vt->view > 0 && (vt->modes & GUT_VT_MODE_ALTSCREEN))
        FAIL("view %d on the alternate screen", vt->view);
    if (vt->view > 0 && (vt->buf != &vt->live || vt->out == vt->buf))
        FAIL("view %d but the emulator writes to the caller", vt->view);
    if (vt->view == 0 && vt->buf != vt->out)
        FAIL("live but the emulator writes elsewhere");
    if (vt->view > 0) {
        check_buf(vt->out);
        if (vt->out->rows != b->rows || vt->out->cols != b->cols)
            FAIL("view %dx%d differs from screen %dx%d", vt->out->rows,
                 vt->out->cols, b->rows, b->cols);
        if (vt->out->cursor_visible)
            FAIL("cursor shown while scrolled back");
    }
    for (int i = 0; i < vt->sb_len; i++) {
        const struct gut_vt_line *line =
            &vt->sb[(vt->sb_head + i) % vt->sb_cap];

        if (line->n < 0 || (line->n > 0 && !line->cells))
            FAIL("scrollback line %d has %d cells", i, line->n);
        for (int c = 0; c < line->n; c++)
            if (line->cells[c].width > 2)
                FAIL("scrollback line %d cell %d width %d", i, c,
                     line->cells[c].width);
    }
}

/* ---- input generators ---- */

static const char csi_finals[] = "ABCDEFGHJJJKLMPSTXZ@cdfghlmnrsuq`";
static const char esc_finals[] = "78DEHM=>c()B0";

static size_t
gen_escape(unsigned char *out, size_t cap)
{
    size_t n = 0;
    int kind = rnd_range(0, 9);

    if (cap < 48)
        return 0;
    out[n++] = 0x1B;
    switch (kind) {
    case 0: case 1: case 2: case 3:
        out[n++] = '[';
        if (rnd_range(0, 3) == 0)
            out[n++] = "?>=<! "[rnd_range(0, 5)];
        for (int p = rnd_range(0, 5); p > 0; p--) {
            int v = rnd_range(0, 4) == 0 ? rnd_range(0, 99999)
                                         : rnd_range(0, 300);

            n += (size_t)snprintf((char *)out + n, cap - n, "%d", v);
            if (p > 1)
                out[n++] = rnd_range(0, 7) == 0 ? ':' : ';';
        }
        if (rnd_range(0, 7) == 0)
            out[n++] = ' ';
        out[n++] = csi_finals[rnd() % (sizeof(csi_finals) - 1)];
        break;
    case 4:
        out[n++] = esc_finals[rnd() % (sizeof(esc_finals) - 1)];
        break;
    case 5:
        out[n++] = '(';
        out[n++] = "0B"[rnd_range(0, 1)];
        break;
    case 6: {
        int len = rnd_range(0, 24);

        out[n++] = ']';
        if (rnd_range(0, 2) == 0) {
            /* OSC 52: a selection list and base64, a query or junk */
            n += (size_t)snprintf((char *)out + n, cap - n, "52;%s;",
                                  (const char *[]){ "", "c", "p", "s0",
                                                    "cp", "x" }
                                  [rnd_range(0, 5)]);
            if (rnd_range(0, 3) == 0) {
                out[n++] = '?';
                len = 0;
            }
            for (int i = 0; i < len; i++)
                out[n++] = (unsigned char)
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                    "0123456789+/=\n*"[rnd_range(0, 67)];
            len = 0;
        } else {
            n += (size_t)snprintf((char *)out + n, cap - n, "%d;",
                                  rnd_range(0, 3) == 0 ? rnd_range(0, 9999)
                                                       : rnd_range(0, 2));
        }
        for (int i = 0; i < len; i++)
            out[n++] = (unsigned char)rnd_range(0x20, 0x7E);
        if (rnd_range(0, 1)) {
            out[n++] = 0x07;
        } else {
            out[n++] = 0x1B;
            out[n++] = '\\';
        }
        break;
    }
    case 7:
        out[n++] = "PX^_"[rnd_range(0, 3)];
        for (int i = rnd_range(0, 16); i > 0; i--)
            out[n++] = (unsigned char)rnd();
        if (rnd_range(0, 1)) {
            out[n++] = 0x1B;
            out[n++] = '\\';
        }
        break;
    case 8:
        /* alt screen and modes most programs toggle */
        n += (size_t)snprintf((char *)out + n, cap - n, "[?%d%c",
                              (int[]){ 1, 6, 7, 25, 47, 1047, 1049, 1000,
                                       1002, 1003, 2004 }[rnd_range(0, 10)],
                              "hl"[rnd_range(0, 1)]);
        break;
    default:
        out[n++] = (unsigned char)rnd();
        break;
    }
    return n;
}

static size_t
gen_utf8(unsigned char *out, size_t cap)
{
    uint32_t cp;
    int pick = rnd_range(0, 5);

    if (cap < 4)
        return 0;
    switch (pick) {
    case 0: cp = (uint32_t)rnd_range(0x20, 0x7E); break;
    case 1: cp = (uint32_t)rnd_range(0xA0, 0x7FF); break;
    case 2: cp = (uint32_t)rnd_range(0x4E00, 0x9FFF); break;  /* wide */
    case 3: cp = (uint32_t)rnd_range(0x300, 0x36F); break;    /* zero */
    case 4: cp = (uint32_t)rnd_range(0x10000, 0x10FFFF); break;
    default: cp = rnd() % 0x110000; break;
    }
    return (size_t)gut_utf8_encode(out, cp);
}

/* Build one chunk of mixed input into an exactly sized heap buffer. */
static unsigned char *
gen_chunk(size_t *len_out)
{
    unsigned char tmp[1024];
    size_t n = 0;
    int target = rnd_range(1, 512);
    unsigned char *copy;

    while ((int)n < target) {
        int kind = rnd_range(0, 9);

        if (kind < 4) {
            n += gen_escape(tmp + n, sizeof(tmp) - n);
        } else if (kind < 7) {
            n += gen_utf8(tmp + n, sizeof(tmp) - n);
        } else if (kind == 7) {
            tmp[n++] = (unsigned char)rnd_range(0, 0x1F);
        } else if (kind == 8) {
            tmp[n++] = (unsigned char)rnd();
        } else {
            tmp[n++] = "\r\n\t\b"[rnd_range(0, 3)];
        }
        if (n > sizeof(tmp) - 64)
            break;
    }
    if (n == 0)
        tmp[n++] = 'x';
    copy = malloc(n);
    memcpy(copy, tmp, n);
    *len_out = n;
    return copy;
}

/* ---- phases ---- */

static void
reply_sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    (void)len;
}

static void
title_sink(void *ctx, const char *title)
{
    (void)ctx;
    (void)strlen(title);
}

static void
clip_sink(void *ctx, int which, const char *text, size_t len)
{
    (void)ctx;
    if (which != GUT_CLIP_CLIPBOARD && which != GUT_CLIP_PRIMARY)
        FAIL("clipboard selection %d", which);
    if (text[len] != '\0')
        FAIL("clipboard text not terminated at %lu", (unsigned long)len);
}

static const char *
clip_source(void *ctx, int which)
{
    (void)ctx;
    return which == GUT_CLIP_CLIPBOARD ? "clip \xe6\xbc\xa2 text" : NULL;
}

static void
torture_vt(unsigned long iterations)
{
    struct gut_buf b;
    struct gut_vt vt;

    gut_buf_init(&b, rnd_range(1, 40), rnd_range(1, 120));
    gut_vt_init(&vt, &b);
    gut_vt_set_reply(&vt, reply_sink, NULL);
    gut_vt_set_title_cb(&vt, title_sink, NULL);
    gut_vt_set_clipboard_cb(&vt, clip_sink, clip_source, NULL);

    for (iter = 0; iter < iterations; iter++) {
        size_t len;
        unsigned char *chunk = gen_chunk(&len);

        /* feed in sub-chunks so sequences split across calls */
        for (size_t off = 0; off < len;) {
            size_t piece = (size_t)rnd_range(1, 16);

            if (piece > len - off)
                piece = len - off;
            gut_vt_feed(&vt, (const char *)chunk + off, piece);
            off += piece;
        }
        free(chunk);
        check_vt(&vt);

        if (rnd_range(0, 19) == 0) {
            if (gut_vt_resize(&vt, rnd_range(1, 40), rnd_range(1, 120)) != 0)
                FAIL("resize failed");
            check_vt(&vt);
        }
        if (rnd_range(0, 7) == 0) {
            int off;

            switch (rnd_range(0, 3)) {
            case 0: off = gut_vt_set_view(&vt, 0); break;
            case 1: off = gut_vt_set_view(&vt, rnd_range(-5, 2000)); break;
            case 2: off = gut_vt_scroll_view(&vt, rnd_range(-50, 50)); break;
            default: off = gut_vt_scroll_view(&vt, rnd_range(0, 1)
                                              ? INT_MAX : INT_MIN); break;
            }
            if (off != gut_vt_view_offset(&vt))
                FAIL("view offset %d reported %d", off,
                     gut_vt_view_offset(&vt));
            check_vt(&vt);
        }
        if (rnd_range(0, 99) == 0) {
            if (gut_vt_set_scrollback(&vt, rnd_range(0, 3) == 0
                                      ? 0 : rnd_range(1, 200)) != 0)
                FAIL("set_scrollback failed");
            check_vt(&vt);
        }
        if (rnd_range(0, 199) == 0) {
            gut_vt_clear_scrollback(&vt);
            if (gut_vt_scrollback_lines(&vt) != 0 ||
                gut_vt_view_offset(&vt) != 0)
                FAIL("clear_scrollback left %d lines, view %d",
                     gut_vt_scrollback_lines(&vt), gut_vt_view_offset(&vt));
            check_vt(&vt);
        }
        if (rnd_range(0, 499) == 0) {
            gut_vt_reset(&vt);
            check_vt(&vt);
        }
    }
    gut_vt_free(&vt);
    gut_buf_free(&b);
}

static struct gut_color
rnd_color(void)
{
    switch (rnd_range(0, 2)) {
    case 0: return gut_color_default();
    case 1: return gut_color_indexed(rnd_range(-5, 300));
    default: return gut_color_rgb(rnd_range(0, 255), rnd_range(0, 255),
                                  rnd_range(0, 255));
    }
}

static void
torture_buf(unsigned long iterations)
{
    struct gut_buf b;
    char text[64];

    gut_buf_init(&b, rnd_range(1, 30), rnd_range(1, 100));
    for (iter = 0; iter < iterations; iter++) {
        int r = rnd_range(-3, b.rows + 3), c = rnd_range(-3, b.cols + 3);
        uint16_t attrs = (uint16_t)rnd();

        switch (rnd_range(0, 8)) {
        case 0:
            gut_buf_put(&b, r, c, rnd() % 0x110000, rnd_color(),
                        rnd_color(), attrs);
            break;
        case 1: {
            size_t n = 0;

            for (int i = rnd_range(0, 12); i > 0 && n < sizeof(text) - 5;
                 i--)
                n += gen_utf8((unsigned char *)text + n, sizeof(text) - n);
            text[n] = '\0';
            gut_buf_text(&b, r, c, text, rnd_color(), rnd_color(), attrs);
            break;
        }
        case 2:
            gut_buf_fill(&b, r, c, rnd_range(-2, 10), rnd_range(-2, 10),
                         rnd() % 0x110000, rnd_color(), rnd_color(), attrs);
            break;
        case 3:
            gut_buf_scroll(&b, rnd_range(-3, b.rows + 3),
                           rnd_range(-3, b.rows + 3), rnd_range(-50, 50),
                           rnd_color());
            break;
        case 4:
            gut_buf_clear_rows(&b, rnd_range(-3, b.rows + 3),
                               rnd_range(-3, b.rows + 3), rnd_color());
            break;
        case 5:
            gut_buf_clear(&b, rnd_color());
            break;
        case 6:
            if (gut_buf_cell(&b, r, c) && (r < 0 || c < 0 || r >= b.rows ||
                                           c >= b.cols))
                FAIL("gut_buf_cell returned out of range %d,%d", r, c);
            break;
        case 7:
            if (gut_buf_resize(&b, rnd_range(1, 30), rnd_range(1, 100)) != 0)
                FAIL("buf resize failed");
            break;
        default:
            gut_buf_dirty_all(&b);
            break;
        }
        check_buf(&b);
    }
    gut_buf_free(&b);
}

static void
torture_encode(unsigned long iterations)
{
    for (iter = 0; iter < iterations; iter++) {
        struct gut_event ev;
        size_t cap = (size_t)rnd_range(0, 20);
        char *out = malloc(cap ? cap : 1);
        char *data = NULL;
        size_t n;

        memset(&ev, 0, sizeof(ev));
        ev.type = rnd_range(GUT_EVENT_NONE, GUT_EVENT_KEY_UP);
        ev.button = rnd_range(0, 4);
        ev.row = rnd_range(-1, 300);
        ev.col = rnd_range(-1, 300);
        ev.dx = rnd_range(-2, 2);
        ev.dy = rnd_range(-2, 2);
        ev.mods = rnd_range(0, 15);
        switch (rnd_range(0, 2)) {
        case 0: ev.key = rnd_range(GUT_KEY_NONE, GUT_KEY_F12 + 3); break;
        case 1: ev.key = rnd_range(0, 0x7F); break;
        default: ev.key = (int)(rnd() % 0x120000) - 0x1000; break;
        }
        for (int i = 0; i < (int)sizeof(ev.text) - 1; i++)
            ev.text[i] = (char)rnd_range(1, 255);
        ev.text[rnd_range(0, (int)sizeof(ev.text) - 1)] = '\0';
        ev.text[sizeof(ev.text) - 1] = '\0';

        /* PASTE and COMPOSE carry their text through data and len */
        if (rnd_range(0, 3) == 0) {
            size_t dlen = (size_t)rnd_range(0, 40);

            data = malloc(dlen + 1);
            for (size_t i = 0; i < dlen; i++)
                data[i] = (char)rnd_range(1, 255);
            data[dlen] = '\0';
            ev.data = data;
            ev.len = dlen;
            ev.type = rnd_range(0, 1) ? GUT_EVENT_PASTE : GUT_EVENT_COMPOSE;
        }

        n = gut_encode_event(&ev, out, cap, rnd_range(0, 511));
        if (cap > 0 && out[n < cap ? n : cap - 1] != '\0')
            FAIL("encode output not terminated, %lu for cap %lu",
                 (unsigned long)n, (unsigned long)cap);
        if (ev.type == GUT_EVENT_PASTE && ev.data && n > ev.len + 12)
            FAIL("paste encoding longer than text plus brackets");
        free(data);
        free(out);
    }
}

static void
torture_copy(unsigned long iterations)
{
    struct gut_buf b;

    gut_buf_init(&b, rnd_range(1, 20), rnd_range(1, 60));
    for (iter = 0; iter < iterations; iter++) {
        size_t cap = (size_t)rnd_range(0, 64);
        char *out = malloc(cap ? cap : 1);
        size_t n;

        if (rnd_range(0, 3) == 0)
            gut_buf_put(&b, rnd_range(0, b.rows - 1), rnd_range(0, b.cols - 1),
                        rnd_range(0, 1) ? (uint32_t)rnd_range(0x21, 0x7E)
                                        : (uint32_t)rnd_range(0x4E00, 0x4E20),
                        gut_color_default(), gut_color_default(), 0);
        n = gut_buf_copy_text(&b, rnd_range(-2, b.rows + 2),
                              rnd_range(-2, b.cols + 2),
                              rnd_range(-2, b.rows + 2),
                              rnd_range(-2, b.cols + 2), rnd_range(0, 1),
                              cap ? out : NULL, cap);
        if (cap > 0 && out[n < cap ? n : cap - 1] != '\0')
            FAIL("copy output not terminated");
        if (cap > 0 && n < cap && strlen(out) != n)
            FAIL("copy length %lu does not match %lu", (unsigned long)n,
                 (unsigned long)strlen(out));
        free(out);
    }
    gut_buf_free(&b);
}


/* Random mouse events over a random buffer; the selection must stay
 * normalised and inside the buffer, and its text must terminate. */
static void
torture_sel(unsigned long iterations)
{
    struct gut_buf b;
    struct gut_sel s;
    char text[64];

    gut_buf_init(&b, rnd_range(1, 20), rnd_range(1, 60));
    gut_sel_clear(&s);
    for (iter = 0; iter < iterations; iter++) {
        struct gut_event ev;
        char out[32];
        size_t cap = (size_t)rnd_range(0, sizeof(out));

        if (rnd_range(0, 3) == 0) {
            for (int i = 0; i < (int)sizeof(text) - 1; i++)
                text[i] = (char)rnd_range(0x20, 0x7E);
            text[rnd_range(0, 63)] = '\0';
            gut_buf_text(&b, rnd_range(0, b.rows - 1), rnd_range(0, b.cols),
                         text, rnd_color(), rnd_color(), 0);
            gut_buf_put(&b, rnd_range(0, b.rows - 1), rnd_range(0, b.cols),
                        0x4E00 + rnd_range(0, 100), gut_color_default(),
                        gut_color_default(), 0);
        }
        memset(&ev, 0, sizeof(ev));
        ev.type = rnd_range(GUT_EVENT_MOUSE_DOWN, GUT_EVENT_MOUSE_WHEEL);
        ev.button = rnd_range(0, 3);
        ev.row = rnd_range(-2, b.rows + 1);
        ev.col = rnd_range(-2, b.cols + 1);
        ev.mods = rnd_range(0, 15);
        ev.clicks = rnd_range(0, 4);
        gut_sel_mouse(&s, &b, &ev);
        if (rnd_range(0, 9) == 0)
            gut_sel_extend(&s, &b, rnd_range(-5, 50), rnd_range(-5, 100));
        if (s.active) {
            if (s.row0 < 0 || s.row1 >= b.rows || s.col0 < 0 ||
                s.col1 >= b.cols)
                FAIL("selection %d,%d..%d,%d outside %dx%d", s.row0, s.col0,
                     s.row1, s.col1, b.rows, b.cols);
            if (s.row0 > s.row1 ||
                (s.mode == GUT_COPY_RECT ? s.col0 > s.col1
                 : s.row0 == s.row1 && s.col0 > s.col1))
                FAIL("selection %d,%d..%d,%d not normalised", s.row0, s.col0,
                     s.row1, s.col1);
            if (!gut_sel_contains(&s, s.row0, s.col0) ||
                !gut_sel_contains(&s, s.row1, s.col1))
                FAIL("selection does not contain its ends");
        }
        gut_sel_text(&s, &b, out, cap);
        if (cap > 0 && memchr(out, '\0', cap) == NULL)
            FAIL("selection text not terminated");
        if (rnd_range(0, 99) == 0) {
            gut_buf_resize(&b, rnd_range(1, 20), rnd_range(1, 60));
            gut_sel_clear(&s);
        }
    }
    gut_buf_free(&b);
}

static void
torture_utf8(unsigned long iterations)
{
    for (iter = 0; iter < iterations; iter++) {
        size_t len = (size_t)rnd_range(0, 6);
        unsigned char *s = malloc(len ? len : 1);
        uint32_t cp;
        int n;

        for (size_t i = 0; i < len; i++)
            s[i] = (unsigned char)rnd();
        n = gut_utf8_decode(&cp, s, len);
        if (len == 0 && n != 0)
            FAIL("decode of empty input consumed %d", n);
        if (len > 0 && (n < 1 || (size_t)n > len))
            FAIL("decode consumed %d of %lu", n, (unsigned long)len);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            FAIL("decode produced U+%X", cp);
        free(s);

        cp = rnd() % 0x110000;
        if (!(cp >= 0xD800 && cp <= 0xDFFF)) {
            unsigned char enc[4];
            uint32_t back;
            int elen = gut_utf8_encode(enc, cp);

            if (elen < 1 || gut_utf8_decode(&back, enc, (size_t)elen) != elen
                || back != cp)
                FAIL("round trip of U+%X failed", cp);
        }
        (void)gut_rune_width(rnd());
        (void)gut_font_lookup(gut_font_default(), rnd());
    }
}

int
main(int argc, char **argv)
{
    unsigned long iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 10)
                             : 0x9E3779B97F4A7C15ULL;

    rng_state = seed ? seed : 1;
    printf("torture: %lu iterations, seed %llu\n", iterations,
           (unsigned long long)seed);
    torture_vt(iterations);
    torture_buf(iterations);
    torture_encode(iterations);
    torture_copy(iterations);
    torture_sel(iterations);
    torture_utf8(iterations);
    printf("torture: %d failures\n", failures);
    return failures ? 1 : 0;
}
