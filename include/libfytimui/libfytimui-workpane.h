/*
 * libfytimui-workpane.h - one region that tiles the screens it holds.
 *
 * A surface opened on its own takes a band of its own, and several of them
 * stand one above the other. That is one screen for each program, and the
 * user reads them as separate things: a parallel run pushes the oldest off
 * the top, and nothing says which screen belongs to which call.
 *
 * A work pane is one band that holds many surfaces and gives each of them a
 * tile of its region:
 *
 *     +----------------+----------------+
 *     | shell          | agent          |
 *     +----------------+----------------+
 *     | agent          | shell          |
 *     +----------------+----------------+
 *
 * The pane owns the grid. A tile learns the size it was given from
 * fytim_surface_granted_rows() and fytim_surface_granted_cols(), which is
 * what the host sizes its pseudo-terminal to, so a program draws what fits
 * in its tile. The host never places a tile.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_WORKPANE_H
#define LIBFYTIMUI_WORKPANE_H

#include <libfytimui/libfytimui-util.h>

#include <stdbool.h>

struct fytim;
struct fytim_surface;
struct fytim_workband;
struct fytim_workpane;   /* opaque; owned by the fytim it was created on */

/*
 * Append a pane below any existing band. NULL on failure. The handle stays
 * valid until fytim_workpane_destroy() or fytim_destroy(). Destroying the
 * pane retires every tile still in it.
 */
struct fytim_workpane *fytim_workpane_create(struct fytim *ft) FYTIM_EXPORT;
void fytim_workpane_destroy(struct fytim_workpane *wp) FYTIM_EXPORT;

/* The tiles the pane holds. A pane with none takes no rows. */
int fytim_workpane_count(const struct fytim_workpane *wp) FYTIM_EXPORT;

/*
 * Chrome rows above and below the whole pane. An empty string draws a
 * full-width rule; NULL removes the row. These frame the live pane only:
 * they are not committed, as a band's are not.
 */
enum fytim_result fytim_workpane_set_top(struct fytim_workpane *wp,
                                         const char *text) FYTIM_EXPORT;
enum fytim_result fytim_workpane_set_bottom(struct fytim_workpane *wp,
                                            const char *text) FYTIM_EXPORT;

/* Rows the pane may occupy, its own chrome included. 0 lifts the cap. */
enum fytim_result fytim_workpane_set_max_rows(struct fytim_workpane *wp,
                                              int rows) FYTIM_EXPORT;

/*
 * Columns of tiles. 0 selects the automatic grid: as many columns as fit
 * while every tile keeps the minimum width, and never more than the grid
 * needs to stay square.
 */
enum fytim_result fytim_workpane_set_columns(struct fytim_workpane *wp,
                                             int cols) FYTIM_EXPORT;

/*
 * The narrowest tile the automatic grid makes. A region that cannot hold two
 * such tiles gives one column, which is a stack of full-width screens: the
 * behaviour of separate bands, kept for a terminal that is too narrow to
 * tile. 0 restores the default.
 */
enum fytim_result fytim_workpane_set_min_tile_cols(struct fytim_workpane *wp,
                                                   int cols) FYTIM_EXPORT;
#define FYTIM_TILE_MIN_COLS 40

/*
 * An explicit grid. A host that wants a particular arrangement - a wide tile
 * over two narrow ones, a fixed-width column beside the work - declares the
 * tracks here and places its tiles in them with fytim_surface_set_cell().
 * The automatic grid above solves a square arrangement of equal cells and
 * cannot express one; this is how a host says what it wants instead.
 *
 * @rows or @cols of 0 restores the automatic grid. Neither may exceed
 * FYTIM_GRID_MAX. Setting a grid does not move the tiles already in the pane:
 * a tile with no cell of its own takes the next free one in reading order.
 */
#define FYTIM_GRID_MAX 16
enum fytim_result fytim_workpane_set_grid(struct fytim_workpane *wp, int rows,
                                          int cols) FYTIM_EXPORT;

/*
 * The size of one track of the explicit grid, in cells. A track of 0 takes an
 * equal share of what the sized tracks leave, which is the default. A sized
 * track keeps its size whatever its tiles ask for, so a host can hold a
 * status column at twenty columns beside a screen that grows.
 */
enum fytim_result fytim_workpane_set_row_size(struct fytim_workpane *wp,
                                              int row, int cells) FYTIM_EXPORT;
enum fytim_result fytim_workpane_set_col_size(struct fytim_workpane *wp,
                                              int col, int cells) FYTIM_EXPORT;

/* The rule drawn between adjacent columns. NULL or "" draws none. */
enum fytim_result fytim_workpane_set_tile_sep(struct fytim_workpane *wp,
                                              const char *text) FYTIM_EXPORT;

/*
 * Append a work band as a tile of @wp, instead of as a band of its own. It is
 * the band of libfytimui-band.h in every other way, and it composes beside a
 * tile that is a screen: what a tile holds - a progressive report, or the
 * grid of a program - does not change where the pane puts it.
 */
struct fytim_workband *fytim_workband_create_in(struct fytim_workpane *wp)
    FYTIM_EXPORT;

/*
 * Open a surface of @rows by @cols as a tile of @wp. It is the surface of
 * libfytimui-surface.h in every other way: the same calls publish cells into
 * it, and closing or committing it retires the tile.
 */
struct fytim_surface *fytim_surface_open_in(struct fytim_workpane *wp,
                                            int rows, int cols) FYTIM_EXPORT;

/*
 * One tile takes the whole pane, which is what a user does to work in a
 * program rather than watch it. NULL restores the grid. A zoomed tile that
 * retires drops the zoom with it.
 */
enum fytim_result fytim_workpane_set_zoom(struct fytim_workpane *wp,
                                          struct fytim_surface *sf)
    FYTIM_EXPORT;
struct fytim_surface *fytim_workpane_zoomed(const struct fytim_workpane *wp)
    FYTIM_EXPORT;

/*
 * Mouse affordances drawn on a tile. They are off by default, and for a
 * reason the host must weigh: the inline band leaves the mouse to the
 * terminal so that selection and copy keep working, and a pane that asks for
 * these takes the mouse for as long as they are on.
 */
#define FYTIM_WORKPANE_SCROLLBAR  (1u << 0)
#define FYTIM_WORKPANE_ARROWS     (1u << 1)
#define FYTIM_WORKPANE_ZOOM       (1u << 2)
#define FYTIM_WORKPANE_CLOSE      (1u << 3)
/*
 * Place a tile in the explicit grid. It occupies @row_span by @col_span cells
 * from (@row, @col), which must lie inside the grid. A span greater than one
 * is what makes a tile wider or taller than its neighbours.
 *
 * The placement is checked against the grid in force when it is set, so
 * declare the grid first.
 */
enum fytim_result fytim_surface_set_cell(struct fytim_surface *s, int row,
                                         int col, int row_span,
                                         int col_span) FYTIM_EXPORT;
enum fytim_result fytim_workband_set_cell(struct fytim_workband *wb, int row,
                                          int col, int row_span,
                                          int col_span) FYTIM_EXPORT;

enum fytim_result fytim_workpane_set_controls(struct fytim_workpane *wp,
                                              unsigned int flags) FYTIM_EXPORT;
unsigned int fytim_workpane_controls(const struct fytim_workpane *wp)
    FYTIM_EXPORT;

/*
 * What the host's scrollback holds for @sf, so that a scroll bar can show a
 * true position: @total_rows lines exist, and the grid shows the one at
 * @top_row. The library stores no rows - the host owns the emulator - and it
 * scrolls nothing itself: it reports the request as FYTIM_EVENT_SURFACE_SCROLL
 * and the host publishes the rows it wants seen.
 */
enum fytim_result fytim_surface_set_scroll_extent(struct fytim_surface *sf,
                                                  int total_rows, int top_row)
    FYTIM_EXPORT;

#endif /* LIBFYTIMUI_WORKPANE_H */
