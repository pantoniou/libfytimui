/*
 * test_fytim_surface_vt.c - what a published cell looks like on a terminal.
 *
 * A grep over the emitted escapes cannot prove a colour reached a cell, that
 * a blank cell kept its background, or that a wide glyph left the cell after
 * it alone. This replays every byte the library emits into libvterm and reads
 * the grid back, which is the only reading that answers those.
 *
 * Only built when libvterm is available.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"
#include <vterm.h>

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
    VTerm *vt;
    VTermScreen *vs;
    int in[2], out[2];
};

static int vth_open(struct vth *h)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    h->vt = vterm_new(ROWS, COLS);
    vterm_set_utf8(h->vt, 1);
    h->vs = vterm_obtain_screen(h->vt);
    vterm_screen_reset(h->vs, 1);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

static void vth_close(struct vth *h)
{
    fytim_destroy(h->ft);
    if(h->vt) vterm_free(h->vt);
    close(h->in[0]); close(h->in[1]);
    close(h->out[0]); close(h->out[1]);
}

static void vth_pump(struct vth *h)
{
    char buf[65536];
    ssize_t n;
    CHECK(fytim_pump(h->ft) == FYTIM_OK);
    while((n = read(h->out[0], buf, sizeof buf)) > 0)
        vterm_input_write(h->vt, buf, (size_t)n);
    vterm_screen_flush_damage(h->vs);
}

static VTermScreenCell cell_at(struct vth *h, int row, int col)
{
    VTermScreenCell c;
    VTermPos pos;
    memset(&c, 0, sizeof c);
    pos.row = row;
    pos.col = col;
    vterm_screen_get_cell(h->vs, pos, &c);
    return c;
}

/* The first screen row whose first cell holds @ch. -1 when there is none. */
static int row_starting_with(struct vth *h, uint32_t ch)
{
    int r;
    for(r = 0; r < ROWS; r++)
        if(cell_at(h, r, 0).chars[0] == ch) return r;
    return -1;
}

static int rgb_equal(struct vth *h, VTermColor got, uint32_t want_rgb)
{
    VTermColor want;
    vterm_color_rgb(&want, (uint8_t)(want_rgb >> 16),
                    (uint8_t)(want_rgb >> 8), (uint8_t)want_rgb);
    vterm_screen_convert_color_to_rgb(h->vs, &got);
    vterm_screen_convert_color_to_rgb(h->vs, &want);
    return vterm_color_is_equal(&got, &want);
}

static void blank_row(struct fytim_cell *cells, int n)
{
    int i;
    memset(cells, 0, (size_t)n * sizeof *cells);
    for(i = 0; i < n; i++){
        cells[i].fg = FYTIM_COLOR_DEFAULT;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].width = 1;
    }
}

/* A published colour reaches the cell, and the cells after the last glyph
 * keep the terminal's own background rather than the row's colour. */
static void test_colour_reaches_the_cell(void)
{
    struct fytim_cell cells[8];
    struct fytim_surface *s;
    VTermScreenCell c;
    struct vth h;
    int row, i;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 1, 8);
    CHECK(s != NULL);
    blank_row(cells, 8);
    for(i = 0; i < 3; i++){
        cells[i].chars[0] = (uint32_t)('X' + i);
        cells[i].fg = 0x00FF8000u;
        cells[i].bg = 0x00102030u;
        cells[i].attrs = FYTIM_ATTR_BOLD | FYTIM_ATTR_UNDERLINE;
    }
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 'X');
    CHECK(row >= 0);
    if(row >= 0){
        c = cell_at(&h, row, 0);
        CHECK(c.chars[0] == 'X');
        CHECK(rgb_equal(&h, c.fg, 0x00FF8000u));
        CHECK(rgb_equal(&h, c.bg, 0x00102030u));
        CHECK(c.attrs.bold);
        CHECK(c.attrs.underline != 0);
        c = cell_at(&h, row, 2);
        CHECK(c.chars[0] == 'Z');
        /* Past the glyphs: blank, and NOT carrying the styled background. */
        c = cell_at(&h, row, 3);
        CHECK(c.chars[0] == 0 || c.chars[0] == ' ');
        CHECK(!rgb_equal(&h, c.bg, 0x00102030u));
        CHECK(!c.attrs.bold);
    }
    fytim_surface_close(s);
    vth_close(&h);
}

/* A double-width glyph occupies two columns and the text after it is not
 * pushed: the filler cell is stepped over, not drawn as a space. */
static void test_wide_glyph_keeps_the_row(void)
{
    struct fytim_cell cells[6];
    struct fytim_surface *s;
    VTermScreenCell c;
    struct vth h;
    int row;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 1, 6);
    blank_row(cells, 6);
    cells[0].chars[0] = 'q';
    cells[1].chars[0] = 0x4E2D;    /* a double-width glyph */
    cells[1].width = 2;
    cells[3].chars[0] = 'e';
    CHECK(fytim_surface_put_row(s, 0, cells, 6) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 'q');
    CHECK(row >= 0);
    if(row >= 0){
        c = cell_at(&h, row, 1);
        CHECK(c.chars[0] == 0x4E2D);
        CHECK(c.width == 2);
        /* Column 2 is the filler of the wide glyph; 'e' stays at column 3. */
        c = cell_at(&h, row, 3);
        CHECK(c.chars[0] == 'e');
    }
    fytim_surface_close(s);
    vth_close(&h);
}

/* The cursor is a reverse-video cell, and only that one cell. */
static void test_cursor_is_a_reverse_cell(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *s;
    VTermScreenCell c;
    struct vth h;
    int row;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 1, 4);
    blank_row(cells, 4);
    cells[0].chars[0] = 'p';
    cells[1].chars[0] = 'r';
    cells[2].chars[0] = 'n';
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_surface_set_cursor(s, 0, 1, true) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 'p');
    CHECK(row >= 0);
    if(row >= 0){
        c = cell_at(&h, row, 1);
        CHECK(c.chars[0] == 'r');
        CHECK(c.attrs.reverse);
        c = cell_at(&h, row, 0);
        CHECK(!c.attrs.reverse);
        c = cell_at(&h, row, 2);
        CHECK(!c.attrs.reverse);
    }
    /* Hidden again: no reverse cell is left behind. */
    CHECK(fytim_surface_set_cursor(s, 0, 1, false) == FYTIM_OK);
    vth_pump(&h);
    row = row_starting_with(&h, 'p');
    if(row >= 0){
        c = cell_at(&h, row, 1);
        CHECK(!c.attrs.reverse);
    }
    fytim_surface_close(s);
    vth_close(&h);
}

/*
 * A surface taller than the region shows its LAST rows: the bottom of a
 * screen is where a program is working.
 */
static void test_short_region_keeps_the_last_rows(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *s;
    struct vth h;
    int i;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 6, 4);
    for(i = 0; i < 6; i++){
        blank_row(cells, 4);
        cells[0].chars[0] = (uint32_t)('0' + i);
        CHECK(fytim_surface_put_row(s, i, cells, 4) == FYTIM_OK);
    }
    CHECK(fytim_surface_set_max_rows(s, 2) == FYTIM_OK);
    vth_pump(&h);

    CHECK(row_starting_with(&h, '5') >= 0);
    CHECK(row_starting_with(&h, '4') >= 0);
    CHECK(row_starting_with(&h, '0') < 0);
    CHECK(row_starting_with(&h, '3') < 0);
    fytim_surface_close(s);
    vth_close(&h);
}

/* Two surfaces stack in the order they were opened, above the prompt. */
static void test_two_surfaces_stack_in_order(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *a, *b;
    struct vth h;
    int ra, rb;

    if(!vth_open(&h)){ CHECK(0); return; }
    a = fytim_surface_open(h.ft, 1, 4);
    b = fytim_surface_open(h.ft, 1, 4);
    blank_row(cells, 4);
    cells[0].chars[0] = 'A';
    CHECK(fytim_surface_put_row(a, 0, cells, 4) == FYTIM_OK);
    blank_row(cells, 4);
    cells[0].chars[0] = 'B';
    CHECK(fytim_surface_put_row(b, 0, cells, 4) == FYTIM_OK);
    vth_pump(&h);

    ra = row_starting_with(&h, 'A');
    rb = row_starting_with(&h, 'B');
    CHECK(ra >= 0);
    CHECK(rb >= 0);
    CHECK(ra < rb);   /* oldest nearest the transcript, newest by the prompt */
    fytim_surface_close(a);
    fytim_surface_close(b);
    vth_close(&h);
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "colour_reaches_the_cell",        test_colour_reaches_the_cell },
    { "wide_glyph_keeps_the_row",       test_wide_glyph_keeps_the_row },
    { "cursor_is_a_reverse_cell",       test_cursor_is_a_reverse_cell },
    { "short_region_keeps_the_last_rows", test_short_region_keeps_the_last_rows },
    { "two_surfaces_stack_in_order",    test_two_surfaces_stack_in_order },
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
