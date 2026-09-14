# The page API

`include/libfytimui/libfytimui-page.h` and the headers it names state the
contract of each call. This file states how the parts fit together for a host.
Decisions 0006, 0007 and 0008 record why they exist.

## Tile pages

A tile of a pane can have a page of its own, `fytim_surface_set_page()`: rows
that the host rendered at the columns the tile was granted, with at most one
slot named `screen` where the grid of the surface is drawn. The rows above the
slot are the head of the tile, and the rows below it are its foot. They take
the place of the `set_top` and `set_bottom` chrome. The library draws no zoom
or close mark on such a tile: the host places its controls as acts, and a click
on one is `FYTIM_EVENT_ACT` with the surface and the cell of the page.

- The screen slot takes the rows that the tile has left. A tile too short for
  its head and its foot sheds the last rows of each first.
- A view, `fytim_surface_set_page_view()`, chooses what is drawn: the whole
  page, the screen alone, or the head alone. It changes nothing that is asked
  or granted, so a host that chooses the view from the grant does not move the
  grant.
- A committed tile keeps its page: the transcript holds the head, the screen
  and the foot, in every view.
- In an explicit grid, a tile page reserves the rows of its page, as the
  automatic grid does.
- A page can place the tiles of a pane itself. A tile bound to a slot is drawn
  there as its pane draws it, and is granted the slot. A tile that no slot
  places is granted nothing. Tiles in slots of one row reserve their tallest
  head and foot, so their screens have one height. `fytim_surface_rows()` and
  `fytim_workband_rows()` give the rows that each tile asks for, for a host
  that sizes a row of its own.

## Cells a host draws

A host that composes a screen of its own draws with the rules that the library
uses, so the two do not drift apart.

- `fytim_cells_draw_text()` parses rendered rows into a grid of cells with the
  parser of the content: the rows are cut to a box, the style carries from row
  to row, and wide and combining characters take their cells.
- `fytim_cells_ground()` puts chrome on the ground of a tile.
  `fytim_cells_wash()` puts the cells that a program drew on that ground, and
  mixes a coloured cell toward it only when `fytim_truecolor()` says that the
  terminal takes 24-bit colour.
- `fytim_surface_row()`, `fytim_surface_cursor()`, `fytim_surface_margin()`
  and `fytim_surface_bg()` read back a surface. `fytim_workband_content()`,
  `fytim_workband_top()`, `fytim_workband_bottom()` and
  `fytim_workband_max_rows()` read back a band. The host then draws them as
  the library would. The texts and the cells belong to the component and stay
  valid until it changes them.
