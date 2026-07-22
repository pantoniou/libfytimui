---
type: Decision
title: CMake as the sole build system, producing both shared and static
date: 2026-07-21
status: Accepted
---

## Context

`timui.h` upstream is a single-header library built through a self-documenting
Makefile inside a nix dev shell. fyai builds with CMake, and the single-header
distribution model is not wanted here.

## Decision

CMake only. No Makefile, no nix shell — deliberately diverging from upstream.
Both flavours are produced from one source list, as libfymd4c does:

- `fytimui` — shared when `BUILD_SHARED_LIBS` (default on non-Windows)
- `fytimui_static` — always a static archive
- `libfytimui::libfytimui` / `libfytimui::libfytimui_static` aliases

The archive is named `libfytimui.a` beside `libfytimui.so` on ELF.

## Consequences

- Consumers pick linkage without reconfiguring this project.
- Upstream's amalgamation tooling is irrelevant here and is not vendored.
- The core's relative-include layout (`core/include/`, `core/src/`,
  `core/tools/vendor/`) is load-bearing and must not be flattened: the core is
  one TU built from `core/src/timui.c`.
