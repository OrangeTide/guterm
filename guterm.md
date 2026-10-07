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
11. The VT layer
12. Threads, blocking and other event sources
13. Portability notes
14. Reference tables

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
`?`. Glyphs are drawn at an integer zoom, so an 8 by 16 font at scale 2
gives 16 by 32 pixel cells.

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
    int scale;                      /* integer pixel zoom, default 2 */
    const struct gut_font *font;    /* NULL for the built-in 8x16 */
    uint32_t fg, bg;                /* 0xRRGGBB defaults; 0 means unset */
    const uint32_t *palette;        /* 16 ANSI colors 0xRRGGBB or NULL */
    int fixed_size;                 /* 1 disables window resizing */
    int no_paste_keys;              /* 1 delivers paste chords as keys */
    int no_compose_overlay;         /* 1 leaves IME preedit drawing to
                                       the program */
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
```

`gut_poll` waits for the next event and returns 1 with `ev` filled, or 0
when `timeout_ms` passed with nothing to report. A negative timeout waits
forever and 0 returns at once. Events SDL delivers that guterm has no use
for are consumed silently.

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
    int button;         /* enum gut_button */
    int dx, dy;         /* wheel notches */
    int cols, rows;     /* GUT_EVENT_RESIZE */
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
| `GUT_EVENT_MOUSE_DOWN`, `GUT_EVENT_MOUSE_UP` | `col`, `row`, `x`, `y`, `button` | A button changed. |
| `GUT_EVENT_MOUSE_MOVE` | `col`, `row`, `x`, `y`, `button` | The pointer moved. `button` is the first button held, or 0. |
| `GUT_EVENT_MOUSE_WHEEL` | `col`, `row`, `x`, `y`, `dx`, `dy` | The wheel turned. Positive `dy` is away from the user. |
| `GUT_EVENT_FOCUS_IN`, `GUT_EVENT_FOCUS_OUT` | | Keyboard focus changed. |

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
| Anything else | Nothing. |

Flags:

| Flag | Effect |
| --- | --- |
| `GUT_ENC_APP_CURSOR` | Cursor keys send SS3 sequences, the DECCKM application mode. |
| `GUT_ENC_BS_BS` | Backspace sends BS (0x08) instead of DEL (0x7F). |
| `GUT_ENC_BRACKET_PASTE` | Bracketed paste, as above. |

The return value and `out` follow snprintf sizing. Sixteen bytes cover
every key; a paste needs `ev->len + 16`. When using the VT layer,
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

## 11. The VT layer

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
extended. Use it instead of `gut_buf_resize` while the VT layer is
attached.

### What is interpreted

Printing: UTF-8 with U+FFFD for malformed input, East Asian wide
characters taking two cells, DEC special graphics when the G0 or G1 set
selects line drawing, auto wrap with the deferred wrap at the last column
that programs expect, insert mode.

Controls: BEL, BS, HT, LF, VT, FF, CR, SO, SI.

Cursor: CUU, CUD, CUF, CUB, CNL, CPL, CHA, HPA, CUP, HVP, VPA, CBT, save
and restore with both DECSC/DECRC and SCOSC/SCORC, IND, RI, NEL.

Erasing and editing: ED including the xterm scrollback variant, EL, ECH,
ICH, DCH, IL, DL, SU, SD.

Attributes: SGR with bold, dim, italic, underline, blink, reverse,
hidden, strike, the 8 and 16 color sets, 256 colors and 24 bit color.

Modes: DECSTBM scroll region, DECOM origin mode, DECAWM auto wrap, IRM
insert mode, DECTCEM cursor visibility, DECSCUSR cursor shape, DECCKM
application cursor keys, DECKPAM application keypad, bracketed paste,
mouse tracking modes 1000, 1002 and 1003 recorded as a mode flag, and
the alternate screen via 47, 1047 and 1049 with the primary screen
parked and restored.

Tabs: HTS, TBC for one stop and for all.

Reports: DA1 answers as a VT100 with advanced video, DSR 5 and 6 answer
status and cursor position. RIS performs a full reset.

OSC 0 and 2 set the title. Other OSC, all DCS, APC, PM and SOS strings,
and unknown CSI and ESC sequences are consumed and ignored, so a program
that emits sequences the emulator does not know still displays sanely.

Not implemented: scrollback, mouse report generation, OSC 52 clipboard,
kitty keyboard protocol, XTWINOPS, character set designations other than
ASCII and line drawing, and combining characters, which are dropped.

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

### Modes and the encoder

`gut_vt_modes()` returns the current mode flags, `GUT_VT_MODE_*`, for a
host that wants to know for instance whether the program has enabled
mouse tracking. `gut_vt_encode_flags()` maps the modes that affect key
encoding, application cursor keys and bracketed paste, onto
`gut_encode_event()` flags.

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

## 12. Threads, blocking and other event sources

guterm is single threaded and not thread safe. All calls on a window come
from the thread that opened it, which on macOS must be the main thread.
The buffer and VT layer have no global state and may be used from any
thread, one at a time.

`gut_poll()` blocks in SDL's event wait, which has no way to watch a file
descriptor or socket. A program with another input source has three
options:

- Alternate: poll the window with timeout 0, then the descriptor with a
  short timeout. This is what the shell example does. Latency is bounded
  by the short timeout and the cost is a wakeup per period while idle.
- Push: read the descriptor on another thread and call
  `SDL_PushEvent()` with a user event to wake `gut_poll()`. guterm
  ignores user events, so the main thread then handles the data it was
  woken for. This keeps the main loop blocking with no idle wakeups.
- Timeout: use `gut_poll()` with the time until the next thing the
  program wants to do, for animations and timers.

`gut_present()` waits for vertical sync, so a program that presents on
every event will be rate limited to the display's refresh when events
arrive faster than that. Coalescing, by draining pending events with a
zero timeout before presenting, keeps it responsive under a flood of
input.

## 13. Portability notes

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

## 14. Reference tables

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

### Functions by layer

Buffer, no dependencies: `gut_color_default`, `gut_color_indexed`,
`gut_color_rgb`, `gut_color_equal`, `gut_cell_erase`, `gut_buf_init`,
`gut_buf_free`, `gut_buf_resize`, `gut_buf_cell`, `gut_buf_clear`,
`gut_buf_clear_rows`, `gut_buf_put`, `gut_buf_text`, `gut_buf_fill`,
`gut_buf_scroll`, `gut_buf_dirty_all`, `gut_buf_copy_text`,
`gut_utf8_decode`, `gut_utf8_encode`, `gut_rune_width`,
`gut_font_default`, `gut_font_lookup`, `gut_encode_event`.

Window, needs SDL3: `gut_open`, `gut_close`, `gut_error`, `gut_present`,
`gut_poll`, `gut_grid_size`, `gut_set_grid_size`, `gut_set_title`,
`gut_set_defaults`, `gut_set_palette`, `gut_clipboard_get`,
`gut_clipboard_set`, `gut_primary_get`, `gut_primary_set`,
`gut_set_text_input`, `gut_set_compose_overlay`, `gut_ticks`.

VT, no dependencies: `gut_vt_init`, `gut_vt_free`, `gut_vt_reset`,
`gut_vt_feed`, `gut_vt_resize`, `gut_vt_modes`, `gut_vt_encode_flags`,
`gut_vt_set_reply`, `gut_vt_set_title_cb`, `gut_vt_set_bell_cb`.
