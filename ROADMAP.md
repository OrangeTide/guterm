# guterm roadmap

Open work, in rough priority order within each section. An item leaves
this file when it is done or decided against; the git history keeps the
record. The README status list is the short public summary of the same
things.

## Next: gvedit integration and the 0.2.0 release

gvedit vendors `guterm.h` at 0.1.0 and still alternates short polls on
its terminal panel ptys, the pattern `gut_wake` replaced. The release
comes last so a real host proves `gut_wake` and sixel before the tag.
Four phases, each landing with its checks before the next.

Decisions:

- One watcher thread for every panel pty, not one per panel. The main
  loop hands it the descriptor set over a control pipe before each
  wait, the thread polls them and calls `gut_wake` when one is ready,
  then stops watching until it is given the set again, so a level
  triggered descriptor never spins. The main loop finds which ones are
  ready with a zero timeout poll of its own. Windows keeps its pseudo
  console path.
- A window close is a quit request, as File > Exit, with the unsaved
  prompt. gvedit already does this; the earlier roadmap item was
  stale.
- `GUT_VERSION` in the header stays the authority on the version. A
  `make release-check` target verifies the manual, the tag and the
  header agree and the tree is clean. No version file.
- Running on a real high density display needs hardware this plan
  cannot reach. It stays a manual item on the release checklist.

Phases (in the vedit checkout for 1 to 3; 1 is done):

1. `gut_wake` in gvedit. Sync the header, add the watcher thread and
   its control pipe, and make the window wait block in `gut_poll` with
   the editor's timeout. Check: a terminal panel open and idle makes no
   wakeups per frame (count with strace), and output still arrives at
   once. Screenshot under Xvfb as the existing script does.
2. Pictures in the terminal panels. Give the emulator the font's glyph
   size and wire the picture budget to a setting. Check: a sixel from
   img2sixel shows in a panel, scrolls with it and survives a panel
   switch; a screenshot under Xvfb.
3. gvedit for Windows. Cross build with mingw and the SDL3 mingw
   package as guterm's CI job does, run `--version` under wine, and add
   both to vedit's CI next to the existing gvedit job.
4. Release 0.2.0 of guterm. Add `release-check`, set the header and the
   manual to 0.2.0, bring the README status up to date, tag, and run
   `make guterm-sync` in vedit against the tag. The push is yours.

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
- A picture wider than the window's GL texture limit draws as nothing.
  `GUT_SIXEL_MAX_DIM` keeps sixel pictures under 4096 a side, which
  every GLES2 device of the last decade holds; a host placing its own
  larger pictures gets a blank.
- Scrollback search, or a host API to read scrollback lines rather than
  only view them.
- Answering OSC 52 queries in the shell example, behind a setting.

## Process

- A release tag and version bump process: phase 4 above.
