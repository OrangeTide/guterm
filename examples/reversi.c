/* reversi.c : the disc flipping board game on a guterm grid */

/*
 * Two players take turns placing discs on an 8x8 board; a move must
 * trap a line of the other side's discs between the new disc and one
 * of your own, and every trapped disc flips. The side with more discs
 * when neither can move wins.
 *
 * Human against human, human against the computer, or the computer
 * against itself. The computer player is deliberately simple: it takes
 * the move with the best weighted square value plus flips, which is
 * enough to punish careless play and no more.
 *
 * Keys: arrows move the cursor, Enter or Space places a disc, H shows
 * the legal moves, U takes back a move, N starts a new game, M returns
 * to the mode menu, Escape quits. The mouse works everywhere.
 */

#define GUTERM_IMPLEMENTATION
#include "guterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 8
#define EMPTY 0
#define DARK 1
#define LIGHT 2

#define BOARD_COL 2
#define BOARD_ROW 2
#define CELL_W 4
#define CELL_H 2
#define PANEL_COL (BOARD_COL + SIZE * CELL_W + 3)

#define COLS 64
#define ROWS 22

#define AI_DELAY_MS 350

enum mode {
    MODE_MENU,
    MODE_HUMAN_HUMAN,
    MODE_HUMAN_AI,
    MODE_AI_AI,
};

struct game {
    unsigned char board[SIZE][SIZE];
    int turn;                   /* DARK or LIGHT */
    int over;
    int last_r, last_c;         /* last placed disc, or -1 */
    int passed;                 /* the previous player had to pass */
};

struct app {
    gut_window *w;
    struct gut_buf buf;
    struct game g;
    struct game history[SIZE * SIZE];
    int nhistory;
    int mode;
    int human[3];               /* indexed by DARK and LIGHT */
    int cur_r, cur_c;           /* keyboard cursor */
    int hover_r, hover_c;       /* mouse cell, or -1 */
    int hints;
    char message[64];
};

/* Square weights: corners are gold, their neighbours are poison. */
static const int weights[SIZE][SIZE] = {
    { 100, -20,  10,   5,   5,  10, -20, 100 },
    { -20, -50,  -2,  -2,  -2,  -2, -50, -20 },
    {  10,  -2,   5,   1,   1,   5,  -2,  10 },
    {   5,  -2,   1,   0,   0,   1,  -2,   5 },
    {   5,  -2,   1,   0,   0,   1,  -2,   5 },
    {  10,  -2,   5,   1,   1,   5,  -2,  10 },
    { -20, -50,  -2,  -2,  -2,  -2, -50, -20 },
    { 100, -20,  10,   5,   5,  10, -20, 100 },
};

static const int dirs[8][2] = {
    { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, -1 },
    { 0, 1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
};

/****************************************************************
 * Rules
 ****************************************************************/

static int
other(int side)
{
    return side == DARK ? LIGHT : DARK;
}

static int
on_board(int r, int c)
{
    return r >= 0 && r < SIZE && c >= 0 && c < SIZE;
}

/* Discs flipped along one direction by a move, 0 when none. */
static int
flips_dir(const struct game *g, int r, int c, int side, int dr, int dc)
{
    int n = 0;

    r += dr;
    c += dc;
    while (on_board(r, c) && g->board[r][c] == other(side)) {
        n++;
        r += dr;
        c += dc;
    }
    if (n == 0 || !on_board(r, c) || g->board[r][c] != side)
        return 0;
    return n;
}

static int
flips(const struct game *g, int r, int c, int side)
{
    int n = 0;

    if (!on_board(r, c) || g->board[r][c] != EMPTY)
        return 0;
    for (int d = 0; d < 8; d++)
        n += flips_dir(g, r, c, side, dirs[d][0], dirs[d][1]);
    return n;
}

static int
has_move(const struct game *g, int side)
{
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            if (flips(g, r, c, side) > 0)
                return 1;
    return 0;
}

static void
count(const struct game *g, int *dark, int *light)
{
    *dark = 0;
    *light = 0;
    for (int r = 0; r < SIZE; r++) {
        for (int c = 0; c < SIZE; c++) {
            if (g->board[r][c] == DARK)
                (*dark)++;
            else if (g->board[r][c] == LIGHT)
                (*light)++;
        }
    }
}

static void
game_init(struct game *g)
{
    memset(g, 0, sizeof(*g));
    g->board[3][3] = LIGHT;
    g->board[3][4] = DARK;
    g->board[4][3] = DARK;
    g->board[4][4] = LIGHT;
    g->turn = DARK;
    g->last_r = -1;
    g->last_c = -1;
}

/* Place a disc and flip; the caller checked the move is legal. Then
 * hand the turn over, passing or ending the game as the board says. */
static void
play(struct game *g, int r, int c)
{
    int side = g->turn;

    g->board[r][c] = (unsigned char)side;
    for (int d = 0; d < 8; d++) {
        int n = flips_dir(g, r, c, side, dirs[d][0], dirs[d][1]);

        for (int i = 1; i <= n; i++)
            g->board[r + i * dirs[d][0]][c + i * dirs[d][1]] =
                (unsigned char)side;
    }
    g->last_r = r;
    g->last_c = c;
    g->passed = 0;
    if (has_move(g, other(side))) {
        g->turn = other(side);
    } else if (has_move(g, side)) {
        g->passed = 1;
    } else {
        g->over = 1;
    }
}

/* The computer's choice: best weight plus flips, ties broken by the
 * first found. Returns 0 when there is no move. */
static int
ai_choose(const struct game *g, int *out_r, int *out_c)
{
    int best = -1000000;
    int found = 0;

    for (int r = 0; r < SIZE; r++) {
        for (int c = 0; c < SIZE; c++) {
            int n = flips(g, r, c, g->turn);
            int score;

            if (n == 0)
                continue;
            score = weights[r][c] + 2 * n;
            if (score > best) {
                best = score;
                *out_r = r;
                *out_c = c;
                found = 1;
            }
        }
    }
    return found;
}

/****************************************************************
 * Drawing
 ****************************************************************/

#define RGB(r, g, b) { GUT_COLOR_RGB, 0, r, g, b }

static const struct gut_color col_board = RGB(0x20, 0x70, 0x30);
static const struct gut_color col_line = RGB(0x10, 0x40, 0x18);
static const struct gut_color col_dark = RGB(0x10, 0x10, 0x10);
static const struct gut_color col_light = RGB(0xF0, 0xF0, 0xF0);
static const struct gut_color col_hint = RGB(0x50, 0xA0, 0x58);
static const struct gut_color col_hover = RGB(0x2C, 0x88, 0x3C);
static const struct gut_color col_cursor = RGB(0x70, 0x90, 0x30);
static const struct gut_color col_last = RGB(0x40, 0x70, 0x60);

static void
text(struct app *a, int row, int col, const char *s, struct gut_color fg,
     uint16_t attrs)
{
    gut_buf_text(&a->buf, row, col, s, fg, gut_color_default(), attrs);
}

static void
draw_cell(struct app *a, int r, int c)
{
    struct gut_buf *b = &a->buf;
    int row = BOARD_ROW + r * CELL_H, col = BOARD_COL + c * CELL_W;
    int disc = a->g.board[r][c];
    struct gut_color bg = col_board;
    int is_cursor = r == a->cur_r && c == a->cur_c && a->human[a->g.turn] &&
                    !a->g.over;
    int is_hover = r == a->hover_r && c == a->hover_c;

    /* the square's background says what is special about it */
    if (r == a->g.last_r && c == a->g.last_c)
        bg = col_last;
    if (is_hover)
        bg = col_hover;
    if (is_cursor)
        bg = col_cursor;
    gut_buf_fill(b, row, col, CELL_H, CELL_W, ' ', gut_color_default(), bg,
                 0);
    if (disc != EMPTY) {
        /* one disc per square, rounded with quadrant blocks:
         *   ..##..   top row:    lower-right quadrant, full, full,
         *   ######               lower-left quadrant
         *   ######   bottom row: upper-right quadrant, full, full,
         *   ..##..               upper-left quadrant */
        static const uint32_t top[CELL_W] = { 0x2597, 0x2588, 0x2588, 0x2596 };
        static const uint32_t bot[CELL_W] = { 0x259D, 0x2588, 0x2588, 0x2598 };
        struct gut_color fg = disc == DARK ? col_dark : col_light;

        for (int i = 0; i < CELL_W; i++) {
            gut_buf_put(b, row, col + i, top[i], fg, bg, 0);
            gut_buf_put(b, row + 1, col + i, bot[i], fg, bg, 0);
        }
    } else if (a->hints && !a->g.over && a->human[a->g.turn] &&
               flips(&a->g, r, c, a->g.turn) > 0) {
        /* a small centred square from half blocks */
        gut_buf_put(b, row, col + 1, 0x2584, col_hint, bg, 0);
        gut_buf_put(b, row, col + 2, 0x2584, col_hint, bg, 0);
        gut_buf_put(b, row + 1, col + 1, 0x2580, col_hint, bg, 0);
        gut_buf_put(b, row + 1, col + 2, 0x2580, col_hint, bg, 0);
    }
}

static void
draw_board(struct app *a)
{
    struct gut_buf *b = &a->buf;
    char label[2] = "a";

    /* frame and coordinates */
    gut_buf_fill(b, BOARD_ROW - 1, BOARD_COL - 1, SIZE * CELL_H + 2,
                 SIZE * CELL_W + 2, ' ', gut_color_default(), col_line, 0);
    for (int c = 0; c < SIZE; c++) {
        label[0] = (char)('a' + c);
        gut_buf_text(b, BOARD_ROW - 1, BOARD_COL + c * CELL_W + 1, label,
                     col_light, col_line, 0);
    }
    for (int r = 0; r < SIZE; r++) {
        label[0] = (char)('1' + r);
        gut_buf_text(b, BOARD_ROW + r * CELL_H, BOARD_COL - 1, label,
                     col_light, col_line, 0);
    }
    for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
            draw_cell(a, r, c);
}

static void
draw_panel(struct app *a)
{
    struct gut_buf *b = &a->buf;
    int dark, light;
    char line[48];
    int row = BOARD_ROW;

    gut_buf_fill(b, 0, PANEL_COL, b->rows, b->cols - PANEL_COL, ' ',
                 gut_color_default(), gut_color_default(), 0);
    text(a, row++, PANEL_COL, "Reversi", gut_color_indexed(11),
         GUT_ATTR_BOLD);
    row++;
    count(&a->g, &dark, &light);
    snprintf(line, sizeof(line), "%s Dark  %2d", a->g.turn == DARK &&
             !a->g.over ? ">" : " ", dark);
    text(a, row++, PANEL_COL, line, gut_color_indexed(15), 0);
    snprintf(line, sizeof(line), "%s Light %2d", a->g.turn == LIGHT &&
             !a->g.over ? ">" : " ", light);
    text(a, row++, PANEL_COL, line, gut_color_indexed(15), 0);
    row++;

    if (a->g.over) {
        const char *result = dark > light ? "Dark wins"
                           : light > dark ? "Light wins" : "A draw";

        text(a, row++, PANEL_COL, "Game over", gut_color_indexed(9),
             GUT_ATTR_BOLD);
        text(a, row++, PANEL_COL, result, gut_color_indexed(15), 0);
    } else {
        snprintf(line, sizeof(line), "%s to move (%s)",
                 a->g.turn == DARK ? "Dark" : "Light",
                 a->human[a->g.turn] ? "you" : "computer");
        text(a, row++, PANEL_COL, line, gut_color_indexed(15), 0);
        if (a->g.passed)
            text(a, row++, PANEL_COL, "Opponent had to pass",
                 gut_color_indexed(13), 0);
    }
    row++;
    if (a->message[0])
        text(a, row, PANEL_COL, a->message, gut_color_indexed(14), 0);
    row += 2;

    text(a, row++, PANEL_COL, "Arrows, Enter: play", gut_color_indexed(8),
         0);
    text(a, row++, PANEL_COL, "Mouse: play", gut_color_indexed(8), 0);
    snprintf(line, sizeof(line), "H: hints %s", a->hints ? "on" : "off");
    text(a, row++, PANEL_COL, line, gut_color_indexed(8), 0);
    text(a, row++, PANEL_COL, "U: undo  N: new game", gut_color_indexed(8),
         0);
    text(a, row++, PANEL_COL, "M: menu  Esc: quit", gut_color_indexed(8),
         0);
}

static void
draw_menu(struct app *a)
{
    static const char *const items[] = {
        "1. Human versus human",
        "2. Human versus computer",
        "3. Computer versus computer",
    };
    int row = BOARD_ROW + 3;

    gut_buf_clear(&a->buf, gut_color_default());
    text(a, BOARD_ROW, BOARD_COL + 2, "Reversi", gut_color_indexed(11),
         GUT_ATTR_BOLD);
    text(a, BOARD_ROW + 1, BOARD_COL + 2,
         "Trap the other side's discs to flip them.",
         gut_color_indexed(7), 0);
    for (int i = 0; i < 3; i++) {
        int hot = a->hover_r == row + i * 2;

        gut_buf_text(&a->buf, row + i * 2, BOARD_COL + 2, items[i],
                     hot ? gut_color_indexed(0) : gut_color_indexed(15),
                     hot ? gut_color_indexed(11) : gut_color_default(), 0);
    }
    text(a, row + 7, BOARD_COL + 2, "Press a number or click. Escape quits.",
         gut_color_indexed(8), 0);
}

static void
draw(struct app *a)
{
    if (a->mode == MODE_MENU) {
        draw_menu(a);
    } else {
        draw_board(a);
        draw_panel(a);
    }
    a->buf.cursor_visible = 0;
    gut_present(a->w, &a->buf);
}

/****************************************************************
 * Control
 ****************************************************************/

static void
new_game(struct app *a)
{
    game_init(&a->g);
    a->nhistory = 0;
    a->cur_r = 2;
    a->cur_c = 3;
    a->message[0] = '\0';
}

static void
start_mode(struct app *a, int mode)
{
    a->mode = mode;
    a->human[DARK] = mode != MODE_AI_AI;
    a->human[LIGHT] = mode == MODE_HUMAN_HUMAN;
    a->hover_r = -1;
    a->hover_c = -1;
    gut_buf_clear(&a->buf, gut_color_default());
    new_game(a);
}

static void
try_move(struct app *a, int r, int c)
{
    if (a->g.over || !on_board(r, c))
        return;
    if (flips(&a->g, r, c, a->g.turn) == 0) {
        snprintf(a->message, sizeof(a->message), "%c%d is not a legal move",
                 'a' + c, 1 + r);
        return;
    }
    a->history[a->nhistory++] = a->g;
    play(&a->g, r, c);
    a->message[0] = '\0';
}

/* Take back to the last position where a human was to move. */
static void
undo(struct app *a)
{
    while (a->nhistory > 0) {
        a->g = a->history[--a->nhistory];
        if (a->human[a->g.turn] || a->mode == MODE_AI_AI)
            break;
    }
    a->message[0] = '\0';
}

/* Mouse cell under a point, or -1. */
static void
cell_at(int col, int row, int *r, int *c)
{
    *r = -1;
    *c = -1;
    if (col < BOARD_COL || row < BOARD_ROW)
        return;
    if ((col - BOARD_COL) / CELL_W >= SIZE ||
        (row - BOARD_ROW) / CELL_H >= SIZE)
        return;
    *c = (col - BOARD_COL) / CELL_W;
    *r = (row - BOARD_ROW) / CELL_H;
}

static void
menu_event(struct app *a, const struct gut_event *ev, int *running)
{
    int first = BOARD_ROW + 3;

    switch (ev->type) {
    case GUT_EVENT_KEY:
        if (ev->key == GUT_KEY_ESCAPE)
            *running = 0;
        else if (ev->key >= '1' && ev->key <= '3')
            start_mode(a, MODE_HUMAN_HUMAN + ev->key - '1');
        break;
    case GUT_EVENT_MOUSE_MOVE:
        a->hover_r = ev->row;
        break;
    case GUT_EVENT_MOUSE_DOWN:
        if (ev->button == GUT_BUTTON_LEFT && ev->row >= first &&
            ev->row <= first + 4 && (ev->row - first) % 2 == 0)
            start_mode(a, MODE_HUMAN_HUMAN + (ev->row - first) / 2);
        break;
    default:
        break;
    }
}

static void
game_event(struct app *a, const struct gut_event *ev, int *running)
{
    int human = a->human[a->g.turn] && !a->g.over;

    switch (ev->type) {
    case GUT_EVENT_KEY:
        switch (ev->key) {
        case GUT_KEY_ESCAPE: *running = 0; break;
        case GUT_KEY_UP:    if (a->cur_r > 0) a->cur_r--; break;
        case GUT_KEY_DOWN:  if (a->cur_r < SIZE - 1) a->cur_r++; break;
        case GUT_KEY_LEFT:  if (a->cur_c > 0) a->cur_c--; break;
        case GUT_KEY_RIGHT: if (a->cur_c < SIZE - 1) a->cur_c++; break;
        case GUT_KEY_ENTER:
        case ' ':
            if (human)
                try_move(a, a->cur_r, a->cur_c);
            break;
        case 'h': a->hints = !a->hints; break;
        case 'u': undo(a); break;
        case 'n': new_game(a); break;
        case 'm':
            a->mode = MODE_MENU;
            a->hover_r = -1;
            break;
        default: break;
        }
        break;
    case GUT_EVENT_MOUSE_MOVE:
        cell_at(ev->col, ev->row, &a->hover_r, &a->hover_c);
        break;
    case GUT_EVENT_MOUSE_DOWN:
        if (ev->button == GUT_BUTTON_LEFT) {
            int r, c;

            cell_at(ev->col, ev->row, &r, &c);
            if (r >= 0) {
                a->cur_r = r;
                a->cur_c = c;
                if (human)
                    try_move(a, r, c);
            }
        } else if (ev->button == GUT_BUTTON_RIGHT) {
            undo(a);
        }
        break;
    case GUT_EVENT_FOCUS_OUT:
        a->hover_r = -1;
        a->hover_c = -1;
        break;
    default:
        break;
    }
}

int
main(void)
{
    struct gut_desc desc = { 0 };
    struct app a;
    struct gut_event ev;
    int running = 1;
    uint64_t ai_due = 0;

    memset(&a, 0, sizeof(a));
    desc.title = "reversi";
    desc.cols = COLS;
    desc.rows = ROWS;
    desc.fixed_size = 1;
    a.w = gut_open(&desc);
    if (!a.w) {
        fprintf(stderr, "gut_open: %s\n", gut_error());
        return 1;
    }
    gut_buf_init(&a.buf, ROWS, COLS);
    a.mode = MODE_MENU;
    a.hover_r = -1;
    a.hover_c = -1;
    a.hints = 1;
    new_game(&a);
    draw(&a);

    while (running) {
        int ai_turn = a.mode != MODE_MENU && !a.g.over &&
                      !a.human[a.g.turn];
        int timeout = -1;
        int got;

        if (!ai_turn)
            ai_due = 0;
        if (ai_turn) {
            uint64_t now = gut_ticks(a.w);

            if (ai_due == 0)
                ai_due = now + AI_DELAY_MS;
            timeout = now >= ai_due ? 0 : (int)(ai_due - now);
        }
        /* Handle everything that is queued, then present once: a
         * present waits for the display, so presenting per event would
         * fall behind a fast mouse. */
        got = gut_poll(a.w, &ev, timeout);
        while (got && running) {
            if (ev.type == GUT_EVENT_QUIT)
                running = 0;
            else if (a.mode == MODE_MENU)
                menu_event(&a, &ev, &running);
            else
                game_event(&a, &ev, &running);
            got = gut_poll(a.w, &ev, 0);
        }
        if (!running)
            break;
        if (ai_turn && gut_ticks(a.w) >= ai_due) {
            int r, c;

            if (ai_choose(&a.g, &r, &c))
                try_move(&a, r, c);
            ai_due = 0;
        }
        draw(&a);
    }

    gut_buf_free(&a.buf);
    gut_close(a.w);
    return 0;
}
