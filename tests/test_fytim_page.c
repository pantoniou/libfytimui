/*
 * test_fytim_page.c - a page of rows and slots that the host rendered.
 *
 * Drives libfytimui through pipes, as the workpane tests do. The contract a
 * host builds on is what is accepted, what is rejected and kept, what a slot
 * grants and which event a click makes; those are asserted here. What the
 * page looks like in cells is test_fytim_page_vt.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdbool.h>
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

static void h_type(struct harness *h, const char *s)
{
    ssize_t n = write(h->in[1], s, strlen(s));
    (void)n;
}

static void h_click(struct harness *h, int col, int row)
{
    char buf[64];
    snprintf(buf, sizeof buf, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm",
             col + 1, row + 1, col + 1, row + 1);
    h_type(h, buf);
}

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

static int h_event(const struct h_events *evs, enum fytim_event_type type,
                   struct fytim_event *out)
{
    int i;

    if(out){
        memset(out, 0, sizeof *out);
        out->type = FYTIM_EVENT_NONE;
    }
    for(i = 0; i < evs->n; i++){
        if(evs->ev[i].type != type) continue;
        if(out) *out = evs->ev[i];
        return 1;
    }
    return 0;
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

static void h_close(struct harness *h)
{
    fytim_destroy(h->ft);
    close(h->in[0]); close(h->in[1]);
    close(h->out[0]); close(h->out[1]);
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

static enum fytim_result set_rows(struct harness *h, const char *rows)
{
    return fytim_page_set(h->ft, rows, strlen(rows), NULL, 0);
}

/* ---- positive ----------------------------------------------------------- */

static void test_a_page_is_set_and_cleared(void)
{
    struct harness h;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(!fytim_page_active(h.ft));
    CHECK(fytim_page_rows(h.ft) == 0);
    CHECK(set_rows(&h, "PAGE-ONE\nPAGE-TWO\n") == FYTIM_OK);
    CHECK(fytim_page_active(h.ft));
    CHECK(fytim_page_rows(h.ft) == 2);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "PAGE-ONE"));
    CHECK(contains(buf, n, "PAGE-TWO"));

    fytim_page_clear(h.ft);
    CHECK(!fytim_page_active(h.ft));
    CHECK(fytim_page_rows(h.ft) == 0);
    h_close(&h);
}

/* A page replaces the chrome of the band stack while it is set. */
static void test_a_page_replaces_the_band_stack(void)
{
    struct harness h;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, "STACK-HEADER") == FYTIM_OK);
    CHECK(set_rows(&h, "PAGE-ROW\n") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "PAGE-ROW"));
    CHECK(!contains(buf, n, "STACK-HEADER"));

    fytim_page_clear(h.ft);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "STACK-HEADER"));
    h_close(&h);
}

/* A region lower than the last row extends the page to its bottom. */
static void test_a_region_extends_the_page(void)
{
    struct harness h;
    struct fytim_page_region r = {
        .id = "pane", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 40, .height = 5
    };

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "top\n", 4, &r, 1) == FYTIM_OK);
    CHECK(fytim_page_rows(h.ft) == 6);
    h_close(&h);
}

/* A surface bound to a slot is granted the region of the slot. */
static void test_a_slot_grants_its_region(void)
{
    struct harness h;
    struct fytim_surface *sf;
    struct fytim_page_region r = {
        .id = "work", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 4, .width = 30, .height = 3
    };
    int rows = -1, cols = -1;

    if(!h_open(&h)){ CHECK(0); return; }
    sf = fytim_surface_open(h.ft, 10, 60);
    CHECK(sf != NULL);
    CHECK(fytim_surface_bind(sf, "work") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "head\n\n\n\nfoot\n", 14, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(sf, &rows) == FYTIM_OK);
    CHECK(fytim_surface_granted_cols(sf, &cols) == FYTIM_OK);
    CHECK(rows == 3);
    CHECK(cols == 30);
    h_close(&h);
}

/* A band bound to no slot is not drawn while the page is set. */
static void test_an_unbound_band_is_not_drawn(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "BAND-TEXT\n", 10) == FYTIM_OK);
    CHECK(set_rows(&h, "PAGE-ROW\n") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "PAGE-ROW"));
    CHECK(!contains(buf, n, "BAND-TEXT"));
    h_close(&h);
}

/* A bound band draws its last rows inside its slot. */
static void test_a_bound_band_draws_in_its_slot(void)
{
    struct harness h;
    struct fytim_workband *wb;
    struct fytim_page_region r = {
        .id = "report", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 40, .height = 1
    };
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(fytim_workband_set(wb, "OLD-ROW\nNEW-ROW\n", 16) == FYTIM_OK);
    CHECK(fytim_workband_bind(wb, "report") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "top\n\n", 5, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "NEW-ROW"));
    CHECK(!contains(buf, n, "OLD-ROW"));
    h_close(&h);
}

/* The prompt slot takes the editor: typing and Enter make a line. */
static void test_the_prompt_slot_edits(void)
{
    struct harness h;
    struct fytim_page_region r = {
        .id = "prompt", .kind = FYTIM_PAGE_SLOT,
        .row = 1, .col = 0, .width = 80, .height = 1
    };
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "top\n\n", 5, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_type(&h, "hello\r");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_LINE, &ev));
    CHECK(ev.text && ev.text_len == 5 && !memcmp(ev.text, "hello", 5));
    h_close(&h);
}

/* Without a prompt slot, typing reaches no editor and makes no line. */
static void test_no_prompt_slot_makes_no_line(void)
{
    struct harness h;
    struct h_events evs;
    char buf[16384];

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(set_rows(&h, "top\n") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_type(&h, "hello\r");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_LINE, NULL));
    h_close(&h);
}

/* A click on an act region reports its id; a click beside it does not. */
static void test_a_click_on_an_act_reports_its_id(void)
{
    struct harness h;
    struct fytim_page_region r = {
        .id = "tile:zoom", .kind = FYTIM_PAGE_ACT,
        .row = 0, .col = 10, .width = 4, .height = 1
    };
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "          zoom\n", 15, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);

    h_click(&h, 12, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_ACT, &ev));
    CHECK(ev.text && ev.text_len == 9 && !memcmp(ev.text, "tile:zoom", 9));

    h_click(&h, 9, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_ACT, NULL));
    h_close(&h);
}

/* Binding an id that another component holds takes it. */
static void test_a_bind_takes_the_id(void)
{
    struct harness h;
    struct fytim_workband *a, *b;
    struct fytim_page_region r = {
        .id = "report", .kind = FYTIM_PAGE_SLOT,
        .row = 0, .col = 0, .width = 40, .height = 1
    };
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    a = fytim_workband_create(h.ft);
    b = fytim_workband_create(h.ft);
    CHECK(fytim_workband_set(a, "FROM-A\n", 7) == FYTIM_OK);
    CHECK(fytim_workband_set(b, "FROM-B\n", 7) == FYTIM_OK);
    CHECK(fytim_workband_bind(a, "report") == FYTIM_OK);
    CHECK(fytim_workband_bind(b, "report") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "\n", 1, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "FROM-B"));
    CHECK(!contains(buf, n, "FROM-A"));
    h_close(&h);
}

/* ---- negative ----------------------------------------------------------- */

static void test_rejects_disallowed_rows(void)
{
    struct harness h;
    const char bad[] = "ok\n\x1b[2Jcleared\n";

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(set_rows(&h, "KEPT\n") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, bad, sizeof bad - 1, NULL, 0) ==
          FYTIM_ERR_INVALID);
    CHECK(fytim_page_active(h.ft));
    CHECK(fytim_page_rows(h.ft) == 1);
    h_close(&h);
}

/* A page with the one region @r is rejected, and the page before it stays. */
#define BAD_REGION(h, ...) bad_region((h), (__VA_ARGS__), __LINE__)
static void bad_region(struct harness *h, struct fytim_page_region r,
                       int line)
{
    if(fytim_page_set(h->ft, "x\n", 2, &r, 1) != FYTIM_ERR_INVALID){
        ++failures;
        printf("  FAIL %s:%d: region accepted\n", __FILE__, line);
    }
}

static void test_rejects_bad_regions(void)
{
    struct harness h;
    struct fytim_page_region r;
    struct fytim_page_region many[FYTIM_PAGE_REGIONS_MAX + 1];
    char longid[FYTIM_PAGE_ID_MAX + 2];
    size_t i;

    if(!h_open(&h)){ CHECK(0); return; }
    BAD_REGION(&h, (struct fytim_page_region){ .id = NULL,
               .kind = FYTIM_PAGE_SLOT, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "",
               .kind = FYTIM_PAGE_SLOT, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "has space",
               .kind = FYTIM_PAGE_SLOT, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "semi;colon",
               .kind = FYTIM_PAGE_SLOT, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_SLOT, .row = -1, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_SLOT, .col = -1, .width = 1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_SLOT, .width = -1, .height = 1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_SLOT, .width = 1, .height = -1 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_ACT, .width = 1, .height = 2 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = FYTIM_PAGE_ACT, .width = 1, .height = 0 });
    BAD_REGION(&h, (struct fytim_page_region){ .id = "ok",
               .kind = (enum fytim_page_region_kind)7, .width = 1,
               .height = 1 });
    memset(longid, 'a', sizeof longid - 1);
    longid[sizeof longid - 1] = '\0';
    r = (struct fytim_page_region){ .id = longid, .kind = FYTIM_PAGE_SLOT,
                                    .width = 1, .height = 1 };
    CHECK(fytim_page_set(h.ft, "x\n", 2, &r, 1) == FYTIM_ERR_INVALID);
    longid[FYTIM_PAGE_ID_MAX] = '\0';
    CHECK(fytim_page_set(h.ft, "x\n", 2, &r, 1) == FYTIM_OK);

    CHECK(fytim_page_set(h.ft, "x\n", 2, NULL, 1) == FYTIM_ERR_INVALID);
    for(i = 0; i < sizeof many / sizeof many[0]; i++)
        many[i] = (struct fytim_page_region){ .id = "s",
            .kind = FYTIM_PAGE_SLOT, .width = 1, .height = 1 };
    CHECK(fytim_page_set(h.ft, "x\n", 2, many, FYTIM_PAGE_REGIONS_MAX + 1) ==
          FYTIM_ERR_INVALID);
    CHECK(fytim_page_set(h.ft, "x\n", 2, many, FYTIM_PAGE_REGIONS_MAX) ==
          FYTIM_OK);
    h_close(&h);
}

/* The regions are copied: the host may free its own after the call. */
static void test_regions_are_copied(void)
{
    struct harness h;
    struct fytim_page_region *r;
    char *id;
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    id = strdup("copied:id");
    r = calloc(1, sizeof *r);
    *r = (struct fytim_page_region){ .id = id, .kind = FYTIM_PAGE_ACT,
                                     .row = 0, .col = 0, .width = 6,
                                     .height = 1 };
    CHECK(fytim_page_set(h.ft, "button\n", 7, r, 1) == FYTIM_OK);
    memset(id, 'z', strlen(id));
    free(id);
    free(r);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    h_click(&h, 2, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_ACT, &ev));
    CHECK(ev.text && ev.text_len == 9 && !memcmp(ev.text, "copied:id", 9));
    h_close(&h);
}

/* No act is reported without the grab: the user cannot click it. */
static void test_no_act_without_the_grab(void)
{
    struct harness h;
    struct fytim_page_region r = {
        .id = "x", .kind = FYTIM_PAGE_ACT,
        .row = 0, .col = 0, .width = 4, .height = 1
    };
    struct h_events evs;
    char buf[16384];

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, "zoom\n", 5, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_click(&h, 1, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_ACT, NULL));
    h_close(&h);
}

static void test_rejects_bad_binds(void)
{
    struct harness h;
    struct fytim_workband *wb;
    struct fytim_workpane *wp;
    struct fytim_surface *tile;

    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(fytim_workband_bind(wb, "tail") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_bind(wb, "prompt") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_bind(wb, "completion") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_bind(wb, "") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_bind(wb, "a b") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_bind(wb, "ok") == FYTIM_OK);
    CHECK(fytim_workband_bind(wb, NULL) == FYTIM_OK);

    wp = fytim_workpane_create(h.ft);
    tile = fytim_surface_open_in(wp, 3, 20);
    CHECK(tile != NULL);
    /* a tile of a pane can stand in a slot of its own */
    CHECK(fytim_surface_bind(tile, "tile") == FYTIM_OK);
    CHECK(fytim_surface_bind(tile, "prompt") == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_bind(wp, "pane") == FYTIM_OK);
    CHECK(fytim_workpane_bind(wp, "prompt") == FYTIM_ERR_INVALID);
    h_close(&h);
}

static void test_null_safety(void)
{
    struct harness h;

    CHECK(fytim_page_set(NULL, "x", 1, NULL, 0) == FYTIM_ERR_INVALID);
    fytim_page_clear(NULL);
    CHECK(!fytim_page_active(NULL));
    CHECK(fytim_page_rows(NULL) == 0);
    CHECK(fytim_workband_bind(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_bind(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_bind(NULL, "x") == FYTIM_ERR_INVALID);
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_page_set(h.ft, NULL, 3, NULL, 0) == FYTIM_ERR_INVALID);
    /* An empty page is a page of no rows, not a cleared one. */
    CHECK(fytim_page_set(h.ft, NULL, 0, NULL, 0) == FYTIM_OK);
    CHECK(fytim_page_active(h.ft));
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_close(&h);
}

/* Destroying a bound component leaves no binding behind. */
static void test_a_retired_component_leaves_its_slot(void)
{
    struct harness h;
    struct fytim_workband *wb;
    struct fytim_surface *sf;
    struct fytim_page_region r[2] = {
        { .id = "band", .kind = FYTIM_PAGE_SLOT, .row = 0, .width = 20,
          .height = 1 },
        { .id = "screen", .kind = FYTIM_PAGE_SLOT, .row = 1, .width = 20,
          .height = 2 },
    };
    char buf[16384];

    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    sf = fytim_surface_open(h.ft, 2, 20);
    CHECK(fytim_workband_bind(wb, "band") == FYTIM_OK);
    CHECK(fytim_surface_bind(sf, "screen") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "\n\n\n", 3, r, 2) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    fytim_workband_destroy(wb);
    fytim_surface_close(sf);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_close(&h);
}

static void complete_two(void *user, const char *text,
                         struct fytim_completions *c)
{
    (void)user;
    (void)text;
    (void)fytim_completion_add(c, "alpha");
    (void)fytim_completion_add(c, "beta");
}

/* The tail rows a page sizes its tail slot from. */
static void test_the_tail_reports_its_rows(void)
{
    struct harness h;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_tail_rows(h.ft) == 0);
    CHECK(fytim_tail_set(h.ft, "one\ntwo\n", 8) == FYTIM_OK);
    CHECK(fytim_tail_rows(h.ft) == 2);
    CHECK(fytim_tail_set(h.ft, "one\ntwo\nthree", 13) == FYTIM_OK);
    CHECK(fytim_tail_rows(h.ft) == 3);
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);
    CHECK(fytim_tail_rows(h.ft) == 0);
    h_close(&h);
}

/* The prompt rows follow the lines edited, keep one row while a surface holds
 * the keys, and are 0 without a prompt. */
static void test_the_prompt_reports_its_rows(void)
{
    struct harness h;
    struct fytim_surface *sf;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_prompt_rows(h.ft) == 1);
    CHECK(fytim_set_input(h.ft, "first\nsecond\nthird") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_prompt_rows(h.ft) == 3);
    sf = fytim_surface_open(h.ft, 2, 20);
    CHECK(fytim_surface_set_keys(sf, true) == FYTIM_OK);
    CHECK(fytim_prompt_rows(h.ft) == 1);
    CHECK(fytim_surface_set_keys(sf, false) == FYTIM_OK);
    CHECK(fytim_prompt_rows(h.ft) == 3);
    CHECK(fytim_set_prompt_enabled(h.ft, false) == FYTIM_OK);
    CHECK(fytim_prompt_rows(h.ft) == 0);
    h_close(&h);
}

/* The prompt stands on a card with a style or a ground of its own, and not
 * while a surface holds the keys. */
static void test_the_prompt_reports_its_card(void)
{
    struct harness h;
    struct fytim_surface *sf;

    CHECK(!fytim_prompt_card(NULL));
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(!fytim_prompt_card(h.ft));
    CHECK(fytim_set_prompt_style(h.ft, "\x1b[7m") == FYTIM_OK);
    CHECK(fytim_prompt_card(h.ft));
    CHECK(fytim_set_prompt_style(h.ft, NULL) == FYTIM_OK);
    CHECK(!fytim_prompt_card(h.ft));
    CHECK(fytim_set_prompt_bg(h.ft, 0x303030) == FYTIM_OK);
    CHECK(fytim_prompt_card(h.ft));
    sf = fytim_surface_open(h.ft, 2, 20);
    CHECK(fytim_surface_set_keys(sf, true) == FYTIM_OK);
    CHECK(!fytim_prompt_card(h.ft));
    CHECK(fytim_surface_set_keys(sf, false) == FYTIM_OK);
    CHECK(fytim_prompt_card(h.ft));
    h_close(&h);
}

/* Completion is active while Tab cycles several candidates. */
static void test_completion_reports_its_state(void)
{
    struct harness h;
    char buf[16384];

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(!fytim_completion_active(h.ft));
    CHECK(fytim_set_complete_fn(h.ft, complete_two, NULL) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_type(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_type(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_completion_active(h.ft));
    h_type(&h, "x");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(!fytim_completion_active(h.ft));
    (void)h_out(&h, buf, sizeof buf);
    h_close(&h);
}

/* The pane rows are its tiles and their heads. */
static void test_the_pane_reports_its_rows(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_rows(wp) == 0);
    a = fytim_surface_open_in(wp, 3, 80);
    CHECK(a != NULL);
    CHECK(fytim_workpane_rows(wp) == 3);
    CHECK(fytim_surface_set_top(a, "TILE") == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 4);
    /* the chrome of the pane is not a row of its slot */
    CHECK(fytim_workpane_set_top(wp, "PANE") == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 4);
    fytim_surface_close(a);
    CHECK(fytim_workpane_rows(wp) == 0);
    h_close(&h);
}

static void test_sizes_are_null_safe(void)
{
    CHECK(fytim_tail_rows(NULL) == 0);
    CHECK(fytim_prompt_rows(NULL) == 0);
    CHECK(!fytim_completion_active(NULL));
    CHECK(fytim_workpane_rows(NULL) == 0);
}

static void paint_tile(struct fytim_surface *sf, uint32_t ch)
{
    struct fytim_cell cells[8];
    int rows = 0, cols = 0, r, c;

    memset(cells, 0, sizeof cells);
    for(c = 0; c < 8; c++){
        cells[c].chars[0] = ch;
        cells[c].fg = FYTIM_COLOR_DEFAULT;
        cells[c].bg = FYTIM_COLOR_DEFAULT;
    }
    fytim_surface_size(sf, &rows, &cols);
    for(r = 0; r < rows; r++)
        (void)fytim_surface_put_row(sf, r, cells, cols < 8 ? cols : 8);
}

/* The head and the foot of a tile page are rows of the tile. */
static void test_a_tile_page_takes_its_rows(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 2, .col = 0, .width = 40, .height = 1
    };

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_surface_set_top(a, "OLD TOP") == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 4);
    /* two head rows and one foot row, where the chrome had one top row */
    CHECK(fytim_surface_set_page(a, "HEAD\nCOMMAND\n\nFOOT\n", 19, &r, 1) ==
          FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 6);
    /* removing the page gives the chrome back */
    CHECK(fytim_surface_set_page(a, NULL, 0, NULL, 0) == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 4);
    h_close(&h);
}

/* A click on an act of a tile page names the tile and the cell. */
static void test_a_tile_act_names_its_tile(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_page_region r[2] = {
        { .id = "tile:zoom", .kind = FYTIM_PAGE_ACT,
          .row = 0, .col = 6, .width = 4, .height = 1 },
        { .id = "screen", .kind = FYTIM_PAGE_SLOT,
          .row = 1, .col = 0, .width = 80, .height = 1 },
    };
    struct fytim_event ev;
    struct h_events evs;
    char buf[16384];

    if(!h_open_mouse(&h, true)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    CHECK(fytim_workpane_set_controls(wp, FYTIM_WORKPANE_ZOOM |
                                          FYTIM_WORKPANE_CLOSE) == FYTIM_OK);
    a = fytim_surface_open_in(wp, 3, 80);
    paint_tile(a, 'A');
    CHECK(fytim_surface_set_page(a, "HEAD  zoom\n\n", 12, r, 2) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);

    h_click(&h, 7, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(h_event(&evs, FYTIM_EVENT_ACT, &ev));
    CHECK(ev.surface == a && ev.row == 0 && ev.col == 7);
    CHECK(ev.text && ev.text_len == 9 && !memcmp(ev.text, "tile:zoom", 9));
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_CLICK, NULL));

    /* the library drew no marks: the right edge is not a control */
    h_click(&h, 79, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_ZOOM, NULL));
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_CLOSE, NULL));

    /* the screen of the program is not an act */
    h_click(&h, 7, 2);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_ACT, NULL));

    /* a page with no head rows still takes the marks off the first row */
    CHECK(fytim_surface_set_page(a, "\n", 1, &r[1], 0) == FYTIM_OK);
    r[1].row = 0;
    CHECK(fytim_surface_set_page(a, "\n", 1, &r[1], 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    h_drain(&h, &evs);
    h_click(&h, 79, 0);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h, &evs);
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_ZOOM, NULL));
    CHECK(!h_event(&evs, FYTIM_EVENT_SURFACE_CLOSE, NULL));
    h_close(&h);
}

/* A view changes what a tile draws, never the rows it was granted. */
static void test_a_page_view_keeps_the_grant(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 2, .col = 0, .width = 80, .height = 1
    };
    char buf[16384];
    int full = -1, rows = -1, wanted;

    CHECK(fytim_surface_set_page_view(NULL, FYTIM_PAGE_VIEW_FULL) ==
          FYTIM_ERR_INVALID);
    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_surface_set_page(a, "HEAD\nCMD\n\nFOOT\n", 16, &r, 1) ==
          FYTIM_OK);
    wanted = fytim_workpane_rows(wp);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(a, &full) == FYTIM_OK);
    CHECK(full == 3);

    CHECK(fytim_surface_set_page_view(a, FYTIM_PAGE_VIEW_SCREEN) == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == wanted);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(a, &rows) == FYTIM_OK);
    CHECK(rows == full);

    CHECK(fytim_surface_set_page_view(a, FYTIM_PAGE_VIEW_HEAD) == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == wanted);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(a, &rows) == FYTIM_OK);
    CHECK(rows == full);

    CHECK(fytim_surface_set_page_view(a, (enum fytim_page_view)9) ==
          FYTIM_ERR_INVALID);
    (void)h_out(&h, buf, sizeof buf);
    h_close(&h);
}

/* A committed tile page keeps its head and its foot in the transcript, as a
 * tile with chrome keeps its chrome. */
static void test_a_committed_tile_page_keeps_its_head(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 2, .col = 0, .width = 80, .height = 1
    };
    char buf[16384];
    size_t n;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 2, 80);
    paint_tile(a, 'G');
    CHECK(fytim_surface_set_top(a, "OLDTOP") == FYTIM_OK);
    CHECK(fytim_surface_set_page(a, "HEADROW\nCMDROW\n\nFOOTROW\n", 24, &r,
                                 1) == FYTIM_OK);
    /* the view does not decide what the transcript keeps */
    CHECK(fytim_surface_set_page_view(a, FYTIM_PAGE_VIEW_SCREEN) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_surface_commit(a) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "HEADROW"));
    CHECK(contains(buf, n, "CMDROW"));
    CHECK(contains(buf, n, "GGGGGGGG"));
    CHECK(contains(buf, n, "FOOTROW"));
    CHECK(!contains(buf, n, "OLDTOP"));
    h_close(&h);
}

/* Tiles bound to slots of their own are granted their slots, and a pane
 * that holds them is not drawn in a slot of its own as well. */
static void test_bound_tiles_take_their_slots(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fytim_page_region r[2] = {
        { .id = "tile:a", .kind = FYTIM_PAGE_SLOT, .row = 0, .col = 0,
          .width = 30, .height = 4 },
        { .id = "tile:b", .kind = FYTIM_PAGE_SLOT, .row = 0, .col = 31,
          .width = 40, .height = 2 },
    };
    int rows = -1, cols = -1;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 6, 80);
    b = fytim_surface_open_in(wp, 6, 80);
    CHECK(fytim_surface_bind(a, "tile:a") == FYTIM_OK);
    CHECK(fytim_surface_bind(b, "tile:b") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "\n\n\n\n", 4, r, 2) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(a, &rows) == FYTIM_OK && rows == 4);
    CHECK(fytim_surface_granted_cols(a, &cols) == FYTIM_OK && cols == 30);
    CHECK(fytim_surface_granted_rows(b, &rows) == FYTIM_OK && rows == 2);
    CHECK(fytim_surface_granted_cols(b, &cols) == FYTIM_OK && cols == 40);
    h_close(&h);
}

/* The rows a tile asks for count its head and its foot. */
static void test_a_tile_reports_its_rows(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_workband *b;
    struct fytim_page_region r = {
        .id = "screen", .kind = FYTIM_PAGE_SLOT,
        .row = 2, .col = 0, .width = 40, .height = 1
    };

    CHECK(fytim_surface_rows(NULL) == 0);
    CHECK(fytim_workband_rows(NULL) == 0);
    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_surface_rows(a) == 3);
    CHECK(fytim_surface_set_top(a, "TOP") == FYTIM_OK);
    CHECK(fytim_surface_rows(a) == 4);
    CHECK(fytim_surface_set_page(a, "HEAD\nCMD\n\nFOOT\n", 16, &r, 1) ==
          FYTIM_OK);
    CHECK(fytim_surface_rows(a) == 3 + 2 + 1);
    b = fytim_workband_create_in(wp);
    CHECK(fytim_workband_set(b, "one\ntwo\n", 8) == FYTIM_OK);
    CHECK(fytim_workband_set_top(b, "HEAD") == FYTIM_OK);
    CHECK(fytim_workband_rows(b) == 3);
    h_close(&h);
}

/* A page that places some tiles of a pane grants the others nothing. */
static void test_an_unplaced_tile_is_granted_nothing(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a, *b;
    struct fytim_page_region r = {
        .id = "tile:a", .kind = FYTIM_PAGE_SLOT, .row = 0, .col = 0,
        .width = 30, .height = 3
    };
    int rows = -1;

    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    b = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_workpane_bind(wp, "pane") == FYTIM_OK);
    {
        struct fytim_page_region pr = {
            .id = "pane", .kind = FYTIM_PAGE_SLOT, .row = 0, .col = 0,
            .width = 80, .height = 3
        };
        CHECK(fytim_page_set(h.ft, "\n\n\n", 3, &pr, 1) == FYTIM_OK);
    }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(b, &rows) == FYTIM_OK && rows > 0);
    /* the page now places a alone */
    CHECK(fytim_workpane_bind(wp, NULL) == FYTIM_OK);
    CHECK(fytim_surface_bind(a, "tile:a") == FYTIM_OK);
    CHECK(fytim_page_set(h.ft, "\n\n\n", 3, &r, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(a, &rows) == FYTIM_OK && rows == 3);
    CHECK(fytim_surface_granted_rows(b, &rows) == FYTIM_OK && rows == 0);
    h_close(&h);
}

#define CG_ROWS 4
#define CG_COLS 10

static void cells_fill(struct fytim_cell *g, uint32_t ch)
{
    int i;

    memset(g, 0, sizeof(*g) * CG_ROWS * CG_COLS);
    for(i = 0; i < CG_ROWS * CG_COLS; i++){
        g[i].chars[0] = ch;
        g[i].fg = g[i].bg = FYTIM_COLOR_DEFAULT;
        g[i].width = 1;
    }
}

#define CELL(g, r, c) ((g)[(r) * CG_COLS + (c)])

/* Rows land at their cell, and what the text does not reach is kept. */
static void test_text_draws_into_cells(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];

    cells_fill(g, 'x');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 1, 2, 8, 3,
                                "ab\ncd", 5) == 2);
    CHECK(CELL(g, 1, 2).chars[0] == 'a' && CELL(g, 1, 3).chars[0] == 'b');
    CHECK(CELL(g, 2, 2).chars[0] == 'c' && CELL(g, 2, 3).chars[0] == 'd');
    CHECK(CELL(g, 1, 1).chars[0] == 'x' && CELL(g, 1, 4).chars[0] == 'x');
    CHECK(CELL(g, 0, 2).chars[0] == 'x' && CELL(g, 3, 2).chars[0] == 'x');
    CHECK(CELL(g, 1, 2).fg == FYTIM_COLOR_DEFAULT && CELL(g, 1, 2).attrs == 0);
}

/* The style carries across rows until it is reset. */
static void test_text_carries_its_style(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];
    const char text[] = "\x1b[1mA\nB\x1b[0mC\n\x1b[38;2;1;2;3mX";

    cells_fill(g, ' ');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, CG_COLS, CG_ROWS,
                                text, sizeof text - 1) == 3);
    CHECK((CELL(g, 0, 0).attrs & FYTIM_ATTR_BOLD) != 0);
    CHECK((CELL(g, 1, 0).attrs & FYTIM_ATTR_BOLD) != 0);
    CHECK(CELL(g, 1, 1).chars[0] == 'C' &&
          (CELL(g, 1, 1).attrs & FYTIM_ATTR_BOLD) == 0);
    CHECK(CELL(g, 2, 0).chars[0] == 'X' && CELL(g, 2, 0).fg == 0x010203);
}

/* A row is cut at its width with an ellipsis, and rows past the height are
 * not drawn. */
static void test_text_is_cut_to_its_box(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];

    cells_fill(g, 'x');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, 4, 2,
                                "abcdef\n12\n34", 13) == 2);
    CHECK(CELL(g, 0, 0).chars[0] == 'a' && CELL(g, 0, 2).chars[0] == 'c');
    CHECK(CELL(g, 0, 3).chars[0] == 0x2026);
    CHECK(CELL(g, 0, 4).chars[0] == 'x');
    CHECK(CELL(g, 1, 0).chars[0] == '1');
    CHECK(CELL(g, 2, 0).chars[0] == 'x');
    /* the box is cut to the grid */
    cells_fill(g, 'x');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 3, 8, 50, 50,
                                "abcd\nef", 7) == 1);
    CHECK(CELL(g, 3, 8).chars[0] == 'a' && CELL(g, 3, 9).chars[0] == 0x2026);
}

/* A wide glyph takes two cells; a combining mark joins its base. */
static void test_text_measures_glyphs(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];

    cells_fill(g, 'x');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, CG_COLS, 1,
                                "\xe4\xb8\xadz", 4) == 1);
    CHECK(CELL(g, 0, 0).chars[0] == 0x4e2d && CELL(g, 0, 0).width == 2);
    CHECK(CELL(g, 0, 1).chars[0] == 0);
    CHECK(CELL(g, 0, 2).chars[0] == 'z');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 1, 0, CG_COLS, 1,
                                "e\xcc\x81!", 4) == 1);
    CHECK(CELL(g, 1, 0).chars[0] == 'e' && CELL(g, 1, 0).chars[1] == 0x301);
    CHECK(CELL(g, 1, 1).chars[0] == '!');
    /* a wide glyph with one column left is cut */
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 2, 0, 2, 1,
                                "a\xe4\xb8\xad", 4) == 1);
    CHECK(CELL(g, 2, 0).chars[0] == 'a' && CELL(g, 2, 1).chars[0] == 0x2026);
}

static void test_text_rejects_bad_input(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];
    const char bad[] = "ok\x1b[2Jgone";

    cells_fill(g, 'x');
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, CG_COLS, CG_ROWS,
                                bad, sizeof bad - 1) == -1);
    CHECK(CELL(g, 0, 0).chars[0] == 'x');
    CHECK(fytim_cells_draw_text(NULL, CG_ROWS, CG_COLS, 0, 0, 1, 1, "a", 1) ==
          -1);
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, 1, 1, NULL, 1) ==
          -1);
    CHECK(fytim_cells_draw_text(g, -1, CG_COLS, 0, 0, 1, 1, "a", 1) == -1);
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, 1, 1, NULL, 0) ==
          0);
    /* a box outside the grid or with no size draws nothing */
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 9, 0, 1, 1, "a", 1) ==
          0);
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, -1, 1, 1, "a", 1) ==
          -1);
    CHECK(fytim_cells_draw_text(g, CG_ROWS, CG_COLS, 0, 0, 0, 1, "a", 1) == 0);
    CHECK(CELL(g, 0, 0).chars[0] == 'x');
}

/* A ground takes the box and nothing past it; a cell keeps what it says. */
static void test_cells_take_a_ground(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];

    cells_fill(g, ' ');
    CELL(g, 0, 1).fg = 0x112233;
    CELL(g, 0, 1).bg = 0x445566;
    CELL(g, 0, 1).attrs = FYTIM_ATTR_DIM | FYTIM_ATTR_BOLD;
    CELL(g, 0, 2).fg = 0x112233;
    CELL(g, 0, 2).attrs = FYTIM_ATTR_REVERSE;
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, 0, 0, 3, 2, 0x0a0b0c) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x0a0b0c && CELL(g, 1, 2).bg == 0x0a0b0c);
    CHECK(CELL(g, 0, 1).fg == 0x112233 && CELL(g, 0, 1).bg == 0x0a0b0c &&
          CELL(g, 0, 1).attrs == FYTIM_ATTR_BOLD);
    /* a reversed cell says its ground as its text */
    CHECK(CELL(g, 0, 2).fg == FYTIM_COLOR_DEFAULT &&
          CELL(g, 0, 2).bg == 0x0a0b0c && CELL(g, 0, 2).attrs == 0);
    CHECK(CELL(g, 0, 3).bg == FYTIM_COLOR_DEFAULT);
    CHECK(CELL(g, 2, 0).bg == FYTIM_COLOR_DEFAULT);

    /* the ground the terminal draws text in */
    cells_fill(g, ' ');
    CELL(g, 0, 0).fg = 0x112233;
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, 0, 0, 2, 1,
                             FYTIM_COLOR_REVERSED) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x112233 && CELL(g, 0, 0).fg ==
          FYTIM_COLOR_DEFAULT && (CELL(g, 0, 0).attrs & FYTIM_ATTR_REVERSE));
    CHECK(CELL(g, 0, 1).bg == FYTIM_COLOR_DEFAULT &&
          (CELL(g, 0, 1).attrs & FYTIM_ATTR_REVERSE));
    CHECK(!(CELL(g, 0, 2).attrs & FYTIM_ATTR_REVERSE));

    /* no ground, a box off the grid, and bad arguments */
    cells_fill(g, ' ');
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, 0, 0, CG_COLS, CG_ROWS,
                             FYTIM_COLOR_DEFAULT) == 0);
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, 3, 8, 50, 50, 0x010101) ==
          0);
    CHECK(CELL(g, 3, 9).bg == 0x010101 && CELL(g, 2, 9).bg ==
          FYTIM_COLOR_DEFAULT && CELL(g, 0, 0).bg == FYTIM_COLOR_DEFAULT);
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, CG_ROWS, 0, 1, 1, 1) == 0);
    CHECK(fytim_cells_ground(NULL, CG_ROWS, CG_COLS, 0, 0, 1, 1, 1) == -1);
    CHECK(fytim_cells_ground(g, CG_ROWS, CG_COLS, -1, 0, 1, 1, 1) == -1);
}

/* The cells of a program take a wash: a colour of its own is mixed only in
 * 24-bit colour, and a palette colour and a ground of its own are kept. */
static void test_cells_take_a_wash(void)
{
    struct fytim_cell g[CG_ROWS * CG_COLS];

    cells_fill(g, ' ');
    CELL(g, 0, 1).bg = 0x101010;
    CELL(g, 0, 1).attrs = FYTIM_ATTR_DIM;
    CELL(g, 0, 2).bg = FYTIM_COLOR_INDEXED | 4;
    CELL(g, 0, 3).fg = 0x101010;
    CELL(g, 0, 3).attrs = FYTIM_ATTR_REVERSE;
    CHECK(fytim_cells_wash(g, CG_ROWS, CG_COLS, 0, 0, 4, 1, 0x707070, 50,
                           true) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x707070);
    CHECK(CELL(g, 0, 1).bg == 0x404040 && CELL(g, 0, 1).attrs == 0);
    CHECK(CELL(g, 0, 2).bg == (FYTIM_COLOR_INDEXED | 4));
    /* a reversed cell shows its foreground as its ground */
    CHECK(CELL(g, 0, 3).fg == 0x404040 &&
          CELL(g, 0, 3).bg == FYTIM_COLOR_DEFAULT);
    CHECK(CELL(g, 1, 0).bg == FYTIM_COLOR_DEFAULT);

    /* without 24-bit colour a colour is kept, unless the mix is whole */
    cells_fill(g, ' ');
    CELL(g, 0, 0).bg = 0x101010;
    CHECK(fytim_cells_wash(g, CG_ROWS, CG_COLS, 0, 0, 1, 1, 0x707070, 50,
                           false) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x101010);
    CHECK(fytim_cells_wash(g, CG_ROWS, CG_COLS, 0, 0, 1, 1, 0x707070, 100,
                           false) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x707070);

    /* the ground the terminal draws text in */
    cells_fill(g, ' ');
    CELL(g, 0, 0).fg = 0x112233;
    CELL(g, 0, 1).bg = 0x445566;
    CHECK(fytim_cells_wash(g, CG_ROWS, CG_COLS, 0, 0, 2, 1,
                           FYTIM_COLOR_REVERSED, 0, false) == 0);
    CHECK(CELL(g, 0, 0).bg == 0x112233 &&
          CELL(g, 0, 0).fg == FYTIM_COLOR_DEFAULT &&
          (CELL(g, 0, 0).attrs & FYTIM_ATTR_REVERSE));
    CHECK(CELL(g, 0, 1).bg == 0x445566 &&
          !(CELL(g, 0, 1).attrs & FYTIM_ATTR_REVERSE));
    CHECK(fytim_cells_wash(NULL, CG_ROWS, CG_COLS, 0, 0, 1, 1, 1, 0,
                           false) == -1);
}

/* A host reads back what a band holds. */
static void test_a_band_reads_back(void)
{
    struct fytim_workband *wb;
    struct harness h;
    const char body[] = "one\ntwo\n\x1b[0m";
    int rows = -1;

    if(!h_open(&h)){
        CHECK(0);
        return;
    }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_top(wb) == NULL && fytim_workband_bottom(wb) == NULL);
    CHECK(fytim_workband_set(wb, body, sizeof body - 1) == FYTIM_OK);
    CHECK(fytim_workband_set_top(wb, "") == FYTIM_OK);
    CHECK(fytim_workband_set_bottom(wb, "FOOT") == FYTIM_OK);
    CHECK(fytim_workband_set_max_rows(wb, 3) == FYTIM_OK);
    /* a last row of styling alone takes no row */
    CHECK(fytim_workband_content(wb, &rows) != NULL && rows == 2);
    CHECK(fytim_workband_content(wb, NULL) != NULL);
    CHECK(fytim_workband_top(wb) != NULL && fytim_workband_top(wb)[0] == '\0');
    CHECK(fytim_workband_bottom(wb) != NULL &&
          !strcmp(fytim_workband_bottom(wb), "FOOT"));
    CHECK(fytim_workband_max_rows(wb) == 3);
    CHECK(fytim_workband_content(NULL, &rows) == NULL && rows == 0);
    CHECK(fytim_workband_top(NULL) == NULL && fytim_workband_max_rows(NULL) == 0);
    fytim_workband_destroy(wb);
    h_close(&h);
}

/* A host reads back the cells and the cursor it published. */
static void test_a_surface_reads_back(void)
{
    struct harness h;
    struct fytim_surface *sf;
    struct fytim_cell row[3];
    const struct fytim_cell *got;
    bool visible = false;
    uint32_t bg = 0;
    int r = -1, c = -1, i;

    if(!h_open(&h)){
        CHECK(0);
        return;
    }
    sf = fytim_surface_open(h.ft, 2, 3);
    CHECK(sf != NULL);
    memset(row, 0, sizeof row);
    for(i = 0; i < 3; i++){
        row[i].fg = row[i].bg = FYTIM_COLOR_DEFAULT;
        row[i].width = 1;
    }
    row[1].chars[0] = 'q';
    CHECK(fytim_surface_put_row(sf, 1, row, 3) == FYTIM_OK);
    got = fytim_surface_row(sf, 1);
    CHECK(got != NULL && got[1].chars[0] == 'q');
    CHECK(fytim_surface_row(sf, 2) == NULL && fytim_surface_row(sf, -1) == NULL &&
          fytim_surface_row(NULL, 0) == NULL);
    CHECK(fytim_surface_set_cursor(sf, 1, 2, true) == FYTIM_OK);
    CHECK(fytim_surface_cursor(sf, &r, &c, &visible) == FYTIM_OK &&
          r == 1 && c == 2 && visible);
    CHECK(fytim_surface_cursor(NULL, &r, &c, &visible) == FYTIM_ERR_INVALID);
    CHECK(!fytim_truecolor(NULL));
    /* the chrome it stands in */
    CHECK(fytim_surface_margin(sf, &c) == NULL && c == 0);
    CHECK(fytim_surface_set_margin(sf, "\x1b[7m::\x1b[27m") == FYTIM_OK);
    CHECK(fytim_surface_margin(sf, &c) != NULL && c == 2);
    CHECK(fytim_surface_margin(sf, NULL) != NULL);
    CHECK(fytim_surface_bg(sf, &bg, &r) == FYTIM_OK &&
          bg == FYTIM_COLOR_DEFAULT);
    CHECK(fytim_surface_set_bg(sf, 0x123456, 150) == FYTIM_OK);
    CHECK(fytim_surface_bg(sf, &bg, &r) == FYTIM_OK && bg == 0x123456 &&
          r == 100);
    CHECK(fytim_surface_bg(NULL, &bg, &r) == FYTIM_ERR_INVALID);
    CHECK(fytim_surface_margin(NULL, &c) == NULL && c == 0);
    fytim_surface_close(sf);
    h_close(&h);
}

static void test_rejects_bad_tile_pages(void)
{
    struct harness h;
    struct fytim_workpane *wp;
    struct fytim_surface *a;
    struct fytim_page_region two[2] = {
        { .id = "screen", .kind = FYTIM_PAGE_SLOT, .row = 0, .width = 1,
          .height = 1 },
        { .id = "screen", .kind = FYTIM_PAGE_SLOT, .row = 1, .width = 1,
          .height = 1 },
    };
    const char bad[] = "HEAD\x1b[2J\n\n";

    CHECK(fytim_surface_set_page(NULL, "x\n", 2, NULL, 0) ==
          FYTIM_ERR_INVALID);
    if(!h_open(&h)){ CHECK(0); return; }
    wp = fytim_workpane_create(h.ft);
    a = fytim_surface_open_in(wp, 3, 80);
    CHECK(fytim_surface_set_page(a, "HEAD\n\n", 6, two, 2) ==
          FYTIM_ERR_INVALID);
    CHECK(fytim_surface_set_page(a, "KEPT\n\n", 6, two, 1) == FYTIM_OK);
    CHECK(fytim_workpane_rows(wp) == 4);
    CHECK(fytim_surface_set_page(a, bad, sizeof bad - 1, NULL, 0) ==
          FYTIM_ERR_INVALID);
    CHECK(fytim_workpane_rows(wp) == 4);
    CHECK(fytim_surface_set_page(a, "x\n", 2, NULL, 1) == FYTIM_ERR_INVALID);
    /* a surface of its own is not a tile */
    CHECK(fytim_surface_set_page(fytim_surface_open(h.ft, 2, 20), "x\n", 2,
                                 NULL, 0) == FYTIM_ERR_INVALID);
    h_close(&h);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    { "a_page_is_set_and_cleared", test_a_page_is_set_and_cleared },
    { "a_page_replaces_the_band_stack", test_a_page_replaces_the_band_stack },
    { "a_region_extends_the_page", test_a_region_extends_the_page },
    { "a_slot_grants_its_region", test_a_slot_grants_its_region },
    { "an_unbound_band_is_not_drawn", test_an_unbound_band_is_not_drawn },
    { "a_bound_band_draws_in_its_slot", test_a_bound_band_draws_in_its_slot },
    { "the_prompt_slot_edits", test_the_prompt_slot_edits },
    { "no_prompt_slot_makes_no_line", test_no_prompt_slot_makes_no_line },
    { "a_click_on_an_act_reports_its_id",
      test_a_click_on_an_act_reports_its_id },
    { "a_bind_takes_the_id", test_a_bind_takes_the_id },
    { "rejects_disallowed_rows", test_rejects_disallowed_rows },
    { "rejects_bad_regions", test_rejects_bad_regions },
    { "regions_are_copied", test_regions_are_copied },
    { "no_act_without_the_grab", test_no_act_without_the_grab },
    { "rejects_bad_binds", test_rejects_bad_binds },
    { "null_safety", test_null_safety },
    { "a_retired_component_leaves_its_slot",
      test_a_retired_component_leaves_its_slot },
    { "the_tail_reports_its_rows", test_the_tail_reports_its_rows },
    { "the_prompt_reports_its_rows", test_the_prompt_reports_its_rows },
    { "completion_reports_its_state", test_completion_reports_its_state },
    { "the_pane_reports_its_rows", test_the_pane_reports_its_rows },
    { "sizes_are_null_safe", test_sizes_are_null_safe },
    { "the_prompt_reports_its_card", test_the_prompt_reports_its_card },
    { "a_tile_page_takes_its_rows", test_a_tile_page_takes_its_rows },
    { "a_tile_act_names_its_tile", test_a_tile_act_names_its_tile },
    { "rejects_bad_tile_pages", test_rejects_bad_tile_pages },
    { "a_page_view_keeps_the_grant", test_a_page_view_keeps_the_grant },
    { "bound_tiles_take_their_slots", test_bound_tiles_take_their_slots },
    { "a_tile_reports_its_rows", test_a_tile_reports_its_rows },
    { "an_unplaced_tile_is_granted_nothing",
      test_an_unplaced_tile_is_granted_nothing },
    { "text_draws_into_cells", test_text_draws_into_cells },
    { "text_carries_its_style", test_text_carries_its_style },
    { "text_is_cut_to_its_box", test_text_is_cut_to_its_box },
    { "text_measures_glyphs", test_text_measures_glyphs },
    { "text_rejects_bad_input", test_text_rejects_bad_input },
    { "cells_take_a_ground", test_cells_take_a_ground },
    { "cells_take_a_wash", test_cells_take_a_wash },
    { "a_surface_reads_back", test_a_surface_reads_back },
    { "a_band_reads_back", test_a_band_reads_back },
    { "a_committed_tile_page_keeps_its_head",
      test_a_committed_tile_page_keeps_its_head },
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
