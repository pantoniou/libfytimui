# libfytimui documentation

## Decisions

The five choices that shaped this API. Read `0001` and `0003` first; they
constrain everything else.

| | Decision | Why it matters |
|---|---|---|
| [0001](decisions/0001-vendor-timui-behind-an-opaque-api.md) | Vendor the timui core behind a fully opaque API | No `timui` symbol in the public surface, enforced at link time. The cost: the host cannot draw custom chrome. |
| [0002](decisions/0002-cmake-with-shared-and-static.md) | CMake only, shared **and** static | Diverges from upstream's Makefile/nix/single-header model. |
| [0003](decisions/0003-alt-screen-with-in-app-scrollback.md) | Alt-screen with in-app scrollback | Supersedes the inline live-region design in the fyai findings doc. Means **no new render mode in the core**. |
| [0004](decisions/0004-host-owns-the-event-loop.md) | The host owns `poll()`; timui never blocks | The one change that required touching the core. |
| [0005](decisions/0005-parse-styling-once-into-retained-cells.md) | Parse SGR once per append into retained cells | Keeps an immediate-mode renderer viable for a thousand-line transcript. |

## Vendored core

[`vendor-deltas.md`](vendor-deltas.md) — every change to `core/`, with the
upstream commit it came from, and the re-vendoring procedure.

**The rule:** core changes land upstream in `timui.h` first, then get
re-vendored. A core edit that exists only in this tree is a defect — it is
silently lost on the next sync.

## Handoff

| Date | Report |
|---|---|
| 2026-07-21 | [Bootstrap — CMake library over a vendored timui core](handoff/2026-07-21-fyai-integration.md) |

Start there for landing state, what is verified versus merely declared, and the
next safe move.

## External context

- `~/work/fyai/ASYNC_MAIN_LOOP_FINDINGS.md` — the originating analysis of fyai's
  event-loop migration and parallel tool calls. Its rendering-manager and
  inline-viewport sections are **partly superseded**; see decision 0003.
- `~/work/libfymd4c` — the precedent this project's structure follows, and the
  producer of the SGR-styled Markdown this library consumes.
- `~/work/timui.h` — upstream core. Branch `fyai-async` holds the changes
  vendored here.
