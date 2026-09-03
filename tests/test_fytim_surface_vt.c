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
		    .struct_size = sizeof(struct fyvt_cfg), .rows = ROWS, .cols = COLS }
		    );
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

/* The first screen row whose first cell holds @ch. -1 when there is none. */
static int row_starting_with(struct vth *h, uint32_t ch)
{
    int r;
    for(r = 0; r < ROWS; r++)
        if(cell_at(h, r, 0).chars[0] == ch) return r;
    return -1;
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
    struct fyvt_screen_cell c;
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

/* The row that holds @text, or -1. The screen is read, not the bytes: an
 * unchanged row emits nothing, so a byte capture cannot say what is on screen.
 */
static int row_with_text(struct vth *h, const char *text)
{
    char line[COLS + 1];
    struct fyvt_rect rect;
    int r;

    for(r = 0; r < ROWS; r++){
        rect.start_row = r;
        rect.end_row = r + 1;
        rect.start_col = 0;
        rect.end_col = COLS;
        memset(line, 0, sizeof line);
        fyvt_screen_get_text(h->vs, line, sizeof line - 1, rect);
        if(strstr(line, text)) return r;
    }
    return -1;
}

/*
 * The prompt stays while a surface holds the keys. It is where the user goes
 * back to, so it keeps its row and its marker; what says the keys are
 * elsewhere is that it is no longer lit, not that it is gone.
 */
static void test_prompt_stays_while_a_surface_holds_keys(void)
{
    struct fytim_surface *s;
    struct vth h;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "PROMPTMARK ") == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") >= 0);

    s = fytim_surface_open(h.ft, 4, 8);
    CHECK(s != NULL);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") >= 0);

    /* Giving the keys back changes nothing about it being there. */
    CHECK(fytim_surface_set_keys(s, false) == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") >= 0);

    fytim_surface_close(s);
    vth_close(&h);
}

/*
 * A host that nobody types into asks for no prompt, and there is none - and
 * with it go the separators that framed it, so a surface reaches the row the
 * prompt would have taken.
 */
static void test_prompt_leaves_when_it_is_not_wanted(void)
{
    struct fytim_cell cells[8];
    struct fytim_surface *s;
    struct vth h;
    int rows = 0, granted, i;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "PROMPTMARK ") == FYTIM_OK);
    /* Ask for the whole screen, so what is granted says what is left over. */
    s = fytim_surface_open(h.ft, ROWS, 8);
    CHECK(s != NULL);
    blank_row(cells, 8);
    for(i = 0; i < 3; i++)
        cells[i].chars[0] = (uint32_t)('P' + i);
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") >= 0);
    CHECK(row_starting_with(&h, 'P') >= 0);
    CHECK(fytim_surface_granted_rows(s, &granted) == FYTIM_OK);
    CHECK(granted > 0);

    CHECK(fytim_set_prompt_enabled(h.ft, false) == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") < 0);
    /*
     * The prompt and the separators that framed it go to the content, and
     * with nothing left in the header or the status those rows follow: the
     * surface has the whole screen.
     */
    CHECK(fytim_surface_granted_rows(s, &rows) == FYTIM_OK);
    CHECK(rows == ROWS);
    CHECK(rows > granted);

    /* Asking for it again brings it back. */
    CHECK(fytim_set_prompt_enabled(h.ft, true) == FYTIM_OK);
    vth_pump(&h);
    CHECK(row_with_text(&h, "PROMPTMARK") >= 0);
    CHECK(fytim_surface_granted_rows(s, &rows) == FYTIM_OK);
    CHECK(rows == granted);

    fytim_surface_close(s);
    vth_close(&h);
}

/*
 * A chrome row with nothing in it is not a row - for a host that asked for no
 * prompt, which is the one that wants the whole screen. A surface that merely
 * holds the keys keeps the screen it had: rows given to it when the keys
 * arrived would be taken back when they left, and everything on the screen
 * would move twice for a keystroke.
 */
static void test_empty_chrome_gives_its_rows_back(void)
{
    struct fytim_cell cells[8];
    struct fytim_surface *s;
    struct vth h;
    int row, i;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, ROWS, 8);
    CHECK(s != NULL);
    CHECK(fytim_set_prompt_enabled(h.ft, false) == FYTIM_OK);
    blank_row(cells, 8);
    for(i = 0; i < 8; i++)
        cells[i].chars[0] = 'b';
    /* The last row of the grid: it can only be seen if nothing is below. */
    CHECK(fytim_surface_put_row(s, ROWS - 1, cells, 8) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 'b');
    CHECK(row == ROWS - 1);
    fytim_surface_close(s);
    vth_close(&h);
}

static void test_margin_shifts_the_grid(void)
{
    struct fytim_cell cells[6];
    struct fytim_surface *s;
    struct fyvt_screen_cell c;
    struct vth h;
    int row;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 6);
    CHECK(s != NULL);
    CHECK(fytim_surface_set_margin(s, "\xe2\x94\x82 ") == FYTIM_OK);
    blank_row(cells, 6);
    cells[0].chars[0] = 'S';
    cells[1].chars[0] = 'T';
    CHECK(fytim_surface_put_row(s, 0, cells, 6) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 0x2502);
    CHECK(row >= 0);
    if(row >= 0){
        /* The bar, a space, then the first cell of the grid. */
        c = cell_at(&h, row, 1);
        CHECK(c.chars[0] == ' ' || c.chars[0] == 0);
        c = cell_at(&h, row, 2);
        CHECK(c.chars[0] == 'S');
        c = cell_at(&h, row, 3);
        CHECK(c.chars[0] == 'T');
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
    struct fyvt_screen_cell c;
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

/* A base character and its combining mark are one cell, not two. */
static void test_combining_stays_one_cell(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *s;
    struct fyvt_screen_cell c;
    struct vth h;
    int row;

    if(!vth_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 1, 4);
    blank_row(cells, 4);
    cells[0].chars[0] = 'e';
    cells[0].chars[1] = 0x0301;    /* combining acute */
    cells[1].chars[0] = '!';
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    vth_pump(&h);

    row = row_starting_with(&h, 'e');
    CHECK(row >= 0);
    if(row >= 0){
        c = cell_at(&h, row, 0);
        CHECK(c.chars[0] == 'e');
        CHECK(c.chars[1] == 0x0301);
        c = cell_at(&h, row, 1);
        CHECK(c.chars[0] == '!');
    }
    fytim_surface_close(s);
    vth_close(&h);
}

/* The cursor is a reverse-video cell, and only that one cell. */
static void test_cursor_is_a_reverse_cell(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *s;
    struct fyvt_screen_cell c;
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
    { "empty_chrome_gives_its_rows_back", test_empty_chrome_gives_its_rows_back },
    { "margin_shifts_the_grid",         test_margin_shifts_the_grid },
    { "prompt_stays_while_a_surface_holds_keys",
      test_prompt_stays_while_a_surface_holds_keys },
    { "prompt_leaves_when_it_is_not_wanted",
      test_prompt_leaves_when_it_is_not_wanted },
    { "wide_glyph_keeps_the_row",       test_wide_glyph_keeps_the_row },
    { "combining_stays_one_cell",       test_combining_stays_one_cell },
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
