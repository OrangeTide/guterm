# guterm roadmap

Open work, in rough priority order within each section. An item leaves
this file when it is done or decided against; the git history keeps the
record. The README status list is the short public summary of the same
things.

## Next

- Run gvedit on a real high density display. Auto scale (`gut_desc.scale`
  of 0) has only exercised the 1x branch, since Xvfb reports a content
  scale of 1.0.
- Cross compile gvedit for Windows and run it under wine, as the guterm
  examples are. Add it to vedit's CI once it builds.
- Decide what a window close means to an embedded editor. gvedit reads it
  as end of input and exits 1, which is consistent but not a quit request.

## Verification gaps

- Windows and macOS on real hardware. Wine covers the Windows code path
  (GL loader, context fallback, SDL input) but not real drivers, the
  Windows IME, controllers through the Windows backend, or clipboard
  interop with other programs.
- Game controllers and IME composition are implemented from the SDL3
  documentation and have not run against a real controller or input
  method.

## VT layer

- Kitty keyboard protocol.
- XTWINOPS.
- Mouse report modes 1005 (UTF-8) and 1015 (urxvt). Only 1000, 1002,
  1003 and 1006 are recognised.
- Scrollback reflow on a width change. Stored lines are clipped or
  padded today.
- Combining characters are dropped.
- Blink is accepted but drawn as normal text.

## Window and renderer

- Fonts beyond the one 8x16 bitmap: a loader for other bitmap fonts and
  sizes, then a vector or TrueType path. This bounds glyph coverage (no
  CJK, so wide characters draw as `?`), the shapes examples can draw, and
  what the zoom means.
- Use the dirty rows the buffer already tracks. The renderer rebuilds
  every quad each present, which is fine at terminal sizes but wasteful.
- An opt out for mouse motion coalescing in `gut_poll`, for programs
  that want the full pointer path.
- Physical key positions for the key state API. `gut_key_held` works by
  the symbol on the key, so WASD moves on QWERTY and other letters
  elsewhere; a scancode view would let a game bind by position.
- Selection does not follow content when the VT view scrolls or output
  moves it; hosts clear it instead.

## Unplanned, possible

- Pictures in the scrollback. A line that scrolls off the top drops its
  band of a picture; keeping it needs pixels per stored line and a view
  that composes them.
- Erasing a picture cell by cell as xterm does. Text draws over a
  picture and only whole row erases and colored backgrounds remove it.
- Scrollback search, or a host API to read scrollback lines rather than
  only view them.
- Answering OSC 52 queries in the shell example, behind a setting.

## Process

- A release tag and version bump process. The header and manual say
  0.1.0 and nothing updates them.
