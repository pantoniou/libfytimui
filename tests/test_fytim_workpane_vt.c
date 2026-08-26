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
    { "a_margin_is_drawn_inside_the_tile",
      test_a_margin_is_drawn_inside_the_tile },
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
