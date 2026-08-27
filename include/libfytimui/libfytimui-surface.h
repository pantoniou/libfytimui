/*
 * libfytimui-surface.h - a grid of cells the host publishes.
 *
 * A work band carries text. Some content is not text: a program driven on a
 * pseudo-terminal draws a grid, and the bytes that produced it are cursor
 * movements that mean nothing on their own. The host interprets those bytes -
 * it owns the emulator - and publishes the result here as cells.
 *
 * A surface composes exactly like a work band: it takes rows in the same
 * region, in the order it was opened, and sheds rows under the same rules.
 * Thus several of them - one for each program being watched - stand side by
 * side above the prompt, and the library still owns layout and repainting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_SURFACE_H
#define LIBFYTIMUI_SURFACE_H

#include <libfytimui/libfytimui-style.h>
#include <libfytimui/libfytimui-util.h>

#include <stdbool.h>
#include <stdint.h>

struct fytim;
struct fytim_surface;   /* opaque; owned by the fytim it was opened on */

/* A base character and the characters that combine with it. */
#define FYTIM_CELL_CHARS 6

/*
 * One cell. A cell with no character is blank and takes the surface style.
 * @width is 2 for a double-width glyph, which leaves the cell after it unused;
 * the library steps over that cell rather than drawing it.
 */
struct fytim_cell {
    uint32_t chars[FYTIM_CELL_CHARS];
    uint32_t fg;            /* FYTIM_COLOR_DEFAULT, INDEXED | n, or 0xRRGGBB */
    uint32_t bg;
    uint32_t attrs;         /* FYTIM_ATTR_* */
    unsigned char width;    /* 0 and 1 are one cell wide, 2 is a wide glyph */
};

/*
 * Open a surface of @rows by @cols. The size is what the host draws into and
 * not what the terminal grants: a short terminal shows the last rows of it,
 * the way a work band shows the last lines of its content.
 */
struct fytim_surface *fytim_surface_open(struct fytim *ft, int rows, int cols)
    FYTIM_EXPORT;
void fytim_surface_close(struct fytim_surface *s) FYTIM_EXPORT;

/* Give the grid a new size. Content inside the new size is kept. */
enum fytim_result fytim_surface_resize(struct fytim_surface *s, int rows,
                                       int cols) FYTIM_EXPORT;
enum fytim_result fytim_surface_size(const struct fytim_surface *s, int *rows,
                                     int *cols) FYTIM_EXPORT;

/*
 * The rows the layout granted at the last frame, which is what the user can
 * see. A host that drives a pseudo-terminal sizes it to this, so that the
 * program draws what fits instead of drawing into rows nobody shows.
 */
enum fytim_result fytim_surface_granted_rows(const struct fytim_surface *s,
                                             int *rows) FYTIM_EXPORT;

/* Never grant more than @rows, whatever the terminal has. 0 lifts the cap. */
enum fytim_result fytim_surface_set_max_rows(struct fytim_surface *s, int rows)
    FYTIM_EXPORT;

/* Replace row @row with @n cells. Cells beyond the width are dropped. */
enum fytim_result fytim_surface_put_row(struct fytim_surface *s, int row,
                                        const struct fytim_cell *cells, int n)
    FYTIM_EXPORT;

/* Blank the whole grid. */
enum fytim_result fytim_surface_clear(struct fytim_surface *s) FYTIM_EXPORT;

/*
 * Where the program left its cursor. The library draws it as a reverse-video
 * cell and does not move the cursor of the terminal, which belongs to the
 * prompt: a surface is watched while the user is typing somewhere else.
 */
enum fytim_result fytim_surface_set_cursor(struct fytim_surface *s, int row,
                                           int col, bool visible) FYTIM_EXPORT;

/*
 * Give the keys to this surface. While a surface holds them, what the user
 * types is encoded back into terminal bytes and delivered as
 * FYTIM_EVENT_SURFACE_KEYS instead of reaching the prompt: the host writes
 * those bytes to the program it is driving. One surface holds the keys at a
 * time, and taking them takes them from whichever held them before.
 *
 * Escape and ^C are the program's while it holds the keys, so a host that
 * wants a way out of the surface reserves a key of its own and watches for it
 * in the bytes it receives.
 */
enum fytim_result fytim_surface_set_keys(struct fytim_surface *s, bool take)
    FYTIM_EXPORT;
bool fytim_surface_has_keys(const struct fytim_surface *s) FYTIM_EXPORT;

/*
 * The program is done: keep its last screen. The grid is written into the
 * transcript as styled rows, exactly as a work band commits its content, and
 * the surface is retired - the handle is invalid afterwards, as it is after
 * fytim_surface_close(). Closing instead leaves nothing behind.
 */
enum fytim_result fytim_surface_commit(struct fytim_surface *s) FYTIM_EXPORT;

/*
 * A margin drawn at the left of every row of the grid, so that the screen of a
 * program reads as one thing and as something that belongs to whatever opened
 * it. The margin is chrome: it takes columns from the grid, and
 * fytim_surface_granted_cols() reports what is left, which is what the host
 * should size its program to. NULL removes it.
 */
enum fytim_result fytim_surface_set_margin(struct fytim_surface *s,
                                           const char *text) FYTIM_EXPORT;

/* The columns the grid was given at the last frame: the width less the
 * margin. */
enum fytim_result fytim_surface_granted_cols(const struct fytim_surface *s,
                                             int *cols) FYTIM_EXPORT;

/*
 * Chrome above and below the grid, as a work band has. Plain text draws dim;
 * text that carries styling of its own keeps it. A slot draws one row
 * for each of its lines, so a head can say what the call is and then what it
 * was asked to run. A region too short for the whole head sheds its last row
 * first. NULL removes the slot.
 */
enum fytim_result fytim_surface_set_top(struct fytim_surface *s,
                                        const char *text) FYTIM_EXPORT;
enum fytim_result fytim_surface_set_bottom(struct fytim_surface *s,
                                           const char *text) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_SURFACE_H */
