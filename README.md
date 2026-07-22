# libfytimui

A minimal, opaque pane-oriented terminal UI for coding-harness applications,
built over a vendored [`timui.h`](https://github.com/pantoniou/timui.h) core.

> **Status: early.** The build, the vendored core, the SGR parser, and the
> lifecycle/pane model work and are tested. **Rendering is not implemented yet** —
> see [Current state](#current-state) before depending on this.

## What it is

Follows the libfymd4c precedent: take a capable core, vendor it, and put a
`libfyaml`-style API on top rather than exposing it.

```text
host (fyai) --> libfytimui public API (fytim_*) --> vendored timui core
```

The host does not draw. It publishes panes and their content, drains events,
and drives the loop. Layout, cursor management, terminal capabilities,
scrolling, and repainting belong to the library. **No `timui` type or symbol
appears in the public API** — the core is compiled with hidden visibility and
absorbed into the library, so `timui_*` is not reachable at compile time or
link time.

The exported surface is 12 symbols, against timui's ~200.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

There is no Makefile and no nix shell — a deliberate divergence from upstream
timui.h. Both a shared library and a static archive are produced:

| Target | Alias | Output |
|---|---|---|
| `fytimui` | `libfytimui::libfytimui` | `libfytimui.so` |
| `fytimui_static` | `libfytimui::libfytimui_static` | `libfytimui.a` |

Options: `BUILD_SHARED_LIBS`, `BUILD_FYTIMUI_TESTS`, `BUILD_TIMUI_CORE_TESTS`,
`ENABLE_ASAN`.

## Usage sketch

The host's existing event loop stays in charge; this library never blocks.

```c
struct fytim_cfg cfg;
fytim_cfg_default(&cfg);
cfg.title = "fyai";

struct fytim *ft = fytim_create(&cfg);

/* add to the host's poll set alongside curl sockets, tool pipes, timers */
int    fd      = fytim_poll_fd(ft);
int    timeout = fytim_poll_timeout_ms(ft);

/* one pane per concurrent tool or agent */
struct fytim_pane *p = fytim_pane_open(ft, "bash: run tests");
fytim_pane_append(p, out, len);            /* SGR styling parsed once, retained */
fytim_pane_set_state(p, FYTIM_PANE_DONE);

/* when poll reports readable, or the timeout expires */
fytim_pump(ft);                             /* never blocks */

struct fytim_event ev;
while(fytim_next_event(ft, &ev)) { /* LINE, INTERRUPT, QUIT, RESIZE... */ }
```

Content passed to `fytim_pane_append` may carry **SGR styling escapes only**
(as produced by libfymd4c). Cursor, erase, and screen-mode controls are
rejected — positioning belongs to the compositor.

## Current state

Implemented and tested:

- CMake build, shared + static, install rules
- Vendored core with verified symbol isolation
- SGR parser — allocation-free, handles escapes split across feeds, rejects
  disallowed controls, clean under ASAN/UBSAN
- `fytim_create`/`destroy`, `fytim_cfg_default`, `fytim_poll_fd`/
  `poll_timeout_ms`, `fytim_transcript`, `fytim_pane_open`/`close`/
  `set_title`/`set_state`, `fytim_result_string`, `fytim_version_string`

Declared in the headers but **not yet implemented** — nothing renders:

- `fytim_pump`
- `fytim_pane_append` / `replace` / `clear`
- `fytim_next_event`
- `fytim_set_prompt` / `set_input` / `set_status`

## Tests

396 CTest cases: 385 vendored timui core tests (carried over so a re-vendor is
validated here) plus 11 SGR parser tests. Each is registered individually by
querying the built runner's `--list`.

```sh
ctest --test-dir build -R fytim.        # this library's tests
ctest --test-dir build -R timui.core.   # the vendored core's tests
```

## Documentation

See [`docs/`](docs/) — decisions, vendored-core deltas, and handoff notes.
Working agreements are in [`CLAUDE.md`](CLAUDE.md).

## Licence

Apache-2.0, as is the vendored timui core. The core's `stb_image` dependency is
MIT/public domain.
