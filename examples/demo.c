/* demo.c : exercises the guterm cell buffer API directly */

#define GUTERM_IMPLEMENTATION
#include "guterm.h"

#include <stdio.h>
#include <string.h>

static const char *const attr_names[] = {
    "bold", "underline", "reverse", "italic", "blink", "dim", "hidden",
    "strike",
};

static void
draw_box(struct gut_buf *b, int row, int col, int h, int w,
         struct gut_color fg)
{
    struct gut_color bg = gut_color_default();

    gut_buf_put(b, row, col, 0x250C, fg, bg, 0);
    gut_buf_put(b, row, col + w - 1, 0x2510, fg, bg, 0);
    gut_buf_put(b, row + h - 1, col, 0x2514, fg, bg, 0);
    gut_buf_put(b, row + h - 1, col + w - 1, 0x2518, fg, bg, 0);
    for (int c = col + 1; c < col + w - 1; c++) {
        gut_buf_put(b, row, c, 0x2500, fg, bg, 0);
        gut_buf_put(b, row + h - 1, c, 0x2500, fg, bg, 0);
    }
    for (int r = row + 1; r < row + h - 1; r++) {
        gut_buf_put(b, r, col, 0x2502, fg, bg, 0);
        gut_buf_put(b, r, col + w - 1, 0x2502, fg, bg, 0);
    }
}

static void
draw_static(struct gut_buf *b)
{
    struct gut_color fg = gut_color_default();
    struct gut_color bg = gut_color_default();
    char line[128];

    gut_buf_clear(b, bg);
    draw_box(b, 0, 0, b->rows, b->cols, gut_color_indexed(8));
    gut_buf_text(b, 0, 2, " guterm " GUT_VERSION " ", gut_color_indexed(15),
                 bg, GUT_ATTR_BOLD);

    /* 16 ANSI colors as foreground and background */
    gut_buf_text(b, 2, 2, "ANSI 16:", fg, bg, 0);
    for (int i = 0; i < 16; i++) {
        snprintf(line, sizeof(line), "%2d", i);
        gut_buf_text(b, 2, 11 + i * 3, line, gut_color_indexed(i), bg, 0);
        gut_buf_text(b, 3, 11 + i * 3, "   ", fg, gut_color_indexed(i), 0);
    }

    /* 6x6x6 cube */
    gut_buf_text(b, 5, 2, "Cube:", fg, bg, 0);
    for (int i = 0; i < 216; i++) {
        int r = 5 + i / 36, c = 11 + (i % 36) * 2;

        gut_buf_text(b, r, c, "  ", fg, gut_color_indexed(16 + i), 0);
    }

    /* grays */
    gut_buf_text(b, 11, 2, "Gray:", fg, bg, 0);
    for (int i = 0; i < 24; i++)
        gut_buf_text(b, 11, 11 + i * 3, "   ", fg,
                     gut_color_indexed(232 + i), 0);

    /* true color ramp */
    gut_buf_text(b, 12, 2, "RGB:", fg, bg, 0);
    for (int i = 0; i < b->cols - 13; i++) {
        int t = i * 255 / (b->cols - 14);

        gut_buf_put(b, 12, 11 + i, ' ', fg, gut_color_rgb(t, 255 - t, 128),
                    0);
    }

    /* attributes */
    gut_buf_text(b, 14, 2, "Attrs:", fg, bg, 0);
    for (int i = 0; i < 8; i++)
        gut_buf_text(b, 14, 11 + i * 10, attr_names[i], fg, bg,
                     (uint16_t)(1 << i));

    /* glyph coverage sample */
    gut_buf_text(b, 16, 2, "Latin: \xc3\xa9\xc3\xa8\xc3\xa0\xc3\xb6\xc3\xbc"
                 "\xc3\x9f\xc3\xa6\xc3\xb8\xc3\xa5  Box: \xe2\x95\x94\xe2\x95"
                 "\x90\xe2\x95\x97 \xe2\x95\x9a\xe2\x95\x90\xe2\x95\x9d "
                 "Blocks: \xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91 "
                 "Arrows: \xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93 "
                 "Wide: \xe6\xbc\xa2\xe5\xad\x97",
                 fg, bg, 0);

    gut_buf_text(b, 18, 2, "Type to echo below. F1-F3 change the cursor "
                 "shape, Escape quits.", gut_color_indexed(11), bg, 0);
}

int
main(int argc, char **argv)
{
    struct gut_desc desc = { 0 };
    struct gut_buf buf;
    gut_window *w;
    struct gut_event ev;
    int running = 1;
    int echo_row, echo_col;
    char status[128];

    desc.title = "guterm demo";
    desc.cols = 100;
    desc.rows = 30;
    desc.scale = argc > 1 ? atoi(argv[1]) : 2;

    w = gut_open(&desc);
    if (!w) {
        fprintf(stderr, "gut_open: %s\n", gut_error());
        return 1;
    }
    gut_buf_init(&buf, desc.rows, desc.cols);
    draw_static(&buf);
    echo_row = 20;
    echo_col = 2;
    buf.cursor_row = echo_row;
    buf.cursor_col = echo_col;
    gut_present(w, &buf);

    while (running && gut_poll(w, &ev, -1)) {
        switch (ev.type) {
        case GUT_EVENT_QUIT:
            running = 0;
            break;
        case GUT_EVENT_RESIZE:
            gut_buf_resize(&buf, ev.rows, ev.cols);
            draw_static(&buf);
            break;
        case GUT_EVENT_KEY:
            if (ev.key == GUT_KEY_ESCAPE)
                running = 0;
            else if (ev.key == GUT_KEY_F1)
                buf.cursor_shape = GUT_CURSOR_BLOCK;
            else if (ev.key == GUT_KEY_F2)
                buf.cursor_shape = GUT_CURSOR_UNDERLINE;
            else if (ev.key == GUT_KEY_F3)
                buf.cursor_shape = GUT_CURSOR_BAR;
            else if (ev.key == GUT_KEY_ENTER) {
                echo_row++;
                echo_col = 2;
            } else if (ev.key == GUT_KEY_BACKSPACE && echo_col > 2) {
                echo_col--;
                gut_buf_put(&buf, echo_row, echo_col, ' ',
                            gut_color_default(), gut_color_default(), 0);
            }
            {
                char bytes[16];
                size_t n = gut_encode_event(&ev, bytes, sizeof(bytes), 0);
                size_t o = 0;

                o = (size_t)snprintf(status, sizeof(status),
                                     "key 0x%X mods %d%s ->",
                                     ev.key, ev.mods,
                                     ev.repeat ? " rep" : "");
                for (size_t i = 0; i < n && o + 5 < sizeof(status); i++)
                    o += (size_t)snprintf(status + o, sizeof(status) - o,
                                          " %02X", (unsigned char)bytes[i]);
            }
            break;
        case GUT_EVENT_TEXT:
            if (echo_row >= buf.rows - 2) {
                echo_row = 20;
                gut_buf_clear_rows(&buf, 20, buf.rows - 2,
                                   gut_color_default());
                draw_box(&buf, 0, 0, buf.rows, buf.cols,
                         gut_color_indexed(8));
            }
            echo_col += gut_buf_text(&buf, echo_row, echo_col, ev.text,
                                     gut_color_indexed(10),
                                     gut_color_default(), 0);
            if (echo_col >= buf.cols - 2) {
                echo_row++;
                echo_col = 2;
            }
            snprintf(status, sizeof(status), "text \"%s\"", ev.text);
            break;
        case GUT_EVENT_MOUSE_MOVE:
        case GUT_EVENT_MOUSE_DOWN:
        case GUT_EVENT_MOUSE_UP:
            snprintf(status, sizeof(status), "mouse %s col %d row %d btn %d",
                     ev.type == GUT_EVENT_MOUSE_DOWN ? "down"
                     : ev.type == GUT_EVENT_MOUSE_UP ? "up" : "move",
                     ev.col, ev.row, ev.button);
            break;
        case GUT_EVENT_MOUSE_WHEEL:
            snprintf(status, sizeof(status), "wheel dx %d dy %d", ev.dx,
                     ev.dy);
            break;
        case GUT_EVENT_PASTE:
            echo_col += gut_buf_text(&buf, echo_row, echo_col, ev.data,
                                     gut_color_indexed(13),
                                     gut_color_default(), 0);
            snprintf(status, sizeof(status), "paste %lu bytes%s",
                     (unsigned long)ev.len, ev.primary ? " (primary)" : "");
            break;
        case GUT_EVENT_COMPOSE:
            snprintf(status, sizeof(status), "compose \"%s\" caret %d",
                     ev.data, ev.cursor);
            break;
        default:
            status[0] = '\0';
            break;
        }
        buf.cursor_row = echo_row;
        buf.cursor_col = echo_col;
        gut_buf_fill(&buf, buf.rows - 1, 1, 1, buf.cols - 2, 0x2500,
                     gut_color_indexed(8), gut_color_default(), 0);
        gut_buf_text(&buf, buf.rows - 1, 2, status, gut_color_indexed(14),
                     gut_color_default(), 0);
        gut_present(w, &buf);
    }

    gut_buf_free(&buf);
    gut_close(w);
    return 0;
}
