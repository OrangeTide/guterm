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

    /* snprintf semantics: full length returned, output truncated */
    {
        struct gut_event ev;
        char small[4];

        memset(&ev, 0, sizeof(ev));
        ev.type = GUT_EVENT_KEY;
        ev.key = GUT_KEY_F5;
        CHECK(gut_encode_event(&ev, small, sizeof(small), 0) == 5);
        CHECK(memcmp(small, "\033[1", 3) == 0 && small[3] == '\0');
        CHECK(gut_encode_event(&ev, NULL, 0, 0) == 5);

        /* paste: newlines become CR, bracketed on request */
        ev.type = GUT_EVENT_PASTE;
        ev.data = "a\r\nb\nc";
        ev.len = 6;
        CHECK(gut_encode_event(&ev, out, 16, 0) == 5 &&
              memcmp(out, "a\rb\rc", 5) == 0);
        CHECK(gut_encode_event(&ev, out, 16, GUT_ENC_BRACKET_PASTE) == 17);
        CHECK(gut_encode_event(&ev, out, 16, GUT_ENC_BRACKET_PASTE) == 17 &&
              memcmp(out, "\033[200~a\rb\rc\033[20", 15) == 0 &&
              out[15] == '\0');
        {
            char big[32];

            CHECK(gut_encode_event(&ev, big, sizeof(big),
                                   GUT_ENC_BRACKET_PASTE) == 17 &&
                  memcmp(big, "\033[200~a\rb\rc\033[201~", 17) == 0);
        }
        /* text events prefer data over the fixed array */
        ev.type = GUT_EVENT_TEXT;
        snprintf(ev.text, sizeof(ev.text), "short");
        ev.data = "the whole committed string";
        ev.len = strlen(ev.data);
        CHECK(gut_encode_event(&ev, out, 16, 0) == ev.len);
        ev.data = NULL;
        CHECK(gut_encode_event(&ev, out, 16, 0) == 5);
    }
}

static void
test_copy(void)
{
    struct gut_buf b;
    char out[64];
    size_t n;

    gut_buf_init(&b, 3, 8);
    gut_buf_text(&b, 0, 0, "ab  cd", gut_color_default(),
                 gut_color_default(), 0);
    gut_buf_text(&b, 1, 2, "x\xe6\xbc\xa2y", gut_color_default(),
                 gut_color_default(), 0);
    gut_buf_text(&b, 2, 0, "end", gut_color_default(), gut_color_default(),
                 0);

    /* stream: whole rows between the corners, trailing blanks trimmed */
    n = gut_buf_copy_text(&b, 0, 4, 2, 1, GUT_COPY_STREAM, out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(strcmp(out, "cd\n  x\xe6\xbc\xa2y\nen") == 0);

    /* reversed corners give the same result */
    n = gut_buf_copy_text(&b, 2, 1, 0, 4, GUT_COPY_STREAM, out, sizeof(out));
    CHECK(strcmp(out, "cd\n  x\xe6\xbc\xa2y\nen") == 0);

    /* rectangle */
    n = gut_buf_copy_text(&b, 0, 0, 2, 2, GUT_COPY_RECT, out, sizeof(out));
    CHECK(strcmp(out, "ab\n  x\nend") == 0);

    /* continuation cell at the rectangle's left edge is skipped */
    n = gut_buf_copy_text(&b, 1, 4, 1, 5, GUT_COPY_RECT, out, sizeof(out));
    CHECK(strcmp(out, "y") == 0);

    /* truncation and sizing */
    n = gut_buf_copy_text(&b, 0, 0, 0, 7, GUT_COPY_RECT, NULL, 0);
    CHECK(n == 6);
    n = gut_buf_copy_text(&b, 0, 0, 0, 7, GUT_COPY_RECT, out, 3);
    CHECK(n == 6 && strcmp(out, "ab") == 0);

    /* out of range corners are clamped */
    n = gut_buf_copy_text(&b, -5, -5, 50, 50, GUT_COPY_STREAM, out,
                          sizeof(out));
    CHECK(strcmp(out, "ab  cd\n  x\xe6\xbc\xa2y\nend") == 0);
    gut_buf_free(&b);
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
    feed(&vt, "\033[?2004h");
    CHECK(gut_vt_encode_flags(&vt) == GUT_ENC_BRACKET_PASTE);
    feed(&vt, "\033[?2004l");
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

static void
test_scrollback(void)
{
    struct gut_buf b;
    struct gut_vt vt;

    CHECK(gut_buf_init(&b, 3, 8) == 0);
    CHECK(gut_vt_init(&vt, &b) == 0);
    CHECK(gut_vt_scrollback_lines(&vt) == 0);
    CHECK(gut_vt_set_scrollback(&vt, 4) == 0);

    /* six lines through a three row screen: three fall off the top */
    feed(&vt, "l1\r\nl2\r\nl3\r\nl4\r\nl5\r\nl6");
    CHECK(gut_vt_scrollback_lines(&vt) == 3);
    CHECK(strcmp(row_text(&b, 0), "l4") == 0);
    CHECK(strcmp(row_text(&b, 2), "l6") == 0);
    CHECK(gut_vt_view_offset(&vt) == 0);

    /* scroll back one line: l3 on top, cursor hidden */
    CHECK(gut_vt_set_view(&vt, 1) == 1);
    CHECK(strcmp(row_text(&b, 0), "l3") == 0);
    CHECK(strcmp(row_text(&b, 1), "l4") == 0);
    CHECK(strcmp(row_text(&b, 2), "l5") == 0);
    CHECK(b.cursor_visible == 0);
    CHECK(b.dirty[0] && b.dirty[2]);

    /* output behind the view keeps the view on the same lines */
    feed(&vt, "\r\nl7");
    CHECK(gut_vt_view_offset(&vt) == 2);
    CHECK(gut_vt_scrollback_lines(&vt) == 4);
    CHECK(strcmp(row_text(&b, 0), "l3") == 0);
    CHECK(strcmp(row_text(&b, 2), "l5") == 0);

    /* clamped, and back to live shows the new output and cursor */
    CHECK(gut_vt_scroll_view(&vt, 100) == 4);
    CHECK(strcmp(row_text(&b, 0), "l1") == 0);
    CHECK(gut_vt_scroll_view(&vt, -100) == 0);
    CHECK(strcmp(row_text(&b, 0), "l5") == 0);
    CHECK(strcmp(row_text(&b, 2), "l7") == 0);
    CHECK(b.cursor_visible == 1);
    CHECK(b.cursor_row == 2 && b.cursor_col == 2);

    /* the ring drops the oldest line, l1 */
    feed(&vt, "\r\nl8");
    CHECK(gut_vt_scrollback_lines(&vt) == 4);
    gut_vt_set_view(&vt, 4);
    CHECK(strcmp(row_text(&b, 0), "l2") == 0);
    gut_vt_set_view(&vt, 0);

    /* the alternate screen neither feeds nor shows the scrollback */
    feed(&vt, "\033[?1049h");
    CHECK(gut_vt_set_view(&vt, 2) == 0);
    feed(&vt, "a\r\nb\r\nc\r\nd\r\ne");
    CHECK(gut_vt_scrollback_lines(&vt) == 4);
    feed(&vt, "\033[?1049l");
    CHECK(strcmp(row_text(&b, 2), "l8") == 0);

    /* growing the screen pulls lines back, shrinking pushes them */
    CHECK(gut_vt_resize(&vt, 5, 8) == 0);
    CHECK(gut_vt_scrollback_lines(&vt) == 2);
    CHECK(strcmp(row_text(&b, 0), "l4") == 0);
    CHECK(strcmp(row_text(&b, 4), "l8") == 0);
    CHECK(b.cursor_row == 4);
    CHECK(gut_vt_resize(&vt, 2, 8) == 0);
    CHECK(gut_vt_scrollback_lines(&vt) == 4);
    CHECK(strcmp(row_text(&b, 0), "l7") == 0);
    CHECK(strcmp(row_text(&b, 1), "l8") == 0);
    CHECK(b.cursor_row == 1);

    /* a narrower screen clips stored lines on the way back */
    CHECK(gut_vt_resize(&vt, 3, 1) == 0);
    CHECK(cp_at(&b, 0, 0) == 'l');
    CHECK(gut_vt_resize(&vt, 3, 8) == 0);

    /* shrinking the ring keeps the newest lines */
    CHECK(gut_vt_set_view(&vt, 3) == 3);
    CHECK(gut_vt_set_scrollback(&vt, 1) == 0);
    CHECK(gut_vt_scrollback_lines(&vt) == 1);
    CHECK(gut_vt_view_offset(&vt) == 1);
    CHECK(gut_vt_set_scrollback(&vt, 0) == 0);
    CHECK(gut_vt_view_offset(&vt) == 0);
    CHECK(gut_vt_set_scrollback(&vt, -1) == -1);
    feed(&vt, "\r\n\r\n\r\n");
    CHECK(gut_vt_scrollback_lines(&vt) == 0);

    /* ED 3 clears the scrollback and nothing else */
    CHECK(gut_vt_set_scrollback(&vt, 10) == 0);
    feed(&vt, "\033[2J\033[Hx\r\n\r\n\r\n\r\ny");
    CHECK(gut_vt_scrollback_lines(&vt) == 2);
    feed(&vt, "\033[3J");
    CHECK(gut_vt_scrollback_lines(&vt) == 0);
    CHECK(strcmp(row_text(&b, 2), "y") == 0);

    gut_vt_free(&vt);
    gut_buf_free(&b);
}


static size_t
mouse(int type, int button, int row, int col, int mods, int dx, int dy,
      int flags, char *out, size_t n)
{
    struct gut_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.button = button;
    ev.row = row;
    ev.col = col;
    ev.mods = mods;
    ev.dx = dx;
    ev.dy = dy;
    return gut_encode_event(&ev, out, n, flags);
}

static void
test_mouse_encode(void)
{
    char out[32];
    size_t n;

    /* nothing without a tracking flag */
    CHECK(mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 0, 0, 0, 0, 0,
                out, sizeof(out)) == 0);

    /* legacy encoding: press, release, wheel, modifiers */
    n = mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 4, 9, 0, 0, 0,
              GUT_ENC_MOUSE_BTN, out, sizeof(out));
    CHECK(n == 6 && memcmp(out, "\033[M *%", 6) == 0);
    n = mouse(GUT_EVENT_MOUSE_UP, GUT_BUTTON_RIGHT, 4, 9, 0, 0, 0,
              GUT_ENC_MOUSE_BTN, out, sizeof(out));
    CHECK(n == 6 && memcmp(out, "\033[M#*%", 6) == 0);
    n = mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_MIDDLE, 0, 0, GUT_MOD_CTRL,
              0, 0, GUT_ENC_MOUSE_BTN, out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 1 + 16);
    n = mouse(GUT_EVENT_MOUSE_WHEEL, 0, 0, 0, 0, 0, 1, GUT_ENC_MOUSE_BTN,
              out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 64);
    n = mouse(GUT_EVENT_MOUSE_WHEEL, 0, 0, 0, 0, 0, -1, GUT_ENC_MOUSE_BTN,
              out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 65);
    /* beyond the legacy range */
    CHECK(mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 230, 0, 0, 0,
                GUT_ENC_MOUSE_BTN, out, sizeof(out)) == 0);

    /* motion only under the drag and any flags */
    CHECK(mouse(GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 1, 1, 0, 0, 0,
                GUT_ENC_MOUSE_BTN, out, sizeof(out)) == 0);
    n = mouse(GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 1, 1, 0, 0, 0,
              GUT_ENC_MOUSE_DRAG, out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 32);
    CHECK(mouse(GUT_EVENT_MOUSE_MOVE, 0, 1, 1, 0, 0, 0, GUT_ENC_MOUSE_DRAG,
                out, sizeof(out)) == 0);
    n = mouse(GUT_EVENT_MOUSE_MOVE, 0, 1, 1, 0, 0, 0, GUT_ENC_MOUSE_ANY,
              out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 35);

    /* SGR encoding, large coordinates, release with its own final */
    n = mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 299, 399, 0, 0, 0,
              GUT_ENC_MOUSE_BTN | GUT_ENC_MOUSE_SGR, out, sizeof(out));
    CHECK(n == 13 && strcmp(out, "\033[<0;400;300M") == 0);
    n = mouse(GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 0, 0, GUT_MOD_SHIFT, 0,
              0, GUT_ENC_MOUSE_BTN | GUT_ENC_MOUSE_SGR, out, sizeof(out));
    CHECK(n == 9 && strcmp(out, "\033[<4;1;1m") == 0);
    n = mouse(GUT_EVENT_MOUSE_WHEEL, 0, 2, 3, 0, 0, 1,
              GUT_ENC_MOUSE_ANY | GUT_ENC_MOUSE_SGR, out, sizeof(out));
    CHECK(n == 10 && strcmp(out, "\033[<64;4;3M") == 0);
    /* snprintf sizing */
    n = mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 0, 0, 0, 0,
              GUT_ENC_MOUSE_BTN | GUT_ENC_MOUSE_SGR, out, 4);
    CHECK(n == 9 && strcmp(out, "\033[<") == 0);
    n = mouse(GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 0, 0, 0, 0,
              GUT_ENC_MOUSE_BTN, out, 3);
    CHECK(n == 6 && out[2] == '\0');

    /* focus */
    {
        struct gut_event ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = GUT_EVENT_FOCUS_IN;
        CHECK(gut_encode_event(&ev, out, sizeof(out), 0) == 0);
        CHECK(gut_encode_event(&ev, out, sizeof(out), GUT_ENC_FOCUS) == 3 &&
              strcmp(out, "\033[I") == 0);
        ev.type = GUT_EVENT_FOCUS_OUT;
        CHECK(gut_encode_event(&ev, out, sizeof(out), GUT_ENC_FOCUS) == 3 &&
              strcmp(out, "\033[O") == 0);
    }
}

static void
test_vt_mouse(void)
{
    struct gut_buf b;
    struct gut_vt vt;
    struct gut_event ev;
    char out[64];
    size_t n;

    gut_buf_init(&b, 10, 20);
    gut_vt_init(&vt, &b);
    memset(&ev, 0, sizeof(ev));
    ev.type = GUT_EVENT_MOUSE_DOWN;
    ev.button = GUT_BUTTON_LEFT;
    ev.row = 2;
    ev.col = 3;

    /* no tracking: the host's event */
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);

    feed(&vt, "\033[?1000h");
    CHECK(gut_vt_encode_flags(&vt) == GUT_ENC_MOUSE_BTN);
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n == 6 && memcmp(out, "\033[M $#", 6) == 0);
    /* shift bypasses tracking */
    ev.mods = GUT_MOD_SHIFT;
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);
    ev.mods = 0;

    /* motion reported once per cell under 1002 */
    feed(&vt, "\033[?1002h\033[?1006h");
    CHECK(gut_vt_encode_flags(&vt) ==
          (GUT_ENC_MOUSE_DRAG | GUT_ENC_MOUSE_SGR));
    ev.type = GUT_EVENT_MOUSE_MOVE;
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n > 0 && strcmp(out, "\033[<32;4;3M") == 0);
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);
    ev.col = 4;
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) > 0);
    ev.button = 0;
    ev.col = 5;
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);
    feed(&vt, "\033[?1003h");
    CHECK(gut_vt_encode_flags(&vt) & GUT_ENC_MOUSE_ANY);
    ev.col = 6;
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n > 0 && strcmp(out, "\033[<35;7;3M") == 0);
    feed(&vt, "\033[?1003l\033[?1006l");
    CHECK(gut_vt_encode_flags(&vt) == 0);

    /* focus reporting */
    ev.type = GUT_EVENT_FOCUS_IN;
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);
    feed(&vt, "\033[?1004h");
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 3 &&
          strcmp(out, "\033[I") == 0);
    feed(&vt, "\033[?1004l");

    /* alternate scroll: wheel becomes cursor keys on the alt screen */
    ev.type = GUT_EVENT_MOUSE_WHEEL;
    ev.dy = 1;
    CHECK(gut_vt_mouse(&vt, &ev, out, sizeof(out)) == 0);
    feed(&vt, "\033[?1049h");
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n == 9 && strcmp(out, "\033[A\033[A\033[A") == 0);
    ev.dy = -1;
    feed(&vt, "\033[?1h");
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n == 9 && strcmp(out, "\033OB\033OB\033OB") == 0);
    CHECK(gut_vt_mouse(&vt, &ev, out, 4) == 9 && strcmp(out, "\033OB") == 0);
    feed(&vt, "\033[?1000h");
    n = gut_vt_mouse(&vt, &ev, out, sizeof(out));
    CHECK(n == 6 && out[3] == 32 + 65);

    gut_vt_free(&vt);
    gut_buf_free(&b);
}

static int
sel_mouse(struct gut_sel *s, const struct gut_buf *b, int type, int button,
          int row, int col, int mods, int clicks)
{
    struct gut_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.button = button;
    ev.row = row;
    ev.col = col;
    ev.mods = mods;
    ev.clicks = clicks;
    return gut_sel_mouse(s, b, &ev);
}

static void
test_selection(void)
{
    struct gut_buf b;
    struct gut_sel s;
    char out[64];

    gut_buf_init(&b, 4, 12);
    gut_buf_text(&b, 0, 0, "one two", gut_color_default(),
                 gut_color_default(), 0);
    gut_buf_text(&b, 1, 0, "three \xe6\xbc\xa2 x", gut_color_default(),
                 gut_color_default(), 0);
    gut_buf_text(&b, 2, 0, "four", gut_color_default(), gut_color_default(),
                 0);
    gut_sel_clear(&s);
    CHECK(!s.active && gut_sel_text(&s, &b, out, sizeof(out)) == 0 &&
          out[0] == '\0');

    /* drag across rows */
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 4, 0,
                    1) == 0);
    CHECK(!s.active && s.dragging);
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 0, 4, 0,
                    0) == 0);
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 1, 2, 0,
                    0) == 1);
    CHECK(s.active && s.row0 == 0 && s.col0 == 4 && s.row1 == 1 &&
          s.col1 == 2);
    CHECK(gut_sel_contains(&s, 0, 4) && gut_sel_contains(&s, 0, 11) &&
          gut_sel_contains(&s, 1, 0) && !gut_sel_contains(&s, 1, 3) &&
          !gut_sel_contains(&s, 0, 3));
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 1, 2, 0,
                    0) == 1);
    CHECK(!s.dragging);
    CHECK(gut_sel_text(&s, &b, out, sizeof(out)) == 7 &&
          strcmp(out, "two\nthr") == 0);

    /* backwards drag normalises */
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 2, 3, 0, 1);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 2, 0, 0, 0);
    CHECK(s.row0 == 2 && s.col0 == 0 && s.col1 == 3);
    CHECK(gut_sel_text(&s, &b, out, sizeof(out)) == 4 &&
          strcmp(out, "four") == 0);

    /* a click clears */
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 3, 3, 0,
                    1) == 1);
    CHECK(!s.active);
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 3, 3, 0,
                    0) == 0);

    /* double click selects a word, and extends by words */
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 5, 0,
                    2) == 1);
    CHECK(s.active && s.row0 == 0 && s.col0 == 4 && s.col1 == 6);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 0, 1, 0, 0);
    CHECK(s.col0 == 0 && s.col1 == 6);
    CHECK(gut_sel_text(&s, &b, out, sizeof(out)) == 7 &&
          strcmp(out, "one two") == 0);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 0, 1, 0, 0);

    /* triple click selects the line */
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 1, 3, 0, 3);
    CHECK(s.row0 == 1 && s.col0 == 0 && s.col1 == 11);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 1, 3, 0, 0);
    CHECK(strcmp(out, "one two") == 0);
    CHECK(gut_sel_text(&s, &b, out, sizeof(out)) == 11 &&
          strcmp(out, "three \xe6\xbc\xa2 x") == 0);

    /* a wide character is taken whole */
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 1, 7, 0, 1);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 1, 9, 0, 0);
    CHECK(s.col0 == 6 && s.col1 == 9);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 1, 9, 0, 0);

    /* shift click moves the nearer end */
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 2, 2,
                    GUT_MOD_SHIFT, 1) == 1);
    CHECK(s.row0 == 1 && s.col0 == 6 && s.row1 == 2 && s.col1 == 2);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_UP, GUT_BUTTON_LEFT, 2, 2, 0, 0);

    /* alt drag is a rectangle */
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_LEFT, 0, 4,
              GUT_MOD_ALT, 1);
    sel_mouse(&s, &b, GUT_EVENT_MOUSE_MOVE, GUT_BUTTON_LEFT, 2, 1, 0, 0);
    CHECK(s.mode == GUT_COPY_RECT && s.row0 == 0 && s.col0 == 1 &&
          s.row1 == 2 && s.col1 == 4);
    CHECK(gut_sel_contains(&s, 1, 2) && !gut_sel_contains(&s, 1, 5));
    CHECK(gut_sel_text(&s, &b, out, sizeof(out)) == 13 &&
          strcmp(out, "ne t\nhree\nour") == 0);

    /* the right button is not a selection */
    CHECK(sel_mouse(&s, &b, GUT_EVENT_MOUSE_DOWN, GUT_BUTTON_RIGHT, 0, 0, 0,
                    1) == 0);
    CHECK(s.active);

    /* out of range points clamp */
    gut_sel_begin(&s, &b, -5, 99, GUT_COPY_STREAM, GUT_SEL_CELL);
    CHECK(s.row0 == 0 && s.col0 == 11);
    gut_sel_extend(&s, &b, 99, -1);
    CHECK(s.row1 == 3 && s.col1 == 0);

    gut_buf_free(&b);
}

int
main(void)
{
    test_buf();
    test_utf8();
    test_font();
    test_encode();
    test_copy();
    test_vt();
    test_scrollback();
    test_mouse_encode();
    test_vt_mouse();
    test_selection();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
