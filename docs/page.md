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

## The rule between columns

A pane that places its tiles in columns draws the rule between two columns for
each row of its grid, after it places the tiles. A tile that spans both columns
covers the rule on its rows, and the tiles in the rows under it keep the rule
between them.

## Keys are bound in modes

A mode is a named table of keys and actions. A key that the table does not
bind is looked up in the parent of the mode. The library has three modes, and
each has the keys the library had before they were data:

- `prompt` is selected at the start. It binds Escape, Ctrl-C, Ctrl-D, Ctrl-L,
  Ctrl-G, Ctrl-T, Ctrl-Tab, Ctrl-Shift-T, Tab, Ctrl-P, Ctrl-N, Up, Down,
  PageUp and PageDown.
- `completion` is selected while the popup is open. Its parent is `prompt`,
  so a key that the popup does not bind is the prompt's.
- `surface` is selected while a surface holds the keys. The surface gets every
  key but the ones this mode binds: Ctrl-Tab and Ctrl-Shift-T.

An action is a built-in action of the library, with a name that starts with
`fytim.` (`fytim_action_name()` lists them), or a name of the host. A key whose
action is a name of the host is `FYTIM_EVENT_KEY` with that action, and
neither the editor nor the other keys of the library see it. An empty action
unbinds a key and so hides the binding of a parent. A host makes a mode of its
own with `fytim_mode_bind()`, gives it a parent with `fytim_mode_set_parent()`
and selects it with `fytim_set_mode()`.

A call that binds keys is refused whole when a name is not a key, an action is
not known, a key is bound twice, the table is too big, or the `prompt` mode
would lose every key that leaves the program. Ctrl-I, Ctrl-M and Ctrl-[ are
Tab, Enter and Escape, because a terminal sends them as the same code.
The codes 0x1c to 0x1f, which a terminal without the kitty keyboard protocol
sends for Ctrl-\, Ctrl-], Ctrl-^ and Ctrl-_, are those chords.

A key is found by one hash of its sequence for each mode of the chain, and
not by a scan of the bindings.

### Chords and double taps

A binding is one key or a chord of up to four keys named in order, such as
`Ctrl-x Ctrl-e`. The keys of a chord are taken, and `FYTIM_EVENT_CHORD` gives
the keys so far, or no text when the chord has ended. A key that the chord does
not continue ends it and is then looked up alone. A chord also ends when
`fytim_set_chord_timeout()` milliseconds pass with no key. The pump ends it
even when no key comes, and `fytim_poll_timeout_ms()` counts the time that is
left, so a host that polls with it needs no timer of its own. The clock is the
monotonic clock, or the one that `fytim_set_clock()` gives, which a test moves
itself.

A double tap is a chord of one key twice, such as `Escape Escape`. A key that
is a binding and begins a longer one runs its action at once and keeps the
chord open. A single press thus has no delay, and a second press inside the
time runs the action of the chord. The `surface` mode binds single keys only,
because a program that holds the keys cannot lose the first key of a chord.

A terminal without the kitty keyboard protocol cannot send some keys:
`fytim_key_available()` says so for a name, with the capabilities known now.

The core takes the key before the frame does, through a filter that is a local
delta of the core: see `vendor-deltas.md`.
