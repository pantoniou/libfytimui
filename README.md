# libfytimui

A minimal, opaque pane-oriented terminal UI for coding-harness applications,
built over a vendored [`timui.h`](https://github.com/pantoniou/timui.h) core.

> **Status: early.** The inline transcript, progressive tail, work bands,
> prompt editor, and host-driven event integration are implemented and tested.

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

/* stable assistant rows commit; its progressive remainder stays in the tail */
fytim_commit(ft, frozen, frozen_len);
fytim_tail_apply(ft, update.backtrack, update.content,
                 update.content_len, update.freeze);

/* tools and other concurrent work use independent bounded bands */
struct fytim_workband *wb = fytim_workband_create(ft);
fytim_workband_set(wb, rendered, rendered_len);
fytim_workband_commit(wb);

/* when poll reports readable, or the timeout expires */
fytim_pump(ft);                             /* never blocks */

struct fytim_event ev;
while(fytim_next_event(ft, &ev)) { /* LINE, INTERRUPT, QUIT, RESIZE... */ }
```

Rendered content passed to the transcript, tail, or work-band APIs may carry
**SGR styling escapes only**
(as produced by libfymd4c). Cursor, erase, and screen-mode controls are
rejected — positioning belongs to the compositor.

## Current state

Implemented and tested: shared/static builds and package exports, hidden vendored
core symbols, non-blocking host-driven pumping, native-scrollback transcript
commits, progressive Markdown tails, independent work bands, SGR/OSC-8 parsing,
resize and input events, multiline editing, history, completion, and external
editor suspend/resume. `examples/agent_md.c` is the integration reference.

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
