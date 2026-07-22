---
type: Decision
title: Parse SGR styling once per append into retained cells
date: 2026-07-21
status: Accepted
---

## Context

libfymd4c renders Markdown to text carrying SGR styling escapes only. timui
consumes text/style primitives and has no ANSI parser — feeding it that output
directly would print escapes literally and lose styling.

timui is immediate-mode: every frame redraws everything. A coding transcript is
thousands of lines, so re-parsing SGR and re-laying-out Markdown per frame would
dominate the event loop — exactly the failure the findings doc warns about.

The parser was placed here rather than adding a styled-run sink to libfymd4c, so
libfymd4c stays untouched and the parser is reusable by any consumer.

## Decision

`fytim_pane_append` parses styling **once, on append**, into a retained cell
block cached on the pane. Repaints blit cached cells. Only the streaming tail
re-renders.

The parser (`src/fytim_sgr.h`) is allocation-free, survives escape sequences
split across arbitrary feed boundaries, and **rejects** cursor, erase, and
screen-mode sequences via `disallowed_seen` — positioning belongs exclusively to
the compositor.

## Consequences

- SGR parsing is a per-message cost, not a per-frame one. Any change moving it
  into the frame path is a performance regression.
- Indexed colours are retained as an index with `FYTIM_COLOR_INDEXED` rather
  than resolved to RGB, so the backend maps them against the active palette.
- Content carrying terminal control sequences is rejected rather than rendered,
  which is a safety property as well as a correctness one.
