/*
 * libfytimui-page.h - a page of rows and slots that the host rendered.
 *
 * The band stack of libfytimui-band.h places a fixed set of chrome. A host
 * that renders its own screen, with UI Markdown for example, gives the
 * library a page instead: the rows it rendered, and the regions of those rows
 * that are slots or clickable labels.
 *
 *     +--------------------------------+
 *     | rows of the host               |
 *     | [ slot "tail"               ]  |  drawn by the component bound
 *     | [ slot "pane"               ]  |  to the id of the slot
 *     | rows of the host  [act "x"]    |
 *     | [ slot "prompt"             ]  |
 *     +--------------------------------+
 *
 * The library draws the rows, then draws each slot region with the component
 * bound to its id. The built-in ids are "tail" (the transcript tail),
 * "prompt" (the marker and the editor) and "completion" (the ribbon while
 * completion is active). A band, a surface or a pane is bound with the
 * fytim_*_bind() calls. A slot with nothing bound is blank, and a band that
 * is bound to no slot is not drawn while a page is set.
 *
 * A slot region is a grant: a surface in a slot is given the rows and the
 * columns of the region. A click on an act region is FYTIM_EVENT_ACT, with
 * the id as the text of the event.
 *
 * In the inline band the page is the live region: its height is its rows,
 * or the bottom of its lowest region when that is lower.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_PAGE_H
#define LIBFYTIMUI_PAGE_H

#include <libfytimui/libfytimui-util.h>

#include <stdbool.h>
#include <stddef.h>

struct fytim;
struct fytim_workband;
struct fytim_surface;
struct fytim_workpane;

/* Regions a page may carry. */
#define FYTIM_PAGE_REGIONS_MAX 256
/* Bytes of a region id: letters, digits and "_-.:/". */
#define FYTIM_PAGE_ID_MAX      63

enum fytim_page_region_kind {
    FYTIM_PAGE_ACT = 0,    /* a clickable label, one row */
    FYTIM_PAGE_SLOT        /* cells that a bound component draws */
};

/* A region of the page, in cells from its first row and column. */
struct fytim_page_region {
    const char *id;
    enum fytim_page_region_kind kind;
    int row;
    int col;
    int width;
    int height;
};

/*
 * Set the page. @rows are the rendered rows, '\n'-separated, under the
 * contract of fytim_commit: SGR and OSC-8 only. The regions are copied. On
 * rejection the previous page, or the band stack, stays. A region with an
 * invalid id, a negative position or size, or an act that is not one row is
 * rejected, as are more than FYTIM_PAGE_REGIONS_MAX regions.
 */
enum fytim_result fytim_page_set(struct fytim *ft, const char *rows,
                                 size_t len,
                                 const struct fytim_page_region *regions,
                                 size_t count) FYTIM_EXPORT;

/* Remove the page. The band stack draws again. */
void fytim_page_clear(struct fytim *ft) FYTIM_EXPORT;

/* Whether a page is set. */
bool fytim_page_active(const struct fytim *ft) FYTIM_EXPORT;

/* The rows the page takes, or 0 when no page is set. */
int fytim_page_rows(const struct fytim *ft) FYTIM_EXPORT;

/*
 * Bind a component to the slot @id. NULL removes the binding. A built-in id
 * cannot be bound. A tile of a pane can be bound, and is then drawn in its
 * slot as its pane draws it: a page binds the pane or its tiles, not both.
 * Binding an id that another component holds takes it from that component.
 */
enum fytim_result fytim_workband_bind(struct fytim_workband *wb,
                                      const char *id) FYTIM_EXPORT;
enum fytim_result fytim_surface_bind(struct fytim_surface *sf,
                                     const char *id) FYTIM_EXPORT;
enum fytim_result fytim_workpane_bind(struct fytim_workpane *wp,
                                      const char *id) FYTIM_EXPORT;

/*
 * Give the tile of @sf a page of its own: its head, its controls and any
 * chrome, rendered by the host at the columns the tile was granted, with at
 * most one slot named "screen" where the grid of the surface is drawn. The
 * rows above the screen slot are the head of the tile and the rows below it
 * its foot; they take the place of the set_top and set_bottom chrome, and a
 * tile too short for them sheds the last rows of each first. The screen slot
 * takes the rows the tile has left, whatever height it was rendered at.
 *
 * While a tile has a page the library draws no zoom or close mark on it: the
 * host places its controls as acts. A click on an act of the page is
 * FYTIM_EVENT_ACT with @surface set and @row and @col the cell of the page.
 * Rows of NULL remove the page. The contract of the rows and the regions is
 * that of fytim_page_set().
 */
enum fytim_result fytim_surface_set_page(struct fytim_surface *sf,
                                         const char *rows, size_t len,
                                         const struct fytim_page_region *regions,
                                         size_t count) FYTIM_EXPORT;

/*
 * What a tile page shows of itself. The view changes what is drawn and
 * nothing that is asked or granted: the rows a tile requests, the rows its
 * grid row reserves and the rows its surface is granted are those of the
 * whole page in every view. A host chooses a view from the grant, and a view
 * that changed the request would change the grant it was chosen from.
 */
enum fytim_page_view {
    FYTIM_PAGE_VIEW_FULL = 0,   /* the head, the screen and the foot */
    FYTIM_PAGE_VIEW_SCREEN,     /* the screen alone, from the top of the tile */
    FYTIM_PAGE_VIEW_HEAD        /* the head alone, and no screen */
};

enum fytim_result fytim_surface_set_page_view(struct fytim_surface *sf,
                                              enum fytim_page_view view)
    FYTIM_EXPORT;

/*
 * What a page needs to size its slots. An inline page is as tall as its rows,
 * so a host states the height of each slot from these; a slot that is given
 * fewer rows than its component asks for shows the component's last rows.
 */

/* Rows the transcript tail holds. */
int fytim_tail_rows(const struct fytim *ft) FYTIM_EXPORT;

/* Rows the prompt asks for: the lines being edited, one row while a surface
 * holds the keys, and 0 when the host asked for no prompt. */
int fytim_prompt_rows(const struct fytim *ft) FYTIM_EXPORT;

/*
 * Whether the prompt stands on a card: it was given a style or a ground of its
 * own, and no surface holds the keys. A prompt slot of three rows or more is
 * then drawn as the card, the editor on its middle rows, as the band stack
 * draws the prompt between its two framing rows. A page gives such a prompt
 * two more rows than fytim_prompt_rows().
 */
bool fytim_prompt_card(const struct fytim *ft) FYTIM_EXPORT;

/* Whether completion is cycling, which is when the ribbon has something to
 * draw. */
bool fytim_completion_active(const struct fytim *ft) FYTIM_EXPORT;

/* Rows the tiles of @wp ask for, their heads included and the chrome of the
 * pane excluded; 0 when it holds no tile. */
int fytim_workpane_rows(const struct fytim_workpane *wp) FYTIM_EXPORT;

/*
 * Rows a tile asks for: its screen or its text within its cap, and the rows of
 * its head and foot, from its page or its chrome. A host that places tiles in
 * a grid of its own sizes a fitted row from these. 0 for NULL.
 */
int fytim_surface_rows(const struct fytim_surface *sf) FYTIM_EXPORT;
int fytim_workband_rows(const struct fytim_workband *wb) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_PAGE_H */
