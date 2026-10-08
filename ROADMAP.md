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

## Sixel

Planned in five phases, each landing with its tests before the next
starts. About 1,200 to 1,400 lines, four to five sessions. No shader
change: the fragment shader multiplies the vertex color by a texture
sample, so an RGBA image drawn with a white vertex color renders as is.

Decisions:

- Placements live in the buffer layer: image id, RGBA pixels, pixel
  size, anchor row and column, rows and columns spanned. Scroll shifts
  anchors and drops placements that leave the screen; clear rows, IL, DL
  and ED cut or drop them. The alternate screen parks and restores its
  own list. A host that draws its own buffer may ignore the list at no
  cost: nothing in the cell API changes.
- A resize drops every placement. This is documented behavior, not a
  gap to fill later.
- Text draws over images. Writing a character onto an image cell leaves
  the image behind it; erase sequences cut it. The xterm behavior of
  punching a hole per overwritten cell is deferred.
- Memory limits are settable on `gut_desc`: a per image pixel cap and a
  total pixel budget per buffer. When a new image would exceed the
  budget the least recently placed images are discarded until it fits.
  An image larger than the per image cap, including the one being
  decoded, is discarded and the decoder disables itself until ST, so a
  hostile stream can never allocate without bound.
- Images that scroll off the top are dropped. Scrollback images are not
  in the first version, since they need pixels per stored line and a
  composing view.

Phases (1 is done):

1. Decoder in the VT layer, streaming: DCS parameter parsing, the sixel
   state machine, palette, repeat, raster attributes, transparent
   background, the limits above. Headless unit tests on known tiny
   images; torture feeds random sixel bytes.
2. Buffer placements and their updates in scroll, clear rows, resize,
   copy and free, with the LRU budget. Tests.
3. VT integration: a cell size setter so image height converts to rows,
   placement at the cursor, cursor advance and DECSDM (mode 80), DA1
   advertising sixel, XTSMGRAPHICS geometry query, ED 2 and RIS dropping
   images.
4. Renderer: present flushes in batches (backgrounds, one textured quad
   per placement, glyphs), a texture cache by image id freed with the
   placement, cleanup in `gut_close`. Images scale by the window zoom.
   Checked by screenshot under Xvfb.
5. Docs and examples: manual section, README status, a sixel in the
   vtdemo script, the shell example passing the cell size.

## Unplanned, possible

- Scrollback search, or a host API to read scrollback lines rather than
  only view them.
- Answering OSC 52 queries in the shell example, behind a setting.

## Process

- A release tag and version bump process. The header and manual say
  0.1.0 and nothing updates them.
