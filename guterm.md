# guterm programming manual

guterm is a single header C library that gives a text-based program a
window. The program fills a grid of character cells, guterm draws it with
a bitmap font, and keyboard, mouse, paste and input method events come
back through one poll call. An optional VT100/xterm emulator can fill the
grid from a byte stream for programs that already produce terminal
output.

This manual covers version 0.1.0 of `guterm.h`.

## Contents

1. Getting started
2. Concepts
3. Cells and colors
4. The cell buffer
5. Fonts
6. The window
7. Events
8. Encoding events for terminal programs
9. Clipboard, selection and paste
10. Input methods
11. Game controllers
12. The VT layer
13. Threads, blocking and other event sources
14. Portability notes
15. Reference tables

## 1. Getting started

### Building

Copy `guterm.h` into the project. In exactly one C file:

```c
#define GUTERM_IMPLEMENTATION
#include "guterm.h"
```

Every other file includes the header plainly. The implementation needs
C99, SDL3 headers at compile time and the SDL3 library at link time. No
OpenGL header or library is needed; GL entry points are resolved at run
time through SDL.

```
cc -std=c99 $(pkg-config --cflags sdl3) app.c $(pkg-config --libs sdl3)
```

Configuration macros, defined before the implementation include:

| Macro | Effect |
| --- | --- |
| `GUTERM_NO_WINDOW` | Leave out SDL and the renderer. The buffer, font tables, key encoder and VT layer remain, with no dependency beyond libc. |
| `GUTERM_NO_VT` | Leave out the VT layer. |
| `GUT_API` | Linkage for the public functions. Default is `extern`. Define it as `static` to keep the symbols private to one file. |

### A first program

```c
#define GUTERM_IMPLEMENTATION
#include "guterm.h"

int
main(void)
{
    struct gut_desc desc = { 0 };
    struct gut_buf buf;
    struct gut_event ev;
    gut_window *w;

    desc.title = "hello";
    desc.cols = 80;
    desc.rows = 25;
    w = gut_open(&desc);
    if (!w)
        return 1;
    gut_buf_init(&buf, 25, 80);
    gut_buf_text(&buf, 0, 0, "Hello, press Escape", gut_color_indexed(10),
                 gut_color_default(), GUT_ATTR_BOLD);
    gut_present(w, &buf);

    while (gut_poll(w, &ev, -1)) {
        if (ev.type == GUT_EVENT_QUIT)
            break;
        if (ev.type == GUT_EVENT_KEY && ev.key == GUT_KEY_ESCAPE)
            break;
        if (ev.type == GUT_EVENT_RESIZE)
            gut_buf_resize(&buf, ev.rows, ev.cols);
        gut_present(w, &buf);
    }
    gut_buf_free(&buf);
    gut_close(w);
    return 0;
}
```

The loop shape is the whole model: wait for an event, change the buffer,
present. guterm never draws on its own, never runs a thread, and never
calls back into the program.

## 2. Concepts

guterm has three layers, each usable without the ones above it.

| Layer | Type | Depends on | Purpose |
| --- | --- | --- | --- |
| Buffer | `struct gut_buf` | libc | A grid of cells plus a cursor. The program owns it. |
| Window | `gut_window` | SDL3 | Draws a buffer and delivers events. |
| VT | `struct gut_vt` | libc | Interprets escape sequences into a buffer. |

Two conventions run through the whole API.

**Rows before columns.** Every function that takes a position takes
`row` then `col`, zero based from the top left. Sizes follow the same
order, `rows` then `cols`, except in `struct gut_desc` and
`GUT_EVENT_RESIZE`, which say `cols` and `rows` because that is how
window sizes are usually written.

**snprintf sizing.** Functions that produce text, `gut_buf_copy_text()`
and `gut_encode_event()`, take an output buffer and its size, write at
most `size - 1` bytes plus a NUL, and return the length the complete
output needs. Call once with size 0 to measure, or use a buffer known to
be large enough and check that the return value is below the size.

All text is UTF-8. All codepoints are `uint32_t`.

## 3. Cells and colors

### struct gut_color

```c
struct gut_color {
    uint8_t type;       /* enum gut_color_type */
    uint8_t index;      /* GUT_COLOR_INDEXED */
    uint8_t r, g, b;    /* GUT_COLOR_RGB */
};
```

A color is one of three kinds:

- `GUT_COLOR_DEFAULT` resolves to the window's default foreground or
  background, whichever role the color plays. This is what an unstyled
  cell uses, and what SGR 39 and 49 select.
- `GUT_COLOR_INDEXED` selects from the 256 entry palette: 0 to 15 are the
  ANSI colors, 16 to 231 the 6 by 6 by 6 cube, 232 to 255 a gray ramp.
- `GUT_COLOR_RGB` is a 24 bit color.

Constructors and a comparison are provided so callers never fill the
struct by hand:

```c
struct gut_color gut_color_default(void);
struct gut_color gut_color_indexed(int index);   /* index & 0xFF */
struct gut_color gut_color_rgb(int r, int g, int b);
int gut_color_equal(struct gut_color a, struct gut_color b);
```

### struct gut_cell

```c
struct gut_cell {
    uint32_t cp;            /* codepoint */
    struct gut_color fg;
    struct gut_color bg;
    uint16_t attrs;         /* enum gut_attr bits */
    uint8_t width;          /* 1, 2 for a wide char, 0 continuation */
};
```

Attribute bits:

| Bit | Rendering |
| --- | --- |
| `GUT_ATTR_BOLD` | Glyph drawn twice with a one pixel shift. Indexed colors 0 to 7 become 8 to 15. |
| `GUT_ATTR_UNDERLINE` | Line near the bottom of the cell. |
| `GUT_ATTR_REVERSE` | Foreground and background swapped. |
| `GUT_ATTR_ITALIC` | Accepted and stored. The bitmap renderer draws it upright. |
| `GUT_ATTR_BLINK` | Accepted and stored. Drawn as normal text. |
| `GUT_ATTR_DIM` | Foreground scaled to two thirds. |
| `GUT_ATTR_HIDDEN` | Glyph not drawn. |
| `GUT_ATTR_STRIKE` | Line through the middle of the cell. |

Attributes are applied in that order, so a reversed dim cell has its
dimmed foreground become the background.

### Wide characters

A character whose display width is two, such as CJK ideographs, occupies
two cells. The left cell holds the codepoint with `width` 2. The right
cell holds the marker `GUT_CELL_CONT` with `width` 0. The renderer skips
continuation cells, the copy function skips them, and the buffer keeps
the pairing consistent: writing over either half blanks the other half,
and shrinking the grid blanks a pair cut by the new edge.

`gut_cell_erase(c, bg)` resets a cell to a space with foreground default,
no attributes, width 1, and the given background. This is the "background
color erase" behavior terminals use.

## 4. The cell buffer

```c
struct gut_buf {
    int rows, cols;
    struct gut_cell *cells;     /* rows * cols, row major */
    uint8_t *dirty;             /* one flag per row */
    int cursor_row, cursor_col;
    int cursor_visible;
    int cursor_shape;           /* enum gut_cursor_shape */
};
```

The struct is public on purpose. A program may read and write the cells
directly. The accessor functions exist to keep wide characters paired and
the dirty flags set, and direct writers take over those duties: set
`dirty[row]` for rows they touch, and do not split a wide pair.

Row `r`, column `c` is `cells[r * cols + c]`, or `gut_buf_cell(b, r, c)`
which returns NULL when the position is out of range.

### Lifecycle

```c
int  gut_buf_init(struct gut_buf *b, int rows, int cols);
void gut_buf_free(struct gut_buf *b);
int  gut_buf_resize(struct gut_buf *b, int rows, int cols);
```

`gut_buf_init` allocates a grid of blank cells and returns 0, or -1 when
the size is below 1 by 1 or memory runs out. The cursor starts at the top
left, visible, block shaped.

`gut_buf_resize` keeps the overlapping top left content, blanks the rest,
clamps the cursor into range and marks every row dirty. It returns 0 or
-1, and the buffer is unchanged on failure.

### Writing

```c
int  gut_buf_put(struct gut_buf *b, int row, int col, uint32_t cp,
                 struct gut_color fg, struct gut_color bg, uint16_t attrs);
int  gut_buf_text(struct gut_buf *b, int row, int col, const char *utf8,
                  struct gut_color fg, struct gut_color bg, uint16_t attrs);
void gut_buf_fill(struct gut_buf *b, int row, int col, int h, int w,
                  uint32_t cp, struct gut_color fg, struct gut_color bg,
                  uint16_t attrs);
```

`gut_buf_put` writes one codepoint and returns the number of cells it
used: 1, 2 for a wide character, or 0 when the position is out of range
or a wide character would not fit before the right edge. Zero width
codepoints such as combining marks are written as ordinary one cell
characters; the renderer will show the mark alone, so callers that want
correct combining behavior should fold marks into the base character
before writing.

`gut_buf_text` writes a string left to right from the position, clipping
at the right edge, and returns the cells used. Control characters and
zero width codepoints are skipped. Malformed UTF-8 produces U+FFFD.

`gut_buf_fill` writes one codepoint into an `h` by `w` rectangle. Out of
range parts are ignored.

### Erasing and scrolling

```c
void gut_buf_clear(struct gut_buf *b, struct gut_color bg);
void gut_buf_clear_rows(struct gut_buf *b, int from, int to,
                        struct gut_color bg);
void gut_buf_scroll(struct gut_buf *b, int top, int bot, int count,
                    struct gut_color bg);
void gut_buf_dirty_all(struct gut_buf *b);
```

Ranges are half open: `from` inclusive, `to` exclusive, and are clamped
to the buffer. `gut_buf_scroll` moves rows `top` to `bot` by `count`
lines, upward for positive counts, and fills the exposed rows with blanks
on background `bg`. A count at least as large as the region clears it.

### Pictures

```c
int  gut_buf_place_image(struct gut_buf *b, struct gut_image *img,
                         int row, int col, int cell_w, int cell_h);
void gut_buf_set_image_budget(struct gut_buf *b, size_t pixels);
const struct gut_placement *gut_buf_images(const struct gut_buf *b, int *n);
void gut_buf_cut_images(struct gut_buf *b, int from, int to);
void gut_buf_drop_images(struct gut_buf *b);
void gut_buf_swap_images(struct gut_buf *b, struct gut_image_list *other);
void gut_image_list_free(struct gut_image_list *list);
```

A buffer can pin pictures to its grid, for sixel graphics and anything
else a host decodes. A `struct gut_image` is `w` by `h` pixels of RGBA,
4 bytes each, row major, alpha 0 where nothing was painted.
`gut_buf_place_image` pins one with its top left at cell `(row, col)`,
each cell showing `cell_w` by `cell_h` of its pixels, and returns the
picture's id, from 1, or -1 when it was not placed. The buffer takes
the pixels either way and `img` is empty afterwards. Rows below the
grid are cut off; columns past the right edge are kept and left for
the renderer to clip.

Placements follow the rows they sit on. `gut_buf_scroll` moves them
with the region and cuts off what leaves it; a placement that straddles
the region's edge is split there, so the part outside stays put.
`gut_buf_clear_rows` and `gut_buf_clear` cut the rows cleared, which may
leave the bands above and below as two placements sharing one
picture. Text written over a placement does not disturb it: the cells
are drawn over the picture. `gut_buf_resize` drops every placement.
That is the documented behavior, not a gap: a resize changes the cell
size and the layout, and the program that placed the picture is the
one that knows whether and where to place it again.

The pictures count toward a budget in pixels, `GUT_BUF_IMAGE_BUDGET`
unless `gut_buf_set_image_budget` says otherwise. When a new picture
needs room the oldest placements are discarded first, by placement
time, so the two bands of a split picture go together. A picture
larger than the whole budget is refused.

`gut_buf_images` hands the placements to a renderer. Each
`struct gut_placement` names its picture through `ref`, shared by the
bands of a split, and the band of it to show: image rows from `src_y`
for `rows * cell_h` pixels or to the bottom of the picture, drawn with
its top left at `(row, col)`. `ref->id` is stable across splits and
scrolls, so a renderer can cache one texture per picture. A host that
draws the cells itself and does not want pictures can ignore the list;
nothing in the cell API changes.

`gut_buf_swap_images` exchanges the buffer's placements with a list
held elsewhere, so an emulator can park the primary screen's pictures
while the alternate screen is up. A parked list is outside the budget
until it comes back, when it is clipped to the grid and the budget
like any other placement. `gut_image_list_free` releases a parked
list; a zeroed `struct gut_image_list` is an empty one.

### Copying cells to text

```c
size_t gut_buf_copy_text(const struct gut_buf *b, int row0, int col0,
                         int row1, int col1, int mode, char *out, size_t n);
```

This turns a region into text for the clipboard. The two corners are
inclusive and may be given in either order. Out of range corners are
clamped.

- `GUT_COPY_STREAM` takes everything in reading order from the first
  corner to the second: the rest of the first row, all of the rows in
  between, and the start of the last row.
- `GUT_COPY_RECT` takes the rectangle with those corners.

Rows are joined with `\n`, trailing blanks are trimmed from each row, and
continuation cells are skipped so a wide character appears once. The
return value and `out` follow snprintf sizing. Pass `n` of 0 and `out`
NULL to measure.

guterm does not track a selection. The program decides what is selected,
highlights it by whatever means it likes, for instance by setting
`GUT_ATTR_REVERSE` on those cells, and calls this function when the user
copies.

## 5. Fonts

```c
struct gut_font {
    int glyph_w, glyph_h;
    int nglyphs;
    const uint8_t *bits;
    const uint32_t *cmap;
};
```

A font is a fixed cell bitmap. Glyph `g` occupies `glyph_h` rows of
`(glyph_w + 7) / 8` bytes each, starting at `bits[g * glyph_h * stride]`.
Bit 0 of the first byte in a row is the leftmost column. `cmap` lists the
codepoint of each glyph in strictly ascending order, which is what
`gut_font_lookup()` binary searches.

```c
const struct gut_font *gut_font_default(void);
int gut_font_lookup(const struct gut_font *font, uint32_t cp);
```

The built-in font is an 8 by 16 subset of unscii, 491 glyphs: ASCII,
Latin-1, general punctuation, superscripts, arrows, box drawing, block
elements and geometric shapes. It has no CJK glyphs; wide characters take
two cells but draw as the fallback glyph.

A codepoint the font lacks is drawn as U+FFFD if the font has it, else as
`?`. Glyphs are drawn at an integer zoom, `gut_desc.scale`, so an 8 by 16
font at scale 2 gives 16 by 32 pixel cells. A scale of 0 picks 1 on an
ordinary display and the rounded content scale that SDL reports on a
high density display, 2 on a typical 200 percent desktop, so text comes
out about the same physical size everywhere. Resizing the window changes
the grid, never the zoom.

To supply a font, fill a `struct gut_font` with static tables and pass
its address in `gut_desc.font`. The tables must outlive the window.
Glyph widths up to any multiple of 8 work; the atlas is laid out from
`glyph_w` and `glyph_h`. The renderer does not support proportional
fonts or more than one font per window.

## 6. The window

### Opening

```c
struct gut_desc {
    const char *title;
    int cols, rows;                 /* initial grid, default 80 x 25 */
    int scale;                      /* integer pixel zoom; 0 picks 1, or
                                       the display content scale on a
                                       high density display */
    const struct gut_font *font;    /* NULL for the built-in 8x16 */
    uint32_t fg, bg;                /* 0xRRGGBB defaults; 0 means unset */
    const uint32_t *palette;        /* 16 ANSI colors 0xRRGGBB or NULL */
    int fixed_size;                 /* 1 disables window resizing */
    int no_paste_keys;              /* 1 delivers paste chords as keys */
    int no_compose_overlay;         /* 1 leaves IME preedit drawing to
                                       the program */
    int no_gamepad;                 /* 1 skips game controller support */
};

gut_window *gut_open(const struct gut_desc *desc);
void gut_close(gut_window *w);
const char *gut_error(void);
```

Zero initialise the descriptor and set what differs from the defaults. A
NULL descriptor means all defaults. The default foreground is 0xD0D0D0
and the default background 0x000000; because 0 means unset, a pure black
foreground or background is requested as 0x000001.

`gut_open` initialises SDL video if the program has not, creates the
window sized to the grid, obtains a GL context, builds the shader and
glyph atlas, shows the window and turns on text input. It returns NULL on
failure, and `gut_error()` then holds a one line reason such as a missing
GL entry point or a shader log.

The context is requested as OpenGL ES 2.0 first. When that is refused the
platform default context is used, which is a desktop compatibility or
legacy context on Windows and macOS. The shaders are written in the
subset shared by GLSL ES 1.00 and GLSL 1.10 so both work. The renderer
uses nothing beyond ES 2.0.

Only one window is supported at a time. `gut_close` releases everything,
and quits the SDL video subsystem when `gut_open` was the one to start
it.

### Drawing

```c
void gut_present(gut_window *w, struct gut_buf *b);
```

This draws the buffer and swaps. The whole visible grid is rebuilt every
call; a full 80 by 25 frame is a few thousand quads, which is cheap. The
buffer's dirty flags are cleared afterwards. A program that wants to skip
redundant frames can check the flags itself before calling.

The buffer may be a different size from the window grid. The overlap
starting at the top left is drawn and the rest of the window is cleared
to the default background. A program normally keeps the two equal by
resizing the buffer on `GUT_EVENT_RESIZE`.

The cursor is drawn at `cursor_row`, `cursor_col` when `cursor_visible`
is set. A block cursor inverts the cell. The underline and bar shapes
overlay it. When the window has lost focus the cursor becomes a hollow
box regardless of shape.

The buffer's pictures, section 4, are drawn under the cells: a cell with
the default background shows the picture through it and a colored one
covers it, so text written over a picture stays readable and an erase
with a background color removes it from view. Each picture is one
texture, uploaded the first time it is seen and released when no
placement shows it any more. A picture is scaled from the cell size it
was placed at to the window's, so at zoom 2 each of its pixels is a
2 by 2 block, like the font's.

`gut_present` also tells the platform where the cursor cell is, which an
input method uses to place its candidate window. See section 10.

### Grid geometry

```c
void gut_grid_size(const gut_window *w, int *cols, int *rows);
void gut_set_grid_size(gut_window *w, int cols, int rows);
```

The grid is however many whole cells fit in the framebuffer. On a high
density display the framebuffer is larger than the window's size in
points, and all cell arithmetic uses the framebuffer, so cells are always
whole pixels. `gut_set_grid_size` resizes the window so the grid is
exactly the requested size.

### Appearance and miscellany

```c
void gut_set_title(gut_window *w, const char *title);
void gut_set_defaults(gut_window *w, uint32_t fg, uint32_t bg);
void gut_set_palette(gut_window *w, const uint32_t *palette16);
uint64_t gut_ticks(const gut_window *w);
```

`gut_set_palette` takes the 16 ANSI colors and rebuilds the cube and gray
ramp from the standard formulas. NULL restores the xterm defaults.
Changes take effect at the next present. `gut_ticks` returns milliseconds
since the window opened.

## 7. Events

```c
int gut_poll(gut_window *w, struct gut_event *ev, int timeout_ms);
void gut_wake(gut_window *w);
```

`gut_poll` waits for the next event and returns 1 with `ev` filled, or 0
when `timeout_ms` passed with nothing to report. A negative timeout waits
forever and 0 returns at once. Events SDL delivers that guterm has no use
for are consumed silently.

`gut_wake` makes a `gut_poll` in progress, or the next one, return a
`GUT_EVENT_WAKE` event. It is the one window call that may be made from
another thread. Wakes that pile up before `gut_poll` delivers one
collapse into a single event, so a program checks its own sources once
per wake and finds everything that arrived. Section 13 shows the use.

```c
struct gut_event {
    int type;           /* enum gut_event_type */
    int key;            /* enum gut_key or codepoint */
    int mods;           /* enum gut_mod bits */
    int repeat;         /* key auto-repeat */
    char text[32];      /* GUT_EVENT_TEXT, NUL terminated, may truncate */
    const char *data;   /* TEXT, COMPOSE, PASTE: full text */
    size_t len;         /* bytes in data */
    int cursor;         /* COMPOSE: caret position in codepoints, or -1 */
    int primary;        /* PASTE: 1 from the primary selection */
    int col, row;       /* mouse position in cells */
    int x, y;           /* mouse position in pixels */
    int button;         /* enum gut_button, or enum gut_pad_button */
    int clicks;         /* MOUSE_DOWN: 1 single, 2 double, 3 triple */
    int dx, dy;         /* wheel notches */
    int cols, rows;     /* GUT_EVENT_RESIZE */
    int pad;            /* PAD events: controller slot 0 to 3 */
    int axis;           /* PAD_AXIS: enum gut_pad_axis */
    int value;          /* PAD_AXIS: -32768 to 32767 */
};
```

Every event carries `mods`, the modifier state at the time. The other
fields are meaningful per type:

| Type | Fields | Meaning |
| --- | --- | --- |
| `GUT_EVENT_KEY` | `key`, `mods`, `repeat` | A key went down. |
| `GUT_EVENT_TEXT` | `text`, `data`, `len` | Text was typed or committed by an input method. |
| `GUT_EVENT_COMPOSE` | `data`, `len`, `cursor` | Input method composition changed. |
| `GUT_EVENT_PASTE` | `data`, `len`, `primary`, `col`, `row` | The user pasted. |
| `GUT_EVENT_RESIZE` | `cols`, `rows` | The grid now fits this many cells. |
| `GUT_EVENT_QUIT` | | The window was asked to close. |
| `GUT_EVENT_MOUSE_DOWN`, `GUT_EVENT_MOUSE_UP` | `col`, `row`, `x`, `y`, `button`, `clicks` | A button changed. `clicks` counts a double or triple click on the way down. |
| `GUT_EVENT_MOUSE_MOVE` | `col`, `row`, `x`, `y`, `button` | The pointer moved. `button` is the first button held, or 0. |
| `GUT_EVENT_MOUSE_WHEEL` | `col`, `row`, `x`, `y`, `dx`, `dy` | The wheel turned. Positive `dy` is away from the user. |
| `GUT_EVENT_FOCUS_IN`, `GUT_EVENT_FOCUS_OUT` | | Keyboard focus changed. |
| `GUT_EVENT_PAD_ADDED`, `GUT_EVENT_PAD_REMOVED` | `pad` | A game controller took or left a slot. |
| `GUT_EVENT_PAD_DOWN`, `GUT_EVENT_PAD_UP` | `pad`, `button` | A controller button changed; `button` is an `enum gut_pad_button`. |
| `GUT_EVENT_PAD_AXIS` | `pad`, `axis`, `value` | A stick or trigger moved. |
| `GUT_EVENT_KEY_UP` | `key`, `mods` | A key was released. |
| `GUT_EVENT_WAKE` | | `gut_wake` was called, from any thread. |

### Key state

```c
int gut_key_held(const gut_window *w, int key);
int gut_mouse_held(const gut_window *w, int button);
int gut_mods_held(const gut_window *w);
int gut_keys_held(const gut_window *w, int *keys, int n);
```

A text program acts on key presses. A game acts on what is held: the
player moves while W is down and stops when it comes up, and two keys
held together mean a diagonal. guterm sits between a terminal, which
only ever sees presses, and a DOS text mode program, which could read
the keyboard directly, so it offers both views.

`gut_key_held` answers whether a key is down right now, by its `gut_key`
code, lowercase for a letter so `'w'` is true whatever Shift is doing.
`gut_mouse_held` does the same for a mouse button, `gut_mods_held`
returns the modifiers as `GUT_MOD_*` bits, and `gut_keys_held` lists
every key down, for a program that wants to show or log them.

The state is kept from the key events `gut_poll` has delivered, so it
is as current as the last poll; a game loop drains the queue, then reads
the state, then simulates and draws. Auto-repeat does not disturb it,
and `GUT_EVENT_KEY_UP` reports each release for programs that want the
edge too. When the window loses focus every key and button is released,
since the releases that happen while another window has the keyboard
are never seen; `GUT_EVENT_FOCUS_OUT` arrives at the same moment.

Keys are named by the symbol on them, so a game that reads `'w'`,
`'a'`, `'s'` and `'d'` reads the letters in those positions on a QWERTY
layout and other letters elsewhere. A game that cares should let the
player rebind.

### Keys versus text

Keyboard input arrives on two channels, and a program should treat them
differently.

`GUT_EVENT_KEY` reports the physical key by its unshifted symbol: a
letter key gives the lowercase letter, the key marked `1` gives `'1'`
whether or not shift is held, and keys with no symbol give a `GUT_KEY_`
code above `GUT_KEY_SPECIAL`. Use it for shortcuts and navigation:
arrows, function keys, Enter, Escape, Tab, Backspace, and Ctrl or Alt
combinations.

`GUT_EVENT_TEXT` reports what the user typed as text, after the keyboard
layout, shift state, dead keys and input methods have done their work.
Use it for every printable character. A plain `a` arrives as both a KEY
event with `key` `'a'` and a TEXT event with `"a"`; a program that acts
on printable KEY events would double its input, so it should not.

The committed text is in `data` and `len`. The fixed `text` array holds
the same text when it fits in 31 bytes, for convenience; a long commit
from an input method is cut there but complete in `data`.

### Ownership of event text

`data` points into memory owned by the window. It stays valid until the
next call to `gut_poll()`, then it is released. A program that needs the
text longer copies it.

### Resize

When the window is resized the grid changes and a `GUT_EVENT_RESIZE`
reports the new `cols` and `rows`. No event is sent when the size changed
by less than a cell. The program is expected to resize its buffer, and
when using the VT layer to call `gut_vt_resize()` instead, which does
both.

## 8. Encoding events for terminal programs

```c
size_t gut_encode_event(const struct gut_event *ev, char *out, size_t n,
                        int flags);
```

A program that was written for a terminal reads bytes, not events. This
function produces the bytes an xterm would send for an event. The
program's own key decoder then works unchanged.

| Event | Output |
| --- | --- |
| `GUT_EVENT_TEXT` | The text as is. |
| `GUT_EVENT_KEY`, special key | The xterm sequence: CSI or SS3 for cursor keys, `CSI n ~` for editing keys and F5 to F12, SS3 for F1 to F4. Modifiers add the `1;m` parameter. Shift+Tab gives `CSI Z`. |
| `GUT_EVENT_KEY`, printable with Ctrl | The control character, with the usual xterm mappings for punctuation. |
| `GUT_EVENT_KEY`, printable with Alt | ESC then the character. |
| `GUT_EVENT_KEY`, printable, no Ctrl or Alt | Nothing; the TEXT event carries it. |
| `GUT_EVENT_PASTE` | The text with newlines turned into carriage returns, as a terminal sends Enter. With `GUT_ENC_BRACKET_PASTE`, wrapped in `CSI 200 ~` and `CSI 201 ~`. |
| Mouse events | An xterm mouse report when a `GUT_ENC_MOUSE_*` flag asks for that kind of event, see below. Nothing otherwise. |
| `GUT_EVENT_FOCUS_IN`, `GUT_EVENT_FOCUS_OUT` | `CSI I` and `CSI O` with `GUT_ENC_FOCUS`. Nothing otherwise. |
| Anything else | Nothing. |

Flags:

| Flag | Effect |
| --- | --- |
| `GUT_ENC_APP_CURSOR` | Cursor keys send SS3 sequences, the DECCKM application mode. |
| `GUT_ENC_BS_BS` | Backspace sends BS (0x08) instead of DEL (0x7F). |
| `GUT_ENC_BRACKET_PASTE` | Bracketed paste, as above. |
| `GUT_ENC_MOUSE_BTN` | Mode 1000: report presses, releases and the wheel. |
| `GUT_ENC_MOUSE_DRAG` | Mode 1002: also motion while a button is held. |
| `GUT_ENC_MOUSE_ANY` | Mode 1003: all motion. |
| `GUT_ENC_MOUSE_SGR` | Mode 1006: the `CSI < b ; x ; y M` form, with `m` for a release, instead of the three byte `CSI M` form. |
| `GUT_ENC_FOCUS` | Mode 1004: focus reports. |

### Mouse reports

The encoder produces what xterm sends. The button code is 0, 1 and 2 for
left, middle and right, 64 and 65 for the wheel away from and towards
the user, 66 and 67 for a horizontal wheel, plus 32 for motion, 4 for
Shift, 8 for Alt and 16 for Ctrl. Coordinates are 1 based. The legacy
form adds 32 to each value and sends them as single bytes, so it cannot
express a column or row above 223; such an event encodes to nothing.
A release in the legacy form uses button code 3. The SGR form has no
such limit and names the released button.

Without the VT layer, a host passes the flags that match the modes the
program turned on. The encoder itself is stateless: it reports every
motion event it is given, while xterm reports one per cell. The VT
layer's `gut_vt_mouse()` in section 12 adds that and the other
conventions.

The return value and `out` follow snprintf sizing. Thirty two bytes
cover every key and mouse report; a paste needs `ev->len + 16`. When
using the VT layer,
`gut_vt_encode_flags()` returns the flags that match the modes the
program has set, so the right thing happens without the caller tracking
modes:

```c
char small[64], *buf = small;
size_t cap = sizeof(small), n;

if (ev.len + 16 > cap) {
    cap = ev.len + 16;
    buf = malloc(cap);
}
n = gut_encode_event(&ev, buf, cap, gut_vt_encode_flags(&vt));
if (n > 0 && n < cap)
    write(pty, buf, n);
if (buf != small)
    free(buf);
```

## 9. Clipboard, selection and paste

Three mechanisms cover copy and paste, in order from lowest level to
highest.

### Clipboard calls

```c
const char *gut_clipboard_get(gut_window *w);
void gut_clipboard_set(gut_window *w, const char *utf8);
const char *gut_primary_get(gut_window *w);
void gut_primary_set(gut_window *w, const char *utf8);
```

The getters return the current text, or NULL when there is none. The
string belongs to the window until the next call of the same getter.

The primary selection is the X11 and Wayland convention where selecting
text makes it available for middle click paste without a copy command.
On other platforms the getter returns NULL and the setter does nothing.
A program that implements selection on Linux should call
`gut_primary_set()` when the selection changes, so other applications can
middle click paste from it.

### Paste events

Rather than watching for paste chords itself, a program receives
`GUT_EVENT_PASTE`. guterm recognises:

- Shift+Insert on every platform.
- Ctrl+Shift+V on Linux and Windows. Ctrl+V alone stays a key event
  because terminal programs use it as a literal next character command.
- Cmd+V on macOS.
- Middle click, for the primary selection. The event's `col` and `row`
  say where the click landed and `primary` is 1.

The chord is consumed only when there is text to paste. An empty
clipboard leaves the key or button event as it was. Setting
`gut_desc.no_paste_keys` turns recognition off entirely, for programs
that bind those keys themselves and call the getters.

### Copying

Copy is the program's business, since only it knows what is selected.
The pieces are `gut_buf_copy_text()` to turn cells into text and
`gut_clipboard_set()` to publish it. There is no built in copy chord.
For the common case of a mouse driven selection over the buffer, the
selection helper below does the tracking.

### Selection

```c
struct gut_sel {
    int active;             /* a selection exists */
    int dragging;           /* the button is still held */
    int mode;               /* GUT_COPY_STREAM or GUT_COPY_RECT */
    int unit;               /* GUT_SEL_CELL, GUT_SEL_WORD, GUT_SEL_LINE */
    int anchor_row, anchor_col;
    int row0, col0;         /* normalised: start, inclusive */
    int row1, col1;         /* end, inclusive */
};

void   gut_sel_clear(struct gut_sel *s);
void   gut_sel_begin(struct gut_sel *s, const struct gut_buf *b,
                     int row, int col, int mode, int unit);
void   gut_sel_extend(struct gut_sel *s, const struct gut_buf *b,
                      int row, int col);
int    gut_sel_mouse(struct gut_sel *s, const struct gut_buf *b,
                     const struct gut_event *ev);
int    gut_sel_contains(const struct gut_sel *s, int row, int col);
size_t gut_sel_text(const struct gut_sel *s, const struct gut_buf *b,
                    char *out, size_t n);
void   gut_set_selection(gut_window *w, const struct gut_sel *sel);
```

`gut_sel` is a plain struct the program owns, zeroed by
`gut_sel_clear`. It lives in the buffer layer and needs no window.

`gut_sel_mouse` consumes mouse events and returns 1 when the selection
changed, which is the cue to present and, on a release, to publish the
text. The gestures are the usual ones:

- Left press and drag selects cells in reading order. A press that is
  released without moving clears the selection.
- A double click selects a word, a run of non-blank cells, and dragging
  extends by whole words. A triple click selects the line and dragging
  extends by lines. The window counts the clicks in `ev->clicks`.
- Shift+click moves the nearer end of an existing selection to the
  click.
- Alt+drag selects a rectangle, `GUT_COPY_RECT`.
- A wide character is taken whole when either of its cells is at an
  end.
- Other buttons and the wheel are ignored and return 0, so the program
  can use them.

`gut_sel_begin` and `gut_sel_extend` do the same without a mouse, for
keyboard selection. Points outside the buffer clamp to its edge.

`gut_sel_contains` answers for one cell, for programs that draw the
selection themselves. `gut_sel_text` gives the selected text with
`gut_buf_copy_text` sizing, and 0 when there is no selection.

`gut_set_selection` hands the window a copy to highlight on the next
`gut_present`, drawn with each cell's colors swapped. NULL clears it.
The highlight is purely visual; the buffer is not touched.

The selection is in buffer coordinates. It does not follow the content
when the buffer scrolls or the VT view moves, so a terminal host clears
it when it sends keys to the child, as `examples/term.c` does. On X11
and Wayland the host also calls `gut_primary_set` with the text when a
drag ends.

## 10. Input methods

An input method, or IME, lets the user compose characters the keyboard
cannot type directly: East Asian scripts, dead key accents on some
platforms, emoji pickers. SDL does the platform work; guterm exposes the
result in a way a cell grid can use.

### Text input state

```c
void gut_set_text_input(gut_window *w, int on);
```

Text input is on from `gut_open()`. While on, the platform composes text
and delivers `GUT_EVENT_TEXT` for what the user types, and on touch
platforms shows the on screen keyboard. Turning it off stops composition,
hides the keyboard, drops any composition in progress, and leaves only
`GUT_EVENT_KEY` arriving. A program would turn it off in a mode where
only raw keys matter, such as a game, and back on for text entry.

### Composition

While the user composes, the text so far is not yet committed and should
be shown provisionally at the cursor. Each change arrives as
`GUT_EVENT_COMPOSE` with the current composition in `data` and `len` and
the caret position within it in `cursor`, counted in codepoints, or -1
when the platform does not say. An empty composition means the user
cancelled.

By default guterm draws the composition itself: at the next present it
overlays the text from the cursor cell rightwards, underlined, with a bar
at the caret, wrapping to the next row at the right edge. The buffer is
not touched. When the user commits, the composition disappears and the
committed text arrives as an ordinary `GUT_EVENT_TEXT`, which the program
writes wherever it writes typed text. A program that needs nothing
special can therefore ignore `GUT_EVENT_COMPOSE` and still behave
correctly.

```c
void gut_set_compose_overlay(gut_window *w, int on);
```

A program that wants to draw the composition itself, for instance to
place it in its own input line, turns the overlay off here or with
`gut_desc.no_compose_overlay`, and renders from the compose events.

### Candidate window placement

Input methods show a list of candidates in a floating window and want to
put it next to the text being composed. `gut_present()` reports the
cursor cell's rectangle to the platform after every frame, so the
candidate list follows the cursor without the program doing anything.
Keep `cursor_row` and `cursor_col` where the text will go and placement
is right.

### Keys during composition

Key down events continue to arrive for the keys the user presses while
composing. A program that follows the rule in section 7, acting on text
events for printable input and on key events only for specials and
modifier combinations, is unaffected.

## 11. Game controllers

```c
#define GUT_MAX_PADS 4

struct gut_pad {
    int connected;
    char name[64];
    uint32_t buttons;       /* bit (1 << gut_pad_button) per held button */
    int16_t axes[GUT_PAD_AXIS_COUNT];
};

int gut_pad_get(const gut_window *w, int slot, struct gut_pad *out);
int gut_pad_rumble(gut_window *w, int slot, uint16_t low, uint16_t high,
                   uint32_t ms);
```

Up to four controllers are tracked in slots 0 to 3. A controller takes
the lowest free slot when it connects and frees it when it leaves; a
fifth controller is ignored until a slot opens. Every controller is
presented in the Xbox layout SDL maps it to, so a PlayStation cross is
`GUT_PAD_A` and its circle is `GUT_PAD_B`.

Buttons: `GUT_PAD_A`, `B`, `X`, `Y`, `BACK`, `GUIDE`, `START`,
`LSTICK`, `RSTICK`, `LSHOULDER`, `RSHOULDER` and the directional pad
`UP`, `DOWN`, `LEFT`, `RIGHT`. Axes: `GUT_PAD_AXIS_LX`, `LY`, `RX`,
`RY` for the sticks, from -32768 at the left or top to 32767, and
`GUT_PAD_AXIS_LT`, `RT` for the triggers, from 0 released to 32767.
Values are raw; a program applies its own dead zone.

Changes arrive as events: `GUT_EVENT_PAD_ADDED` and
`GUT_EVENT_PAD_REMOVED` with `pad`, `GUT_EVENT_PAD_DOWN` and
`GUT_EVENT_PAD_UP` with `pad` and `button`, and `GUT_EVENT_PAD_AXIS`
with `pad`, `axis` and `value`. A stick in motion produces many axis
events. A program that would rather sample reads the whole state with
`gut_pad_get`, which returns 1 while a controller is connected in that
slot. Controllers already plugged in when the window opens produce
`GUT_EVENT_PAD_ADDED` from the first polls.

`gut_pad_rumble` vibrates for `ms` milliseconds with the low and high
frequency motors at 0 to 65535. It returns -1 for an empty slot or a
controller without rumble.

Setting `gut_desc.no_gamepad` leaves the SDL gamepad subsystem
uninitialised, for programs that never want controller events or that
manage SDL gamepads themselves.

## 12. The VT layer

The VT layer is a terminal emulator that writes into a `gut_buf`. A
program that already produces terminal output, or that hosts a child
process on a pseudo terminal, feeds those bytes in and presents the
buffer.

```c
int  gut_vt_init(struct gut_vt *vt, struct gut_buf *buf);
void gut_vt_free(struct gut_vt *vt);
void gut_vt_reset(struct gut_vt *vt);
void gut_vt_feed(struct gut_vt *vt, const char *data, size_t len);
int  gut_vt_resize(struct gut_vt *vt, int rows, int cols);
unsigned gut_vt_modes(const struct gut_vt *vt);
int  gut_vt_encode_flags(const struct gut_vt *vt);
```

`gut_vt_init` attaches the emulator to a buffer the program has
initialised and resets it: modes to their power on state, cursor home,
tabs every eight columns, screen cleared. The buffer remains the
program's; it may read the cells at any time and should not write them
while the emulator is in use, except through `gut_vt_feed`.

`gut_vt_feed` takes any number of bytes, including partial sequences;
state carries across calls. After each call the buffer's cursor fields
reflect the emulator's cursor, so presenting shows it in the right
place.

`gut_vt_resize` resizes the buffer and keeps the emulation consistent:
the scroll region is reset when it covered the whole screen or no longer
fits, the cursor and saved cursor are clamped, and tab stops are kept or
extended. Lines above the cursor move into the scrollback when the
screen becomes too short for it, and lines come back out of the
scrollback when the screen grows. Use it instead of `gut_buf_resize`
while the VT layer is attached.

### Scrollback

```c
int  gut_vt_set_scrollback(struct gut_vt *vt, int lines);
int  gut_vt_scrollback_lines(const struct gut_vt *vt);
void gut_vt_clear_scrollback(struct gut_vt *vt);
int  gut_vt_set_view(struct gut_vt *vt, int offset);
int  gut_vt_scroll_view(struct gut_vt *vt, int delta);
int  gut_vt_view_offset(const struct gut_vt *vt);
```

Lines that scroll off the top of the primary screen are kept in a ring
of `GUT_VT_SCROLLBACK_DEFAULT` (1000) lines. `gut_vt_set_scrollback`
changes the capacity at any time, keeping the newest lines, and 0
disables the scrollback. Each stored line is trimmed to its last
non-blank cell, so memory follows the content rather than the width.

A line enters the scrollback when a line feed, IND or NEL at the bottom
of the screen, or SU, scrolls the whole screen, that is when the scroll
region starts at row 0. Scrolling inside a DECSTBM region that starts
lower, IL and DL, and anything on the alternate screen leave the
scrollback alone. ED 3 (`CSI 3 J`, the second half of `clear`) and
`gut_vt_reset` discard it, as does `gut_vt_clear_scrollback`.

The program looks into the scrollback by moving the view.
`gut_vt_set_view(vt, n)` shows the screen scrolled back by `n` lines:
the top `n` rows of the buffer come from the scrollback and the rest
from the top of the live screen, with the cursor hidden. `n` is clamped
to what the scrollback holds and to 0 on the alternate screen, and the
offset in effect is returned. `gut_vt_scroll_view` moves relative to the
current offset, positive being further back. `gut_vt_set_view(vt, 0)`
returns to the live screen. A typical host binds Shift+PageUp and the
wheel to `gut_vt_scroll_view` and calls `gut_vt_set_view(vt, 0)` before
sending any key to the child, which is what `examples/term.c` does.

While the view is scrolled back, `gut_vt_feed` keeps interpreting output
into a private copy of the screen and the buffer keeps showing the view,
which stays on the same lines as new ones arrive, until the oldest line
in view falls out of the ring. The rows the view changes are marked
dirty, so presenting works as usual. The program should not write to
the buffer itself while scrolled back, since the live screen is restored
over it when the view returns to 0.

### What is interpreted

Printing: UTF-8 with U+FFFD for malformed input, East Asian wide
characters taking two cells, DEC special graphics when the G0 or G1 set
selects line drawing, auto wrap with the deferred wrap at the last column
that programs expect, insert mode.

Controls: BEL, BS, HT, LF, VT, FF, CR, SO, SI.

Cursor: CUU, CUD, CUF, CUB, CNL, CPL, CHA, HPA, CUP, HVP, VPA, CBT, save
and restore with both DECSC/DECRC and SCOSC/SCORC, IND, RI, NEL.

Erasing and editing: ED including ED 3 that clears the scrollback, EL,
ECH, ICH, DCH, IL, DL, SU, SD.

Attributes: SGR with bold, dim, italic, underline, blink, reverse,
hidden, strike, the 8 and 16 color sets, 256 colors and 24 bit color.

Modes: DECSTBM scroll region, DECOM origin mode, DECAWM auto wrap, IRM
insert mode, DECTCEM cursor visibility, DECSCUSR cursor shape, DECCKM
application cursor keys, DECKPAM application keypad, bracketed paste,
mouse tracking modes 1000, 1002 and 1003 with the SGR encoding 1006,
focus reporting 1004, and the alternate screen via 47, 1047 and 1049
with the primary screen parked and restored.

Tabs: HTS, TBC for one stop and for all.

Reports: DA1 answers as a VT100 with advanced video and sixel graphics, DSR 5 and 6 answer
status and cursor position. XTSMGRAPHICS
answers the color register count and the pixel area a picture may
cover. RIS performs a full reset.

OSC 0 and 2 set the title and OSC 52 reaches the clipboard, both
through callbacks. Sixel pictures are placed on the screen, see below.
Other OSC, other DCS, APC, PM and SOS strings, and unknown CSI and ESC
sequences are consumed and ignored, so a program
that emits sequences the emulator does not know still displays sanely.
An OSC string is kept up to `GUT_VT_OSC_MAX`, one mebibyte; a longer
one is dropped whole.

Not implemented: kitty keyboard protocol, XTWINOPS, character set
designations other than ASCII and line drawing, and combining
characters, which are dropped.

### Pictures

A sixel picture, DCS q, is decoded as it streams by `gut_sixel_begin`,
`gut_sixel_put` and `gut_sixel_end`, which a program may also use on
their own, and placed on the screen with `gut_buf_place_image` as
section 4 describes. `gut_vt_set_image_limit` bounds a picture in
pixels, `GUT_VT_IMAGE_MAX_PIXELS` by default; a larger one is dropped
and the rest of its data ignored. The buffer's own budget bounds what
all the pictures hold together.

`gut_vt_set_cell_size` tells the emulator how many pixels a cell is,
8 by 16 by default, so it can turn a picture's size into rows and
columns. Give it the font's glyph size rather than the zoomed cell:
a window that draws the font at a zoom draws the pictures at the same
zoom, so a picture keeps its place under the text.

With sixel scrolling, the default, a picture starts at the cursor. The
scroll region scrolls up to make room for it and for a line below it,
where the cursor ends, in the column the picture started from. A
picture taller than the region loses its top, as it would on a
terminal that drew it band by band. With DECSDM, DECSET 80, the
picture sits at the home position and the cursor stays put.

Pictures follow the text: they scroll with it, are cut by erases, go
with the alternate screen and come back with the primary one, and show
in a scrolled back view at their place below the scrollback lines.
Lines that scroll off the top take their part of a picture with them;
the scrollback keeps text only. A resize drops every picture, the
alternate screen's parked ones included.

### Callbacks

```c
void gut_vt_set_reply(struct gut_vt *vt,
                      void (*fn)(void *ctx, const char *data, size_t len),
                      void *ctx);
void gut_vt_set_title_cb(struct gut_vt *vt,
                         void (*fn)(void *ctx, const char *title),
                         void *ctx);
void gut_vt_set_bell_cb(struct gut_vt *vt, void (*fn)(void *ctx),
                        void *ctx);
```

The reply callback receives the bytes a terminal sends back to the
program in answer to DA and DSR. A host with a child process writes them
to the pseudo terminal. Without a reply callback those queries go
unanswered, which well behaved programs treat as unsupported.

The title callback receives the string from OSC 0 or 2; passing it to
`gut_set_title()` is the usual action. The bell callback fires on BEL and
does nothing by itself.

### OSC 52 clipboard

```c
void gut_vt_set_clipboard_cb(struct gut_vt *vt,
                             void (*set)(void *ctx, int which,
                                         const char *text, size_t len),
                             const char *(*get)(void *ctx, int which),
                             void *ctx);
```

Programs such as tmux, vim and the `clip` helpers copy through the
terminal with `OSC 52 ; Pc ; Pd`. `Pc` names the selections, `c` for
the clipboard and `p` or `s` for the primary selection, empty meaning
primary; `Pd` is the text in base64. The emulator decodes it and calls
`set` once per named selection with `which` as `GUT_CLIP_CLIPBOARD` or
`GUT_CLIP_PRIMARY` and the text NUL terminated, so the host can pass it
to `gut_clipboard_set()` or `gut_primary_set()`. Text that is not
base64 arrives empty, which xterm treats as clearing. The text may
contain NUL bytes, which is why `len` is given.

A `Pd` of `?` is a query. When a `get` callback is set, the emulator
replies through the reply callback with `OSC 52 ; Pc ; base64 ST`,
taking the text from the first named selection that has any. Without
`get` the query is ignored. That is the safe default and what
`examples/term.c` does: answering lets any program running in the
terminal read whatever the user copied elsewhere, so a host should
enable it knowingly, as xterm's `allowWindowOps` and other terminals'
settings do.

### Modes and the encoder

`gut_vt_modes()` returns the current mode flags, `GUT_VT_MODE_*`, for a
host that wants to know for instance whether the program has enabled
mouse tracking. `gut_vt_encode_flags()` maps the modes that affect
encoding, application cursor keys, bracketed paste, mouse tracking and
focus reporting, onto `gut_encode_event()` flags.

### Mouse reporting

```c
size_t gut_vt_mouse(struct gut_vt *vt, const struct gut_event *ev,
                    char *out, size_t n);
```

A host that forwards mouse and focus events through `gut_encode_event`
with `gut_vt_encode_flags` already gets correct reports. `gut_vt_mouse`
wraps that with the conventions xterm users expect, so the host does
not need to know the protocol at all. For a mouse or focus event it
returns the bytes to send, with `gut_encode_event` sizing, or 0 when the
event is the host's to use, for selection or scrolling:

- When the program has not enabled tracking, every mouse event is the
  host's. The exception is the wheel on the alternate screen, which
  becomes three Up or Down key presses per notch, so a pager or editor
  scrolls under the wheel the way it does in xterm.
- With tracking on, a Shift modifier bypasses it, so the user can still
  select text in a program that owns the mouse.
- Motion is reported once per cell, as xterm does, however often the
  pointer moves inside a cell.
- Focus events are reported when the program asked with mode 1004.

A host therefore handles the four mouse event types and the two focus
events with one call, and falls through to its own handling on 0:

```c
char bytes[64];
size_t n = gut_vt_mouse(&vt, &ev, bytes, sizeof(bytes));

if (n > 0)
    write(pty, bytes, n);
else if (ev.type == GUT_EVENT_MOUSE_WHEEL)
    gut_vt_scroll_view(&vt, ev.dy * 3);
else if (gut_sel_mouse(&sel, &buf, &ev))
    gut_set_selection(w, &sel);
```

The call keeps the last reported cell in the emulator, so it must be
made once per event.

### A terminal host

The shape of a program that runs a shell, from `examples/term.c`:

1. Open the window, initialise a buffer and the VT layer, set the reply
   callback to write to the pseudo terminal master and the title callback
   to set the window title.
2. Fork the child on a pseudo terminal with the grid size as its window
   size and `TERM` set to `xterm-256color`.
3. Loop: poll the window with a zero timeout, then poll the master
   descriptor with a short timeout, feed whatever the child wrote, encode
   and write whatever the user did, and present.
4. On `GUT_EVENT_RESIZE`, call `gut_vt_resize()` and set the pseudo
   terminal's window size.
5. Shift+PageUp, Shift+PageDown and the wheel move the view with
   `gut_vt_scroll_view()`; any key, text or paste sent to the child
   first returns the view to the live screen and clears the selection.
6. Mouse events go through `gut_vt_mouse()` first. What comes back as
   the host's drives the selection; a finished drag sets the primary
   selection and Ctrl+Shift+C copies to the clipboard. Focus events go
   through it too.
7. The OSC 52 callback stores what the child copies into the system
   clipboard or primary selection; no `get` callback is given, so the
   child cannot read them.

## 13. Threads, blocking and other event sources

guterm is single threaded and not thread safe. All calls on a window come
from the thread that opened it, which on macOS must be the main thread.
The buffer and VT layer have no global state and may be used from any
thread, one at a time.

`gut_poll()` blocks in SDL's event wait, which has no way to watch a file
descriptor or socket. A program with another input source has three
options:

- Wake: watch the descriptor on another thread and call `gut_wake()`
  when it is readable. `gut_poll()` returns `GUT_EVENT_WAKE` and the main
  thread reads the data it was woken for. The main loop keeps blocking
  with no idle wakeups and no added latency. This is what the shell
  example does: its watcher thread polls the pty, wakes the main loop,
  then waits on a pipe for the main loop to say it has drained the pty
  before watching again, so the two never read the same data.
- Alternate: poll the window with timeout 0, then the descriptor with a
  short timeout. Latency is bounded by the short timeout and the cost is
  a wakeup per period while idle. No thread is needed.
- Timeout: use `gut_poll()` with the time until the next thing the
  program wants to do, for animations and timers.

`gut_present()` waits for vertical sync, so a program that presents on
every event will be rate limited to the display's refresh when events
arrive faster than that. Coalescing, by draining pending events with a
zero timeout before presenting, keeps it responsive under a flood of
input. The examples all do this: handle everything that is queued, then
present once. `gut_poll` helps with the commonest flood by merging a run
of consecutive mouse motion events into the last one, so a program sees
the current pointer position rather than every step of its path; an
event of another type between two motions keeps its place.

## 14. Portability notes

**Linux.** The native path. SDL creates an EGL context with OpenGL ES
2.0, which exists on every Mesa driver and on Raspberry Pi 3 and later.
Both X11 and Wayland work.

**Windows.** The ES 2.0 request succeeds where the driver exposes the
WGL ES profile extension, otherwise the fallback gives a desktop
compatibility context, which every driver provides. No ANGLE is needed.
GL entry points are declared with the `__stdcall` convention the platform
requires. Shipping `SDL3.dll` beside the executable is the only runtime
requirement.

**macOS.** There is no ES driver, so the fallback gives the legacy 2.1
context, which accepts the GLSL 1.10 shaders. High density displays are
handled: cells are whole framebuffer pixels and the window is sized in
points accordingly. The SDL event loop must run on the main thread.

**Raspberry Pi.** Use the KMS or X11 driver with the VC4 or V3D Mesa
stack. The renderer's one program, one buffer and one texture are well
within what the hardware does comfortably. Scale 1 is sensible on a small
display.

**Terminal programs.** A program that handles its own ANSI output and
expects xterm keys needs no changes to its protocol handling: feed its
output to the VT layer, send it the encoder's bytes, and give it an
`xterm-256color` terminfo entry.

## 15. Reference tables

### Default palette

The 16 ANSI entries are the xterm defaults: black, red, green, yellow,
blue, magenta, cyan, white, then their bright variants. Entries 16 to 231
form the cube with levels 0, 95, 135, 175, 215 and 255, index `16 + 36r +
6g + b`. Entries 232 to 255 are grays from 8 to 238 in steps of 10.

### Key codes

Printable keys report their codepoint. The rest:

| Code | Key |
| --- | --- |
| `GUT_KEY_UP`, `GUT_KEY_DOWN`, `GUT_KEY_LEFT`, `GUT_KEY_RIGHT` | Cursor keys |
| `GUT_KEY_HOME`, `GUT_KEY_END`, `GUT_KEY_PAGEUP`, `GUT_KEY_PAGEDOWN` | Navigation |
| `GUT_KEY_INSERT`, `GUT_KEY_DELETE`, `GUT_KEY_BACKSPACE` | Editing |
| `GUT_KEY_TAB`, `GUT_KEY_ENTER`, `GUT_KEY_ESCAPE` | Control; keypad Enter reports as Enter |
| `GUT_KEY_F1` to `GUT_KEY_F12` | Function keys |

Keys with no mapping, such as the modifier keys themselves and keypad
digits, produce no key event. Keypad digits still produce text.

### Modifier bits

`GUT_MOD_SHIFT`, `GUT_MOD_CTRL`, `GUT_MOD_ALT` and `GUT_MOD_SUPER`, where
Super is the Windows key or Command key.

### Encoder output for special keys

| Key | Plain | With modifiers (m = 1 + shift 1 + alt 2 + ctrl 4) |
| --- | --- | --- |
| Up, Down, Right, Left | `ESC [ A` to `ESC [ D`, or `ESC O x` in application mode | `ESC [ 1 ; m x` |
| Home, End | `ESC [ H`, `ESC [ F` | `ESC [ 1 ; m H` |
| Insert, Delete, Page Up, Page Down | `ESC [ 2 ~`, `3 ~`, `5 ~`, `6 ~` | `ESC [ n ; m ~` |
| F1 to F4 | `ESC O P` to `ESC O S` | `ESC [ 1 ; m P` |
| F5 to F12 | `ESC [ 15 ~`, `17 ~`, `18 ~`, `19 ~`, `20 ~`, `21 ~`, `23 ~`, `24 ~` | `ESC [ n ; m ~` |
| Enter, Tab, Escape | CR, HT, ESC | Alt prefixes ESC; Shift+Tab is `ESC [ Z` |
| Backspace | DEL, or BS with `GUT_ENC_BS_BS` | Alt prefixes ESC |

### VT mode flags

| Flag | Set by |
| --- | --- |
| `GUT_VT_MODE_AUTOWRAP` | DECSET 7, on by default |
| `GUT_VT_MODE_ORIGIN` | DECSET 6 |
| `GUT_VT_MODE_INSERT` | SM 4 |
| `GUT_VT_MODE_ALTSCREEN` | DECSET 47, 1047, 1049 |
| `GUT_VT_MODE_BRACKETPASTE` | DECSET 2004 |
| `GUT_VT_MODE_APP_CURSOR` | DECSET 1 |
| `GUT_VT_MODE_APP_KEYPAD` | ESC = |
| `GUT_VT_MODE_MOUSE` | DECSET 1000, 1002, 1003; the specific mode is in `vt->mouse_mode` |
| `GUT_VT_MODE_MOUSE_SGR` | DECSET 1006 |
| `GUT_VT_MODE_FOCUS` | DECSET 1004 |
| `GUT_VT_MODE_SIXEL_DISPLAY` | DECSET 80, DECSDM |

### Functions by layer

Buffer, no dependencies: `gut_color_default`, `gut_color_indexed`,
`gut_color_rgb`, `gut_color_equal`, `gut_cell_erase`, `gut_buf_init`,
`gut_buf_free`, `gut_buf_resize`, `gut_buf_cell`, `gut_buf_clear`,
`gut_buf_clear_rows`, `gut_buf_put`, `gut_buf_text`, `gut_buf_fill`,
`gut_buf_scroll`, `gut_buf_dirty_all`, `gut_buf_copy_text`,
`gut_buf_place_image`, `gut_buf_set_image_budget`, `gut_buf_images`,
`gut_buf_cut_images`, `gut_buf_drop_images`, `gut_buf_swap_images`,
`gut_image_list_free`, `gut_image_free`,
`gut_utf8_decode`, `gut_utf8_encode`, `gut_rune_width`,
`gut_font_default`, `gut_font_lookup`, `gut_encode_event`,
`gut_sel_clear`, `gut_sel_begin`, `gut_sel_extend`, `gut_sel_mouse`,
`gut_sel_contains`, `gut_sel_text`.

Window, needs SDL3: `gut_open`, `gut_close`, `gut_error`, `gut_present`,
`gut_poll`, `gut_wake`, `gut_grid_size`, `gut_set_grid_size`, `gut_set_title`,
`gut_set_defaults`, `gut_set_palette`, `gut_clipboard_get`,
`gut_clipboard_set`, `gut_primary_get`, `gut_primary_set`,
`gut_set_text_input`, `gut_set_compose_overlay`, `gut_set_selection`,
`gut_pad_get`, `gut_pad_rumble`, `gut_key_held`, `gut_mouse_held`,
`gut_mods_held`, `gut_keys_held`, `gut_ticks`.

VT, no dependencies: `gut_vt_init`, `gut_vt_free`, `gut_vt_reset`,
`gut_vt_feed`, `gut_vt_resize`, `gut_vt_modes`, `gut_vt_encode_flags`,
`gut_vt_mouse`,
`gut_vt_set_scrollback`, `gut_vt_scrollback_lines`, `gut_vt_set_image_limit`,
`gut_vt_set_cell_size`,
`gut_sixel_begin`, `gut_sixel_put`, `gut_sixel_end`, `gut_sixel_abort`,
`gut_vt_clear_scrollback`, `gut_vt_set_view`, `gut_vt_scroll_view`,
`gut_vt_view_offset`, `gut_vt_set_reply`, `gut_vt_set_title_cb`,
`gut_vt_set_bell_cb`, `gut_vt_set_clipboard_cb`.
