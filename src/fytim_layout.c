/*
 * fytim_layout.c - vertical band geometry for the coding-agent screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <libfytimui/libfytimui-layout.h>

#include <string.h>

/* Order in which chrome is given up as the terminal shrinks, least important
 * first. The prompt is absent by construction: it is never dropped. The
 * transcript is absent too -- it is the flexible band and is squeezed by the
 * arithmetic below rather than dropped from this list.
 *
 * The trailing separator is listed but is usually retired ahead of its turn by
 * the borders-nothing invariant below. */
static const enum fytim_band shed_order[] = {
    FYTIM_BAND_STATUS,        /* two rows, shed one at a time */
    FYTIM_BAND_SEP_BOTTOM,
    FYTIM_BAND_HEADER,
    FYTIM_BAND_SEP_TOP
};

bool fytim_layout_compute_ex(int w, int h, int prompt_rows,
                             struct fytim_layout *out)
{
    int heights[FYTIM_BAND_COUNT];
    int chrome, deficit, y;
    size_t i;

    if(!out) return false;
    memset(out, 0, sizeof *out);
    if(w <= 0 || h <= 0) return false;
    if(prompt_rows < 1) prompt_rows = 1;   /* the prompt is never absent */
    if(prompt_rows > h) prompt_rows = h;   /* cannot exceed the screen */
    if(prompt_rows > (1 << 24)) prompt_rows = 1 << 24;   /* chrome sum must not
                                              overflow even for an INT_MAX
                                              screen (UBSan-found) */

    heights[FYTIM_BAND_TRANSCRIPT] = 0;   /* solved for last, from the slack */
    heights[FYTIM_BAND_HEADER]     = FYTIM_HEADER_ROWS;
    heights[FYTIM_BAND_SEP_TOP]    = FYTIM_SEP_ROWS;
    heights[FYTIM_BAND_PROMPT]     = prompt_rows;
    heights[FYTIM_BAND_SEP_BOTTOM] = FYTIM_SEP_ROWS;
    heights[FYTIM_BAND_STATUS]     = FYTIM_STATUS_ROWS;

    /* One row for the transcript is part of the budget, not a leftover: the
     * transcript is squeezed to its last row before any chrome is dropped,
     * and only then does chrome start to go. */
    chrome  = (FYTIM_CHROME_ROWS - FYTIM_PROMPT_ROWS) + prompt_rows;
    deficit = (chrome + 1) - h;

    for(i = 0; i < sizeof shed_order / sizeof shed_order[0] && deficit > 0; ++i){
        enum fytim_band b = shed_order[i];
        int give = (heights[b] < deficit) ? heights[b] : deficit;
        heights[b] -= give;
        deficit    -= give;
    }

    /* Extra prompt rows are the most valuable chrome -- the user is mid-edit
     * in them -- so they are shed only after everything above, and only down
     * to the single row that is never given up. */
    if(deficit > 0){
        int give = heights[FYTIM_BAND_PROMPT] - 1;
        if(give > deficit) give = deficit;
        if(give > 0){
            heights[FYTIM_BAND_PROMPT] -= give;
            deficit -= give;
        }
    }

    /* A separator earns its row only by separating something. Once the status
     * rows are gone the trailing rule borders nothing and would draw along the
     * bottom edge of the screen, so it goes with them -- regardless of whether
     * the deficit still demanded it. Its row returns to the transcript. */
    if(heights[FYTIM_BAND_STATUS] == 0)
        heights[FYTIM_BAND_SEP_BOTTOM] = 0;

    /* Everything sheddable is gone and it still does not fit: the prompt is
     * all that remains, and the transcript gives up its final row. */
    chrome = 0;
    for(i = 0; i < FYTIM_BAND_COUNT; ++i)
        if((enum fytim_band)i != FYTIM_BAND_TRANSCRIPT)
            chrome += heights[i];
    heights[FYTIM_BAND_TRANSCRIPT] = (h > chrome) ? h - chrome : 0;

    out->width  = w;
    out->height = h;
    for(i = 0, y = 0; i < FYTIM_BAND_COUNT; ++i){
        if(heights[i] == 0) continue;     /* hidden: left zeroed, takes no rows */
        out->band[i].x = 0;
        out->band[i].y = y;
        out->band[i].w = w;
        out->band[i].h = heights[i];
        y += heights[i];
    }
    return true;
}

bool fytim_layout_compute(int w, int h, struct fytim_layout *out)
{
    return fytim_layout_compute_ex(w, h, FYTIM_PROMPT_ROWS, out);
}
