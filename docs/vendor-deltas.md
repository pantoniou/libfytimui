# Vendored core deltas

`core/` is a vendored copy of the `timui.h` repo, and we still re-vendor from
it periodically. Core changes land **here** first; this file is what survives
a sync, so every local change must be listed. Upstreaming is optional and
shrinks the reconcile, but does not gate landing anything.

## Upstreamed

Already in `timui.h`, so a re-vendor carries them for free.

| Upstream commit | Change | Why it cannot be a wrapper |
|---|---|---|
| `6bf01f2` | `TIMUI_FLAG_EXTERNAL_POLL` + `timui_poll_fd`/`timui_poll_timeout_ms` | `timui_begin` performs a blocking wait internally (16ms tty poll, or a nanosleep throttle on a non-tty fd). No layer above it can un-block that. |
| `ecb759c` | Named test table; `--list` and single-case invocation | CTest must register each case individually; only the linked binary knows which cases are compiled in. |

## Local only

Not upstream. **A re-vendor overwrites these — reapply them.**

| Change | Detail |
|---|---|
| `timui_core.c`: discard `write` into an `ssize_t` | `(void)write(...)` does not satisfy gcc's `warn_unused_result`. Two sites: the debug logger and `timui_signal_write_`. |
| `timui_core.c`: one-per-line `if` guards in `timui_pad` | Four `if`s on one line tripped `-Wmisleading-indentation`. No behaviour change. |
| `CMakeLists.txt`: `-Wno-unused-function` on `fytimui-core` | Not a core edit, but paired with one. `stb_image.h` declares STBIDEF entry points it never defines; the diagnostic fires at end-of-TU, so the existing pragma push/pop around the `#include` in `timui_images.c` cannot reach it. |
| `TIMUI_FLAG_MOUSE_DRAG` (`timui.h`, `timui_term.c`) | Adds `?1002` button-event tracking so motion is reported while a button is held. Plain `TIMUI_FLAG_MOUSE` emits `?1000` only, which reports press/release but never motion, so a drag cannot be followed and in-app selection is impossible. |
| `timui_transport()` (`timui.h`, `timui_core.c`) | Exposes the transport a `Timui` already owns. `timui_clipboard_set` (OSC 52) takes a `TimuiTransport *` and there was no way to obtain one from a `Timui *`, so clipboard copy was unreachable. |
| `timui_mouse_state()` (`timui.h`, `timui_core.c`) | Exposes pointer cell + held-button state. `timui_mouse_clicked` reports only the press edge, and `timui_begin` drains every mouse event into its aggregators so `timui_poll_event` sees none — leaving no way to follow a drag. |
| `timui_textarea.c`: scroll back on viewport growth | `regression/textarea-scroll-on-grow`: `scroll_y` chased the cursor down but never returned when the rect grew, hiding the first lines forever. Test in `tests/test_local_core.c`. Worth upstreaming. |
| `timui_textarea.c`: fill the widget rect | The text area painted only glyph cells; empty cells kept the screen background, so the input never read as a box. Now fills the rect in the widget style first, like `input_field`. Test in `tests/test_local_core.c`. Worth upstreaming. |
| readline chords in the text area (`timui.h`, `timui_core.c`, `timui_textarea.c`) | Adds `TIMUI_KEYIN_WORD_LEFT/WORD_RIGHT/KILL_WORD_FWD/TRANSPOSE` (Alt-b/f, Alt-d, Ctrl-T; Alt chords were unmapped in `timui_begin`) and gives the text area the kill ops (Ctrl-K/U/W) it lacked -- they existed only in `input_field`. Ctrl-K at end of line joins lines. Tests in `tests/test_local_core.c`. Worth upstreaming. |
| `timui_slot_style()` (`timui.h`, `timui_core.c`) | Exposes the slot style from the theme the ui resolved at open, so application chrome can match widget styling without re-deriving the theme from the config. Test in `tests/test_local_core.c`. |
| `timui_key_codepoint()` (`timui.h`, `timui_core.c`) | Reports the frame's key-event codepoint. Control chords the key table cannot name arrive as `KEY_UNKNOWN + MOD_CTRL` with the letter only in the codepoint, which never reached the application -- Ctrl-L refresh was undetectable. Test in `tests/test_local_core.c`. |
| `TIMUI_FLAG_INLINE` + `cfg.inline_rows` + `timui_inline_paint/commit/commit_emit` (`timui.h`, `timui_core.c`, `timui_render.c`, `timui_term.c`) | Inline band mode: manage N rows at the cursor anchor on the normal screen instead of the alt screen; finished lines scroll into native scrollback via commit, so scrolling/selection/copy stay with the terminal. Cursor-anchor contract documented in `timui.h`. Commits queue and flush inside the next `timui_end` (erase + lines + repaint, one sync bracket); an unchanged band emits nothing; the style is closed before every row break (BCE terminals fill scroll-revealed lines with the current background), and a content-only change (a keystroke) takes `timui_inline_paint_diff`, which rewrites only the changed cell span of each touched row in place (relative moves, no EL) without erasing the band; the parked cursor is hidden across a repaint. The hardware cursor of a focused input is parked at its cell with relative moves and un-parked back to the anchor before any later band output (including the exit erase). Note: adds a field to `TimuiConfig` and `TIMUI_CONFIG_INIT` — a re-vendor must re-add both. Tests in `tests/test_inline.c`. |

| Overwrite-in-place inline painting + trust protocol (`timui_render.c`, `timui_core.c`, `timui_int.h`: `inline_trusted`, `inline_prev_rows`) | `timui_inline_paint` and `timui_inline_commit_emit` overwrite rows in place (EL per row, style closed) instead of erase-down + repaint, which flashed on every streaming commit. `timui_end` erases down once when the screen is untrusted (first paint, `timui_full_redraw`, resume). A shrink COMPACTS the band upward with a targeted erase after the repaint, sharing one synchronized frame so the chrome moves in a single atomic hop; mid-stream shrinks never occur because the library holds the frame height while the transcript streams. Tests in `tests/test_inline.c` (`test_inline_trust_protocol`, `test_inline_shrink_cleans_below`). |
| `timui_suspend` / `timui_resume` + `ui->suspended` (`timui.h`, `timui_core.c`, `timui_int.h`) | Release the terminal to a child process (external editor) and take it back: suspend closes the style, erases the band, un-parks the cursor and restores termios + input-fd flags; frames in between neither read input (the fd may be blocking again) nor write output; resume re-enters raw + screen modes and forces a full redraw. Tests in `tests/test_inline.c` (`test_inline_suspend_resume`). |

| `TIMUI_COLOR_ANSI` indexed colours (`timui.h`, `timui_render.c`: `emit_indexed`) | `TIMUI_COLOR_ANSI \| n` in a style colour selects palette entry n: 0..15 emit the classic 30-37/90-97 (40-47/100-107) codes and 16..255 emit `38;5;n`, so the terminal's own theme palette applies instead of a hard-coded RGB approximation. Used by the SGR run-to-cell conversion for indexed input colours. Test `fytim.vt.regression_indexed_colors_mapped`. |
| Buffered fd transport, one flush per frame (`timui_int.h`: `TimuiFdCtx.obuf`, `timui_core.c`: `fd_write`/`fd_flush`) | A frame emitted as dozens of small `write()`s renders partially on terminals without DEC 2026 (VTE): the chrome visibly shifts between the commit scroll and the repaint. Output is buffered (32 KiB, flush-then-spill for oversized writes, order preserved) and flushed once per frame in `timui_end`, plus at open/suspend/resume/close so nothing lingers. Note `timui_write_all_` returns bytes written, not 0. Test `fytim.band.large_commit_spills_intact` counts exact row occurrences across the boundary. |

Tests for these live in `tests/test_mouse_drag.c` and `tests/test_inline.c`,
deliberately outside the
upstream test files (which a re-vendor overwrites). They are registered in
`tests/test.h` and `tests/test_main.c`, both of which **are** upstream files —
those two registrations must be reapplied after a sync.

Not a delta, but adjacent: the core samples the terminal size once in
`timui_open` and has no `SIGWINCH` handling. Resize reflow is therefore the
application's job; `fytim_pump` (src/libfytimui.c) runs the
`timui_term_size` → `timui_ui_resize` → `timui_full_redraw` sequence.

## Re-vendoring

```sh
cp -r ../timui.h/src/.            core/src/
cp    ../timui.h/include/timui.h  core/include/
cp -r ../timui.h/tools/vendor/*   core/tools/vendor/
cp -r ../timui.h/tests/.          tests/
cp    ../timui.h/examples/*.h     examples/
```

Then `ctest --test-dir build` — the vendored core tests are carried over
precisely so a sync is validated here.

## `TIMUI_TERMIOS_INTR_SIGNAL` / `TIMUI_FLAG_INTR_SIGNAL`

**Files:** `core/include/timui.h`, `core/src/timui_term.c`,
`core/src/timui_core.c`

`timui_termios_enter()` cleared `ISIG` unconditionally, so `^C` was always
delivered as a byte. A host that drives its own loop then has no way to be
interrupted when that loop is wedged — reading the `^C` requires the very loop
that is stuck, which is exactly the failure the key exists for.

Added `timui_termios_enter_flags()` with `TIMUI_TERMIOS_INTR_SIGNAL`, which
keeps `ISIG` set and disables `VQUIT`/`VSUSP` so precisely one key becomes a
signal and `^\` / `^Z` stay application keys. `timui_termios_enter()` is now a
wrapper passing zero, so existing callers and the ABI are unchanged. The core
selects it from the new `TIMUI_FLAG_INTR_SIGNAL` config flag at both
`timui_termios_enter` call sites (open, and the resume path).

Covered by `timui.core.test_termios_intr_signal`. Worth upstreaming: the
wedged-loop problem is general to any host-driven poll loop.

## `fd_read()` polls before reading

**File:** `core/src/timui_core.c`

`fd_read()` read the input fd directly, relying on the `O_NONBLOCK` that
`timui_open()` sets once. That flag lives on the *open file description*, which
`fork()` shares — so a child, or any grandchild it execs, that clears it on an
inherited terminal makes the parent's read block. The no-wait guarantee of
`TIMUI_FLAG_EXTERNAL_POLL` was therefore defeasible by an unrelated process.

Observed in fyai: a forked tool child's descendants cleared it on the shared
tty, and the host then blocked inside `read(0, …)` in a repaint reached from a
timer callback — the loop wedged with no way left to interrupt it.

`fd_read()` now does a zero-timeout `poll()` first and returns "nothing
pending" unless the fd is actually readable. One extra syscall per read, and
the guarantee stops depending on the rest of the process tree.

Covered by `timui.core.test_external_poll_survives_blocking_fd`, which clears
`O_NONBLOCK` and runs the frame under `alarm()` in a child, so a regression
fails instead of hanging. Worth upstreaming: any host embedding timui in its
own loop is exposed.

## A combining mark stays on the cell it modifies

**Files:** `core/include/timui.h`, `core/src/timui_render.c`

`timui_draw_text_linked()` decoded each code point and drew it only when
`timui_utf8_width()` reported a width above zero. A combining mark has no width
of its own, so every mark was dropped: text that arrives decomposed - `e` plus
U+0301 rather than `é` - lost its accent, silently, on every path that draws
text.

A grapheme is one cell, thus a mark belongs to the base character and not to a
cell of its own. `TimuiCell` now carries `combining[TIMUI_CELL_COMBINING_MAX]`,
a zero-width code point is attached to the glyph before it, the frame diff
compares the marks, and each of the three emit paths writes them after the base
character.

Observed with `fytim.surface.vt.combining_stays_one_cell`, which reads the
grid back out of libvterm: the mark was missing from the cell before the fix.
Worth upstreaming: the defect is in the general text path, not in anything this
library added.

## A frame keeps its input in the order it arrived

**Files:** `core/include/timui.h`, `core/src/timui_int.h`, `core/src/timui_core.c`

`timui_begin` drained the frame's events into aggregates: one `key_pressed`
field, which keeps the LAST key of the frame, and a text buffer collected apart
from it. That answers "was this key pressed", which is what a widget asks, and
it cannot answer "what was typed, in order". Two consequences, both visible to
a host that forwards input somewhere else:

- a chord and the key after it arrive as two facts with no order, so a host
  that reserves a key for itself and reads the key after it gets them the wrong
  way round - `^\ q` typed in one burst sent `q` to the program and kept the
  `^\`; and
- two presses of one key inside a frame became one.

`TimuiInputRecord input_log[256]` now records each key and typed character as
the drain sees it, pasted text included, and
`timui_input_log_count()` / `timui_input_log_at()` read it back. The aggregates
are untouched, so no widget behaviour changes. The log is bounded; input past
the bound is dropped rather than reordered.

Covered by `fytim.surface.keys.keys_keep_their_order` and
`fytim.surface.keys.repeated_keys_all_arrive`, which type into a surface
through the public interface. Worth upstreaming: any host embedding timui and
forwarding input - a terminal pane, a remote session - has the same need.
