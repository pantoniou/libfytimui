---
type: Decision
title: A page of slots, inline and full screen, beside the band stack
date: 2026-09-13
status: Accepted
supersedes: 0003-alt-screen-with-in-app-scrollback.md
---

## Context

Decision 0003 chose an alternate screen with in-app scrollback. The library
did not implement it. It draws an inline band on the normal screen: committed
rows go to the scrollback of the terminal, and `draw_band()` places a fixed
stack under them - the tail, the work bands, a pane, the header, the prompt
between two rules, and two status rows. `fytim_layout_compute_ex()` sheds the
rows of that stack in an order fixed in C.

The host (fyai) now needs arrangements that a fixed stack cannot state: chrome
it writes as Markdown, controls it places, tiles that are pages of their own,
and a full-screen view whose transcript scrolls and reflows. libfymd4c renders
UI Markdown with `fy-slot` placeholders, a page height, and a region list,
which is what a host needs to state such a screen.

## Decision

Add a **page** beside the band stack. The band stack stays, unchanged, and is
what a host that sets no page gets.

- **The host renders; the library composes.** The host renders its page with
  libfymd4c and gives the library the rows and the regions
  (`fytim_page_set()`). The library does not link libfymd4c.
- **A slot names a component.** The library draws the rows of the page, then
  draws each slot region with the component bound to its id: the built-in
  `tail`, `prompt` and `completion`, or a band, a surface or a pane that the
  host bound with `fytim_*_bind()`. An unbound slot is blank.
- **A slot region is a grant.** A surface in a slot is granted the rows and
  the columns of the region, as a tile is.
- **An act is an event.** A click on an act region is `FYTIM_EVENT_ACT` with
  the id. The library does nothing else with it.
- **Two screen modes.** Inline: the page is the live region under the
  scrollback, and its height is its rows. Full screen: the page is the
  alternate screen, its height is the terminal, and the host draws the
  transcript into a slot. The full-screen mode is a later change; the page
  API does not depend on the mode.
- **Retained or immediate is measured.** The cost of a page render per frame
  against a page kept until its source or size changes is measured before a
  host commits to one. The result is recorded in a later decision.

## Consequences

- `draw_band()` and the layout solver keep their behaviour and their tests.
- The page path is a second compositor. A test that compares the cells of the
  two, for the same content, keeps them equal where the page reproduces the
  band stack.
- In inline mode the rules of the tail still hold: a page that shrinks while
  the tail streams keeps its rows until commits cover them.
- Decision 0003 is superseded. Scrollback in full-screen mode is the host's,
  drawn into a slot; selection and copy there use OSC 52.
