/* vtdemo.c : feeds an ANSI byte stream through the guterm VT layer */

/*
 * With a file argument the file (ANSI art, a script capture, anything)
 * is fed through the emulator and shown. Without one, a built-in
 * sequence demonstrates the escape codes. Typed keys are encoded with
 * gut_encode_event() and looped straight back into the emulator, so
 * the window acts as a terminal talking to itself.
 */

#define GUTERM_IMPLEMENTATION
#include "guterm.h"

#include <stdio.h>
#include <string.h>

static const char builtin[] =
    "\033[2J\033[H"
    "\033[1;33mguterm VT layer\033[0m\r\n"
    "\r\n"
    "SGR: \033[1mbold\033[0m \033[2mdim\033[0m \033[4munder\033[0m "
    "\033[7mreverse\033[0m \033[9mstrike\033[0m "
    "\033[31mred\033[32m green\033[34m blue\033[0m "
    "\033[38;5;208m256\033[0m \033[38;2;255;100;200mrgb\033[0m\r\n"
    "\r\n"
    "DEC graphics: \033(0lqqqqkx x\033(B  \033(0mqqqqj\033(B\r\n"
    "\r\n"
    "Tab\tstops\tevery\teight\r\n"
    "\r\n"
    "Cursor save/restore: \0337XXXX\0338OK  \r\n"
    "Insert: \033[4h__\033[4l (the underscores were inserted)\r\n"
    "\r\n"
    "\033[3;1H\033[s\033[20;1H\033[u"
    "\033[12;1HScroll region test:\r\n"
    "\033[13;18r\033[13;1H"
    "line 1\r\nline 2\r\nline 3\r\nline 4\r\nline 5\r\nline 6\r\n"
    "line 7 (scrolled)\r\nline 8 (scrolled)"
    "\033[r"
    "\033[20;1H\033[5 q"
    "Type here; Escape quits.\r\n";

struct app {
    gut_window *w;
    struct gut_vt *vt;
};

static void
on_title(void *ctx, const char *title)
{
    struct app *a = ctx;

    gut_set_title(a->w, title);
}

/* loopback: the emulator's answers go back in as input */
static void
on_reply(void *ctx, const char *data, size_t len)
{
    struct app *a = ctx;

    gut_vt_feed(a->vt, data, len);
}

int
main(int argc, char **argv)
{
    struct gut_desc desc = { 0 };
    struct gut_buf buf;
    struct gut_vt vt;
    struct app app;
    gut_window *w;
    struct gut_event ev;
    int running = 1;

    desc.title = "guterm vtdemo";
    desc.cols = 80;
    desc.rows = 25;

    w = gut_open(&desc);
    if (!w) {
        fprintf(stderr, "gut_open: %s\n", gut_error());
        return 1;
    }
    gut_buf_init(&buf, desc.rows, desc.cols);
    gut_vt_init(&vt, &buf);
    app.w = w;
    app.vt = &vt;
    gut_vt_set_title_cb(&vt, on_title, &app);
    gut_vt_set_reply(&vt, on_reply, &app);

    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        char chunk[4096];
        size_t n;

        if (!f) {
            perror(argv[1]);
            return 1;
        }
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            gut_vt_feed(&vt, chunk, n);
        fclose(f);
    } else {
        gut_vt_feed(&vt, builtin, sizeof(builtin) - 1);
    }
    gut_present(w, &buf);

    while (running && gut_poll(w, &ev, -1)) {
        char bytes[64];
        char *big = NULL;
        char *enc = bytes;
        size_t n;

        switch (ev.type) {
        case GUT_EVENT_QUIT:
            running = 0;
            break;
        case GUT_EVENT_RESIZE:
            gut_vt_resize(&vt, ev.rows, ev.cols);
            break;
        case GUT_EVENT_KEY:
            if (ev.key == GUT_KEY_ESCAPE && ev.mods == 0) {
                running = 0;
                break;
            }
            if (ev.key == GUT_KEY_ENTER && ev.mods == 0) {
                gut_vt_feed(&vt, "\r\n", 2);
                break;
            }
            if (ev.key == GUT_KEY_BACKSPACE && ev.mods == 0) {
                gut_vt_feed(&vt, "\b \b", 3);
                break;
            }
            /* fall through */
        case GUT_EVENT_TEXT:
        case GUT_EVENT_PASTE:
            /* a paste can be larger than the stack buffer */
            if (ev.len + 16 > sizeof(bytes))
                enc = big = malloc(ev.len + 16);
            n = gut_encode_event(&ev, enc, big ? ev.len + 16 : sizeof(bytes),
                                 gut_vt_encode_flags(&vt));
            if (n > 0)
                gut_vt_feed(&vt, enc, n);
            free(big);
            break;
        default:
            break;
        }
        gut_present(w, &buf);
    }

    gut_vt_free(&vt);
    gut_buf_free(&buf);
    gut_close(w);
    return 0;
}
