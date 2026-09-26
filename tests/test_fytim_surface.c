/*
 * test_fytim_surface.c - the public cell-surface interface.
 *
 * Drives libfytimui through pipes, as the band tests do: cells in through the
 * public calls, escape bytes out. What a cell means on screen is proved
 * against a real terminal in test_fytim_surface_vt.c; these cases prove the
 * interface itself - geometry, clipping, composition and refusal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

struct harness {
    struct fytim *ft;
    int in[2];
    int out[2];
};

static int h_open_screen(struct harness *h, bool alt)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    if(alt) cfg.screen = FYTIM_SCREEN_ALT;
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

static int h_open(struct harness *h)
{
    return h_open_screen(h, false);
}

static void h_close(struct harness *h)
{
    fytim_destroy(h->ft);
    close(h->in[0]); close(h->in[1]);
    close(h->out[0]); close(h->out[1]);
}

static size_t h_out(struct harness *h, char *buf, size_t cap)
{
    size_t n = 0;
    ssize_t r;
    while(n < cap - 1 && (r = read(h->out[0], buf + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = '\0';
    return n;
}

static int contains(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if(nl == 0 || n < nl) return 0;
    for(i = 0; i + nl <= n; i++)
        if(memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

/* A row of one repeated character, in the terminal's own colours. */
static void fill_row(struct fytim_cell *cells, int n, uint32_t ch)
{
    int i;
    memset(cells, 0, (size_t)n * sizeof *cells);
    for(i = 0; i < n; i++){
        cells[i].chars[0] = ch;
        cells[i].fg = FYTIM_COLOR_DEFAULT;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].width = 1;
    }
}

static void test_open_reports_its_size(void)
{
    struct harness h;
    struct fytim_surface *s;
    int rows = 0, cols = 0;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 4, 10);
    CHECK(s != NULL);
    CHECK(fytim_surface_size(s, &rows, &cols) == FYTIM_OK);
    CHECK(rows == 4);
    CHECK(cols == 10);
    /* Either pointer may be left out. */
    CHECK(fytim_surface_size(s, NULL, NULL) == FYTIM_OK);
    fytim_surface_close(s);
    h_close(&h);
}

static void test_put_row_paints(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 3, 8);
    CHECK(s != NULL);
    fill_row(cells, 8, 'A');
    CHECK(fytim_surface_put_row(s, 1, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "AAAAAAAA"));
    fytim_surface_close(s);
    h_close(&h);
}

/* Cells published AFTER a frame has already been painted must reach the
 * terminal: the first paint is blank, and what a program draws arrives later.
 */
static void test_put_row_after_a_frame_paints(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 3, 8);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);      /* the first, blank frame */
    fill_row(cells, 8, 'L');
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "LLLLLLLL"));
    fytim_surface_close(s);
    h_close(&h);
}

/* A closed surface leaves the band; its content stops being painted. */
static void test_close_removes_content(void)
{
    struct fytim_cell cells[4];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    fill_row(cells, 4, 'Z');
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "ZZZZ"));
    fytim_surface_close(s);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "ZZZZ"));
    h_close(&h);
}

/* A surface and a work band share the region: both are shown. */
static void test_composes_with_workband(void)
{
    struct fytim_cell cells[6];
    struct fytim_workband *wb;
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "bandtext", 8) == FYTIM_OK);
    s = fytim_surface_open(h.ft, 2, 6);
    fill_row(cells, 6, 'S');
    CHECK(fytim_surface_put_row(s, 0, cells, 6) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "bandtext"));
    CHECK(contains(buf, n, "SSSSSS"));
    fytim_surface_close(s);
    fytim_workband_destroy(wb);
    h_close(&h);
}

/* Two surfaces are independent: one does not draw the other's cells. */
static void test_two_surfaces_are_independent(void)
{
    struct fytim_cell cells[4];
    struct fytim_surface *a, *b;
    struct harness h;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    a = fytim_surface_open(h.ft, 1, 4);
    b = fytim_surface_open(h.ft, 1, 4);
    CHECK(a != NULL && b != NULL);
    fill_row(cells, 4, 'a');
    CHECK(fytim_surface_put_row(a, 0, cells, 4) == FYTIM_OK);
    fill_row(cells, 4, 'b');
    CHECK(fytim_surface_put_row(b, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "aaaa"));
    CHECK(contains(buf, n, "bbbb"));
    fytim_surface_close(a);
    fytim_surface_close(b);
    h_close(&h);
}

static void test_granted_rows_respects_cap(void)
{
    struct harness h;
    struct fytim_surface *s;
    int granted = -1;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 20, 8);
    CHECK(fytim_surface_set_max_rows(s, 3) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(s, &granted) == FYTIM_OK);
    CHECK(granted >= 0);
    CHECK(granted <= 3);
    fytim_surface_close(s);
    h_close(&h);
}

static void test_resize_keeps_content(void)
{
    struct fytim_cell cells[4];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    int rows = 0, cols = 0;
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    fill_row(cells, 4, 'k');
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_surface_resize(s, 3, 9) == FYTIM_OK);
    CHECK(fytim_surface_size(s, &rows, &cols) == FYTIM_OK);
    CHECK(rows == 3 && cols == 9);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "kkkk"));
    fytim_surface_close(s);
    h_close(&h);
}

static void test_clear_blanks_the_grid(void)
{
    struct fytim_cell cells[4];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    fill_row(cells, 4, 'q');
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_clear(s) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "qqqq"));
    fytim_surface_close(s);
    h_close(&h);
}

static void test_chrome_rows_paint(void)
{
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_top(s, "TOPROW") == FYTIM_OK);
    CHECK(fytim_surface_set_bottom(s, "BOTROW") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TOPROW"));
    CHECK(contains(buf, n, "BOTROW"));
    /* NULL takes a chrome row away again. */
    CHECK(fytim_surface_set_top(s, NULL) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "TOPROW"));
    fytim_surface_close(s);
    h_close(&h);
}

/* What is committed is what was shown: the chrome rows a surface carries are
 * part of its screen, and a title says what the screen was. */
static void test_commit_keeps_the_chrome(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_top(s, "TITLEROW") == FYTIM_OK);
    CHECK(fytim_surface_set_bottom(s, "STATEROW") == FYTIM_OK);
    fill_row(cells, 8, 'm');
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(s) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TITLEROW"));
    CHECK(contains(buf, n, "mmmmmmmm"));
    CHECK(contains(buf, n, "STATEROW"));
    h_close(&h);
}

/* A committed surface leaves its screen in the transcript: the handle is
 * gone, and what the program drew is still there. */
static void test_commit_keeps_the_screen(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    fill_row(cells, 8, 'C');
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(s) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "CCCCCCCC"));
    h_close(&h);
}

/*
 * A short region takes rows from the grid, not from the chrome: the state row
 * of a surface says what the program is doing, and a screen one row shorter is
 * a far smaller loss than losing that. A work band sheds the other way round.
 */
static void test_chrome_survives_a_short_region(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    int granted = -1;
    if(!h_open(&h)){ CHECK(0); return; }
    /* Taller than any region the default geometry can grant. */
    s = fytim_surface_open(h.ft, 40, 8);
    CHECK(fytim_surface_set_bottom(s, "STATEROW") == FYTIM_OK);
    fill_row(cells, 8, 'g');
    CHECK(fytim_surface_put_row(s, 39, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "STATEROW"));
    CHECK(fytim_surface_granted_rows(s, &granted) == FYTIM_OK);
    CHECK(granted > 0);
    CHECK(granted < 40);
    fytim_surface_close(s);
    h_close(&h);
}

/* The margin is drawn on every row of the grid and takes its columns from it. */
static void test_margin_takes_columns_from_the_grid(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    int wide = 0, narrow = 0;
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    fill_row(cells, 8, 'x');
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_granted_cols(s, &wide) == FYTIM_OK);

    CHECK(fytim_surface_set_margin(s, "| ") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "| "));
    CHECK(fytim_surface_granted_cols(s, &narrow) == FYTIM_OK);
    CHECK(wide > 0);
    CHECK(narrow == wide - 2);

    /* Taking it away gives the columns back. */
    CHECK(fytim_surface_set_margin(s, NULL) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_granted_cols(s, &narrow) == FYTIM_OK);
    CHECK(narrow == wide);
    fytim_surface_close(s);
    h_close(&h);
}

/* What is committed keeps the margin: the alignment is part of the screen. */
static void test_commit_keeps_the_margin(void)
{
    struct fytim_cell cells[4];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    CHECK(fytim_surface_set_margin(s, "| ") == FYTIM_OK);
    fill_row(cells, 4, 'y');
    CHECK(fytim_surface_put_row(s, 0, cells, 4) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(s) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "| yy"));
    h_close(&h);
}

/* ---- negative cases ---------------------------------------------------- */

static void test_rejects_bad_geometry(void)
{
    struct fytim_cell cells[4];
    struct harness h;
    struct fytim_surface *s;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_surface_open(h.ft, 0, 10) == NULL);
    CHECK(fytim_surface_open(h.ft, 4, 0) == NULL);
    CHECK(fytim_surface_open(h.ft, -1, -1) == NULL);
    s = fytim_surface_open(h.ft, 2, 4);
    CHECK(s != NULL);
    CHECK(fytim_surface_resize(s, 0, 4) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_resize(s, 2, -3) == FYTIM_ERR_INVALID);
    fill_row(cells, 4, 'x');
    CHECK(fytim_surface_put_row(s, -1, cells, 4) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_put_row(s, 2, cells, 4) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_put_row(s, 0, cells, -1) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_put_row(s, 0, NULL, 4) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_max_rows(s, -1) == FYTIM_ERR_INVALID);
    fytim_surface_close(s);
    h_close(&h);
}

/* More cells than the width: the extra ones are dropped, not written past
 * the row. The row below is what proves it: an overrun lands there, so its
 * own content going missing is the failure this case is for. */
static void test_put_row_clips_to_width(void)
{
    struct fytim_cell cells[64];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    fill_row(cells, 4, '.');
    CHECK(fytim_surface_put_row(s, 1, cells, 4) == FYTIM_OK);
    fill_row(cells, 64, 'w');
    CHECK(fytim_surface_put_row(s, 0, cells, 64) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "wwww"));
    CHECK(!contains(buf, n, "wwwww"));
    CHECK(contains(buf, n, "...."));
    fytim_surface_close(s);
    h_close(&h);
}

/* A cursor outside the grid is refused; one inside is taken. */
static void test_cursor_is_bounded(void)
{
    struct harness h;
    struct fytim_surface *s;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 4);
    CHECK(fytim_surface_set_cursor(s, 1, 3, true) == FYTIM_OK);
    CHECK(fytim_surface_set_cursor(s, 2, 0, true) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_cursor(s, 0, 4, true) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_cursor(s, -1, 0, true) == FYTIM_ERR_INVALID);
    /* Hiding it needs no position. */
    CHECK(fytim_surface_set_cursor(s, 0, 0, false) == FYTIM_OK);
    fytim_surface_close(s);
    h_close(&h);
}

static void test_null_safety(void)
{
    struct fytim_cell cells[2];
    int rows = 7, cols = 7;
    fill_row(cells, 2, 'n');
    CHECK(fytim_surface_open(NULL, 2, 2) == NULL);
    CHECK(fytim_surface_resize(NULL, 2, 2) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_size(NULL, &rows, &cols) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_granted_rows(NULL, &rows) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_max_rows(NULL, 2) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_put_row(NULL, 0, cells, 2) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_clear(NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_cursor(NULL, 0, 0, true) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_top(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_bottom(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_commit(NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_margin(NULL, "|") == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_granted_cols(NULL, &rows) == FYTIM_ERR_INVALID);
    fytim_surface_close(NULL);   /* must not crash */
}

/* A surface open when the UI is destroyed must not leak or dangle: the UI
 * owns it, so it goes with the UI. */
static void test_destroy_with_open_surface(void)
{
    struct fytim_cell cells[3];
    struct harness h;
    struct fytim_surface *s;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 3);
    fill_row(cells, 3, 'd');
    CHECK(fytim_surface_put_row(s, 0, cells, 3) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_close(&h);   /* destroys the UI with the surface still open */
    CHECK(1);
}

/* A table holds each URI once, whole, and refuses what it cannot write back. */
static void test_links_table(void)
{
    struct fytim_links *l = fytim_links_create();
    uint32_t a, b;

    CHECK(l != NULL);
    if(!l) return;
    a = fytim_links_add(l, "https://a.example/");
    b = fytim_links_add(l, "https://b.example/");
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(fytim_links_add(l, "https://a.example/") == a);
    CHECK(fytim_links_count(l) == 2);
    CHECK(strcmp(fytim_links_uri(l, b), "https://b.example/") == 0);
    CHECK(fytim_links_uri(l, 0) == NULL && fytim_links_uri(l, 3) == NULL);
    CHECK(fytim_links_add(l, "") == 0 && fytim_links_add(l, NULL) == 0);
    CHECK(fytim_links_add(l, "https://x\x1b\\y") == 0);
    CHECK(fytim_links_add(l, "https://x\ay") == 0);
    fytim_links_clear(l);
    CHECK(fytim_links_count(l) == 0 && fytim_links_uri(l, a) == NULL);
    fytim_links_destroy(l);
    fytim_links_destroy(NULL);
}

/* Text drawn with a table links the cells of each link and no others. */
static void test_draw_text_links_cells(void)
{
    static const char text[] =
        "a\x1b]8;;https://x.example/\x1b\\bc\x1b]8;;\x1b\\d";
    struct fytim_cell grid[8];
    struct fytim_links *l = fytim_links_create();
    uint32_t id;

    memset(grid, 0, sizeof grid);
    CHECK(fytim_cells_draw_text_links(grid, 1, 8, 0, 0, 8, 1, text,
                                      sizeof text - 1, l) == 1);
    id = grid[1].link;
    CHECK(grid[0].chars[0] == 'a' && grid[0].link == 0);
    CHECK(grid[1].chars[0] == 'b' && id != 0 && grid[2].link == id);
    CHECK(grid[3].chars[0] == 'd' && grid[3].link == 0);
    CHECK(strcmp(fytim_links_uri(l, id), "https://x.example/") == 0);

    /* Without a table the same text links nothing. */
    memset(grid, 0, sizeof grid);
    CHECK(fytim_cells_draw_text(grid, 1, 8, 0, 0, 8, 1, text,
                                sizeof text - 1) == 1);
    CHECK(grid[1].chars[0] == 'b' && grid[1].link == 0);
    fytim_links_destroy(l);
}

/* A URI far past 256 bytes, drawn into a surface. */
static char *long_uri(void)
{
    static const char head[] = "https://auth.example.com/authorize?q=";
    char *u = malloc(sizeof head + 2000);
    size_t i;

    if(!u) return NULL;
    memcpy(u, head, sizeof head - 1);
    for(i = 0; i < 2000; i++) u[sizeof head - 1 + i] = 'a' + (char)(i % 26);
    u[sizeof head - 1 + 2000] = '\0';
    return u;
}

static void surface_draw_link(struct fytim_surface *s, const char *uri)
{
    struct fytim_cell grid[12];
    char text[4096];
    int n;

    memset(grid, 0, sizeof grid);
    n = snprintf(text, sizeof text, "go \x1b]8;;%s\x1b\\here\x1b]8;;\x1b\\", uri);
    CHECK(n > 0 && (size_t)n < sizeof text);
    CHECK(fytim_cells_draw_text_links(grid, 1, 12, 0, 0, 12, 1, text,
                                      (size_t)n, fytim_surface_links(s)) == 1);
    CHECK(fytim_surface_put_row(s, 0, grid, 12) == FYTIM_OK);
}

/* A linked cell reaches the terminal inside an OSC 8 link, with its URI
 * whole. */
static void test_frame_writes_links(void)
{
    struct harness h;
    struct fytim_surface *s;
    char open_seq[4096], buf[16384];
    char *uri = long_uri();
    size_t n;

    if(!uri || !h_open(&h)){ CHECK(0); free(uri); return; }
    s = fytim_surface_open(h.ft, 1, 12);
    surface_draw_link(s, uri);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    snprintf(open_seq, sizeof open_seq, "\x1b]8;;%s\x1b\\", uri);
    CHECK(contains(buf, n, open_seq));
    CHECK(contains(buf, n, "\x1b]8;;\x1b\\"));
    fytim_surface_close(s);
    h_close(&h);
    free(uri);
}

/*
 * The same text drawn again with another URI is drawn again: the link is part
 * of what a cell shows. Both screens diff frames, and both must see it.
 */
static void frame_follows_a_changed_link(bool alt)
{
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n;

    if(!h_open_screen(&h, alt)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 1, 12);
    surface_draw_link(s, "https://one.example/");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "\x1b]8;;https://one.example/\x1b\\"));
    CHECK(fytim_surface_clear(s) == FYTIM_OK);
    surface_draw_link(s, "https://two.example/");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "\x1b]8;;https://two.example/\x1b\\"));
    CHECK(!contains(buf, n, "https://one.example/"));
    fytim_surface_close(s);
    h_close(&h);
}

static void test_inline_follows_a_changed_link(void)
{
    frame_follows_a_changed_link(false);
}

static void test_alt_follows_a_changed_link(void)
{
    frame_follows_a_changed_link(true);
}

/* A committed surface keeps its links in the transcript, and a cleared one
 * forgets them. */
static void test_commit_keeps_links(void)
{
    struct harness h;
    struct fytim_surface *s;
    char open_seq[4096], buf[16384];
    char *uri = long_uri();
    size_t n;

    if(!uri || !h_open(&h)){ CHECK(0); free(uri); return; }
    s = fytim_surface_open(h.ft, 1, 12);
    surface_draw_link(s, uri);
    CHECK(fytim_surface_clear(s) == FYTIM_OK);
    CHECK(fytim_links_count(fytim_surface_links(s)) == 0);
    surface_draw_link(s, uri);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(s) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    snprintf(open_seq, sizeof open_seq, "\x1b]8;;%s\x1b\\here", uri);
    CHECK(contains(buf, n, open_seq));
    h_close(&h);
    free(uri);
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "open_reports_its_size",       test_open_reports_its_size },
    { "put_row_paints",              test_put_row_paints },
    { "put_row_after_a_frame_paints", test_put_row_after_a_frame_paints },
    { "close_removes_content",       test_close_removes_content },
    { "composes_with_workband",      test_composes_with_workband },
    { "two_surfaces_are_independent", test_two_surfaces_are_independent },
    { "granted_rows_respects_cap",   test_granted_rows_respects_cap },
    { "resize_keeps_content",        test_resize_keeps_content },
    { "clear_blanks_the_grid",       test_clear_blanks_the_grid },
    { "chrome_rows_paint",           test_chrome_rows_paint },
    { "commit_keeps_the_screen",     test_commit_keeps_the_screen },
    { "commit_keeps_the_chrome",     test_commit_keeps_the_chrome },
    { "margin_takes_columns_from_the_grid", test_margin_takes_columns_from_the_grid },
    { "commit_keeps_the_margin",     test_commit_keeps_the_margin },
    { "chrome_survives_a_short_region", test_chrome_survives_a_short_region },
    { "rejects_bad_geometry",        test_rejects_bad_geometry },
    { "put_row_clips_to_width",      test_put_row_clips_to_width },
    { "cursor_is_bounded",           test_cursor_is_bounded },
    { "null_safety",                 test_null_safety },
    { "destroy_with_open_surface",   test_destroy_with_open_surface },
    { "links_table",                 test_links_table },
    { "draw_text_links_cells",       test_draw_text_links_cells },
    { "frame_writes_links",          test_frame_writes_links },
    { "commit_keeps_links",          test_commit_keeps_links },
    { "inline_follows_a_changed_link", test_inline_follows_a_changed_link },
    { "alt_follows_a_changed_link",  test_alt_follows_a_changed_link },
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
