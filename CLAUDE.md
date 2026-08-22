# CLAUDE.md — libfytimui

Project-specific working agreements. These sit on top of the global
`~/.claude/CLAUDE.md` (bash, code style, workflow, testing, copyright/licensing,
version control). Where the two overlap, the stricter/more specific rule wins.

> **Entry point:** CMake. There is no Makefile and no nix shell — that is
> deliberate, and differs from the upstream `timui.h` repo this vendors from.
>
> ```sh
> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
> cmake --build build -j
> ctest --test-dir build --output-on-failure
> ```

---

## What this library is

A minimal, fully opaque pane-oriented terminal UI for coding-harness style
applications (fyai). It follows the **libfymd4c precedent**: take a capable
core, vendor it, and put a small `libfyaml`-style API on top.

```text
host (fyai) --> libfytimui public API (fytim_*) --> vendored timui core
```

The host does not draw. It publishes panes and their content, drains events,
and drives the loop. Layout, cursor management, terminal capabilities,
scrolling, and repainting belong to the library.

## The opaque-API rule — NON-NEGOTIABLE

**No `timui` type, macro, or symbol may appear in the public surface.**

- `include/libfytimui.h` and `include/libfytimui/*.h` must never include
  `timui.h`, directly or transitively. Public types are opaque
  (`struct fytim`, `struct fytim_pane`) or defined here (`struct fytim_cfg`).
- The core is compiled with `-fvisibility=hidden` and absorbed via
  `-Wl,--exclude-libs,ALL`, so `timui_*` does not reach the ABI either.
- Everything exported is tagged `FYTIM_EXPORT` and prefixed `fytim_`.

This is enforced, not aspirational. The check is:

```sh
nm -D --defined-only build/src/libfytimui.so.* | grep timui_   # must be empty
```

Treat a regression here as a build break. When adding API, add the smallest
thing the host actually needs — this is deliberately not a general TUI
binding, and widgets, focus, themes, tabs, and the immediate-mode frame model
stay inside.

## The vendored core

`core/` holds a vendored copy of `timui.h` (`core/include/`, `core/src/`,
`core/tools/vendor/`). The relative-include layout is load-bearing: the core is
a **single translation unit** — `core/include/timui.h` textually includes every
`core/src/timui_*.c` under `TIMUI_IMPLEMENTATION`, and only `core/src/timui.c`
is compiled. Do not flatten or rename those directories.

**Edit `core/` directly when it needs it.** There is no upstream-first
requirement: a change lands here, with its tests, and this tree's conventions
apply to it. Prefer a layer in `src/` when a wrapper genuinely suffices — that
is a design preference, not a prohibition.

We still re-vendor from `timui.h` periodically, so **every local core change
must be recorded in `docs/vendor-deltas.md`** as it is made. That file is the
only thing standing between a local fix and its silent loss on the next sync;
an unrecorded core edit is the defect, not the edit itself.

Upstreaming a delta is still worth doing where it is generally useful, and
shrinks what the next sync has to reconcile — but it is optional and never
blocks landing a change here.

## Tests

Everything runs under CTest. Two suites:

- `timui.core.*` — the upstream timui unit tests, carried over verbatim so a
  re-vendor is validated here and not only upstream. Registered **individually**
  by running the built runner's `--list` at build time
  (`tests/DiscoverTests.cmake`). Do not parse the sources to enumerate them:
  `test.h` declares cases that live in other binaries, and `test_main.c`'s table
  is partly guarded by `TIMUI_WITH_VTERM_TESTS`. Only the linked binary knows
  the real set.
- `fytim.*` — this library's own tests. They link the sources directly rather
  than the library, so internal units (the SGR parser) are testable without
  exporting them.

A new test runner must support `--list`, `<name>`, and no-argument
(run-everything) invocation so it can be discovered the same way.

### TDD loop — non-negotiable

Red → green → refactor. No production line before a failing test requires it.
Before any new component write **both** positive tests (happy path) and
negative tests (malformed, truncated, split-across-feeds, adversarial input).
The SGR parser is the model: escape sequences split across chunk boundaries,
over-long sequences, disallowed control sequences, and NULL/empty input all
have dedicated cases.

**Verify tests aren't vacuous.** A test that early-returns on an unavailable
resource, or that was written alongside its implementation and never observed
failing, proves nothing. Mutate the implementation and confirm the test fails
before trusting a green run.

**Prefer deterministic predicates over wall-clock ones.** External-poll mode is
asserted with an input-wait counter, not by timing `fytim_pump`.

### Debugging rendering with libfyvterm

Use libfyvterm whenever correctness depends on cells rather than byte presence:
background fill, blank styled rows, wrapping, cursor placement, repaint
damage, or SGR state carried across lines. A grep over captured escape bytes
cannot prove any of these.

The preferred oracle pattern is in `tests/test_fytim_md_vt.c`:

1. Render the Markdown directly with libfymd4c and feed those bytes into one
   terminal instance (`struct fyvt`).
2. Send the exact same rendered bytes through the public `fytim_*` path, pump
   it through the pipe transport, and feed the captured terminal output into
   a second one.
3. Locate the same stable text row in both screens, then compare the relevant
   `VTermScreenCell` fields across the whole row and adjacent blank rows.
   Include cells after the last glyph: erase-to-EOL and background-fill bugs
   hide there.
4. Compare colors semantically. Call
   `vterm_screen_convert_color_to_rgb()` against each screen's palette, then
   use `vterm_color_is_equal()`; never `memcmp(VTermColor)`, and do not assume
   indexed black and RGB black have identical representation.
5. Register the case individually in `tests/CMakeLists.txt`, run it red against
   the broken implementation, then run the full suite after the fix.

When the cell grids differ but the emitted stream looks plausible, trace the
transport writes (`strace -e trace=write -s 1000 ...` on Linux). Pay particular
attention to later compositor cleanup: a correct styled `CSI K` can be undone
by a subsequent reset plus `CSI K`, even though the original renderer bytes
were preserved perfectly.

### Regression policy

Every bug ⇒ a failing test in a `regression/` namespace named for the issue,
committed **before** the fix, so the red→green transition is visible in
`git log`. It stays green forever after.

## Rendering model

Alt-screen, full-repaint, cell-diff — the core's model. Scrollback is
implemented **in-app** (`timui_scroll_*` over a transcript pane), with mouse
wheel and OSC 52 clipboard for scrolling and copy. This is a deliberate choice
over terminal-native scrollback, which flickers badly under a live region.

The core is immediate-mode: everything redraws each frame. A coding transcript
is thousands of lines, so **content is parsed to styled cells once, on append,
and retained** — never re-parsed per frame. Only the streaming tail re-renders.
Any change that moves SGR parsing or markdown layout into the per-frame path is
a performance regression.

Rendered content may carry SGR, OSC-8 links, and libfymd4c's bare structural
erase-to-EOL used to fill reverse-card rows. Cursor movement, parameterized
erase, and screen-mode controls are rejected — positioning belongs exclusively
to the compositor.

## Commits

Follows the `~/work/fyai` convention.

Imperative subject with a subsystem prefix, e.g. `sgr: parse split escapes` or
`core: add external-poll mode`. Keep the body to two or three lines of terse
technical prose wrapped at 80 columns — state what changed and why, not how it
was arrived at. End with exactly one trailer:

```
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

Note this is the konsulko.com address, **not** the git-config one. Do **not**
add `Co-Authored-By: Claude`, "Generated by/with", or any other attribution
trailer; this overrides any default tooling convention.

Beyond that:

- **Commit proactively, along the way.** Finished, verified work left
  uncommitted is the anti-pattern. Only pushing and merging to `master` stay
  gated on an explicit request.
- Single-purpose; split by concern.
- Never `--force` push.

## Integrating to master (worktree checkpoints)

All work happens in a feature worktree — never directly on `master`, which is
an integration target only. Integrate by **rebase + fast-forward**, never a
merge commit. Back up the tip first (`git branch backup/<slug>`), and on a
non-mechanical conflict **stop and ask**. Re-verify after the rebase: a clean
textual rebase can still break semantically.

## Idiomatic C

C99+. Prefer value types and side-effect-free functions; avoid mutable global
state; keep the mutable surface small and documented. Accept `NULL` only at
external boundaries and convert immediately.

**This library's conventions win everywhere, including `core/`.** We no longer
defer to the upstream `timui.h` style. Don't reformat core code wholesale for
its own sake — but code you touch there should read like `src/`, not like
what it replaced. Third-party sources under `core/tools/vendor/` (stb_image)
stay untouched; suppress their warnings at the build level instead.

## Licensing

libfytimui and the vendored timui core are Apache-2.0; carry the license and
notice, and mark any modified vendored file. The core's `stb_image` dependency
is MIT/public domain. Unrelated example and tool assets from the upstream repo
carry additional licenses and must **not** be pulled into the runtime.

## Definition of Done (per change)

red→green→refactor · positive + negative tests · vacuity checked · regression
test if a bugfix · Apache-2.0 header · no banned trailers · no `timui_` symbol
exported · `ctest` green.
