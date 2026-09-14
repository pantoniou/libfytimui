/*
 * test_fytim_page_vt.c - what a page of slots looks like on a terminal.
 *
 * A slot is a grant of cells, and only a terminal can say whether a component
 * stayed inside the cells it was given. This replays every byte the library
 * emits into libfyvterm and reads the grid back.
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

static uint32_t ch_at(struct vth *h, int row, int col)
{
    struct fyvt_screen_cell c;
    struct fyvt_pos pos;
    memset(&c, 0, sizeof c);
    pos.row = row;
    pos.col = col;
    fyvt_screen_get_cell(h->vs, pos, &c);
    return c.chars[0];
}

/* The first screen row holding @ch anywhere, and its column. -1 if none. */
static int find_char(struct vth *h, uint32_t ch, int *colp)
{
    int r, c;
    for(r = 0; r < ROWS; r++)
        for(c = 0; c < COLS; c++)
            if(ch_at(h, r, c) == ch){
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
        if(ch_at(h, row, c) == ch){
            if(*first < 0) *first = c;
            *last = c;
        }
}

static void paint(struct fytim_surface *sf, uint32_t ch, int rows, int cols)
{
    struct fytim_cell *row = calloc((size_t)cols, sizeof *row);
    int r, c;
    for(c = 0; c < cols; c++){
        row[c].chars[0] = ch;
        row[c].fg = FYTIM_COLOR_DEFAULT;
        row[c].bg = FYTIM_COLOR_DEFAULT;
    }
    for(r = 0; r < rows; r++)
        CHECK(fytim_surface_put_row(sf, r, row, cols) == FYTIM_OK);
    free(row);
}

/* The rows of a page are drawn in order, one screen row each. */
static void test_the_rows_stand_in_order(void)
{
    struct vth h;
    int ra, rb, rc, col = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "A\nB\nC\n", 6, NULL, 0) == FYTIM_OK);
    vth_pump(&h);
    ra = find_char(&h, 'A', &col);
    CHECK(col == 0);
    rb = find_char(&h, 'B', NULL);
    rc = find_char(&h, 'C', NULL);
    CHECK(ra >= 0 && rb == ra + 1 && rc == rb + 1);
    vth_close(&h);
}

/* A surface in a slot is drawn at the slot and cut at its edge. */
static void test_a_surface_stays_in_its_slot(void)
{
    struct vth h;
    struct fytim_surface *sf;
    struct fytim_page_region r = {
        .id = "work", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 4, .width = 10, .height = 2
    };
    int top, first, last;

    if(!vth_open(&h)){ CHECK(0); return; }
    sf = fytim_surface_open(h.ft, 4, 60);
    paint(sf, 'S', 4, 60);
    CHECK(fytim_surface_bind(sf, "work") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "T\n\n\nF\n", 6, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(top >= 0);
    run_of(&h, top + 1, 'S', &first, &last);
    CHECK(first == 4 && last == 13);
    run_of(&h, top + 2, 'S', &first, &last);
    CHECK(first == 4 && last == 13);
    run_of(&h, top + 3, 'S', &first, &last);
    CHECK(first == -1);
    CHECK(find_char(&h, 'F', NULL) == top + 3);
    vth_close(&h);
}

/* The tail shows its last rows in a slot shorter than it is. */
static void test_the_tail_shows_its_last_rows(void)
{
    struct vth h;
    struct fytim_page_region r = {
        .id = "tail", .kind = FYTIM_PAGE_SLOT,
        .row = 0, .col = 0, .width = 80, .height = 2
    };
    int top;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_tail_set(h.ft, "1\n2\n3\n4\n", 8) == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "\n\nE\n", 4, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'E', NULL);
    CHECK(top >= 2);
    CHECK(ch_at(&h, top - 2, 0) == '3');
    CHECK(ch_at(&h, top - 1, 0) == '4');
    CHECK(find_char(&h, '2', NULL) == -1);
    vth_close(&h);
}

/* A pane in a slot tiles its screens inside the columns of the slot. */
static void test_a_pane_tiles_inside_its_slot(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fytim_page_region r = {
        .id = "pane", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 20, .width = 40, .height = 3
    };
    int top, fa, la, fb, lb;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_min_tile_cols(wp, 10) == FYTIM_OK);
    a = fytim_surface_open_in(wp, 3, 80);
    b = fytim_surface_open_in(wp, 3, 80);
    paint(a, 'a', 3, 80);
    paint(b, 'b', 3, 80);
    CHECK(fytim_workpane_bind(wp, "pane") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "T\n\n\n\n", 5, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(top >= 0);
    run_of(&h, top + 1, 'a', &fa, &la);
    run_of(&h, top + 1, 'b', &fb, &lb);
    CHECK(fa == 20);
    CHECK(la < fb);
    CHECK(lb == 59);
    vth_close(&h);
}

/* A slot with nothing bound is blank: the rows under it show nothing. */
static void test_an_unbound_slot_is_blank(void)
{
    struct vth h;
    struct fytim_page_region r = {
        .id = "nothing", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 80, .height = 2
    };
    int top, c;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "T\n\n\nF\n", 6, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(top >= 0);
    for(c = 0; c < COLS; c++){
        CHECK(ch_at(&h, top + 1, c) == 0 || ch_at(&h, top + 1, c) == ' ');
        CHECK(ch_at(&h, top + 2, c) == 0 || ch_at(&h, top + 2, c) == ' ');
    }
    CHECK(find_char(&h, 'F', NULL) == top + 3);
    vth_close(&h);
}

/* A page that is cleared gives the screen back to the band stack. */
static void test_a_cleared_page_restores_the_stack(void)
{
    struct vth h;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, "H") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "P\n", 2, NULL, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(find_char(&h, 'P', NULL) >= 0);
    CHECK(find_char(&h, 'H', NULL) == -1);
    fytim_page_clear(h.ft);
    vth_pump(&h);
    CHECK(find_char(&h, 'H', NULL) >= 0);
    CHECK(find_char(&h, 'P', NULL) == -1);
    vth_close(&h);
}

/* The head and the foot of a tile page stand around the screen of the tile. */
static void test_a_tile_page_frames_its_screen(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *sf;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 80, .height = 1
    };
    int head, first, last;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    sf = fytim_surface_open_in(wp, 2, 80);
    paint(sf, 'S', 2, 80);
    CHECK(fytim_surface_set_top(sf, "OLD") == FYTIM_OK);
    CHECK(fytim_surface_set_page(sf, "HHHH\n\nFFFF\n", 11, &r, 1) ==
          FYTIM_OK);
    vth_pump(&h);
    head = find_char(&h, 'H', NULL);
    CHECK(head >= 0);
    run_of(&h, head + 1, 'S', &first, &last);
    CHECK(first == 0);
    run_of(&h, head + 2, 'S', &first, &last);
    CHECK(first == 0);
    CHECK(find_char(&h, 'F', NULL) == head + 3);
    CHECK(find_char(&h, 'O', NULL) == -1);
    vth_close(&h);
}

/* A tile page in an explicit grid frames its screen as in the automatic one. */
static void test_a_tile_page_frames_its_screen_in_a_grid(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *sf;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 2, .col = 0, .width = 80, .height = 1
    };
    int head, first, last;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_grid(wp, 1, 1) == FYTIM_OK);
    sf = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_surface_set_cell(sf, 0, 0, 1, 1) == FYTIM_OK);
    paint(sf, 'S', 3, 80);
    CHECK(fytim_surface_set_page(sf, "HHHH\nCCCC\n\nFFFF\n", 16, &r, 1) ==
          FYTIM_OK);
    vth_pump(&h);
    head = find_char(&h, 'H', NULL);
    CHECK(head >= 0);
    CHECK(find_char(&h, 'C', NULL) == head + 1);
    run_of(&h, head + 2, 'S', &first, &last);
    CHECK(first == 0);
    CHECK(find_char(&h, 'F', NULL) == head + 5);
    vth_close(&h);
}

/* The screen view draws the grid from the top of the tile; the head view
 * draws the head and leaves the screen blank. */
static void test_a_page_view_draws_its_part(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *sf;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 80, .height = 1
    };
    int head, top;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "T\n", 2, NULL, 0) == FYTIM_OK);
    wp = fytim_workpane_create(h.ft);
    sf = fytim_surface_open_in(wp, 2, 80);
    paint(sf, 'S', 2, 80);
    CHECK(fytim_surface_set_page(sf, "HHHH\n\nFFFF\n", 11, &r, 1) ==
          FYTIM_OK);
    fytim_page_clear(h.ft);
    vth_pump(&h);
    head = find_char(&h, 'H', NULL);
    CHECK(head >= 0 && find_char(&h, 'S', NULL) == head + 1);

    CHECK(fytim_surface_set_page_view(sf, FYTIM_PAGE_VIEW_SCREEN) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'S', NULL);
    CHECK(top == head);
    CHECK(find_char(&h, 'H', NULL) == -1);
    CHECK(find_char(&h, 'F', NULL) == -1);

    CHECK(fytim_surface_set_page_view(sf, FYTIM_PAGE_VIEW_HEAD) == FYTIM_OK);
    vth_pump(&h);
    CHECK(find_char(&h, 'H', NULL) == head);
    CHECK(find_char(&h, 'S', NULL) == -1);
    CHECK(find_char(&h, 'F', NULL) == -1);
    vth_close(&h);
}

/* Two tiles bound to slots stand where the page put them, each with its head
 * and its screen, and no screen runs into the other. */
static void test_bound_tiles_stand_in_their_slots(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fytim_page_region r[2] = {
        { .id = "tile:a", .kind = FYTIM_PAGE_SLOT, .row = 1, .col = 0,
          .width = 20, .height = 3 },
        { .id = "tile:b", .kind = FYTIM_PAGE_SLOT, .row = 1, .col = 25,
          .width = 30, .height = 3 },
    };
    int top, first, last;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 80);
    b = fytim_surface_open_in(wp, 2, 80);
    paint(a, 'a', 2, 80);
    paint(b, 'b', 2, 80);
    CHECK(fytim_surface_set_top(a, "HEADA") == FYTIM_OK);
    CHECK(fytim_surface_bind(a, "tile:a") == FYTIM_OK);
    CHECK(fytim_surface_bind(b, "tile:b") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "T\n\n\n\nF\n", 6, r, 2) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(top >= 0);
    CHECK(find_char(&h, 'H', NULL) == top + 1);
    run_of(&h, top + 2, 'a', &first, &last);
    CHECK(first == 0 && last == 19);
    run_of(&h, top + 1, 'b', &first, &last);
    CHECK(first == 25 && last == 54);
    CHECK(find_char(&h, 'F', NULL) == top + 4);
    vth_close(&h);
}

/* Tiles whose slots start on one row reserve the tallest head among them, as
 * the tiles of one grid row do: equal slots give equal screens, whatever head
 * each tile carries. */
static void test_tiles_of_a_row_share_their_head(void)
{
    struct vth h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fytim_page_region r[2] = {
        { .id = "tile:a", .kind = FYTIM_PAGE_SLOT, .row = 1, .col = 0,
          .width = 20, .height = 5 },
        { .id = "tile:b", .kind = FYTIM_PAGE_SLOT, .row = 1, .col = 25,
          .width = 20, .height = 5 },
    };
    struct fytim_page_region screen = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT, .row = 2, .col = 0,
        .width = 20, .height = 1
    };
    int ga = -1, gb = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 6, 80);
    b = fytim_surface_open_in(wp, 6, 80);
    paint(a, 'a', 6, 80);
    paint(b, 'b', 6, 80);
    /* a has a head of two rows, b a head of one */
    CHECK(fytim_surface_set_page(a, "HHHH\nCCCC\n\n", 12, &screen, 1) ==
          FYTIM_OK);
    screen.row = 1;
    CHECK(fytim_surface_set_page(b, "GGGG\n\n", 6, &screen, 1) == FYTIM_OK);
    CHECK(fytim_surface_bind(a, "tile:a") == FYTIM_OK);
    CHECK(fytim_surface_bind(b, "tile:b") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "T\n\n\n\n\n\n", 7, r, 2) == FYTIM_OK);
    vth_pump(&h);
    CHECK(fytim_surface_granted_rows(a, &ga) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(b, &gb) == FYTIM_OK);
    CHECK(ga == 3 && gb == 3);
    vth_close(&h);
}

/* A prompt on a card takes the middle rows of its slot; a slot too short
 * for the card is the editor alone. */
static void test_a_prompt_card_frames_the_editor(void)
{
    struct vth h;
    struct fytim_page_region r = {
        .id = "prompt", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 80, .height = 3
    };
    int top;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_marker(h.ft, "Q ") == FYTIM_OK);
    CHECK(fytim_set_prompt_style(h.ft, "\x1b[7m") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "T\n\n\n\nF\n", 6, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(top >= 0);
    CHECK(find_char(&h, 'Q', NULL) == top + 2);
    CHECK(find_char(&h, 'F', NULL) == top + 4);

    /* one row: no card */
    r.height = 1;
    CHECK(fytim_page_set(h.ft, "T\n\nF\n", 4, &r, 1) == FYTIM_OK);
    vth_pump(&h);
    top = find_char(&h, 'T', NULL);
    CHECK(find_char(&h, 'Q', NULL) == top + 1);
    vth_close(&h);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    { "the_rows_stand_in_order", test_the_rows_stand_in_order },
    { "a_surface_stays_in_its_slot", test_a_surface_stays_in_its_slot },
    { "the_tail_shows_its_last_rows", test_the_tail_shows_its_last_rows },
    { "a_pane_tiles_inside_its_slot", test_a_pane_tiles_inside_its_slot },
    { "an_unbound_slot_is_blank", test_an_unbound_slot_is_blank },
    { "a_cleared_page_restores_the_stack",
      test_a_cleared_page_restores_the_stack },
    { "a_tile_page_frames_its_screen", test_a_tile_page_frames_its_screen },
    { "a_prompt_card_frames_the_editor",
      test_a_prompt_card_frames_the_editor },
    { "a_tile_page_frames_its_screen_in_a_grid",
      test_a_tile_page_frames_its_screen_in_a_grid },
    { "a_page_view_draws_its_part", test_a_page_view_draws_its_part },
    { "bound_tiles_stand_in_their_slots",
      test_bound_tiles_stand_in_their_slots },
    { "tiles_of_a_row_share_their_head",
      test_tiles_of_a_row_share_their_head },
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
