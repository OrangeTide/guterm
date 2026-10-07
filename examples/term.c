/* term.c : a minimal terminal emulator, a shell in a guterm window */

/*
 * POSIX only. Spawns $SHELL (or /bin/sh) on a pseudo terminal, feeds its
 * output through the VT layer and sends encoded key events back. It is a
 * demonstration of the VT layer, not a complete terminal: there is no
 * scrollback, selection or mouse reporting.
 */

#define GUTERM_IMPLEMENTATION
#include "guterm.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#else
#include <pty.h>
#endif

struct app {
    gut_window *w;
    struct gut_vt vt;
    struct gut_buf buf;
    int master;
    pid_t child;
};

static void
pty_write(struct app *a, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(a->master, data, len);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

static void
on_reply(void *ctx, const char *data, size_t len)
{
    pty_write(ctx, data, len);
}

static void
on_title(void *ctx, const char *title)
{
    struct app *a = ctx;

    gut_set_title(a->w, title);
}

static int
spawn_shell(struct app *a, int rows, int cols)
{
    struct winsize ws;
    const char *shell = getenv("SHELL");

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (!shell || !shell[0])
        shell = "/bin/sh";
    a->child = forkpty(&a->master, NULL, NULL, &ws);
    if (a->child < 0) {
        perror("forkpty");
        return -1;
    }
    if (a->child == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execlp(shell, shell, (char *)NULL);
        perror(shell);
        _exit(127);
    }
    fcntl(a->master, F_SETFL, fcntl(a->master, F_GETFL) | O_NONBLOCK);
    return 0;
}

/* Drain whatever the child wrote. Returns 0 at EOF. */
static int
drain(struct app *a)
{
    char chunk[8192];

    for (;;) {
        ssize_t n = read(a->master, chunk, sizeof(chunk));

        if (n > 0) {
            gut_vt_feed(&a->vt, chunk, (size_t)n);
            continue;
        }
        if (n == 0)
            return 0;
        if (errno == EAGAIN)
            return 1;
        if (errno == EINTR)
            continue;
        return 0;   /* EIO when the child closes its side on Linux */
    }
}

static void
resize(struct app *a, int rows, int cols)
{
    struct winsize ws;

    gut_vt_resize(&a->vt, rows, cols);
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(a->master, TIOCSWINSZ, &ws);
}

int
main(void)
{
    struct gut_desc desc = { 0 };
    struct app a;
    struct gut_event ev;
    int running = 1;

    memset(&a, 0, sizeof(a));
    desc.title = "guterm";
    desc.cols = 80;
    desc.rows = 25;
    a.w = gut_open(&desc);
    if (!a.w) {
        fprintf(stderr, "gut_open: %s\n", gut_error());
        return 1;
    }
    gut_buf_init(&a.buf, desc.rows, desc.cols);
    gut_vt_init(&a.vt, &a.buf);
    gut_vt_set_reply(&a.vt, on_reply, &a);
    gut_vt_set_title_cb(&a.vt, on_title, &a);
    if (spawn_shell(&a, desc.rows, desc.cols) != 0)
        return 1;
    signal(SIGCHLD, SIG_DFL);

    while (running) {
        struct pollfd pfd = { a.master, POLLIN, 0 };
        int have_event;

        /* Wait for either the window or the child. SDL has no way to
         * watch a file descriptor, so this alternates short waits. */
        have_event = gut_poll(a.w, &ev, 0);
        if (!have_event) {
            if (poll(&pfd, 1, 16) > 0) {
                if (!drain(&a))
                    break;
            }
            have_event = gut_poll(a.w, &ev, 0);
        }
        if (have_event) {
            char bytes[16];
            size_t n;

            switch (ev.type) {
            case GUT_EVENT_QUIT:
                running = 0;
                break;
            case GUT_EVENT_RESIZE:
                resize(&a, ev.rows, ev.cols);
                break;
            case GUT_EVENT_KEY:
            case GUT_EVENT_TEXT:
                n = gut_encode_event(&ev, bytes, sizeof(bytes),
                                     gut_vt_encode_flags(&a.vt));
                if (n > 0)
                    pty_write(&a, bytes, n);
                break;
            default:
                break;
            }
            while (gut_poll(a.w, &ev, 0)) {
                if (ev.type == GUT_EVENT_QUIT)
                    running = 0;
                else if (ev.type == GUT_EVENT_RESIZE)
                    resize(&a, ev.rows, ev.cols);
                else if (ev.type == GUT_EVENT_KEY ||
                         ev.type == GUT_EVENT_TEXT) {
                    n = gut_encode_event(&ev, bytes, sizeof(bytes),
                                         gut_vt_encode_flags(&a.vt));
                    if (n > 0)
                        pty_write(&a, bytes, n);
                }
            }
        }
        if (!drain(&a))
            break;
        gut_present(a.w, &a.buf);
    }

    close(a.master);
    kill(a.child, SIGHUP);
    waitpid(a.child, NULL, 0);
    gut_vt_free(&a.vt);
    gut_buf_free(&a.buf);
    gut_close(a.w);
    return 0;
}
