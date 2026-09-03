/*
 * test_fytim_workpane_vt.c - what a tiled pane looks like on a terminal.
 *
 * That a tile was GRANTED forty columns is the library's word for it. What
 * the user sees is cells, and only a terminal can say whether two screens
 * stand side by side on the same rows, whether the rule between them is
 * where it should be, and whether a screen was clipped to its tile instead
 * of running over its neighbour. This replays every byte the library emits
 * into libfyvterm and reads the grid back.
 *
 * Only built when libfyvterm is available.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"
#include <libfyvterm.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROWS 24
#define COLS 80

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

struct vth {
    struct fytim *ft;
    struct fyvt *vt;
    struct fyvt_screen *vs;
    int in[2], out[2];
};

static int vth_open(struct vth *h)
{
    struct fytim_cfg cfg;

    /* This harness feeds 24-bit colours to a virtual terminal.  Make the
     * matching capability deterministic: CI has no COLORTERM, while an
     * interactive developer shell commonly does. */
    setenv("COLORTERM", "truecolor", 1);
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    h->vt = fyvt_create(&(struct fyvt_cfg){
                    .struct_size = sizeof(struct fyvt_cfg),
                    .rows = ROWS, .cols = COLS });
    fyvt_set_utf8(h->vt, 1);
    h->vs = fyvt_obtain_screen(h->vt);
    fyvt_screen_reset(h->vs, 1);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

static void vth_close(struct vth *h)
{
    fytim_destroy(h->ft);
    if(h->vt) fyvt_destroy(h->vt);
    close(h->in[0]); close(h->in[1]);
    close(h->out[0]); close(h->out[1]);
}

static void vth_pump(struct vth *h)
{
    char buf[65536];
    ssize_t n;
    CHECK(fytim_pump(h->ft) == FYTIM_OK);
    while((n = read(h->out[0], buf, sizeof buf)) > 0)
        fyvt_input_write(h->vt, buf, (size_t)n);
    fyvt_screen_flush_damage(h->vs);
}

static struct fyvt_screen_cell cell_at(struct vth *h, int row, int col)
{
    struct fyvt_screen_cell c;
    struct fyvt_pos pos;
    memset(&c, 0, sizeof c);
    pos.row = row;
    pos.col = col;
    fyvt_screen_get_cell(h->vs, pos, &c);
    return c;
}

/* The first screen row holding @ch anywhere, and its column. -1 if none. */
static int find_char(struct vth *h, uint32_t ch, int *colp)
{
    int r, c;
    for(r = 0; r < ROWS; r++)
        for(c = 0; c < COLS; c++)
            if(cell_at(h, r, c).chars[0] == ch){
                if(colp) *colp = c;
                return r;
            }
    return -1;
}

/* Columns of row @row holding @ch, as a first and last column. */
static void run_of(struct vth *h, int row, uint32_t ch, int *first, int *last)
{
    int c;
    *first = *last = -1;
    for(c = 0; c < COLS; c++)
        if(cell_at(h, row, c).chars[0] == ch){
            if(*first < 0) *first = c;
            *last = c;
        }
}

static int rgb_equal(struct vth *h, union fyvt_color got, uint32_t want_rgb)
{
    union fyvt_color want;
    fyvt_color_rgb(&want, (uint8_t)(want_rgb >> 16),
                    (uint8_t)(want_rgb >> 8), (uint8_t)want_rgb);
    fyvt_screen_convert_color_to_rgb(h->vs, &got);
    fyvt_screen_convert_color_to_rgb(h->vs, &want);
    return fyvt_color_is_equal(&got, &want);
}

/* A row of one repeated character, optionally coloured. */
static void fill_row(struct fytim_cell *cells, int n, uint32_t ch, uint32_t fg)
{
    int i;
    memset(cells, 0, (size_t)n * sizeof *cells);
    for(i = 0; i < n; i++){
        cells[i].chars[0] = ch;
        cells[i].fg = fg;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].width = 1;
    }
}

static void paint(struct fytim_surface *sf, uint32_t ch, uint32_t fg)
{
    struct fytim_cell cells[COLS];
    int row, rows = 0, cols = 0;

    fytim_surface_size(sf, &rows, &cols);
    if(cols > COLS) cols = COLS;
    fill_row(cells, cols, ch, fg);
    for(row = 0; row < rows; row++)
        fytim_surface_put_row(sf, row, cells, cols);
}

/*
 * Two screens on the same rows, in disjoint column ranges. This is the whole
 * claim of a work pane, and a byte capture cannot make it: the escapes that
 * place the second screen say nothing about where the first one ended.
 */
static void test_two_tiles_stand_side_by_side(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int arow, brow, af, al, bf, bl, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 30);
    b = fytim_surface_open_in(wp, 3, 30);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    arow = find_char(&h, 'A', &col);
    brow = find_char(&h, 'B', &col);
    CHECK(arow >= 0);
    CHECK(brow >= 0);
    /* The same row holds both: they are beside each other, not stacked. */
    CHECK(arow == brow);
    if(arow < 0 || arow != brow){ vth_close(&h); return; }

    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, arow, 'B', &bf, &bl);
    /* The first is at the left edge, the second starts after it ends, and
     * neither ran over the other. */
    CHECK(af == 0);
    CHECK(al < bf);
    CHECK(bl < COLS);
    /* Each was clipped to its half of an eighty-column terminal. */
    CHECK(al - af + 1 == 30);   /* the screen is 30 wide in a 40 tile */
    CHECK(bf == 40);
    vth_close(&h);
}

/* Every row of the two tiles lines up: one grid row, two screens. */
static void test_the_tiles_share_every_row(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int arow, r, af, al, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 40);
    b = fytim_surface_open_in(wp, 3, 40);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    if(arow < 0){ vth_close(&h); return; }
    for(r = arow; r < arow + 3; r++){
        run_of(&h, r, 'A', &af, &al);
        run_of(&h, r, 'B', &bf, &bl);
        CHECK(af == 0);
        CHECK(al == 39);
        CHECK(bf == 40);
        CHECK(bl == 79);
    }
    vth_close(&h);
}

/* A screen wider than its tile is clipped at the tile, not at the terminal:
 * the neighbour's columns are the neighbour's. */
static void test_a_wide_screen_is_clipped_to_its_tile(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int arow, af, al, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    /* Both screens are the width of the whole terminal. */
    a = fytim_surface_open_in(wp, 2, 80);
    b = fytim_surface_open_in(wp, 2, 80);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    if(arow < 0){ vth_close(&h); return; }
    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, arow, 'B', &bf, &bl);
    CHECK(af == 0);
    CHECK(al == 39);       /* clipped at its tile, not at column 79 */
    CHECK(bf == 40);
    CHECK(bl == 79);
    vth_close(&h);
}

/* The rule between the columns is drawn where the tiles meet. */
static void test_the_rule_sits_between_the_tiles(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int arow, af, al, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 80);
    b = fytim_surface_open_in(wp, 2, 80);
    CHECK(fytim_workpane_set_tile_sep(wp, "|") == FYTIM_OK);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    if(arow < 0){ vth_close(&h); return; }
    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, arow, 'B', &bf, &bl);
    /* The rule is one cell, and it is between them: the first tile ends,
     * the rule stands, the second begins. */
    CHECK(cell_at(&h, arow, al + 1).chars[0] == '|');
    CHECK(bf == al + 2);
    vth_close(&h);
}

/* A colour published into one tile is that tile's, and stops at its edge. */
static void test_a_colour_stops_at_the_tile_edge(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fyvt_screen_cell c;
    struct vth h;
    int arow, af, al;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 40);
    b = fytim_surface_open_in(wp, 2, 40);
    paint(a, 'A', 0x00FF8000u);
    paint(b, 'B', 0x000080FFu);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    if(arow < 0){ vth_close(&h); return; }
    run_of(&h, arow, 'A', &af, &al);
    c = cell_at(&h, arow, af);
    CHECK(rgb_equal(&h, c.fg, 0x00FF8000u));
    c = cell_at(&h, arow, al);
    CHECK(rgb_equal(&h, c.fg, 0x00FF8000u));
    /* The first cell of the neighbour carries the neighbour's colour. */
    c = cell_at(&h, arow, al + 1);
    CHECK(c.chars[0] == 'B');
    CHECK(rgb_equal(&h, c.fg, 0x000080FFu));
    vth_close(&h);
}

/* Three tiles: two above, one below, and the rows do not overlap. */
static void test_the_grid_puts_the_third_below(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *sf[3];
    struct vth h;
    int arow, brow, crow, af, al, bf, bl, cf, cl, i;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    for(i = 0; i < 3; i++)
        sf[i] = fytim_surface_open_in(wp, 2, 40);
    paint(sf[0], 'A', FYTIM_COLOR_DEFAULT);
    paint(sf[1], 'B', FYTIM_COLOR_DEFAULT);
    paint(sf[2], 'C', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    brow = find_char(&h, 'B', NULL);
    crow = find_char(&h, 'C', NULL);
    CHECK(arow >= 0 && brow >= 0 && crow >= 0);
    if(arow < 0 || brow < 0 || crow < 0){ vth_close(&h); return; }
    /* The first two share their rows; the third is on the row after them. */
    CHECK(arow == brow);
    CHECK(crow == arow + 2);
    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, brow, 'B', &bf, &bl);
    run_of(&h, crow, 'C', &cf, &cl);
    CHECK(af == 0 && al == 39);
    CHECK(bf == 40 && bl == 79);
    /* And the third starts a new grid row at the left edge. */
    CHECK(cf == 0 && cl == 39);
    vth_close(&h);
}

/* A zoomed tile has the rows to itself: the other is nowhere on screen. */
static void test_a_zoomed_tile_owns_the_rows(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int brow, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    b = fytim_surface_open_in(wp, 3, 80);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    CHECK(fytim_workpane_set_zoom(wp, b) == FYTIM_OK);
    vth_pump(&h);

    CHECK(find_char(&h, 'A', NULL) < 0);
    brow = find_char(&h, 'B', NULL);
    CHECK(brow >= 0);
    if(brow < 0){ vth_close(&h); return; }
    run_of(&h, brow, 'B', &bf, &bl);
    CHECK(bf == 0);
    CHECK(bl == 79);
    vth_close(&h);
}

/* A tile's own title row is over its own tile and nowhere else. */
static void test_a_title_stays_over_its_tile(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int trow, tcol = -1, arow;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 40);
    b = fytim_surface_open_in(wp, 2, 40);
    CHECK(fytim_surface_set_top(b, "TITLE") == FYTIM_OK);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    trow = find_char(&h, 'T', &tcol);
    CHECK(trow >= 0);
    /* It starts at the second tile's left edge, not at the terminal's. */
    CHECK(tcol == 40);
    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    /* The tile with no title of its own is not pushed down by its
     * neighbour's: both grid rows are the same height, and the untitled
     * screen simply starts at the top of the tile. */
    CHECK(arow == trow);
    vth_close(&h);
}

/*
 * A head of two rows draws both, and the second stays over its own tile: a
 * shell says what it is and then what it was asked to run, whether or not it
 * has a screen of its own.
 */
static void test_a_head_of_two_rows_draws_both(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int trow, tcol = -1, crow, ccol = -1, arow, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 40);
    b = fytim_surface_open_in(wp, 2, 40);
    CHECK(fytim_surface_set_top(b, "TITLE\nRUNS") == FYTIM_OK);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    trow = find_char(&h, 'T', &tcol);
    crow = find_char(&h, 'R', &ccol);
    CHECK(trow >= 0);
    CHECK(crow == trow + 1);
    /* Both rows of the head start at the tile's own left edge. */
    CHECK(tcol == 40);
    CHECK(ccol == 40);
    /* The screen starts under the whole head, not under its first row. */
    run_of(&h, crow, 'B', &bf, &bl);
    CHECK(bf < 0);
    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    /* The untitled neighbour is not pushed down by a head it does not have. */
    CHECK(arow == trow);
    vth_close(&h);
}

/*
 * A region too short for the whole head sheds its last row first: the row
 * that names the call is the one worth keeping.
 */
static void test_a_short_tile_sheds_the_last_head_row(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 8, 40);
    CHECK(fytim_surface_set_top(a, "TITLE\nRUNS") == FYTIM_OK);
    CHECK(fytim_workpane_set_max_rows(wp, 2) == FYTIM_OK);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    CHECK(find_char(&h, 'T', NULL) >= 0);
    CHECK(find_char(&h, 'R', NULL) < 0);
    vth_close(&h);
}

/*
 * A band with a head and nothing under it yet takes only its head: the row
 * kept so an idle band still shows is not needed when its chrome already
 * says what it is, and an empty row there reads as a gap.
 */
static void test_a_head_with_no_content_takes_no_row(void)
{
    struct fytim_workband *a, *b;
    struct vth h;
    int hrow, mrow;

    if(!vth_open(&h)){ CHECK(0); return; }
    a = fytim_workband_create(h.ft);
    b = fytim_workband_create(h.ft);
    CHECK(fytim_workband_set_top(a, "HEAD") == FYTIM_OK);
    CHECK(fytim_workband_set(b, "MARK", 4) == FYTIM_OK);
    vth_pump(&h);

    hrow = find_char(&h, 'H', NULL);
    mrow = find_char(&h, 'M', NULL);
    CHECK(hrow >= 0);
    CHECK(mrow == hrow + 1);
    vth_close(&h);
}

/*
 * A head that carries its own styling keeps it. Chrome draws dim, which is
 * right for a rule: chrome that frames the work is not the work. A head
 * naming the call was styled by whoever set it, and dimming it a second time
 * takes the emphasis it was given.
 */
static void test_a_styled_head_is_not_dimmed(void)
{
    struct fytim_workband *a, *b;
    struct vth h;
    int brow, prow, pcol = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    a = fytim_workband_create(h.ft);
    b = fytim_workband_create(h.ft);
    /* One head styled by its author, one left to the library. */
    CHECK(fytim_workband_set_top(a, "\x1b[31mBRIGHT\x1b[0m") == FYTIM_OK);
    CHECK(fytim_workband_set_top(b, "PLAIN") == FYTIM_OK);
    CHECK(fytim_workband_set(b, "x", 1) == FYTIM_OK);
    vth_pump(&h);

    brow = find_char(&h, 'B', NULL);
    CHECK(brow >= 0);
    if(brow < 0){ vth_close(&h); return; }
    CHECK(cell_at(&h, brow, 0).attrs.dim == 0);
    /* Chrome the library styles itself still draws dim. */
    prow = find_char(&h, 'P', &pcol);
    CHECK(prow >= 0);
    if(prow >= 0)
        CHECK(cell_at(&h, prow, pcol).attrs.dim == 1);
    vth_close(&h);
}

/* A left margin is drawn inside the tile it belongs to. */
static void test_a_margin_is_drawn_inside_the_tile(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct vth h;
    int brow, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 40);
    b = fytim_surface_open_in(wp, 2, 40);
    CHECK(fytim_surface_set_margin(b, "> ") == FYTIM_OK);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    paint(b, 'B', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    brow = find_char(&h, 'B', NULL);
    CHECK(brow >= 0);
    if(brow < 0){ vth_close(&h); return; }
    /* The margin is at the tile's own left edge. */
    CHECK(cell_at(&h, brow, 40).chars[0] == '>');
    run_of(&h, brow, 'B', &bf, &bl);
    CHECK(bf == 42);
    CHECK(bl == 79);
    vth_close(&h);
}

/* A surface margin spans its title, grid, and status chrome. */
static void test_regression_margin_spans_surface_chrome(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *sf;
    struct vth h;
    int trow, grow, srow, mcol = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    sf = fytim_surface_open_in(wp, 2, 40);
    CHECK(fytim_surface_set_margin(sf, "\x1b[7m> \x1b[27m") == FYTIM_OK);
    CHECK(fytim_surface_set_top(sf, "TITLE\nCOMMAND") == FYTIM_OK);
    CHECK(fytim_surface_set_bottom(sf, "STATUS") == FYTIM_OK);
    paint(sf, 'A', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);

    trow = find_char(&h, 'T', NULL);
    grow = find_char(&h, 'A', NULL);
    srow = find_char(&h, 'S', NULL);
    CHECK(find_char(&h, '>', &mcol) >= 0);
    CHECK(trow >= 0 && grow >= 0 && srow >= 0 && mcol >= 0);
    if(trow >= 0){
        CHECK(cell_at(&h, trow, mcol).chars[0] == '>');
        CHECK(cell_at(&h, trow, mcol).attrs.reverse == 1);
        CHECK(cell_at(&h, trow + 1, mcol).chars[0] == '>');
        CHECK(cell_at(&h, trow + 1, mcol).attrs.reverse == 1);
    }
    if(grow >= 0){
        CHECK(cell_at(&h, grow, mcol).chars[0] == '>');
        CHECK(cell_at(&h, grow, mcol).attrs.reverse == 1);
    }
    if(srow >= 0){
        CHECK(cell_at(&h, srow, mcol).chars[0] == '>');
        CHECK(cell_at(&h, srow, mcol).attrs.reverse == 1);
    }
    vth_close(&h);
}

/* Two tiles of TEXT stand side by side, as two screens do: what a tile holds
 * does not change where the pane puts it. */
static void test_two_band_tiles_stand_side_by_side(void)
{
    struct fytim_workpane *wp;
    struct fytim_workband *a, *b;
    struct vth h;
    int arow, brow, af, al, bf, bl;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_workband_create_in(wp);
    b = fytim_workband_create_in(wp);
    CHECK(a != NULL && b != NULL);
    /* Chrome text that shares no character with the content, so that a
     * search for the content cannot land on a title row. */
    CHECK(fytim_workband_set_top(a, "one") == FYTIM_OK);
    CHECK(fytim_workband_set_top(b, "two") == FYTIM_OK);
    CHECK(fytim_workband_set(a, "AAAA\nAAAA", 9) == FYTIM_OK);
    CHECK(fytim_workband_set(b, "BBBB\nBBBB", 9) == FYTIM_OK);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    brow = find_char(&h, 'B', NULL);
    CHECK(arow >= 0 && brow >= 0);
    if(arow < 0 || brow < 0){ vth_close(&h); return; }
    CHECK(arow == brow);
    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, arow, 'B', &bf, &bl);
    CHECK(af == 0);
    CHECK(bf == 40);
    /* Neither ran into the other's columns. */
    CHECK(al < 40);
    vth_close(&h);
}

/* A tile's rows are the tile's: text wider than the tile does not run into
 * the neighbour. */
static void test_a_wide_row_is_clipped_to_its_tile(void)
{
    struct fytim_workpane *wp;
    struct fytim_workband *a, *b;
    struct vth h;
    int arow, af, al, bf, bl;
    char wide[COLS + 1];

    if(!vth_open(&h)){ CHECK(0); return; }
    memset(wide, 'A', COLS);
    wide[COLS] = '\0';
    wp = fytim_workpane_create(h.ft);
    a = fytim_workband_create_in(wp);
    b = fytim_workband_create_in(wp);
    /* A row as wide as the whole terminal, in a tile half that. */
    CHECK(fytim_workband_set(a, wide, strlen(wide)) == FYTIM_OK);
    CHECK(fytim_workband_set(b, "BBBB", 4) == FYTIM_OK);
    vth_pump(&h);

    arow = find_char(&h, 'A', NULL);
    CHECK(arow >= 0);
    if(arow < 0){ vth_close(&h); return; }
    run_of(&h, arow, 'A', &af, &al);
    run_of(&h, arow, 'B', &bf, &bl);
    CHECK(af == 0);
    /* Clipped at its tile, and the last cell says the row goes on: a row
     * that simply stopped there would read as damage. */
    CHECK(al == 38);
    CHECK(cell_at(&h, arow, 39).chars[0] == 0x2026);
    CHECK(bf == 40);
    vth_close(&h);
}


/*
 * Where the pane stands. By default it is the last thing above the prompt
 * block; asked for, it takes the rows below it, so the user types over the
 * work instead of under it. A byte capture cannot say which came out first
 * on the screen: only the rows can.
 */
static void place_check(enum fytim_workpane_place place, int *panerow,
                        int *promptrow)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;

    *panerow = *promptrow = -1;
    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_place(wp, place) == FYTIM_OK);
    a = fytim_surface_open_in(wp, 3, 30);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    CHECK(fytim_set_input(h.ft, "zzzz") == FYTIM_OK);
    vth_pump(&h);
    *panerow = find_char(&h, 'A', NULL);
    *promptrow = find_char(&h, 'z', NULL);
    vth_close(&h);
}

static void test_the_pane_stands_above_the_prompt(void)
{
    int pane = -1, prompt = -1;

    place_check(FYTIM_WORKPANE_ABOVE_PROMPT, &pane, &prompt);
    CHECK(pane >= 0);
    CHECK(prompt >= 0);
    CHECK(pane < prompt);
}

static void test_the_pane_stands_below_the_prompt(void)
{
    int pane = -1, prompt = -1;

    place_check(FYTIM_WORKPANE_BELOW_PROMPT, &pane, &prompt);
    CHECK(pane >= 0);
    CHECK(prompt >= 0);
    CHECK(prompt < pane);
}

/* The rows below the prompt are the pane's own: the tile keeps the height it
 * asked for, and the chrome above it is drawn whole. */
static void test_a_pane_below_keeps_its_rows(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int first = -1, last = -1, row, r;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_place(wp, FYTIM_WORKPANE_BELOW_PROMPT) ==
          FYTIM_OK);
    a = fytim_surface_open_in(wp, 3, 30);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    CHECK(fytim_set_input(h.ft, "zzzz") == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', NULL);
    CHECK(row >= 0);
    for(r = 0; r < ROWS; r++){
        run_of(&h, r, 'A', &first, &last);
        if(first < 0) continue;
        CHECK(first == 0);
        CHECK(last == 29);
    }
    /* Three rows of screen, and none of them over the prompt. */
    CHECK(find_char(&h, 'z', NULL) < row);
    vth_close(&h);
}


/* ---- the focus wash ----------------------------------------------------- */

/*
 * A tile can be given a background of its own, under whatever the program
 * draws: the ground of the tile, its margin and its chrome take it, a cell
 * the program coloured is mixed toward it, and the cursor stays legible.
 * Only cells can say any of this.
 */
#define WASH 0x0000ffu

/* The mix the library must make: @pct of @over on top of @under. */
static uint32_t mix_rgb(uint32_t under, uint32_t over, int pct)
{
    uint32_t out = 0;
    int i, a, b;

    for(i = 16; i >= 0; i -= 8){
        a = (int)((under >> i) & 0xff);
        b = (int)((over >> i) & 0xff);
        out |= (uint32_t)(a + (b - a) * pct / 100) << i;
    }
    return out;
}

/* The background the cell shows: a reversed cell shows its foreground. */
static union fyvt_color shown_bg(struct vth *h, int row, int col)
{
    struct fyvt_screen_cell c = cell_at(h, row, col);
    union fyvt_color out = c.attrs.reverse ? c.fg : c.bg;

    fyvt_screen_convert_color_to_rgb(h->vs, &out);
    return out;
}

static int shown_is(struct vth *h, int row, int col, uint32_t rgb)
{
    union fyvt_color want, got = shown_bg(h, row, col);

    fyvt_color_rgb(&want, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8),
                   (uint8_t)rgb);
    fyvt_screen_convert_color_to_rgb(h->vs, &want);
    return fyvt_color_is_equal(&got, &want);
}

/* A tile whose program painted @ch on a bg of @bg, with the wash at @mix. */
/* The tile the last wash_tile() made, for a test that changes it after. */
static struct fytim_surface *wash_last;

static struct fytim_surface *wash_tile(struct vth *h, uint32_t ch, uint32_t bg,
                                       int mix)
{
    struct fytim_cell cells[COLS];
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    int i, row;

    wp = fytim_workpane_create(h->ft);
    a = fytim_surface_open_in(wp, 3, 20);
    CHECK(fytim_surface_set_margin(a, "  ") == FYTIM_OK);
    CHECK(fytim_surface_set_top(a, "head") == FYTIM_OK);
    memset(cells, 0, sizeof cells);
    for(i = 0; i < 20; i++){
        cells[i].chars[0] = ch;
        cells[i].fg = FYTIM_COLOR_DEFAULT;
        cells[i].bg = bg;
        cells[i].width = 1;
    }
    for(row = 0; row < 3; row++)
        fytim_surface_put_row(a, row, cells, 20);
    CHECK(fytim_surface_set_bg(a, WASH, mix) == FYTIM_OK);
    wash_last = a;
    return a;
}

/* The ground of the tile: a cell the program left alone takes the wash. */
static void test_a_wash_grounds_the_tile(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', FYTIM_COLOR_DEFAULT, 50);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        /* the program's cell, the margin beside it, and the head above it */
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, WASH));
        CHECK(rgb_equal(&h, cell_at(&h, row, 0).bg, WASH));
        CHECK(rgb_equal(&h, cell_at(&h, row - 1, 0).bg, WASH));
        /* and the columns of the tile the grid does not cover */
        CHECK(rgb_equal(&h, cell_at(&h, row, 30).bg, WASH));
    }
    vth_close(&h);
}

/* A colour the program set is kept, mixed toward the wash. */
static void test_a_program_colour_is_mixed(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', 0xff0000u, 50);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg,
                        mix_rgb(0xff0000u, WASH, 50)));
        /* the ground beside it is the wash itself, not the mix */
        CHECK(rgb_equal(&h, cell_at(&h, row, 0).bg, WASH));
    }
    vth_close(&h);
}

/* A mix of zero is the program's colour: the wash grounds and nothing else. */
static void test_a_zero_mix_keeps_the_colour(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', 0xff0000u, 0);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, 0xff0000u));
        CHECK(rgb_equal(&h, cell_at(&h, row, 0).bg, WASH));
    }
    vth_close(&h);
}

/* A full mix is the wash: the program's colour is gone. */
static void test_a_full_mix_replaces_the_colour(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', 0xff0000u, 100);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0)
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, WASH));
    vth_close(&h);
}

/*
 * An indexed colour is the terminal's own palette entry, and this library
 * does not guess what the user set it to. It is left alone, whatever the
 * mix says.
 */
static void test_an_indexed_colour_is_left_alone(void)
{
    struct vth plain, washed;
    struct fyvt_screen_cell a, b;
    int row, col = -1;

    if(!vth_open(&plain)){ CHECK(0); return; }
    wash_tile(&plain, 'A', FYTIM_COLOR_INDEXED | 1u, 0);
    vth_pump(&plain);
    row = find_char(&plain, 'A', &col);
    CHECK(row >= 0);
    a = cell_at(&plain, row, col);
    fyvt_screen_convert_color_to_rgb(plain.vs, &a.bg);
    vth_close(&plain);

    if(!vth_open(&washed)){ CHECK(0); return; }
    wash_tile(&washed, 'A', FYTIM_COLOR_INDEXED | 1u, 100);
    vth_pump(&washed);
    row = find_char(&washed, 'A', &col);
    CHECK(row >= 0);
    b = cell_at(&washed, row, col);
    fyvt_screen_convert_color_to_rgb(washed.vs, &b.bg);
    vth_close(&washed);

    CHECK(fyvt_color_is_equal(&a.bg, &b.bg));
}

/*
 * The cursor is drawn by reversing a cell, so a wash forced onto it would
 * take away the only thing that shows it.
 */
static void test_the_cursor_stays_visible(void)
{
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    a = wash_tile(&h, 'A', FYTIM_COLOR_DEFAULT, 50);
    CHECK(fytim_surface_set_cursor(a, 1, 2, true) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        union fyvt_color cur = shown_bg(&h, row + 1, col + 2);
        union fyvt_color nb = shown_bg(&h, row + 1, col + 3);
        /* the cell beside it is washed ground; the cursor is not */
        CHECK(cell_at(&h, row + 1, col + 2).attrs.reverse == 1);
        CHECK(shown_is(&h, row + 1, col + 3, WASH));
        CHECK(!fyvt_color_is_equal(&cur, &nb));
    }
    vth_close(&h);
}

/*
 * A cell the program reversed shows its foreground as its background, so
 * that is where the wash goes: a reversed row grounds with the tile instead
 * of standing out of it.
 */
static void test_a_reversed_cell_is_washed(void)
{
    struct fytim_cell cells[COLS];
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int i, row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    memset(cells, 0, sizeof cells);
    for(i = 0; i < 20; i++){
        cells[i].chars[0] = 'R';
        cells[i].fg = FYTIM_COLOR_DEFAULT;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].attrs = FYTIM_ATTR_REVERSE;
        cells[i].width = 1;
    }
    fytim_surface_put_row(a, 0, cells, 20);
    CHECK(fytim_surface_set_bg(a, WASH, 50) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'R', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(shown_is(&h, row, col, WASH));
    }
    vth_close(&h);
}

/* Removing the wash gives the tile its ground back. */
static void test_a_wash_is_removed(void)
{
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    a = wash_tile(&h, 'A', FYTIM_COLOR_DEFAULT, 50);
    vth_pump(&h);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_DEFAULT, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0)
        CHECK(!rgb_equal(&h, cell_at(&h, row, col).bg, WASH));
    vth_close(&h);
}

/*
 * A head is chrome of the tile, not output of the program: the renderer that
 * draws it paints the theme's own ground, and that ground is not information
 * the tile has to keep. A washed tile owns every row of itself.
 */
static void test_a_head_takes_the_ground(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    /* a head drawn on a background of its own, as a theme draws one */
    CHECK(fytim_surface_set_top(a, "\x1b[38;2;255;255;255m\x1b[48;2;0;0;0m"
                                   "HEAD\x1b[0m") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, WASH, 50) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(shown_is(&h, row, col, WASH));
        /* what the head says is kept: only its ground is the tile's */
        CHECK(rgb_equal(&h, cell_at(&h, row, col).fg, 0xffffffu));
    }
    vth_close(&h);
}

/* ---- a ground the terminal names --------------------------------------- */

/*
 * A colour is a colour, and a terminal the user made light or dark has its
 * own. FYTIM_COLOR_REVERSED asks for the ground the terminal draws text in,
 * which contrasts on either and needs no colour support at all.
 */
static void test_a_reversed_ground_grounds_the_tile(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', FYTIM_COLOR_DEFAULT, 0);
    CHECK(fytim_surface_set_bg(wash_last, FYTIM_COLOR_REVERSED, 0) ==
          FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        /* the cell, the margin beside it and the ground past the grid */
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(cell_at(&h, row, 0).attrs.reverse == 1);
        CHECK(cell_at(&h, row, 30).attrs.reverse == 1);
    }
    vth_close(&h);
}

/*
 * A colour the program set is what it says, so it is kept - as the colour of
 * the text, because the ground is now the terminal's.
 */
static void test_a_reversed_ground_keeps_a_colour(void)
{
    struct fytim_cell cells[COLS];
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int i, row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    memset(cells, 0, sizeof cells);
    for(i = 0; i < 20; i++){
        cells[i].chars[0] = 'C';
        cells[i].fg = 0x00ff00u;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].width = 1;
    }
    fytim_surface_put_row(a, 0, cells, 20);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'C', &col);
    CHECK(row >= 0);
    if(row >= 0){
        /* reversed, so the stored background is what the text shows as */
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, 0x00ff00u));
    }
    vth_close(&h);
}

/* A cell with a ground of its own has nothing to gain and keeps it. */
static void test_a_reversed_ground_leaves_a_bg(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', 0xff0000u, 0);
    CHECK(fytim_surface_set_bg(wash_last, FYTIM_COLOR_REVERSED, 0) ==
          FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(cell_at(&h, row, col).attrs.reverse == 0);
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, 0xff0000u));
        /* and the ground beside it is still the terminal's */
        CHECK(cell_at(&h, row, 0).attrs.reverse == 1);
    }
    vth_close(&h);
}

/* The head is a row of the tile, so it stands on the same ground. */
static void test_a_reversed_ground_takes_the_head(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_top(a, "\x1b[38;2;0;255;0m\x1b[48;2;0;0;0m"
                                   "HEAD\x1b[0m") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        /* what the head said is kept, as the colour of its text */
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, 0x00ff00u));
    }
    vth_close(&h);
}

/* The cursor is the one cell that is not on the ground, so it is seen. */
static void test_a_reversed_ground_shows_the_cursor(void)
{
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wash_tile(&h, 'A', FYTIM_COLOR_DEFAULT, 0);
    CHECK(fytim_surface_set_bg(wash_last, FYTIM_COLOR_REVERSED, 0) ==
          FYTIM_OK);
    CHECK(fytim_surface_set_cursor(wash_last, 1, 2, true) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'A', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(cell_at(&h, row + 1, col + 2).attrs.reverse == 0);
        CHECK(cell_at(&h, row + 1, col + 3).attrs.reverse == 1);
    }
    vth_close(&h);
}

/*
 * Chrome is drawn dim, which is right for a rule beside content the terminal
 * itself grounds. On a ground of the tile's own it is not: dim darkens the
 * ground it stands on, and the head then reads as a grey band across a tile
 * whose body is not grey. A ground is one colour or it is not a ground.
 */
static void test_a_ground_is_not_dimmed(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_margin(a, "  ") == FYTIM_OK);
    CHECK(fytim_surface_set_top(a, "HEAD") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(cell_at(&h, row, col).attrs.dim == 0);
        /* the margin beside it stands on the same ground */
        CHECK(cell_at(&h, row, 0).attrs.reverse == 1);
        CHECK(cell_at(&h, row, 0).attrs.dim == 0);
    }
    vth_close(&h);
}

/* The same for a ground of the host's own: dim would darken that too. */
static void test_a_colour_ground_is_not_dimmed(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_top(a, "HEAD") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, WASH, 50) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, WASH));
        CHECK(cell_at(&h, row, col).attrs.dim == 0);
    }
    vth_close(&h);
}

/* A row that dims itself does not dim the ground either. */
static void test_a_dim_run_keeps_the_ground(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_top(a, "\x1b[2mHEAD\x1b[0m") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, WASH, 50) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, WASH));
        CHECK(cell_at(&h, row, col).attrs.dim == 0);
    }
    vth_close(&h);
}

/*
 * A tile that takes the keys does not take the prompt away with them. The
 * prompt is where the user goes back to, and a row that is there but not
 * lit says that better than a row that is gone: the ground moves to the
 * tile, and the prompt keeps its marker and what was typed into it.
 */
static void test_the_prompt_stays_when_a_tile_has_the_keys(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int prow, trow, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "PROMPT ") == FYTIM_OK);
    CHECK(fytim_set_input(h.ft, "typed") == FYTIM_OK);
    CHECK(fytim_set_prompt_bg(h.ft, FYTIM_COLOR_REVERSED) == FYTIM_OK);
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 20);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    CHECK(fytim_surface_set_keys(a, true) == FYTIM_OK);
    vth_pump(&h);

    /* the prompt is still drawn, marker and all */
    prow = find_char(&h, 'P', &col);
    CHECK(prow >= 0);
    trow = find_char(&h, 'A', NULL);
    CHECK(trow >= 0);
    if(prow >= 0 && trow >= 0){
        /* under the tile, which is where the prompt always is */
        CHECK(trow < prow);
        /* the tile is lit and the prompt is not */
        CHECK(cell_at(&h, trow, 0).attrs.reverse == 1);
        CHECK(cell_at(&h, prow, col).attrs.reverse == 0);
    }
    vth_close(&h);
}

/* Giving the keys back lights the prompt again. */
static void test_the_prompt_is_lit_when_the_keys_return(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int prow, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "PROMPT ") == FYTIM_OK);
    CHECK(fytim_set_prompt_bg(h.ft, FYTIM_COLOR_REVERSED) == FYTIM_OK);
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 20);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    CHECK(fytim_surface_set_keys(a, true) == FYTIM_OK);
    vth_pump(&h);
    CHECK(fytim_surface_set_keys(a, false) == FYTIM_OK);
    vth_pump(&h);

    prow = find_char(&h, 'P', &col);
    CHECK(prow >= 0);
    if(prow >= 0)
        CHECK(cell_at(&h, prow, col).attrs.reverse == 1);
    vth_close(&h);
}

/*
 * Nothing moves when the keys do. The prompt block stands whether it is lit
 * or not, so a tile is granted the same rows before and after it takes the
 * keys: a program that is being watched does not reflow because the user
 * looked at it.
 */
static void test_the_keys_do_not_move_anything(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int before = 0, during = 0, after = 0, row_before, row_during;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "PROMPT ") == FYTIM_OK);
    CHECK(fytim_set_prompt_bg(h.ft, FYTIM_COLOR_REVERSED) == FYTIM_OK);
    wp = fytim_workpane_create(h.ft);
    /* Ask for the whole screen: what is granted then says what the chrome
     * left, which is the thing that must not change. */
    a = fytim_surface_open_in(wp, ROWS, 20);
    paint(a, 'A', FYTIM_COLOR_DEFAULT);
    vth_pump(&h);
    CHECK(fytim_surface_granted_rows(a, &before) == FYTIM_OK);
    row_before = find_char(&h, 'A', NULL);

    CHECK(fytim_surface_set_keys(a, true) == FYTIM_OK);
    vth_pump(&h);
    CHECK(fytim_surface_granted_rows(a, &during) == FYTIM_OK);
    row_during = find_char(&h, 'A', NULL);

    CHECK(fytim_surface_set_keys(a, false) == FYTIM_OK);
    vth_pump(&h);
    CHECK(fytim_surface_granted_rows(a, &after) == FYTIM_OK);

    CHECK(before > 0);
    CHECK(before < ROWS);   /* the chrome took rows, or this proves nothing */
    CHECK(during == before);
    CHECK(after == before);
    /* and the screen did not slide up or down under it */
    CHECK(row_before >= 0);
    CHECK(row_during == row_before);
    vth_close(&h);
}

/*
 * A head that carries reverse of its own - a card, a marked word - is still
 * a row of the tile. Reverse is how a run says "my ground is my foreground",
 * and the tile's ground is not the run's to keep: what it says survives as
 * the colour of its text.
 */
static void test_a_reversed_head_takes_the_ground(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    /* white on green, reversed: the run draws a green card of its own */
    CHECK(fytim_surface_set_top(a, "\x1b[38;2;255;255;255m\x1b[48;2;0;255;0m"
                                   "\x1b[7mHEAD\x1b[0m") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, WASH, 50) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0)
        CHECK(shown_is(&h, row, col, WASH));
    vth_close(&h);
}

/* The same on a ground the terminal names. */
static void test_a_reversed_head_takes_a_named_ground(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_top(a, "\x1b[38;2;0;255;0m\x1b[7mHEAD\x1b[0m") ==
          FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        /* the ground of the row, and the ground of the tile beside it */
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(cell_at(&h, row, 19).attrs.reverse == 1);
    }
    vth_close(&h);
}

/*
 * A head whose renderer painted a background of its own - a fenced block, a
 * card - on a ground the terminal names. The ground of the tile is reversed,
 * so every run of the row starts out reversed too; a run that then declares
 * a background must not be read as one that already has a ground of its own,
 * or it keeps the theme's and the head reads as a band of it.
 */
static void test_a_head_bg_takes_a_named_ground(void)
{
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct vth h;
    int row, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 20);
    CHECK(fytim_surface_set_top(a, "\x1b[38;2;0;255;0m\x1b[48;2;40;42;46m"
                                   "HEAD\x1b[0m") == FYTIM_OK);
    CHECK(fytim_surface_set_bg(a, FYTIM_COLOR_REVERSED, 0) == FYTIM_OK);
    vth_pump(&h);
    row = find_char(&h, 'H', &col);
    CHECK(row >= 0);
    if(row >= 0){
        /* the row stands on the ground, and so does the tile beside it */
        CHECK(cell_at(&h, row, col).attrs.reverse == 1);
        CHECK(cell_at(&h, row, 19).attrs.reverse == 1);
        /* what it said is the colour of its text, not of its ground */
        CHECK(rgb_equal(&h, cell_at(&h, row, col).bg, 0x00ff00u));
    }
    vth_close(&h);
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "two_tiles_stand_side_by_side", test_two_tiles_stand_side_by_side },
    { "the_tiles_share_every_row",    test_the_tiles_share_every_row },
    { "a_wide_screen_is_clipped_to_its_tile",
      test_a_wide_screen_is_clipped_to_its_tile },
    { "the_rule_sits_between_the_tiles",
      test_the_rule_sits_between_the_tiles },
    { "a_colour_stops_at_the_tile_edge",
      test_a_colour_stops_at_the_tile_edge },
    { "the_grid_puts_the_third_below", test_the_grid_puts_the_third_below },
    { "a_zoomed_tile_owns_the_rows",  test_a_zoomed_tile_owns_the_rows },
    { "a_title_stays_over_its_tile",  test_a_title_stays_over_its_tile },
    { "a_head_of_two_rows_draws_both",
      test_a_head_of_two_rows_draws_both },
    { "a_short_tile_sheds_the_last_head_row",
      test_a_short_tile_sheds_the_last_head_row },
    { "a_head_with_no_content_takes_no_row",
      test_a_head_with_no_content_takes_no_row },
    { "a_styled_head_is_not_dimmed", test_a_styled_head_is_not_dimmed },
    { "a_margin_is_drawn_inside_the_tile",
      test_a_margin_is_drawn_inside_the_tile },
    { "regression_margin_spans_surface_chrome",
      test_regression_margin_spans_surface_chrome },
    { "two_band_tiles_stand_side_by_side",
      test_two_band_tiles_stand_side_by_side },
    { "the_pane_stands_above_the_prompt",
      test_the_pane_stands_above_the_prompt },
    { "the_pane_stands_below_the_prompt",
      test_the_pane_stands_below_the_prompt },
    { "a_pane_below_keeps_its_rows", test_a_pane_below_keeps_its_rows },
    { "a_wash_grounds_the_tile", test_a_wash_grounds_the_tile },
    { "a_program_colour_is_mixed", test_a_program_colour_is_mixed },
    { "a_zero_mix_keeps_the_colour", test_a_zero_mix_keeps_the_colour },
    { "a_full_mix_replaces_the_colour", test_a_full_mix_replaces_the_colour },
    { "an_indexed_colour_is_left_alone",
      test_an_indexed_colour_is_left_alone },
    { "the_cursor_stays_visible", test_the_cursor_stays_visible },
    { "a_reversed_cell_is_washed", test_a_reversed_cell_is_washed },
    { "a_head_takes_the_ground", test_a_head_takes_the_ground },
    { "a_ground_is_not_dimmed", test_a_ground_is_not_dimmed },
    { "a_reversed_head_takes_the_ground",
      test_a_reversed_head_takes_the_ground },
    { "a_head_bg_takes_a_named_ground",
      test_a_head_bg_takes_a_named_ground },
    { "a_reversed_head_takes_a_named_ground",
      test_a_reversed_head_takes_a_named_ground },
    { "a_dim_run_keeps_the_ground", test_a_dim_run_keeps_the_ground },
    { "the_prompt_stays_when_a_tile_has_the_keys",
      test_the_prompt_stays_when_a_tile_has_the_keys },
    { "the_keys_do_not_move_anything", test_the_keys_do_not_move_anything },
    { "the_prompt_is_lit_when_the_keys_return",
      test_the_prompt_is_lit_when_the_keys_return },
    { "a_colour_ground_is_not_dimmed",
      test_a_colour_ground_is_not_dimmed },
    { "a_reversed_ground_grounds_the_tile",
      test_a_reversed_ground_grounds_the_tile },
    { "a_reversed_ground_keeps_a_colour",
      test_a_reversed_ground_keeps_a_colour },
    { "a_reversed_ground_leaves_a_bg", test_a_reversed_ground_leaves_a_bg },
    { "a_reversed_ground_takes_the_head",
      test_a_reversed_ground_takes_the_head },
    { "a_reversed_ground_shows_the_cursor",
      test_a_reversed_ground_shows_the_cursor },
    { "a_wash_is_removed", test_a_wash_is_removed },
    { "a_wide_row_is_clipped_to_its_tile",
      test_a_wide_row_is_clipped_to_its_tile },
};

int main(int argc, char **argv)
{
    size_t i;
    if(argc > 1 && strcmp(argv[1], "--list") == 0){
        for(i = 0; i < sizeof cases / sizeof cases[0]; i++)
            printf("%s\n", cases[i].name);
        return 0;
    }
    for(i = 0; i < sizeof cases / sizeof cases[0]; i++){
        if(argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        printf("== %s\n", cases[i].name);
        cases[i].fn();
    }
    return failures ? 1 : 0;
}
