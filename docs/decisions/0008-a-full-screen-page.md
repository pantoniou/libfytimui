---
type: Decision
title: A page on the alternate screen, with text the host lets the user select
date: 2026-09-13
status: Accepted
---

## Context

Decision 0006 states two screen modes for a page and leaves the full-screen
mode for a later change. In that mode the page is the whole alternate screen,
the host draws its transcript into a slot, and there is no scrollback of the
terminal: scrolling, selection and copy become the host's and this library's.

Selection has two readings. The library could select cells of the screen and
copy what they show, or it could let the host say which region holds text and
copy what the host says that text is. The cells of a rendered transcript are
not its text: a wrapped paragraph is several rows, a table is drawn with
borders, and a code block carries a gutter. Only the host has the source.

## Decision

- **A screen mode in the configuration.** `fytim_cfg.screen` is
  `FYTIM_SCREEN_INLINE`, the default and the behaviour of every earlier
  release, or `FYTIM_SCREEN_ALT`. The alternate screen takes the terminal:
  the frame is as tall as the terminal, the mouse is grabbed with drag
  tracking, and the screen is left on destroy, on suspend and on a fatal
  signal, as the core already does for its alternate screen.
- **No commits on the alternate screen.** There is no scrollback to commit
  to. `fytim_commit()` fails with `FYTIM_ERR_UNSUPPORTED`.
- **Text regions.** A page region of kind `FYTIM_PAGE_TEXT` is a slot whose
  rows the user may select. A press in it starts a selection, a drag extends
  it within the region, and a release reports `FYTIM_EVENT_SELECT` with the
  id of the region and the first and last cell, counted from the region. A
  press and release in one cell is a click, not a selection. The library
  draws the selection in reverse video, in reading order between the two
  cells, until the next press, `fytim_selection_clear()`, or a page that no
  longer has the region.
- **The host copies.** `fytim_copy()` puts the text it is given on the
  clipboard of the terminal with OSC 52. It needs `fytim_cfg.clipboard`: a
  host that did not ask for it cannot write to the clipboard. The library
  never copies what cells show.

## Consequences

- The inline mode and the band stack keep their behaviour and their tests.
- A host in full-screen mode draws its transcript into a slot and answers
  `FYTIM_EVENT_SCROLLBACK`; the selection is cleared when the host scrolls,
  because its cells then show other text.
- Selection across regions, and selection of a tile or of chrome, are not
  offered. A later decision can add them if a host needs them.
- The screen escapes are written only to a terminal. Tests of them need a
  pseudo-terminal; tests of the page and of events use pipes.
