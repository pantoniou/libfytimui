---
type: Decision
title: Render the page again for each frame
date: 2026-09-13
status: Accepted
---

## Context

Decision 0006 left open how much of a page a host renders again for each
frame. Three candidates were considered:

- **immediate**: render the root page and every tile head for each frame;
- **retained page**: keep the rendered rows of each part and render a part
  again only when its source or its size changes;
- **retained cells**: keep the cells of each part in the library.

`tests/bench_fytim_page.c` measures the first two against the band stack. Each
frame, every tile gets a row of output, the tail changes, and the prompt holds
a line. The pseudo-terminal has the size under test and is read by a thread,
so a pump never waits on it. The rule set before the measurement: choose
immediate if its 99th percentile frame time at 200x60 with 8 tiles is below
4 ms.

## Measurement

600 frames, release build, libfymd4c without a palette. Times in ms. "render"
is the host's libfymd4c work and `fytim_page_set()` or the chrome setters;
"pump" is the compose, the cell diff and the write.

| size | tiles | mode | render p99 | pump p99 | frame p99 |
|---|---:|---|---:|---:|---:|
| 80x24 | 8 | stack | 0.012 | 0.079 | 0.086 |
| 80x24 | 8 | immediate | 0.083 | 0.081 | 0.160 |
| 80x24 | 8 | retained | 0.021 | 0.085 | 0.096 |
| 200x60 | 8 | stack | 0.018 | 0.221 | 0.230 |
| 200x60 | 8 | immediate | 0.076 | 0.243 | 0.327 |
| 200x60 | 8 | retained | 0.027 | 0.231 | 0.245 |
| 200x60 | 16 | immediate | 0.138 | 0.316 | 0.433 |
| 400x120 | 0 | stack | 0.018 | 0.079 | 0.079 |
| 400x120 | 0 | immediate | 0.041 | 0.351 | 0.386 |
| 400x120 | 16 | stack | 0.024 | 0.571 | 0.580 |
| 400x120 | 16 | immediate | 0.160 | 0.812 | 0.974 |
| 400x120 | 16 | retained | 0.033 | 0.808 | 0.816 |

The full table is the output of `bench_fytim_page`.

## Decision

Render the page, and every tile head, again for each frame. At 200x60 with 8
tiles the 99th percentile frame is 0.33 ms, a twelfth of the limit; the
largest case, 400x120 with 16 tiles, is below 1 ms.

Do not retain rendered pages or cells for this. The Markdown render is at most
0.16 ms of a frame. The pump - compose, diff and write - is the larger part,
and neither retained candidate changes it. A cache would add invalidation for
a fifth of the frame.

## Consequences

- A host builds its page source from its state for each frame that changes
  something, and needs no invalidation keys.
- The page pump costs more than the band stack when the page is taller than
  the stack would be: at 400x120 with no tiles the page takes all 120 rows and
  the stack takes its chrome. The cost follows the rows drawn, not the page.
- Measure again when the page holds a grid of tile pages (`fy-grid`) or a
  palette, and when a full-screen transcript slot draws a viewport for each
  frame. Those add work that this measurement does not include.
