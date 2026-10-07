/* test_vt.c : checks for the cell buffer, key encoder and VT layer */

#define GUTERM_IMPLEMENTATION
#define GUTERM_NO_WINDOW
#include "guterm.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static uint32_t
cp_at(const struct gut_buf *b, int row, int col)
{
    return b->cells[row * b->cols + col].cp;
}

/* Read a row back as ASCII, with '.' for non-ASCII and trimmed spaces. */
static const char *
row_text(const struct gut_buf *b, int row)
{
    static char out[256];
    int n = 0;

    for (int c = 0; c < b->cols && n < (int)sizeof(out) - 1; c++) {
        uint32_t cp = cp_at(b, row, c);

        out[n++] = (cp >= 0x20 && cp < 0x7F) ? (char)cp : '.';
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    return out;
}

static void
feed(struct gut_vt *vt, const char *s)
{
    gut_vt_feed(vt, s, strlen(s));
}

static void
test_buf(void)
{
    struct gut_buf b;

    CHECK(gut_buf_init(&b, 3, 10) == 0);
    CHECK(gut_buf_text(&b, 0, 0, "hello", gut_color_default(),
                       gut_color_default(), 0) == 5);
    CHECK(strcmp(row_text(&b, 0), "hello") == 0);
    memset(b.dirty, 0, 3);
    gut_buf_put(&b, 0, 5, '!', gut_color_default(), gut_color_default(), 0);
    CHECK(b.dirty[0] == 1 && b.dirty[1] == 0);

    /* wide character takes two cells and clips at the edge */
    CHECK(gut_buf_put(&b, 1, 8, 0x6F22, gut_color_default(),
                      gut_color_default(), 0) == 2);
    CHECK(b.cells[1 * 10 + 9].cp == GUT_CELL_CONT);
    CHECK(gut_buf_put(&b, 1, 9, 0x6F22, gut_color_default(),
                      gut_color_default(), 0) == 0);
    /* overwriting half of it clears the partner */
    gut_buf_put(&b, 1, 9, 'x', gut_color_default(), gut_color_default(), 0);
    CHECK(cp_at(&b, 1, 8) == ' ');
    CHECK(cp_at(&b, 1, 9) == 'x');

    gut_buf_scroll(&b, 0, 3, 1, gut_color_default());
    CHECK(strcmp(row_text(&b, 0), "         x") == 0);
    CHECK(strcmp(row_text(&b, 2), "") == 0);

    CHECK(gut_buf_resize(&b, 2, 5) == 0);
    CHECK(b.rows == 2 && b.cols == 5);
    CHECK(strcmp(row_text(&b, 0), "") == 0);
    gut_buf_free(&b);
}

static void
test_utf8(void)
{
    unsigned char out[4];
    uint32_t cp;

    CHECK(gut_utf8_encode(out, 0x6F22) == 3);
    CHECK(gut_utf8_decode(&cp, out, 3) == 3 && cp == 0x6F22);
    CHECK(gut_utf8_decode(&cp, (const unsigned char *)"\xff", 1) == 1 &&
          cp == 0xFFFD);
    CHECK(gut_utf8_decode(&cp, (const unsigned char *)"\xc0\x80", 2) == 1 &&
          cp == 0xFFFD);
    CHECK(gut_rune_width('a') == 1);
    CHECK(gut_rune_width(0x6F22) == 2);
    CHECK(gut_rune_width(0x0301) == 0);
    CHECK(gut_rune_width(0x1B) == -1);
}

static void
test_font(void)
{
    const struct gut_font *f = gut_font_default();

    CHECK(f->glyph_w == 8 && f->glyph_h == 16);
    CHECK(gut_font_lookup(f, 'A') >= 0);
    CHECK(gut_font_lookup(f, 0x2500) >= 0);
    CHECK(gut_font_lookup(f, 0x2588) >= 0);
    CHECK(gut_font_lookup(f, 0x1F600) < 0);
    for (int i = 1; i < f->nglyphs; i++)
        CHECK(f->cmap[i] > f->cmap[i - 1]);
}

static size_t
enc(int type, int key, int mods, const char *text, int flags, char *out)
{
    struct gut_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.key = key;
    ev.mods = mods;
    if (text)
        snprintf(ev.text, sizeof(ev.text), "%s", text);
    return gut_encode_event(&ev, out, 16, flags);
}

static void
test_encode(void)
{
    char out[16];

    CHECK(enc(GUT_EVENT_TEXT, 0, 0, "ab", 0, out) == 2 &&
          memcmp(out, "ab", 2) == 0);
    CHECK(enc(GUT_EVENT_KEY, 'a', 0, NULL, 0, out) == 0);
    CHECK(enc(GUT_EVENT_KEY, 'c', GUT_MOD_CTRL, NULL, 0, out) == 1 &&
          out[0] == 3);
    CHECK(enc(GUT_EVENT_KEY, 'x', GUT_MOD_ALT, NULL, 0, out) == 2 &&
          memcmp(out, "\033x", 2) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_UP, 0, NULL, 0, out) == 3 &&
          memcmp(out, "\033[A", 3) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_UP, 0, NULL, GUT_ENC_APP_CURSOR,
              out) == 3 && memcmp(out, "\033OA", 3) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_UP, GUT_MOD_CTRL, NULL, 0, out) == 6 &&
          memcmp(out, "\033[1;5A", 6) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_DELETE, 0, NULL, 0, out) == 4 &&
          memcmp(out, "\033[3~", 4) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_F1, 0, NULL, 0, out) == 3 &&
          memcmp(out, "\033OP", 3) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_F5, GUT_MOD_SHIFT, NULL, 0, out) == 7 &&
          memcmp(out, "\033[15;2~", 7) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_TAB, GUT_MOD_SHIFT, NULL, 0, out) == 3 &&
          memcmp(out, "\033[Z", 3) == 0);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_BACKSPACE, 0, NULL, 0, out) == 1 &&
          out[0] == 0x7F);
    CHECK(enc(GUT_EVENT_KEY, GUT_KEY_ENTER, 0, NULL, 0, out) == 1 &&
          out[0] == '\r');
}

static char last_title[64];
static char last_reply[64];

static void
title_cb(void *ctx, const char *title)
{
    (void)ctx;
    snprintf(last_title, sizeof(last_title), "%s", title);
}

static void
reply_cb(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    if (len >= sizeof(last_reply))
        len = sizeof(last_reply) - 1;
    memcpy(last_reply, data, len);
    last_reply[len] = '\0';
}

static void
test_vt(void)
{
    struct gut_buf b;
    struct gut_vt vt;
    struct gut_cell *c;

    CHECK(gut_buf_init(&b, 5, 10) == 0);
    CHECK(gut_vt_init(&vt, &b) == 0);
    gut_vt_set_title_cb(&vt, title_cb, NULL);
    gut_vt_set_reply(&vt, reply_cb, NULL);

    feed(&vt, "hello\r\nworld");
    CHECK(strcmp(row_text(&b, 0), "hello") == 0);
    CHECK(strcmp(row_text(&b, 1), "world") == 0);
    CHECK(b.cursor_row == 1 && b.cursor_col == 5);

    /* autowrap with pending wrap at the last column */
    feed(&vt, "\033[2J\033[H0123456789X");
    CHECK(strcmp(row_text(&b, 0), "0123456789") == 0);
    CHECK(strcmp(row_text(&b, 1), "X") == 0);
    feed(&vt, "\033[1;10HA\033[1;10HB");
    CHECK(cp_at(&b, 0, 9) == 'B');
    CHECK(b.cursor_row == 0);

    /* SGR */
    feed(&vt, "\033[2J\033[H\033[1;31;44mR\033[0m\033[38;5;100mG"
         "\033[48;2;1;2;3mT\033[m");
    c = gut_buf_cell(&b, 0, 0);
    CHECK(c->attrs == GUT_ATTR_BOLD);
    CHECK(c->fg.type == GUT_COLOR_INDEXED && c->fg.index == 1);
    CHECK(c->bg.type == GUT_COLOR_INDEXED && c->bg.index == 4);
    c = gut_buf_cell(&b, 0, 1);
    CHECK(c->attrs == 0 && c->fg.index == 100);
    c = gut_buf_cell(&b, 0, 2);
    CHECK(c->bg.type == GUT_COLOR_RGB && c->bg.r == 1 && c->bg.g == 2 &&
          c->bg.b == 3);
    CHECK(vt.attrs == 0 && vt.fg.type == GUT_COLOR_DEFAULT);

    /* erase and insert/delete */
    feed(&vt, "\033[2J\033[Habcdefghij\033[1;3H\033[K");
    CHECK(strcmp(row_text(&b, 0), "ab") == 0);
    feed(&vt, "\033[2J\033[Habcdefghij\033[1;3H\033[2@");
    CHECK(strcmp(row_text(&b, 0), "ab  cdefgh") == 0);
    feed(&vt, "\033[1;1H\033[2P");
    CHECK(strcmp(row_text(&b, 0), "  cdefgh") == 0);
    feed(&vt, "\033[1;1H\033[3X");
    CHECK(strcmp(row_text(&b, 0), "   defgh") == 0);

    /* scroll region, index and reverse index */
    feed(&vt, "\033[2J\033[H1\r\n2\r\n3\r\n4\r\n5");
    feed(&vt, "\033[2;4r\033[4;1H\033D");
    CHECK(strcmp(row_text(&b, 0), "1") == 0);
    CHECK(strcmp(row_text(&b, 1), "3") == 0);
    CHECK(strcmp(row_text(&b, 2), "4") == 0);
    CHECK(strcmp(row_text(&b, 3), "") == 0);
    CHECK(strcmp(row_text(&b, 4), "5") == 0);
    feed(&vt, "\033[2;1H\033M");
    CHECK(strcmp(row_text(&b, 1), "") == 0);
    CHECK(strcmp(row_text(&b, 2), "3") == 0);
    feed(&vt, "\033[r");
    CHECK(vt.scroll_top == 0 && vt.scroll_bot == 5);

    /* insert and delete lines */
    feed(&vt, "\033[2J\033[Ha\r\nb\r\nc\033[1;1H\033[L");
    CHECK(strcmp(row_text(&b, 0), "") == 0);
    CHECK(strcmp(row_text(&b, 1), "a") == 0);
    feed(&vt, "\033[2M");
    CHECK(strcmp(row_text(&b, 0), "b") == 0);
    CHECK(strcmp(row_text(&b, 1), "c") == 0);

    /* tabs */
    feed(&vt, "\033[2J\033[H\tx");
    CHECK(cp_at(&b, 0, 8) == 'x');
    feed(&vt, "\033[1;3H\033H\033[1;1H\ty");
    CHECK(cp_at(&b, 0, 2) == 'y');

    /* save and restore, DEC graphics */
    feed(&vt, "\033[2J\033[H\0337\033[3;3H\0338z");
    CHECK(cp_at(&b, 0, 0) == 'z');
    feed(&vt, "\033(0q\033(B");
    CHECK(cp_at(&b, 0, 1) == 0x2500);

    /* alternate screen */
    feed(&vt, "\033[2J\033[Hmain\033[?1049h");
    CHECK(strcmp(row_text(&b, 0), "") == 0);
    CHECK(vt.modes & GUT_VT_MODE_ALTSCREEN);
    feed(&vt, "alt\033[?1049l");
    CHECK(strcmp(row_text(&b, 0), "main") == 0);
    CHECK(b.cursor_col == 4);

    /* modes and cursor visibility */
    feed(&vt, "\033[?25l");
    CHECK(b.cursor_visible == 0);
    feed(&vt, "\033[?25h\033[?1h");
    CHECK(b.cursor_visible == 1);
    CHECK(gut_vt_encode_flags(&vt) == GUT_ENC_APP_CURSOR);
    feed(&vt, "\033[?1l");
    CHECK(gut_vt_encode_flags(&vt) == 0);

    /* OSC title with BEL and ST terminators */
    feed(&vt, "\033]2;hello\007");
    CHECK(strcmp(last_title, "hello") == 0);
    feed(&vt, "\033]0;again\033\\");
    CHECK(strcmp(last_title, "again") == 0);

    /* replies */
    feed(&vt, "\033[2;3H\033[6n");
    CHECK(strcmp(last_reply, "\033[2;3R") == 0);
    feed(&vt, "\033[c");
    CHECK(strcmp(last_reply, "\033[?1;2c") == 0);

    /* UTF-8 and wide characters */
    feed(&vt, "\033[2J\033[H\xe6\xbc\xa2x");
    CHECK(cp_at(&b, 0, 0) == 0x6F22);
    CHECK(cp_at(&b, 0, 1) == GUT_CELL_CONT);
    CHECK(cp_at(&b, 0, 2) == 'x');
    feed(&vt, "\033[1;10H\xe6\xbc\xa2");
    CHECK(cp_at(&b, 1, 0) == 0x6F22);

    /* unknown and malformed sequences do not stick */
    feed(&vt, "\033[?9999;1z\033[>1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17m"
         "\033P garbage \033\\\033_apc\033\\ok");
    CHECK(vt.state == GUT_ST_GROUND);

    /* resize */
    CHECK(gut_vt_resize(&vt, 3, 6) == 0);
    CHECK(b.rows == 3 && b.cols == 6);
    CHECK(vt.scroll_bot == 3);
    feed(&vt, "\033[9;9Hq");
    CHECK(cp_at(&b, 2, 5) == 'q');

    gut_vt_free(&vt);
    gut_buf_free(&b);
}

int
main(void)
{
    test_buf();
    test_utf8();
    test_font();
    test_encode();
    test_vt();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
