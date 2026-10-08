# guTerm : Graphical micro Terminal

A single header C file to provide a graphical window for text-based apps.

`guterm.h` draws a grid of character cells with a bitmap font and returns
keyboard, mouse and game controller input as events. A program can fill
the grid directly, or feed a stream of terminal escape sequences through
the optional VT layer and let that fill the grid.

## Architecture

- SDL3 provides the window, the GL context and input.
- Rendering uses the OpenGL ES 2.0 subset: one program, one vertex
  buffer, one RGBA glyph atlas. The shaders are GLSL ES 1.00, which also
  compiles as GLSL 1.10 on desktop compatibility contexts, so the same
  source runs on GLES 2 and 3, on desktop GL, and through ANGLE when a
  platform needs it. ANGLE is not required and is not used on Linux.
- GL entry points are resolved at run time through SDL, so no GL header
  or library is needed to build.
- The built-in font is an 8x16 subset of unscii (public domain), drawn
  at an integer zoom factor. A caller can supply its own bitmap font.

The header has three layers:

| Layer | Depends on | Purpose |
| --- | --- | --- |
| `gut_buf` | libc | The cell grid: codepoint, colors, attributes, cursor. |
| `gut_window` | SDL3 | Opens a window, draws a `gut_buf`, delivers `gut_event`s. |
| `gut_vt` | libc | Optional VT100/xterm emulator that writes into a `gut_buf`. |

`gut_encode_event()` turns input events into the byte sequences an xterm
sends, for programs that already speak the terminal protocol, including
bracketed paste, mouse reports and focus reports. Pastes arrive as
events from the platform chords and middle click, IME composition is
shown over the cursor and delivered as events, `gut_sel` tracks a mouse
driven selection that the window highlights, `gut_vt_mouse()` decides
whether the program or the host gets a mouse event, and up to four game
controllers report as events.

## Documentation

`guterm.md` is the programming manual: concepts, every function, the
event model, clipboard and input method handling, the VT layer and
portability notes.

## Usage

In exactly one C file:

```c
#define GUTERM_IMPLEMENTATION
#include "guterm.h"
```

Every other file includes the header plainly. Link with SDL3.

```c
struct gut_desc desc = { .title = "hello", .cols = 80, .rows = 25 };
gut_window *w = gut_open(&desc);
struct gut_buf buf;
struct gut_event ev;

gut_buf_init(&buf, 25, 80);
gut_buf_text(&buf, 0, 0, "Hello", gut_color_indexed(10),
             gut_color_default(), GUT_ATTR_BOLD);
gut_present(w, &buf);
while (gut_poll(w, &ev, -1) && ev.type != GUT_EVENT_QUIT) {
    if (ev.type == GUT_EVENT_RESIZE)
        gut_buf_resize(&buf, ev.rows, ev.cols);
    gut_present(w, &buf);
}
gut_buf_free(&buf);
gut_close(w);
```

With the VT layer:

```c
struct gut_vt vt;

gut_vt_init(&vt, &buf);
gut_vt_feed(&vt, "\033[1;31mred\033[0m\r\n", 17);
gut_present(w, &buf);
```

Configuration macros, set before the implementation include:

- `GUTERM_NO_WINDOW` leaves out SDL and the renderer. The buffer, font
  tables, key encoder and VT layer remain with no dependency beyond libc.
- `GUTERM_NO_VT` leaves out the VT layer.

## Examples and tests

The Makefile only builds the examples and tests. End users drop
`guterm.h` into their own project and compile it their own way.

```
make          # builds _out/demo, _out/vtdemo, _out/term and the tests
make test     # unit tests and a short torture run, no display needed
make torture  # longer torture run, TORTURE_ITER=n TORTURE_SEED=n
make asan     # tests under address sanitizer
make ubsan    # tests under undefined behavior sanitizer
make cov      # tests with gcov, line report in _out/cov/guterm.h.gcov
```

- `examples/demo.c` exercises the cell API: palettes, attributes, glyph
  coverage, cursor shapes and input echo.
- `examples/vtdemo.c` feeds a built-in escape sequence script, or a file
  given on the command line, through the VT layer. Typed keys are
  encoded and looped back into the emulator.
- `examples/term.c` runs `$SHELL` on a pseudo terminal inside the window
  with scrollback on Shift+PageUp and the wheel.
  POSIX only.
- `tests/test_vt.c` checks the buffer, UTF-8, font map, key encoder and
  VT layer, built with `GUTERM_NO_WINDOW`.
- `tests/torture.c` feeds random byte streams, escape sequences and
  UTF-8 fragments through the VT layer with random resizes, and calls
  the buffer API and key encoder with random arguments, checking the
  structural invariants after every step. Deterministic per seed.

SDL3 is found with `pkg-config`.

The GitHub Actions workflow in `.github/workflows/ci.yml` runs the tests
with gcc and clang, the long torture run, both sanitizers, coverage, the
examples against an SDL3 built from source with the demos started under
Xvfb, a macOS build with Homebrew SDL3, and a mingw cross build with the
unit tests run under wine.

## Target Platforms

- Linux x86_64, aarch64 (Raspberry Pi 3+)
- Windows 10+
- macOS arm64 25.0+

Only Linux has been exercised so far. The context setup asks for GLES 2.0
first and falls back to the platform default context, which is what
Windows and macOS are expected to take.

## Status

Not yet done:

- OSC 52, the kitty keyboard protocol and XTWINOPS in the VT layer.
- Blink is accepted but drawn as normal text.
- Combining characters are dropped; wide characters use two cells but
  the built-in font has no CJK glyphs, so they draw as `?`.
- Windows and macOS builds are untested.
