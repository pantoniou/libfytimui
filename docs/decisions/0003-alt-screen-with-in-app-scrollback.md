---
type: Decision
title: Alt-screen with in-app scrollback, not an inline live region
date: 2026-07-21
status: Accepted
supersedes: ASYNC_MAIN_LOOP_FINDINGS.md "Progressive rendering API"
---

## Context

Two models were considered for a coding-harness UI.

**Inline live region** — transcript in the terminal's real scrollback, only a
few rows at the bottom repainted, output committed by printing. This is the
`backtrack_rows`/`freeze_rows` design in the fyai findings doc, and what
libfymd4c already does. timui cannot do it: `src/timui_render.c` positions every
cell with absolute `emit_cup(x, y)` into a full-screen buffer and diffs it.

**Alt-screen task view** — timui as-is; the transcript becomes a scrollable pane.

The initial recommendation favoured inline, on the grounds that alt-screen loses
terminal-native selection, copy, and scrollback.

## Decision

Alt-screen, with scrollback implemented in-app.

That argument was withdrawn. timui has OSC 52 clipboard and SGR mouse support,
so selection and copy move in-app rather than being lost. And native scrollback
under a live region flickers and jitters badly — Claude Code added an alt-screen
rendering mode specifically to fix that. Alt-screen is where the design
converges, not a retreat from it.

## Consequences

- **No new render mode is needed in the timui core.** `timui_scroll_begin` +
  clipping, mouse wheel, and OSC 52 already cover it. This keeps core changes
  minimal, which was a hard requirement.
- The findings doc's inline-viewport/freeze API is not implemented.
- Scrolling, selection, and copy are now this library's responsibility.
- Non-interactive output must not use this path at all; an append-only stream
  backend stays separate, with no screen takeover.
