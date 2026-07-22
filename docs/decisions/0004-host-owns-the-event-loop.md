---
type: Decision
title: The host owns poll(); timui never blocks
date: 2026-07-21
status: Accepted
---

## Context

fyai's event loop already owns curl sockets, tool pipes, child exits, and
timers. `timui_begin` performed its own blocking wait before draining input —
a 16 ms `poll` on a tty (`src/timui_core.c:541`) or a `nanosleep` throttle on a
non-tty fd. That wait is redundant once the host has already polled, and it
caps how fast the host services its other sources. `timui_post` also did not
wake a blocked poll, so worker results waited up to a frame.

Alternatives rejected: inverting control so timui owns the loop (a large fyai
refactor), and a wake-fd bridging two loops (keeps a nested `run_until`
boundary the findings doc explicitly warns against).

## Decision

fyai's loop owns waiting. `TIMUI_FLAG_EXTERNAL_POLL` suppresses both internal
waits; `fytim_poll_fd` and `fytim_poll_timeout_ms` let the host add this library
to its own poll set; `fytim_pump` never blocks.

This required a **core** change — no wrapper can un-block a wait inside
`timui_begin`. It was made upstream first (`6bf01f2`) and re-vendored, per
`docs/vendor-deltas.md`.

## Consequences

- One loop per invocation, as the findings doc's invariants require.
- Default timui behaviour is unchanged and pinned by a negative-control test.
- The host **must** call `fytim_pump` on the timeout as well as on readability,
  or escape-sequence timeouts and animation stall.
- Correctness is asserted with an input-wait counter, never wall-clock timing.
