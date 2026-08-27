/*
 * test_fytim_workpane.c - one region that tiles the screens it holds.
 *
 * Drives libfytimui through pipes, as the surface tests do. What a tile is
 * given is the contract a host builds on - it sizes its pseudo-terminal to
 * the granted rows and columns - so that is what these cases assert, along
 * with the bytes that prove two screens stand side by side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

/* The glyphs the library draws for the controls. */
#define FYTIM_TILE_ZOOM_MARK_UTF8   "\xe2\xa4\xa2"
#define FYTIM_TILE_CLOSE_MARK_UTF8  "\xc3\x97"
#define FYTIM_TILE_BAR_TRACK_UTF8   "\xe2\x94\x82"
#define FYTIM_TILE_BAR_THUMB_UTF8   "\xe2\x96\x88"

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

static int h_open_mouse(struct harness *h, bool mouse)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    cfg.mouse = mouse;
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

static int h_open(struct harness *h)
{
    return h_open_mouse(h, false);
}

/* Feed bytes as the terminal would, and let the pump read them. */
static void h_type(struct harness *h, const char *s)
{
    ssize_t n = write(h->in[1], s, strlen(s));
    (void)n;
}

/* An SGR mouse press and release at (@col, @row), both one-based on the
 * wire, as a terminal reports them. */
static void h_click(struct harness *h, int col, int row)
{
    char buf[64];
    snprintf(buf, sizeof buf, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm",
             col + 1, row + 1, col + 1, row + 1);
    h_type(h, buf);
}

/* One wheel-up notch at (@col, @row). */
static void h_wheel(struct harness *h, int col, int row)
{
    char buf[64];
    snprintf(buf, sizeof buf, "\x1b[<64;%d;%dM", col + 1, row + 1);
    h_type(h, buf);
}

/*
 * The events of one pump. Draining is destructive and a case asks about more
 * than one kind, so the queue is emptied once into here and questioned after.
 */
#define H_EVENTS_MAX 32
struct h_events {
    struct fytim_event ev[H_EVENTS_MAX];
    int n;
};

static void h_drain(struct harness *h, struct h_events *out)
{
    out->n = 0;
    while(out->n < H_EVENTS_MAX && fytim_next_event(h->ft, &out->ev[out->n]))
        out->n++;
}

/* The first event of @type that was drained, if any. */
static int h_event(const struct h_events *evs, enum fytim_event_type type,
                   struct fytim_event *out)
{
    int i;

    if (out) {
	    memset(out, 0, sizeof(*out));
	    out->type = FYTIM_EVENT_NONE;
    }
    for(i = 0; i < evs->n; i++){
        if(evs->ev[i].type != type) continue;
        if(out) *out = evs->ev[i];
        return 1;
    }
    return 0;
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

/* Paint every row of a tile with @ch, so the tile is findable in the bytes. */
static void paint(struct fytim_surface *sf, uint32_t ch, int n)
{
    struct fytim_cell cells[16];
    int row, rows = 0, cols = 0;

    fytim_surface_size(sf, &rows, &cols);
    if(n > (int)(sizeof cells / sizeof cells[0])) n = (int)(sizeof cells / sizeof cells[0]);
    if(n > cols) n = cols;
    fill_row(cells, n, ch);
    for(row = 0; row < rows; row++)
        fytim_surface_put_row(sf, row, cells, n);
}

static int granted_cols(struct fytim_surface *sf)
{
    int cols = -1;
    fytim_surface_granted_cols(sf, &cols);
    return cols;
}

static int granted_rows(struct fytim_surface *sf)
{
    int rows = -1;
    fytim_surface_granted_rows(sf, &rows);
    return rows;
}

/* Two tiles split the width and are both shown. */
static void test_two_tiles_share_the_width(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(wp != NULL);
    a = fytim_surface_open_in(wp, 3, 30);
    b = fytim_surface_open_in(wp, 3, 30);
    CHECK(a != NULL && b != NULL);
    CHECK(fytim_workpane_count(wp) == 2);
    paint(a, 'A', 6);
    paint(b, 'B', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    /* Both screens are on the terminal at once. */
    CHECK(contains(buf, n, "AAAAAA"));
    CHECK(contains(buf, n, "BBBBBB"));
    /* Each was given half of an 80-column terminal, and neither was given
     * the whole of it: they stand side by side. */
    CHECK(granted_cols(a) == 40);
    CHECK(granted_cols(b) == 40);
    /* And the same rows: one grid row holds them both. */
    CHECK(granted_rows(a) == 3);
    CHECK(granted_rows(b) == 3);
    h_close(&h);
}

/* One tile is the whole width: a pane with a single screen is not a grid. */
static void test_one_tile_takes_the_width(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 80);
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_cols(a) == 80);
    h_close(&h);
}

/* Three tiles wrap into a second grid row: two columns, two rows. */
static void test_the_grid_wraps(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *sf[3];
    int i;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    for(i = 0; i < 3; i++){
        sf[i] = fytim_surface_open_in(wp, 2, 30);
        CHECK(sf[i] != NULL);
        paint(sf[i], (uint32_t)('X' + i), 4);
    }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    /* Two columns of 40, and every tile still has its rows: the pane grew
     * to hold two grid rows rather than shedding one. */
    for(i = 0; i < 3; i++){
        CHECK(granted_cols(sf[i]) == 40);
        CHECK(granted_rows(sf[i]) == 2);
    }
    h_close(&h);
}

/* A region too narrow for two tiles falls back to a stack. */
static void test_a_narrow_region_stacks(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    /* 80 columns cannot hold two tiles of 60. */
    CHECK(fytim_workpane_set_min_tile_cols(wp, 60) == FYTIM_OK);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_cols(a) == 80);
    CHECK(granted_cols(b) == 80);
    h_close(&h);
}

/* A host that asks for a column count gets it. */
static void test_columns_are_settable(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *sf[3];
    int i;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_columns(wp, 3) == FYTIM_OK);
    for(i = 0; i < 3; i++){
        sf[i] = fytim_surface_open_in(wp, 2, 20);
        paint(sf[i], (uint32_t)('X' + i), 4);
    }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    /* 80 over three: the leftmost tiles take the spare columns. */
    CHECK(granted_cols(sf[0]) == 27);
    CHECK(granted_cols(sf[1]) == 27);
    CHECK(granted_cols(sf[2]) == 26);
    h_close(&h);
}

/* A zoomed tile takes the pane, and the others are given nothing. */
static void test_zoom_takes_the_pane(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 30);
    b = fytim_surface_open_in(wp, 3, 30);
    paint(a, 'A', 6);
    paint(b, 'B', 6);
    CHECK(fytim_workpane_set_zoom(wp, b) == FYTIM_OK);
    CHECK(fytim_workpane_zoomed(wp) == b);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "BBBBBB"));
    CHECK(!contains(buf, n, "AAAAAA"));
    CHECK(granted_cols(b) == 80);
    /* The tile nobody can see must be told so: a host sizes its program to
     * what it was granted. */
    CHECK(granted_rows(a) == 0);
    /* And the grid comes back. */
    CHECK(fytim_workpane_set_zoom(wp, NULL) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_cols(a) == 40);
    h_close(&h);
}

/* A zoomed tile that retires drops the zoom with it. */
static void test_a_retired_zoom_is_dropped(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    fytim_workpane_set_zoom(wp, b);
    fytim_surface_close(b);
    CHECK(fytim_workpane_zoomed(wp) == NULL);
    CHECK(fytim_workpane_count(wp) == 1);
    paint(a, 'A', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_cols(a) == 80);
    h_close(&h);
}

/* A zoom must name a tile of the pane it is set on. */
static void test_zoom_refuses_a_stranger(void)
{
    struct harness h;
    struct fytim_workpane *wp, *other;
    struct fytim_surface *a, *loose;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    other = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(other, 2, 10);
    loose = fytim_surface_open(h.ft, 2, 10);
    CHECK(fytim_workpane_set_zoom(wp, a) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_zoom(wp, loose) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_zoomed(wp) == NULL);
    h_close(&h);
}

/* The margin takes its columns from the tile, not from the terminal. */
static void test_a_margin_takes_columns_from_the_tile(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    CHECK(fytim_surface_set_margin(a, "| ") == FYTIM_OK);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_cols(a) == 38);
    CHECK(granted_cols(b) == 40);
    h_close(&h);
}

/* A tile keeps its own chrome rows, and the pane keeps its own frame. */
static void test_chrome_frames_the_pane_and_the_tiles(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    CHECK(fytim_workpane_set_top(wp, "PANETOP") == FYTIM_OK);
    CHECK(fytim_surface_set_top(a, "TILEA") == FYTIM_OK);
    CHECK(fytim_surface_set_top(b, "TILEB") == FYTIM_OK);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "PANETOP"));
    CHECK(contains(buf, n, "TILEA"));
    CHECK(contains(buf, n, "TILEB"));
    h_close(&h);
}

/* The separator is drawn between adjacent columns. */
static void test_a_separator_divides_the_columns(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    CHECK(fytim_workpane_set_tile_sep(wp, "!") == FYTIM_OK);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "!"));
    /* It costs a column, and the two tiles share what is left. */
    CHECK(granted_cols(a) + granted_cols(b) == 79);
    h_close(&h);
}

/* The pane's cap bounds the whole region, and a tile then sheds content. */
static void test_the_cap_bounds_the_pane(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 6, 30);
    b = fytim_surface_open_in(wp, 6, 30);
    CHECK(fytim_workpane_set_max_rows(wp, 3) == FYTIM_OK);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_rows(a) == 3);
    CHECK(granted_rows(b) == 3);
    h_close(&h);
}

/* Closing a tile leaves the pane; the last one leaves nothing painted. */
static void test_closing_tiles_empties_the_pane(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    paint(a, 'A', 6);
    paint(b, 'B', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    fytim_surface_close(a);
    CHECK(fytim_workpane_count(wp) == 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "AAAAAA"));
    /* The one left takes the width back. */
    CHECK(granted_cols(b) == 80);
    fytim_surface_close(b);
    CHECK(fytim_workpane_count(wp) == 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "BBBBBB"));
    fytim_workpane_destroy(wp);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_close(&h);
}

/* A committed tile keeps its screen and retires from the pane. */
static void test_commit_retires_a_tile(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    b = fytim_surface_open_in(wp, 2, 30);
    paint(a, 'A', 6);
    paint(b, 'B', 6);
    CHECK(fytim_surface_set_top(a, "DONEA") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(a) == FYTIM_OK);
    CHECK(fytim_workpane_count(wp) == 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    /* The screen went into the transcript, its title with it. */
    CHECK(contains(buf, n, "DONEA"));
    CHECK(contains(buf, n, "AAAAAA"));
    h_close(&h);
}

/* A pane and a band of its own compose in the same region. */
static void test_a_pane_composes_with_a_band(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_workband *wb;
    struct fytim_surface *a;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(fytim_workband_set(wb, "bandrow", 7) == FYTIM_OK);
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "bandrow"));
    CHECK(contains(buf, n, "AAAAAA"));
    h_close(&h);
}

/* A tile keeps its own row cap; the pane's is a separate bound. */
static void test_a_tile_keeps_its_own_cap(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 6, 30);
    b = fytim_surface_open_in(wp, 6, 30);
    CHECK(fytim_surface_set_max_rows(a, 2) == FYTIM_OK);
    paint(a, 'A', 4);
    paint(b, 'B', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    /* The tallest tile sets the grid row, and the capped one keeps its cap. */
    CHECK(granted_rows(a) == 2);
    CHECK(granted_rows(b) == 6);
    h_close(&h);
}

/* A resized tile is given the new size at the next frame. */
static void test_a_resized_tile_is_regranted(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    paint(a, 'A', 4);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_rows(a) == 2);
    CHECK(fytim_surface_resize(a, 4, 30) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(granted_rows(a) == 4);
    h_close(&h);
}

/* The scroll extent is the host's to report and is refused when impossible. */
static void test_scroll_extent_is_checked(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 30);
    CHECK(fytim_surface_set_scroll_extent(a, 500, 120) == FYTIM_OK);
    CHECK(fytim_surface_set_scroll_extent(a, -1, 0) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_scroll_extent(NULL, 1, 0) == FYTIM_ERR_INVALID);
    h_close(&h);
}

/* Controls are off until a host asks for them. */
static void test_controls_are_off_by_default(void)
{
    struct harness h;
    struct fytim_workpane *wp;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_controls(wp) == 0);
    CHECK(fytim_workpane_set_controls(wp,
            FYTIM_WORKPANE_SCROLLBAR | FYTIM_WORKPANE_ZOOM) == FYTIM_OK);
    CHECK(fytim_workpane_controls(wp) ==
          (FYTIM_WORKPANE_SCROLLBAR | FYTIM_WORKPANE_ZOOM));
    h_close(&h);
}

static void test_rejects_bad_geometry(void)
{
    struct harness h;
    struct fytim_workpane *wp;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_surface_open_in(wp, 0, 10) == NULL);
    CHECK(fytim_surface_open_in(wp, 10, 0) == NULL);
    CHECK(fytim_surface_open_in(NULL, 2, 2) == NULL);
    CHECK(fytim_workpane_set_columns(wp, -1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_min_tile_cols(wp, -1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_max_rows(wp, -1) == FYTIM_ERR_INVALID);
    h_close(&h);
}

static void test_null_safety(void)
{
    CHECK(fytim_workpane_create(NULL) == NULL);
    fytim_workpane_destroy(NULL);
    CHECK(fytim_workpane_count(NULL) == 0);
    CHECK(fytim_workpane_zoomed(NULL) == NULL);
    CHECK(fytim_workpane_controls(NULL) == 0);
    CHECK(fytim_workpane_set_top(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_bottom(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_tile_sep(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_zoom(NULL, NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_controls(NULL, 0) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_columns(NULL, 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_min_tile_cols(NULL, 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_set_max_rows(NULL, 1) == FYTIM_ERR_INVALID);
}

/* Destroying the UI with tiles still open frees them. */
static void test_destroy_with_open_tiles(void)
{
    struct harness h;
    struct fytim_workpane *wp;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_surface_open_in(wp, 2, 10) != NULL);
    CHECK(fytim_surface_open_in(wp, 2, 10) != NULL);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_close(&h);
}

/* Without the grab there are no controls to draw: the columns stay with the
 * program, and the marks the user could not click are absent. */
static void test_controls_need_the_grab(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(!fytim_mouse_enabled(h.ft));
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_SCROLLBAR |
                                    FYTIM_WORKPANE_ZOOM |
                                    FYTIM_WORKPANE_CLOSE);
    a = fytim_surface_open_in(wp, 3, 80);
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, FYTIM_TILE_CLOSE_MARK_UTF8));
    /* And the program keeps every column. */
    CHECK(granted_cols(a) == 80);
    h_close(&h);
}

/* With the grab, the bar takes a column and the marks are drawn. */
static void test_controls_take_a_column(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    char buf[16384];
    size_t n;

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    CHECK(fytim_mouse_enabled(h.ft));
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_SCROLLBAR |
                                    FYTIM_WORKPANE_ZOOM |
                                    FYTIM_WORKPANE_CLOSE);
    a = fytim_surface_open_in(wp, 3, 80);
    fytim_surface_set_top(a, "TILE");
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, FYTIM_TILE_CLOSE_MARK_UTF8));
    CHECK(contains(buf, n, FYTIM_TILE_ZOOM_MARK_UTF8));
    /* The bar is chrome: the grid is told what it was left. */
    CHECK(granted_cols(a) == 79);
    h_close(&h);
}

/* Clicking the close mark asks for the tile to go; the library removes
 * nothing itself - the host owns what the tile stands for. */
static void test_a_click_asks_to_close(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_CLOSE);
    a = fytim_surface_open_in(wp, 3, 80);
    fytim_surface_set_top(a, "TILE");
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    /* The mark is the last cell of the tile's chrome row. */
    h_click(&h, 79, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_SURFACE_CLOSE, &ev));
    CHECK(ev.surface == a);
    CHECK(fytim_workpane_count(wp) == 1);
    h_close(&h);
}

/* Clicking the zoom mark asks for the zoom; the host decides. */
static void test_a_click_asks_to_zoom(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_ZOOM);
    a = fytim_surface_open_in(wp, 3, 80);
    fytim_surface_set_top(a, "TILE");
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    h_click(&h, 79, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_SURFACE_ZOOM, &ev));
    CHECK(ev.surface == a);
    /* The library zoomed nothing: it reported what was asked. */
    CHECK(fytim_workpane_zoomed(wp) == NULL);
    h_close(&h);
}

/* The wheel over a tile is that tile's, and does not reach the transcript. */
static void test_the_wheel_belongs_to_the_tile(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_SCROLLBAR);
    a = fytim_surface_open_in(wp, 3, 80);
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    h_wheel(&h, 10, 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_SURFACE_SCROLL, &ev));
    CHECK(ev.surface == a);
    CHECK(ev.delta > 0);
    CHECK(!h_event(&evs, FYTIM_EVENT_SCROLLBACK, NULL));
    h_close(&h);
}

/* A tile with no controls keeps the wheel with the transcript. */
static void test_the_wheel_stays_with_the_transcript(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    paint(a, 'A', 6);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    h_wheel(&h, 10, 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_SCROLL, NULL));
    CHECK(h_event(&evs, FYTIM_EVENT_SCROLLBACK, NULL));
    h_close(&h);
}

/* The bar shows where the host's scrollback stands. */
static void test_the_bar_follows_the_extent(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    char buf[16384];
    size_t n;

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    fytim_workpane_set_controls(wp, FYTIM_WORKPANE_SCROLLBAR);
    a = fytim_surface_open_in(wp, 6, 80);
    paint(a, 'A', 6);
    /* Far more behind the screen than on it: a track with a small thumb. */
    CHECK(fytim_surface_set_scroll_extent(a, 600, 0) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, FYTIM_TILE_BAR_THUMB_UTF8));
    CHECK(contains(buf, n, FYTIM_TILE_BAR_TRACK_UTF8));
    h_close(&h);
}

/* A pane tiles a work band beside a screen: what a tile holds does not
 * change where it goes. Parallel work is not all of one kind. */
static void test_a_band_is_a_tile_too(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_workband *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 30);
    b = fytim_workband_create_in(wp);
    CHECK(a != NULL && b != NULL);
    CHECK(fytim_workpane_count(wp) == 2);
    paint(a, 'A', 6);
    CHECK(fytim_workband_set(b, "BANDROW", 7) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "AAAAAA"));
    CHECK(contains(buf, n, "BANDROW"));
    /* The screen took its half, so the band has the other. */
    CHECK(granted_cols(a) == 40);
    h_close(&h);
}

/* Two bands tile, which is the case a parallel run of shells makes. */
static void test_two_bands_tile(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_workband *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_workband_create_in(wp);
    b = fytim_workband_create_in(wp);
    CHECK(fytim_workband_set(a, "AAAAAA", 6) == FYTIM_OK);
    CHECK(fytim_workband_set(b, "BBBBBB", 6) == FYTIM_OK);
    CHECK(fytim_workband_set_top(a, "TOPA") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "AAAAAA"));
    CHECK(contains(buf, n, "BBBBBB"));
    CHECK(contains(buf, n, "TOPA"));
    h_close(&h);
}

/* A band tile commits into the transcript and leaves the pane, as a band of
 * its own does. */
static void test_a_band_tile_commits(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_workband *a, *b;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_workband_create_in(wp);
    b = fytim_workband_create_in(wp);
    CHECK(fytim_workband_set(a, "DONEBAND", 8) == FYTIM_OK);
    CHECK(fytim_workband_set(b, "STAYBAND", 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_workband_commit(a) == FYTIM_OK);
    CHECK(fytim_workpane_count(wp) == 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "DONEBAND"));
    /* Destroying the other leaves nothing behind. */
    fytim_workband_destroy(b);
    CHECK(fytim_workpane_count(wp) == 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_close(&h);
}

/* A band tile keeps the band's own cap and shows its last rows. */
static void test_a_band_tile_keeps_its_cap(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_workband *a;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_workband_create_in(wp);
    CHECK(fytim_workband_set_max_rows(a, 2) == FYTIM_OK);
    CHECK(fytim_workband_set(a, "one\ntwo\nthree\nfour", 18) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    /* The end of a report is what is being made. */
    CHECK(contains(buf, n, "three"));
    CHECK(contains(buf, n, "four"));
    CHECK(!contains(buf, n, "one"));
    h_close(&h);
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "two_tiles_share_the_width",   test_two_tiles_share_the_width },
    { "one_tile_takes_the_width",    test_one_tile_takes_the_width },
    { "the_grid_wraps",              test_the_grid_wraps },
    { "a_narrow_region_stacks",      test_a_narrow_region_stacks },
    { "columns_are_settable",        test_columns_are_settable },
    { "zoom_takes_the_pane",         test_zoom_takes_the_pane },
    { "a_retired_zoom_is_dropped",   test_a_retired_zoom_is_dropped },
    { "zoom_refuses_a_stranger",     test_zoom_refuses_a_stranger },
    { "a_margin_takes_columns_from_the_tile",
      test_a_margin_takes_columns_from_the_tile },
    { "chrome_frames_the_pane_and_the_tiles",
      test_chrome_frames_the_pane_and_the_tiles },
    { "a_separator_divides_the_columns",
      test_a_separator_divides_the_columns },
    { "the_cap_bounds_the_pane",     test_the_cap_bounds_the_pane },
    { "closing_tiles_empties_the_pane", test_closing_tiles_empties_the_pane },
    { "commit_retires_a_tile",       test_commit_retires_a_tile },
    { "a_pane_composes_with_a_band", test_a_pane_composes_with_a_band },
    { "a_tile_keeps_its_own_cap",    test_a_tile_keeps_its_own_cap },
    { "a_resized_tile_is_regranted", test_a_resized_tile_is_regranted },
    { "scroll_extent_is_checked",    test_scroll_extent_is_checked },
    { "controls_are_off_by_default", test_controls_are_off_by_default },
    { "rejects_bad_geometry",        test_rejects_bad_geometry },
    { "null_safety",                 test_null_safety },
    { "destroy_with_open_tiles",     test_destroy_with_open_tiles },
    { "a_band_is_a_tile_too",        test_a_band_is_a_tile_too },
    { "two_bands_tile",              test_two_bands_tile },
    { "a_band_tile_commits",         test_a_band_tile_commits },
    { "a_band_tile_keeps_its_cap",   test_a_band_tile_keeps_its_cap },
    { "controls_need_the_grab",      test_controls_need_the_grab },
    { "controls_take_a_column",      test_controls_take_a_column },
    { "a_click_asks_to_close",       test_a_click_asks_to_close },
    { "a_click_asks_to_zoom",        test_a_click_asks_to_zoom },
    { "the_wheel_belongs_to_the_tile", test_the_wheel_belongs_to_the_tile },
    { "the_wheel_stays_with_the_transcript",
      test_the_wheel_stays_with_the_transcript },
    { "the_bar_follows_the_extent",  test_the_bar_follows_the_extent },
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
