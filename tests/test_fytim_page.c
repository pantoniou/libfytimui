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
    CHECK(fytim_surface_bind(tile, "tile") == FYTIM_ERR_INVALID);
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
