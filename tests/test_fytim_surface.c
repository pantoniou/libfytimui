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

static int h_open(struct harness *h)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
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
    { "chrome_survives_a_short_region", test_chrome_survives_a_short_region },
    { "rejects_bad_geometry",        test_rejects_bad_geometry },
    { "put_row_clips_to_width",      test_put_row_clips_to_width },
    { "cursor_is_bounded",           test_cursor_is_bounded },
    { "null_safety",                 test_null_safety },
    { "destroy_with_open_surface",   test_destroy_with_open_surface },
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
