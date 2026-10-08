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

static struct gut_image
make_image(int w, int h)
{
    struct gut_image img;

    img.w = w;
    img.h = h;
    img.rgba = calloc((size_t)w * h, 4);
    return img;
}

static const struct gut_placement *
placement_at(const struct gut_buf *b, int row)
{
    int n;
    const struct gut_placement *p = gut_buf_images(b, &n);

    for (int i = 0; i < n; i++)
        if (p[i].row == row)
            return &p[i];
    return NULL;
}

static int
placements(const struct gut_buf *b)
{
    int n;

    gut_buf_images(b, &n);
    return n;
}

static void
test_images(void)
{
    struct gut_buf b;
    struct gut_image img;
    struct gut_image_list parked;
    const struct gut_placement *p;

    CHECK(gut_buf_init(&b, 6, 10) == 0);

    /* geometry from the picture size and the cell size */
    img = make_image(16, 32);
    memset(b.dirty, 0, 6);
    CHECK(gut_buf_place_image(&b, &img, 1, 2, 8, 16) == 1);
    CHECK(img.rgba == NULL && img.w == 0);
    p = placement_at(&b, 1);
    CHECK(p && p->col == 2 && p->rows == 2 && p->cols == 2 && p->src_y == 0);
    CHECK(p->ref->id == 1 && p->ref->refs == 1 && p->ref->img.h == 32);
    CHECK(!b.dirty[0] && b.dirty[1] && b.dirty[2] && !b.dirty[3]);
    CHECK(b.images.pixels == 16 * 32);

    /* scrolling moves it, then cuts it row by row at the top */
    gut_buf_scroll(&b, 0, 6, 1, gut_color_default());
    p = placement_at(&b, 0);
    CHECK(p && p->rows == 2 && p->src_y == 0);
    gut_buf_scroll(&b, 0, 6, 1, gut_color_default());
    p = placement_at(&b, 0);
    CHECK(p && p->rows == 1 && p->src_y == 16);
    gut_buf_scroll(&b, 0, 6, 1, gut_color_default());
    CHECK(placements(&b) == 0 && b.images.pixels == 0);

    /* clearing rows inside a picture leaves two bands sharing pixels */
    img = make_image(8, 64);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 2);
    gut_buf_clear_rows(&b, 1, 2, gut_color_default());
    CHECK(placements(&b) == 2);
    p = placement_at(&b, 0);
    CHECK(p && p->rows == 1 && p->src_y == 0 && p->ref->refs == 2);
    p = placement_at(&b, 2);
    CHECK(p && p->rows == 2 && p->src_y == 32 && p->ref->id == 2);
    CHECK(b.images.pixels == 8 * 64);
    gut_buf_clear_rows(&b, 0, 1, gut_color_default());
    CHECK(placements(&b) == 1 && b.images.pixels == 8 * 64);
    p = placement_at(&b, 2);
    CHECK(p && p->ref->refs == 1);
    gut_buf_clear(&b, gut_color_default());
    CHECK(placements(&b) == 0 && b.images.pixels == 0);

    /* a scroll region: the part outside stays, the part inside moves */
    img = make_image(8, 64);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 3);
    gut_buf_scroll(&b, 2, 4, 1, gut_color_default());
    CHECK(placements(&b) == 2);
    p = placement_at(&b, 0);
    CHECK(p && p->rows == 2 && p->src_y == 0);
    p = placement_at(&b, 2);
    CHECK(p && p->rows == 1 && p->src_y == 48);
    /* scrolling down cuts at the bottom of the region */
    gut_buf_scroll(&b, 0, 2, -1, gut_color_default());
    p = placement_at(&b, 1);
    CHECK(p && p->rows == 1 && p->src_y == 0);
    CHECK(placement_at(&b, 0) == NULL);
    /* a scroll as large as the region clears it */
    gut_buf_scroll(&b, 0, 6, 6, gut_color_default());
    CHECK(placements(&b) == 0);

    /* rows below the grid are cut off, bad positions are refused */
    img = make_image(8, 64);
    CHECK(gut_buf_place_image(&b, &img, 4, 9, 8, 16) == 4);
    p = placement_at(&b, 4);
    CHECK(p && p->rows == 2 && p->cols == 1);
    img = make_image(8, 8);
    CHECK(gut_buf_place_image(&b, &img, 6, 0, 8, 16) == -1);
    CHECK(img.rgba == NULL);
    img = make_image(8, 8);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 0, 16) == -1);
    img.rgba = NULL;
    img.w = 8;
    img.h = 8;
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == -1);
    CHECK(placements(&b) == 1);

    /* a resize drops everything */
    CHECK(gut_buf_resize(&b, 6, 12) == 0);
    CHECK(placements(&b) == 0 && b.images.pixels == 0);

    /* the budget evicts the oldest; one picture over it is refused */
    gut_buf_set_image_budget(&b, 1000);
    img = make_image(30, 20);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 5);
    img = make_image(10, 30);
    CHECK(gut_buf_place_image(&b, &img, 2, 0, 8, 16) == 6);
    CHECK(placements(&b) == 2 && b.images.pixels == 900);
    img = make_image(10, 20);
    CHECK(gut_buf_place_image(&b, &img, 4, 0, 8, 16) == 7);
    CHECK(placements(&b) == 2 && b.images.pixels == 500);
    CHECK(placement_at(&b, 0) == NULL && placement_at(&b, 2) != NULL);
    img = make_image(40, 30);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == -1);
    CHECK(placements(&b) == 2 && b.images.pixels == 500);
    img = make_image(20, 30);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 8);
    CHECK(placements(&b) == 2 && b.images.pixels == 800);
    CHECK(placement_at(&b, 0) != NULL && placement_at(&b, 2) == NULL);
    gut_buf_set_image_budget(&b, 599);
    CHECK(placements(&b) == 0);
    /* both bands of a split picture go together, as the oldest */
    gut_buf_set_image_budget(&b, 1000);
    img = make_image(8, 64);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 9);
    gut_buf_clear_rows(&b, 1, 2, gut_color_default());
    CHECK(placements(&b) == 2 && b.images.pixels == 512);
    img = make_image(20, 30);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) == 10);
    CHECK(placements(&b) == 1 && b.images.pixels == 600);
    gut_buf_set_image_budget(&b, 0);
    gut_buf_drop_images(&b);

    /* the placement cap evicts the oldest bands too */
    for (int i = 0; i < GUT_BUF_IMAGE_MAX_PLACEMENTS + 10; i++) {
        img = make_image(1, 1);
        CHECK(gut_buf_place_image(&b, &img, i % 3, 0, 8, 16) > 0);
    }
    CHECK(placements(&b) == GUT_BUF_IMAGE_MAX_PLACEMENTS);
    gut_buf_drop_images(&b);

    /* parking a screen's pictures and bringing them back */
    memset(&parked, 0, sizeof(parked));
    img = make_image(8, 16);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) > 0);
    gut_buf_swap_images(&b, &parked);
    CHECK(placements(&b) == 0 && parked.n == 1 && parked.pixels == 128);
    img = make_image(8, 16);
    CHECK(gut_buf_place_image(&b, &img, 1, 0, 8, 16) > 0);
    gut_buf_swap_images(&b, &parked);
    CHECK(placements(&b) == 1 && placement_at(&b, 0) != NULL);
    CHECK(parked.n == 1 && placement_at(&b, 1) == NULL);
    /* a parked list is clipped to the grid it comes back to */
    CHECK(gut_buf_resize(&b, 3, 12) == 0);
    img = make_image(8, 64);
    CHECK(gut_buf_place_image(&b, &img, 1, 0, 8, 16) > 0);
    gut_buf_swap_images(&b, &parked);
    CHECK(gut_buf_resize(&b, 2, 12) == 0);
    gut_buf_swap_images(&b, &parked);
    p = placement_at(&b, 1);
    CHECK(placements(&b) == 1 && p && p->rows == 1);
    gut_buf_swap_images(&b, &parked);
    CHECK(gut_buf_resize(&b, 1, 12) == 0);
    gut_buf_swap_images(&b, &parked);
    CHECK(placements(&b) == 0);
    /* and to the budget */
    CHECK(gut_buf_resize(&b, 4, 12) == 0);
    img = make_image(8, 16);
    CHECK(gut_buf_place_image(&b, &img, 0, 0, 8, 16) > 0);
    img = make_image(8, 16);
    CHECK(gut_buf_place_image(&b, &img, 1, 0, 8, 16) > 0);
    gut_buf_swap_images(&b, &parked);
    gut_buf_set_image_budget(&b, 200);
    gut_buf_swap_images(&b, &parked);
    CHECK(placements(&b) == 1 && placement_at(&b, 1) != NULL);
    gut_buf_set_image_budget(&b, 0);
    gut_image_list_free(&parked);
    CHECK(parked.n == 0 && parked.v == NULL);
    gut_buf_drop_images(&b);
    CHECK(placements(&b) == 0);

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
    CHECK(strcmp(last_reply, "\033[?1;2;4c") == 0);

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


static char clip_text[2][4096];
static size_t clip_len[2];
static int clip_calls;

static void
clip_set(void *ctx, int which, const char *text, size_t len)
{
    (void)ctx;
    clip_calls++;
    if (which < 0 || which > 1)
        return;
    clip_len[which] = len;
    snprintf(clip_text[which], sizeof(clip_text[which]), "%s", text);
}

static const char *
clip_get(void *ctx, int which)
{
    (void)ctx;
    return which == GUT_CLIP_CLIPBOARD ? "hi" : NULL;
}

static void
test_osc52(void)
{
    struct gut_buf b;
    struct gut_vt vt;
    char *big;

    gut_buf_init(&b, 3, 10);
    gut_vt_init(&vt, &b);
    gut_vt_set_reply(&vt, reply_cb, NULL);
    gut_vt_set_title_cb(&vt, title_cb, NULL);
    last_reply[0] = '\0';

    /* no callback: nothing happens, no reply */
    feed(&vt, "\033]52;c;aGVsbG8=\a\033]52;c;?\a");
    CHECK(clip_calls == 0 && last_reply[0] == '\0');

    gut_vt_set_clipboard_cb(&vt, clip_set, NULL, NULL);
    feed(&vt, "\033]52;c;aGVsbG8=\a");
    CHECK(clip_calls == 1 && clip_len[0] == 5 &&
          strcmp(clip_text[0], "hello") == 0);
    /* ST terminator, primary, whitespace inside the base64 */
    feed(&vt, "\033]52;p;d29y\nbGQ=\033\\");
    CHECK(clip_calls == 2 && strcmp(clip_text[1], "world") == 0);
    /* empty selection list means primary; both at once */
    feed(&vt, "\033]52;;eA==\a");
    CHECK(clip_calls == 3 && strcmp(clip_text[1], "x") == 0);
    feed(&vt, "\033]52;cp;eQ==\a");
    CHECK(clip_calls == 5 && strcmp(clip_text[0], "y") == 0 &&
          strcmp(clip_text[1], "y") == 0);
    /* unknown selection letters are ignored, repeated ones once */
    feed(&vt, "\033]52;qcc;eg==\a");
    CHECK(clip_calls == 6 && strcmp(clip_text[0], "z") == 0);
    /* not base64 clears */
    feed(&vt, "\033]52;c;not*base64\a");
    CHECK(clip_calls == 7 && clip_len[0] == 0 && clip_text[0][0] == '\0');
    /* NUL and binary survive */
    feed(&vt, "\033]52;c;AGE=\a");
    CHECK(clip_calls == 8 && clip_len[0] == 2 && clip_text[0][0] == '\0');
    /* missing data part is ignored */
    feed(&vt, "\033]52;c\a");
    CHECK(clip_calls == 8);

    /* queries only answer with a getter */
    feed(&vt, "\033]52;c;?\a");
    CHECK(last_reply[0] == '\0');
    gut_vt_set_clipboard_cb(&vt, clip_set, clip_get, NULL);
    feed(&vt, "\033]52;c;?\a");
    CHECK(strcmp(last_reply, "\033]52;c;aGk=\033\\") == 0);
    last_reply[0] = '\0';
    feed(&vt, "\033]52;pc;?\a");
    CHECK(strcmp(last_reply, "\033]52;pc;aGk=\033\\") == 0);
    last_reply[0] = '\0';
    feed(&vt, "\033]52;p;?\a");
    CHECK(strcmp(last_reply, "\033]52;p;\033\\") == 0);
    last_reply[0] = '\0';

    /* a payload past the old fixed buffer size arrives whole */
    big = malloc(16384);
    memcpy(big, "\033]52;c;", 7);
    for (int i = 0; i < 3000; i++)
        memcpy(big + 7 + i * 4, "YWJj", 4);
    big[7 + 3000 * 4] = '\a';
    gut_vt_feed(&vt, big, 7 + 3000 * 4 + 1);
    CHECK(clip_calls == 9 && clip_len[0] == 9000 &&
          strncmp(clip_text[0], "abcabcabc", 9) == 0);
    free(big);

    /* a string past the limit is dropped, and the next one works */
    big = malloc(GUT_VT_OSC_MAX + 16);
    memset(big, 'A', GUT_VT_OSC_MAX + 16);
    memcpy(big, "\033]52;c;", 7);
    big[GUT_VT_OSC_MAX + 15] = '\a';
    gut_vt_feed(&vt, big, GUT_VT_OSC_MAX + 16);
    CHECK(clip_calls == 9 && vt.state == GUT_ST_GROUND);
    free(big);
    feed(&vt, "\033]52;c;b2s=\a");
    CHECK(clip_calls == 10 && strcmp(clip_text[0], "ok") == 0);

    /* an empty OSC and the title still work */
    feed(&vt, "\033]\a\033]2;t\a");
    CHECK(strcmp(last_title, "t") == 0);

    gut_vt_free(&vt);
    gut_buf_free(&b);
}

/* Decode a complete sixel body with the given DCS P2 and limit. */
static int
sixel(struct gut_image *img, int p2, size_t limit, const char *body)
{
    struct gut_sixel s;

    memset(&s, 0, sizeof(s));
    gut_sixel_begin(&s, 0, p2, 0, limit);
    for (; *body; body++)
        gut_sixel_put(&s, (unsigned char)*body);
    return gut_sixel_end(&s, img);
}

static uint32_t
pixel(const struct gut_image *img, int x, int y)
{
    const uint8_t *p = img->rgba + ((size_t)y * img->w + x) * 4;

    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
           ((uint32_t)p[1] << 8) | p[2];
}

static void
test_sixel(void)
{
    struct gut_image img;
    struct gut_sixel s;
    struct gut_buf b;
    struct gut_vt vt;

    /* one full column in default color 1, VT340 blue */
    CHECK(sixel(&img, 1, 0, "#1~") == 1);
    CHECK(img.w == 1 && img.h == 6);
    CHECK(pixel(&img, 0, 0) == 0xFF3333CC);
    CHECK(pixel(&img, 0, 5) == 0xFF3333CC);
    gut_image_free(&img);

    /* RGB color definition and repeat */
    CHECK(sixel(&img, 1, 0, "#2;2;100;0;0!3@") == 1);
    CHECK(img.w == 3 && img.h == 1);
    CHECK(pixel(&img, 2, 0) == 0xFFFF0000);
    gut_image_free(&img);

    /* HLS: sixel hue 120 is red, 0 is blue, 240 is green */
    CHECK(sixel(&img, 1, 0, "#5;1;120;50;100@") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFFFF0000);
    gut_image_free(&img);
    CHECK(sixel(&img, 1, 0, "#5;1;0;50;100@") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFF0000FF);
    gut_image_free(&img);
    CHECK(sixel(&img, 1, 0, "#5;1;240;50;100@") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFF00FF00);
    gut_image_free(&img);
    CHECK(sixel(&img, 1, 0, "#5;1;0;50;0@") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFF808080);
    gut_image_free(&img);

    /* transparent background: only painted bits have alpha */
    CHECK(sixel(&img, 1, 0, "#1A") == 1);
    CHECK(img.w == 1 && img.h == 2);
    CHECK(pixel(&img, 0, 0) == 0 && pixel(&img, 0, 1) == 0xFF3333CC);
    gut_image_free(&img);

    /* opaque background: unpainted pixels take color 0 */
    CHECK(sixel(&img, 0, 0, "#0;2;0;0;100#1A") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFF0000FF && pixel(&img, 0, 1) == 0xFF3333CC);
    gut_image_free(&img);

    /* raster attributes are a floor on the size; the aspect stretches */
    CHECK(sixel(&img, 0, 0, "\"1;1;4;4#1@") == 1);
    CHECK(img.w == 4 && img.h == 4);
    CHECK(pixel(&img, 0, 0) == 0xFF3333CC && pixel(&img, 3, 3) == 0xFF000000);
    gut_image_free(&img);
    CHECK(sixel(&img, 1, 0, "\"2;1;1;1#1@") == 1);
    CHECK(img.w == 1 && img.h == 2);
    CHECK(pixel(&img, 0, 1) == 0xFF3333CC);
    gut_image_free(&img);

    /* $ returns to the left, - starts the next band of six */
    CHECK(sixel(&img, 1, 0, "#1@@$#2@-#1@") == 1);
    CHECK(img.w == 2 && img.h == 7);
    CHECK(pixel(&img, 0, 0) == 0xFFCC2121 && pixel(&img, 1, 0) == 0xFF3333CC);
    CHECK(pixel(&img, 0, 6) == 0xFF3333CC && pixel(&img, 1, 6) == 0);
    gut_image_free(&img);

    /* a blank repeat advances the pen and widens the picture */
    CHECK(sixel(&img, 1, 0, "!5?#1@") == 1);
    CHECK(img.w == 6 && img.h == 1);
    CHECK(pixel(&img, 0, 0) == 0 && pixel(&img, 5, 0) == 0xFF3333CC);
    gut_image_free(&img);

    /* nothing painted, bytes that are not sixel, color index wrap */
    CHECK(sixel(&img, 1, 0, "") == 0);
    CHECK(sixel(&img, 1, 0, "!5?$-") == 0);
    CHECK(sixel(&img, 1, 0, "#1\r\n@\x01") == 1);
    CHECK(img.w == 1 && img.h == 1);
    gut_image_free(&img);
    CHECK(sixel(&img, 1, 0, "#257;2;100;100;100#1@") == 1);
    CHECK(pixel(&img, 0, 0) == 0xFFFFFFFF);
    gut_image_free(&img);

    /* over the limit: dropped, then the decoder is reusable */
    CHECK(sixel(&img, 1, 16, "!100@") == 0);
    CHECK(img.rgba == NULL);
    CHECK(sixel(&img, 1, 16, "\"1;1;100;100") == 0);
    CHECK(sixel(&img, 1, 16, "#1@") == 1);
    CHECK(img.w == 1 && img.h == 1);
    gut_image_free(&img);
    memset(&s, 0, sizeof(s));
    gut_sixel_begin(&s, 0, 1, 0, 16);
    gut_sixel_put(&s, '!');
    gut_sixel_put(&s, '9');
    gut_sixel_put(&s, '9');
    gut_sixel_put(&s, '@');
    CHECK(s.failed && s.rgba == NULL);
    gut_sixel_put(&s, '@');
    CHECK(gut_sixel_end(&s, &img) == 0);
    gut_sixel_abort(&s);

    /* a side longer than GUT_SIXEL_MAX_DIM fails the picture */
    CHECK(sixel(&img, 1, 0, "!4097@") == 0);
    CHECK(sixel(&img, 1, 0, "\"1;1;1;4097") == 0);
    CHECK(sixel(&img, 1, 0, "!4096@") == 1);
    CHECK(img.w == 4096);
    gut_image_free(&img);

    /* through the VT: the DCS is consumed, the picture is placed and
     * the cursor moves below it; a DCS that is not sixel is swallowed,
     * and controls inside a picture do not execute */
    gut_buf_init(&b, 3, 10);
    gut_vt_init(&vt, &b);
    gut_vt_set_cell_size(&vt, 8, 6);
    feed(&vt, "A\033P0;1;0q#1~~~\033\\B");
    CHECK(strcmp(row_text(&b, 0), "A") == 0);
    CHECK(strcmp(row_text(&b, 1), " B") == 0);
    CHECK(placements(&b) == 1 && placement_at(&b, 0) != NULL);
    feed(&vt, "\033P1$r0q\033\\E\033P+q544e\033\\F");
    CHECK(strcmp(row_text(&b, 1), " BEF") == 0);
    feed(&vt, "\033[1;1H\033P");
    feed(&vt, "q#1");
    feed(&vt, "\r\n~\033");
    feed(&vt, "\\C");
    CHECK(strcmp(row_text(&b, 1), "CBEF") == 0 && placements(&b) == 2);
    /* ESC other than ST ends the picture and is interpreted */
    feed(&vt, "\033[1;1H\033Pq#1~\033[3;1HD");
    CHECK(strcmp(row_text(&b, 2), "D") == 0 && placements(&b) == 3);
    /* a reset mid picture leaves the parser in ground and no pictures */
    feed(&vt, "\033Pq#1~");
    gut_vt_reset(&vt);
    feed(&vt, "H");
    CHECK(strcmp(row_text(&b, 0), "H") == 0 && placements(&b) == 0);
    gut_vt_set_image_limit(&vt, 4);
    feed(&vt, "\033Pq!100~\033\\I");
    CHECK(strcmp(row_text(&b, 0), "HI") == 0 && placements(&b) == 0);
    /* CAN abandons a picture and the bytes after it are text */
    gut_vt_set_image_limit(&vt, 0);
    feed(&vt, "\033Pq#1~\030J");
    CHECK(strcmp(row_text(&b, 0), "HIJ") == 0 && placements(&b) == 0);
    gut_vt_free(&vt);
    gut_buf_free(&b);
}

/* A picture of n bands, 8 pixels wide; with a 6 pixel cell each band
 * is one row. */
static void
feed_bands(struct gut_vt *vt, int n)
{
    feed(vt, "\033Pq");
    for (int i = 0; i < n; i++)
        feed(vt, i ? "-!8~" : "!8~");
    feed(vt, "\033\\");
}

static void
test_vt_images(void)
{
    struct gut_buf b;
    struct gut_vt vt;
    const struct gut_placement *p;

    gut_buf_init(&b, 5, 10);
    gut_vt_init(&vt, &b);
    gut_vt_set_reply(&vt, reply_cb, NULL);
    gut_vt_set_cell_size(&vt, 8, 6);

    /* placed at the cursor, cursor to the row below in the same column */
    feed(&vt, "\033[2;3H");
    feed_bands(&vt, 2);
    p = placement_at(&b, 1);
    CHECK(p && p->col == 2 && p->rows == 2 && p->cols == 1);
    CHECK(b.cursor_row == 3 && b.cursor_col == 2);

    /* the screen scrolls to fit the picture and a line for the cursor */
    gut_vt_clear_scrollback(&vt);
    feed(&vt, "\033[2J\033[1;1Ha\033[5;1H");
    feed_bands(&vt, 2);
    p = placement_at(&b, 2);
    CHECK(p && p->rows == 2 && p->src_y == 0 && placements(&b) == 1);
    CHECK(b.cursor_row == 4 && gut_vt_scrollback_lines(&vt) == 2);
    CHECK(strcmp(row_text(&b, 0), "") == 0);

    /* taller than the screen: the top is lost, the bottom shows */
    feed(&vt, "\033[2J\033[1;1H");
    feed_bands(&vt, 7);
    p = placement_at(&b, 0);
    CHECK(p && p->rows == 4 && p->src_y == 18 && placements(&b) == 1);
    CHECK(b.cursor_row == 4 && b.cursor_col == 0);

    /* inside a scroll region the region scrolls and the picture stays
     * within it */
    feed(&vt, "\033[2J\033[2;4r\033[4;1H");
    feed_bands(&vt, 4);
    p = placement_at(&b, 1);
    CHECK(p && p->rows == 2 && p->src_y == 12 && placements(&b) == 1);
    CHECK(b.cursor_row == 3);
    feed(&vt, "\033[r");

    /* a cursor outside the region places without scrolling */
    feed(&vt, "\033[2J\033[1;3r\033[5;1H");
    feed_bands(&vt, 3);
    p = placement_at(&b, 4);
    CHECK(p && p->rows == 1 && placements(&b) == 1 && b.cursor_row == 4);
    feed(&vt, "\033[r");

    /* DECSDM: at the home position, cursor unchanged */
    feed(&vt, "\033[2J\033[?80h\033[3;4H");
    feed_bands(&vt, 2);
    p = placement_at(&b, 0);
    CHECK(p && p->col == 0 && b.cursor_row == 2 && b.cursor_col == 3);
    feed(&vt, "\033[?80l");

    /* the alternate screen has its own pictures; the primary's return */
    feed(&vt, "\033[2J\033[1;1H");
    feed_bands(&vt, 1);
    feed(&vt, "\033[?1049h");
    CHECK(placements(&b) == 0);
    feed(&vt, "\033[3;1H");
    feed_bands(&vt, 1);
    CHECK(placements(&b) == 1 && placement_at(&b, 2) != NULL);
    feed(&vt, "\033[?1049l");
    CHECK(placements(&b) == 1 && placement_at(&b, 0) != NULL);

    /* scrolled back, pictures move down with the live screen */
    feed(&vt, "\033[2J\033[1;1Hx\r\ny\r\n");
    feed_bands(&vt, 1);
    CHECK(placement_at(&b, 2) != NULL);
    CHECK(gut_vt_set_view(&vt, 2) == 2);
    CHECK(placements(&b) == 1 && placement_at(&b, 4) != NULL);
    feed(&vt, "\033[1;1H");
    feed_bands(&vt, 1);
    CHECK(placements(&b) == 2 && placement_at(&b, 2) != NULL);
    CHECK(placement_at(&b, 4) != NULL);
    CHECK(gut_vt_set_view(&vt, 0) == 0);
    CHECK(placements(&b) == 2 && placement_at(&b, 0) != NULL);

    /* EL 2 and the cursor row of ED cut a picture; ECH and EL 0 leave it */
    feed(&vt, "\033[2J\033[1;1H");
    feed_bands(&vt, 3);
    CHECK(placements(&b) == 1 && placement_at(&b, 0) != NULL);
    feed(&vt, "\033[2;1H\033[3X\033[0K");
    CHECK(placements(&b) == 1);
    feed(&vt, "\033[2K");
    CHECK(placements(&b) == 2 && placement_at(&b, 1) == NULL);
    feed(&vt, "\033[3;1H\033[0J");
    CHECK(placements(&b) == 1 && placement_at(&b, 0) != NULL);
    feed(&vt, "\033[1;1H\033[1J");
    CHECK(placements(&b) == 0);

    /* queries */
    feed(&vt, "\033[?2;1;0S");
    CHECK(strcmp(last_reply, "\033[?2;0;80;30S") == 0);
    feed(&vt, "\033[?1;4;0S");
    CHECK(strcmp(last_reply, "\033[?1;0;256S") == 0);
    feed(&vt, "\033[?3;1;0S");
    CHECK(strcmp(last_reply, "\033[?3;1S") == 0);
    feed(&vt, "\033[?2;9;0S");
    CHECK(strcmp(last_reply, "\033[?2;2S") == 0);
    feed(&vt, "\033[4;1Hz\033[1S");
    CHECK(strcmp(row_text(&b, 2), "z") == 0);

    /* a resize drops the pictures, parked ones too */
    feed(&vt, "\033[?1049h");
    feed_bands(&vt, 1);
    CHECK(gut_vt_resize(&vt, 5, 12) == 0);
    CHECK(placements(&b) == 0);
    feed(&vt, "\033[?1049l");
    CHECK(placements(&b) == 0);

    gut_vt_free(&vt);
    gut_buf_free(&b);
}

int
main(void)
{
    test_buf();
    test_images();
    test_utf8();
    test_font();
    test_encode();
    test_copy();
    test_vt();
    test_scrollback();
    test_mouse_encode();
    test_vt_mouse();
    test_selection();
    test_osc52();
    test_sixel();
    test_vt_images();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
