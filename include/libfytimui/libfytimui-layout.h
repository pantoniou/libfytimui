/*
 * libfytimui-layout.h - vertical band geometry for the coding-agent screen.
 *
 * Public: hosts that render their own chrome (or reason about where the
 * library will put it) can solve the same geometry the library uses.
 *
 * The screen is a stack of full-width horizontal bands. The transcript takes
 * whatever the fixed bottom chrome does not:
 *
 *     +--------------------------------+
 *     | transcript (rendered markdown) |  flexible, >= 1 row when it fits
 *     |                                |
 *     +--------------------------------+
 *     | header status                  |  1
 *     | -------- separator ----------- |  1
 *     | > prompt                       |  1
 *     | -------- separator ----------- |  1
 *     | status                         |  2
 *     | status                         |
 *     +--------------------------------+
 *
 * A band with h == 0 is hidden. As height shrinks the chrome degrades in a
 * fixed order, least important first, so that the prompt is the last thing
 * standing: it is the only band the user cannot operate without - unless the
 * host asked for no prompt at all, which a full-screen program does.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_LAYOUT_H
#define LIBFYTIMUI_LAYOUT_H

#include <stdbool.h>

#include <libfytimui/libfytimui-util.h>

/* Bands, in top-to-bottom screen order. The enum order is the draw order. */
enum fytim_band {
    FYTIM_BAND_TRANSCRIPT = 0,
    FYTIM_BAND_HEADER,        /* status row above the prompt block */
    FYTIM_BAND_SEP_TOP,
    FYTIM_BAND_PROMPT,
    FYTIM_BAND_SEP_BOTTOM,
    FYTIM_BAND_STATUS,        /* two rows of trailing status */
    FYTIM_BAND_COUNT
};

/* Natural height of each band when nothing has to be dropped. */
#define FYTIM_HEADER_ROWS 1
#define FYTIM_SEP_ROWS    1
#define FYTIM_PROMPT_ROWS 1
#define FYTIM_STATUS_ROWS 2

/* Rows of fixed chrome below the transcript at full size. */
#define FYTIM_CHROME_ROWS \
    (FYTIM_HEADER_ROWS + FYTIM_SEP_ROWS + FYTIM_PROMPT_ROWS + \
     FYTIM_SEP_ROWS + FYTIM_STATUS_ROWS)

struct fytim_rect {
    int x, y, w, h;
};

struct fytim_layout {
    int width, height;                          /* the geometry solved for */
    struct fytim_rect band[FYTIM_BAND_COUNT];
};

/* Solve the band geometry for a terminal of w x h cells.
 *
 * Returns false and zeroes *out for a degenerate terminal (w or h <= 0) or a
 * NULL out; every band is then hidden, so a caller that ignores the return
 * value still draws nothing rather than drawing garbage. */
bool fytim_layout_compute(int w, int h, struct fytim_layout *out)
    FYTIM_EXPORT;

/* As above, with a prompt band of `prompt_rows` rows (clamped to >= 1) for a
 * multiline edit in flight. Extra prompt rows outrank all other chrome in
 * the degradation order -- the user is mid-edit in them -- but the transcript
 * still keeps its final row against any request, and the prompt's single
 * guaranteed row is never given up. rows == FYTIM_PROMPT_ROWS reproduces
 * fytim_layout_compute exactly.
 *
 * rows == 0 asks for NO prompt: a host whose keys belong to something else has
 * nothing to type into one, and the separators that frame it go with it. It is
 * the only value that removes the prompt - a negative count is a caller's slip
 * and keeps it. */
bool fytim_layout_compute_ex(int w, int h, int prompt_rows,
                             struct fytim_layout *out)
    FYTIM_EXPORT;

#endif /* LIBFYTIMUI_LAYOUT_H */
