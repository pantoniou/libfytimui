---
type: Report
title: libfytimui bootstrap — CMake library over a vendored timui core
date: 2026-07-21
---

# Landing state

| | |
|---|---|
| Repo | `~/work/libfytimui` (new, this session) |
| Branch | `master` |
| Commit | `4df8b00` — initial libfytimui |
| Push status | not pushed; no remote configured |
| Upstream repo | `~/work/timui.h`, worktree `~/work/timui.h-fyai-async` |
| Upstream branch | `fyai-async` (commits `6bf01f2`, `ecb759c`) |
| Upstream merge status | **not** integrated to `timui.h` master; still a feature branch |
| Verification | `ctest --test-dir build` → **396/396 pass** |

Origin of the work: `~/work/fyai/ASYNC_MAIN_LOOP_FINDINGS.md`, which analysed
fyai's move to an event-loop model with parallel tool calls and identified
`timui.h` as the candidate rendering engine.

# Read first

- `~/work/fyai/ASYNC_MAIN_LOOP_FINDINGS.md` — the originating analysis. Note
  that its §"Local timui.h checkout" conclusions were **partly superseded**
  this session; see Decisions below.
- `CLAUDE.md` — working agreements, including the opaque-API rule.
- `docs/vendor-deltas.md` — every core change, upstream commit first.
- `docs/decisions/` — the five decisions that shaped this API.

# Accepted state (artifact-backed)

These are verified, not inferred:

- **Both libraries build**, per the libfymd4c pattern:
  `build/src/libfytimui.so.0.0.1` and `build/src/libfytimui.a`, two targets
  from one source list with `libfytimui::` aliases.
- **Namespace isolation holds at link time.** Verified with
  `nm -D --defined-only build/src/libfytimui.so.0.0.1`: exactly 12 `fytim_*`
  symbols exported, **zero** `timui_*`. Mechanism: core compiled as a single TU
  with `-fvisibility=hidden`, absorbed via `-Wl,--exclude-libs,ALL`.
- **396 CTest cases pass**: 385 vendored timui core tests + 11 SGR parser
  tests, each registered individually.
- **The SGR parser is complete and hardened.** Allocation-free; handles escape
  sequences split across arbitrary feed boundaries; flags disallowed
  cursor/erase/screen-mode sequences rather than rendering them; clean under
  `-fsanitize=address,undefined`. Its tests were **mutation-checked** (breaking
  `handle_csi`'s final-byte test produces a failure), so the green run is not
  vacuous.
- **The upstream core change is real and minimal.** `TIMUI_FLAG_EXTERNAL_POLL`
  suppresses both internal waits in `timui_begin` (the 16 ms tty poll at
  `src/timui_core.c:541` and the non-tty `nanosleep` throttle). Its test uses an
  input-wait counter, not wall-clock timing, and includes a negative control
  pinning default behaviour. Vacuity was checked with a deliberate probe.

# Blockers / not done — do not claim these work

**The library does not render anything yet.** The following are **declared in
the public headers but have no implementation**, and are absent from the build's
source list in `src/CMakeLists.txt`:

- `fytim_pump` — the non-blocking frame driver
- `fytim_pane_append`, `fytim_pane_replace`, `fytim_pane_clear`
- `fytim_next_event`
- `fytim_set_prompt`, `fytim_set_input`, `fytim_set_status`

Implemented and working: `fytim_create`/`destroy`, `fytim_cfg_default`,
`fytim_poll_fd`/`poll_timeout_ms`, `fytim_transcript`, `fytim_pane_open`/
`close`/`set_title`/`set_state`, `fytim_result_string`, `fytim_version_string`.

Consequently there is **no end-to-end proof** that timui's cell-diff renderer
drives this pane model correctly. That is the next phase's main risk.

Also outstanding:

- The `fyai` side is untouched. No adapter, no call site, nothing removed from
  its existing rendering manager.
- `~/work/timui.h` master has not been fast-forwarded to `fyai-async`.
- No install/export verification: `cmake/libfytimui-config.cmake.in` and a
  `.pc.in` are referenced by the libfymd4c precedent but **not yet written**;
  `install()` rules exist but an installed-consumer build was never tested.
- No `examples/` program for libfytimui itself (the `examples/*.h` present are
  vendored timui test fixtures, not demos of this API).

# Next safe move

Implement `fytim_pane_append` and `fytim_pump` together, TDD, as the smallest
slice that proves the design end to end:

1. Red: a test that appends SGR content to a pane and asserts the retained cell
   block — parsed **once**, not per frame (assert a parse counter, not timing).
2. Green: feed `fytim_sgr_feed` output into a cell block cached on the pane.
3. Then `fytim_pump` rendering panes through `timui_begin`/`timui_end`, verified
   against a fake transport and a cell snapshot — the vendored tests
   (`test_snapshot.c`, `test_render.c`) show the pattern.

Add each new case to CTest via the existing `--list` discovery; a new runner
must support `--list`, `<name>`, and no-argument invocation.

# Verification already run

```sh
# libfytimui
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build                       # 396/396 passed
nm -D --defined-only build/src/libfytimui.so.0.0.1 | grep -c timui_   # 0

# SGR parser under sanitizers (ad hoc, not yet a CTest target)
gcc -Wall -Wextra -fsanitize=address,undefined -Isrc \
    -o /tmp/sgrasan src/fytim_sgr.c tests/test_fytim_sgr.c && /tmp/sgrasan

# upstream core
cd ~/work/timui.h-fyai-async && make test    # all tests passed (385)
```

**Caveat on the upstream runs:** `timui.h`'s CLAUDE.md mandates
`nix develop -c make <target>`. `nix` is not installed on this machine, so
`make` was invoked directly. The user confirmed nix is not a concern. The ASAN
run above is ad hoc and is **not** wired into CTest — worth adding.

# Decisions taken this session

Recorded in `docs/decisions/`. Two were judgement calls made without explicit
instruction and are cheap to revisit:

- **`fytim_` prefix** (by analogy with libfymd4c's `fymd_`).
- **The prompt/input line lives inside libfytimui** (`fytim_set_prompt`,
  `FYTIM_EVENT_LINE`) rather than fyai retaining readline. This is the more
  consequential of the two: the findings doc left it open, and it means
  readline moves in-house. Reversing it later means removing API, not adding.

# Corrections to the originating findings doc

The findings doc's §"Local timui.h checkout" recommended against alt-screen and
worried about losing terminal-native scrollback. That framing was challenged and
**withdrawn** during this session: timui has clipboard (OSC 52) and mouse
support, so selection and copy move in-app rather than being lost, and
terminal-native scrollback under a live region flickers badly enough that
Claude Code itself added an alt-screen mode. Alt-screen with in-app scrollback
is the accepted direction. See `docs/decisions/0003`.

The doc's `backtrack_rows`/`freeze_rows` inline-viewport design (its §"Progressive
rendering API") is therefore **not** being implemented in timui's core.
