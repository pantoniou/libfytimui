---
type: Decision
title: Vendor the timui core behind a fully opaque API
date: 2026-07-21
status: Accepted
---

## Context

fyai needs a terminal UI capable of multiple simultaneously-updating panes.
`timui.h` is a capable v0.2.0 immediate-mode TUI, but it is pre-1.0 with a
~200-function surface covering widgets, tabs, focus, themes, images, and an
immediate-mode frame model that fyai has no use for.

## Decision

Follow the libfymd4c precedent: vendor the core into `core/` and expose a small
`libfyaml`-style API over it. **No `timui` type, macro, or symbol appears in the
public surface.** Public types are opaque (`struct fytim`, `struct fytim_pane`)
or defined here.

Enforced in three layers, not by convention:

1. Public headers never include `timui.h`.
2. The core is compiled `-fvisibility=hidden` as a single TU.
3. `-Wl,--exclude-libs,ALL` keeps `timui_*` out of the shared object's dynamic
   symbol table.

## Consequences

- fyai is insulated from a pre-1.0 API. Core churn is absorbed here.
- The exported surface is 12 symbols against timui's ~200.
- fyai **cannot draw custom chrome**. Anything it needs on screen must become
  explicit API here. This is the real cost and it is deliberate.
- Verified by `nm -D --defined-only ... | grep timui_` returning nothing;
  treat a regression as a build break.
