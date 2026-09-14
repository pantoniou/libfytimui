/*
 * bench_fytim_page.c - the cost of a page against the band stack.
 *
 * A host that states its screen as UI Markdown renders it with libfymd4c and
 * gives the library the rows and the regions. The question this measures is
 * how much of that page a host can render again for each frame:
 *
 *   stack      the band stack: chrome as SGR strings, a tile head rendered
 *              when it changes. The reference.
 *   immediate  the root page and every tile head are rendered for each frame.
 *   retained   a part is rendered only when its source changes: one tile head
 *              for each frame, the root page once a second.
 *
 * Each frame every tile gets a new row of output, the tail changes and the
 * prompt holds a line, as a busy session does. The terminal is a
 * pseudo-terminal of the size under test, read by a thread for the whole run
 * so that a large frame never blocks the pump.
 *
 * Usage: bench_fytim_page [--frames N]   print a table of frame times
 *        bench_fytim_page smoke           run each mode for a few frames
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"
#include <libfymd4c.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TILES_MAX 16
#define SURFACE_ROWS 8

enum mode { MODE_STACK, MODE_IMMEDIATE, MODE_RETAINED, MODE_COUNT };

static const char *mode_name[MODE_COUNT] = { "stack", "immediate", "retained" };

struct run {
    enum mode mode;
    int tiles;
    int rows, cols;
    int frames;
};

struct result {
    double render_p50, render_p99;
    double pump_p50, pump_p99;
    double total_p99, total_mean;
    bool ok;
};

struct drain {
    int fd;
    atomic_bool stop;
};

/* Read the terminal for the whole run: a pump never waits on a full pty. */
static void *drain_main(void *arg)
{
    struct drain *d = arg;
    struct pollfd pfd = { .fd = d->fd, .events = POLLIN };
    char buf[65536];

    while(!atomic_load(&d->stop)){
        if(poll(&pfd, 1, 20) <= 0) continue;
        while(read(d->fd, buf, sizeof buf) > 0)
            ;
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

static double pct(double *v, int n, double p)
{
    int i = (int)(p * (n - 1) + 0.5);

    return v[i < n ? i : n - 1];
}

static const char *no_margin(void *userdata, size_t row)
{
    (void)userdata;
    (void)row;
    return "";
}

static struct fymd_renderer *renderer(int width, int height)
{
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;

    memset(&cfg, 0, sizeof cfg);
    cfg.flags = FYMD_RF_DEFAULT | FYMD_RF_UI;
    cfg.width = width > 1 ? width : 1;
    cfg.background = FYMD_BG_DARK;
    r = fymd_renderer_create(&cfg);
    if(r && height > 0 && fymd_renderer_set_height(r, height) != 0){
        fymd_renderer_destroy(r);
        return NULL;
    }
    return r;
}

/* Render @md; the rows without the trailing newlines, owned by the caller. */
static char *render(struct fymd_renderer *r, const char *md, size_t *lenp)
{
    char *out = NULL;
    size_t len = 0;

    if(fymd_render_with_margins(r, md, strlen(md), no_margin, NULL, &out,
                                &len) != 0)
        return NULL;
    while(len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    *lenp = len;
    return out;
}

static int head_md(char *buf, size_t size, int tile, int frame)
{
    return snprintf(buf, size,
                    "<fy-act id=\"tile:focus\">shell: make -j12 target-%d"
                    "</fy-act><fy-fill/><fy-role name=\"chrome\">%ds "
                    "%c</fy-role> <fy-act id=\"tile:zoom\">[+]</fy-act>"
                    "<fy-act id=\"tile:close\">[x]</fy-act>\n",
                    tile, frame / 60, "|/-\\"[frame % 4]);
}

static int root_md(char *buf, size_t size, const struct run *run, int frame)
{
    return snprintf(buf, size,
                    "<fy-role name=\"chrome\">work</fy-role> main \xc2\xb7 "
                    "gpt-5<fy-fill/><fy-act id=\"pane:height\">half</fy-act>"
                    " \xc2\xb7 %d tiles \xc2\xb7 %ds\n\n"
                    "<fy-slot id=\"tail\" height=\"3\"/>\n\n"
                    "<fy-slot id=\"pane\" height=\"*\" min=\"3\"/>\n\n"
                    "<fy-slot id=\"prompt\" height=\"1\"/>\n\n"
                    "<fy-role name=\"chrome\">3 active \xc2\xb7 2 waiting"
                    "</fy-role><fy-fill/>184.2k tokens \xc2\xb7 $1.37\n",
                    run->tiles, frame / 60);
}

static bool set_head(struct fymd_renderer *r, struct fytim_surface *sf,
                     int tile, int frame)
{
    char md[512];
    char *rows;
    size_t len;
    bool ok;

    head_md(md, sizeof md, tile, frame);
    rows = render(r, md, &len);
    if(!rows) return false;
    ok = fytim_surface_set_top(sf, rows) == FYTIM_OK;
    fymd_free(rows);
    return ok;
}

static bool set_root(struct fymd_renderer *r, struct fytim *ft,
                     const struct run *run, int frame)
{
    struct fytim_page_region regions[FYTIM_PAGE_REGIONS_MAX];
    const struct fymd_region *fr;
    char md[2048];
    char *rows;
    size_t len, count, i, n = 0;
    bool ok;

    root_md(md, sizeof md, run, frame);
    rows = render(r, md, &len);
    if(!rows) return false;
    if(fymd_renderer_get_regions(r, &fr, &count) != 0){
        fymd_free(rows);
        return false;
    }
    for(i = 0; i < count && n < FYTIM_PAGE_REGIONS_MAX; i++, n++){
        regions[n].id = fr[i].id;
        regions[n].kind = fr[i].kind == FYMD_REGION_ACT ? FYTIM_PAGE_ACT
                                                         : FYTIM_PAGE_SLOT;
        regions[n].row = (int)fr[i].row;
        regions[n].col = fr[i].col;
        regions[n].width = fr[i].width;
        regions[n].height = fr[i].height;
    }
    ok = fytim_page_set(ft, rows, len, regions, n) == FYTIM_OK;
    fymd_free(rows);
    return ok;
}

/* The chrome of the band stack: the same header and status, rendered. */
static bool set_stack_chrome(struct fymd_renderer *r, struct fytim *ft,
                             const struct run *run, int frame)
{
    char md[512];
    char *rows;
    size_t len;
    bool ok;

    snprintf(md, sizeof md,
             "<fy-role name=\"chrome\">work</fy-role> main \xc2\xb7 gpt-5"
             "<fy-fill/>half \xc2\xb7 %d tiles \xc2\xb7 %ds\n",
             run->tiles, frame / 60);
    rows = render(r, md, &len);
    if(!rows) return false;
    ok = fytim_set_header(ft, rows) == FYTIM_OK;
    fymd_free(rows);
    if(!ok) return false;
    snprintf(md, sizeof md,
             "3 active \xc2\xb7 2 waiting<fy-fill/>184.2k tokens \xc2\xb7 "
             "$1.37\n");
    rows = render(r, md, &len);
    if(!rows) return false;
    ok = fytim_set_status_row(ft, 1, rows) == FYTIM_OK;
    fymd_free(rows);
    return ok;
}

static void put_line(struct fytim_surface *sf, int cols, int frame, int tile)
{
    struct fytim_cell row[512];
    char text[64];
    int c, n;

    if(cols > (int)(sizeof row / sizeof row[0]))
        cols = (int)(sizeof row / sizeof row[0]);
    n = snprintf(text, sizeof text, "[%d] frame %d output line", tile, frame);
    for(c = 0; c < cols; c++){
        row[c].chars[0] = c < n ? (uint32_t)(unsigned char)text[c] : ' ';
        row[c].chars[1] = 0;
        row[c].fg = FYTIM_COLOR_DEFAULT;
        row[c].bg = FYTIM_COLOR_DEFAULT;
        row[c].attrs = 0;
        row[c].width = 1;
    }
    (void)fytim_surface_put_row(sf, frame % SURFACE_ROWS, row, cols);
}

static struct result bench(const struct run *run)
{
    struct result res = { 0 };
    struct fytim_surface *sf[TILES_MAX] = { NULL };
    struct fymd_renderer *root = NULL, *head = NULL;
    struct fytim_workpane *wp = NULL;
    struct fytim_cfg cfg;
    struct winsize ws;
    struct drain d;
    pthread_t thread;
    double *render_ms = NULL, *pump_ms = NULL, *total_ms = NULL;
    double t1, t2, t3, sum = 0;
    char tail[256];
    struct fytim *ft = NULL;
    int mfd = -1, sfd = -1, i, f, tile_cols;
    bool ok = true, threaded = false;

    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)run->rows;
    ws.ws_col = (unsigned short)run->cols;
    if(openpty(&mfd, &sfd, NULL, NULL, &ws) != 0) return res;
    fcntl(mfd, F_SETFL, O_NONBLOCK);
    d.fd = mfd;
    atomic_init(&d.stop, false);
    if(pthread_create(&thread, NULL, drain_main, &d) != 0) goto out;
    threaded = true;

    fytim_cfg_default(&cfg);
    cfg.input_fd = sfd;
    cfg.output_fd = sfd;
    ft = fytim_create(&cfg);
    render_ms = calloc((size_t)run->frames, sizeof *render_ms);
    pump_ms = calloc((size_t)run->frames, sizeof *pump_ms);
    total_ms = calloc((size_t)run->frames, sizeof *total_ms);
    if(!ft || !render_ms || !pump_ms || !total_ms) goto out;

    tile_cols = run->tiles > 1 ? run->cols / 2 : run->cols;
    root = renderer(run->cols, run->mode == MODE_STACK ? 0 : run->rows);
    head = renderer(tile_cols, 0);
    if(!root || !head) goto out;
    if(run->tiles){
        wp = fytim_workpane_create(ft);
        if(!wp) goto out;
        (void)fytim_workpane_set_max_rows(wp, run->rows - 8);
        for(i = 0; i < run->tiles; i++){
            sf[i] = fytim_surface_open_in(wp, SURFACE_ROWS, run->cols);
            if(!sf[i] || !set_head(head, sf[i], i, 0)) goto out;
        }
        if(run->mode != MODE_STACK &&
           fytim_workpane_bind(wp, "pane") != FYTIM_OK)
            goto out;
    }
    if(run->mode == MODE_STACK)
        ok = set_stack_chrome(root, ft, run, 0);
    else
        ok = set_root(root, ft, run, 0);
    if(!ok) goto out;
    (void)fytim_set_input(ft, "explain the failing test in tests/page.c");

    for(f = 0; f < run->frames && ok; f++){
        for(i = 0; i < run->tiles; i++)
            put_line(sf[i], run->cols, f, i);
        snprintf(tail, sizeof tail, "streamed row %d\nstreamed row %d\n",
                 f, f + 1);
        (void)fytim_tail_set(ft, tail, strlen(tail));

        t1 = now_ms();
        switch(run->mode){
        case MODE_STACK:
            if(run->tiles)
                ok = set_head(head, sf[f % run->tiles], f % run->tiles, f);
            if(ok && f % 60 == 0)
                ok = set_stack_chrome(root, ft, run, f);
            break;
        case MODE_IMMEDIATE:
            for(i = 0; i < run->tiles && ok; i++)
                ok = set_head(head, sf[i], i, f);
            if(ok)
                ok = set_root(root, ft, run, f);
            break;
        case MODE_RETAINED:
            if(run->tiles)
                ok = set_head(head, sf[f % run->tiles], f % run->tiles, f);
            if(ok && f % 60 == 0)
                ok = set_root(root, ft, run, f);
            break;
        default:
            ok = false;
        }
        t2 = now_ms();
        if(fytim_pump(ft) != FYTIM_OK) ok = false;
        t3 = now_ms();
        render_ms[f] = t2 - t1;
        pump_ms[f] = t3 - t2;
        total_ms[f] = t3 - t1;
        sum += t3 - t1;
    }
    if(!ok) goto out;

    qsort(render_ms, (size_t)run->frames, sizeof *render_ms, cmp_double);
    qsort(pump_ms, (size_t)run->frames, sizeof *pump_ms, cmp_double);
    qsort(total_ms, (size_t)run->frames, sizeof *total_ms, cmp_double);
    res.render_p50 = pct(render_ms, run->frames, 0.50);
    res.render_p99 = pct(render_ms, run->frames, 0.99);
    res.pump_p50 = pct(pump_ms, run->frames, 0.50);
    res.pump_p99 = pct(pump_ms, run->frames, 0.99);
    res.total_p99 = pct(total_ms, run->frames, 0.99);
    res.total_mean = sum / run->frames;
    res.ok = true;

out:
    if(ft) fytim_destroy(ft);
    if(root) fymd_renderer_destroy(root);
    if(head) fymd_renderer_destroy(head);
    if(threaded){
        atomic_store(&d.stop, true);
        pthread_join(thread, NULL);
    }
    free(render_ms);
    free(pump_ms);
    free(total_ms);
    if(sfd >= 0) close(sfd);
    if(mfd >= 0) close(mfd);
    return res;
}

static int smoke(void)
{
    struct run run = { .tiles = 2, .rows = 24, .cols = 80, .frames = 20 };
    struct result res;
    int m, failures = 0;

    for(m = 0; m < MODE_COUNT; m++){
        run.mode = (enum mode)m;
        res = bench(&run);
        if(!res.ok){
            printf("  FAIL %s: the run did not complete\n", mode_name[m]);
            failures++;
        }
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    static const int tiles[] = { 0, 1, 4, 8, 16 };
    static const int sizes[][2] = { { 24, 80 }, { 60, 200 }, { 120, 400 } };
    struct run run;
    struct result res;
    int frames = 600, s, t, m;

    if(argc > 1 && !strcmp(argv[1], "--list")){
        printf("smoke\n");
        return 0;
    }
    if(argc > 1 && !strcmp(argv[1], "smoke"))
        return smoke();
    if(argc > 2 && !strcmp(argv[1], "--frames"))
        frames = atoi(argv[2]);
    if(frames < 10){
        fprintf(stderr, "--frames: at least 10\n");
        return 2;
    }

    printf("| size | tiles | mode | render p50 | render p99 | pump p50 | "
           "pump p99 | frame p99 | fps |\n");
    printf("|---|---:|---|---:|---:|---:|---:|---:|---:|\n");
    for(s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++)
        for(t = 0; t < (int)(sizeof tiles / sizeof tiles[0]); t++)
            for(m = 0; m < MODE_COUNT; m++){
                run.mode = (enum mode)m;
                run.tiles = tiles[t];
                run.rows = sizes[s][0];
                run.cols = sizes[s][1];
                run.frames = frames;
                res = bench(&run);
                if(!res.ok){
                    printf("| %dx%d | %d | %s | failed |\n", run.cols,
                           run.rows, run.tiles, mode_name[m]);
                    continue;
                }
                printf("| %dx%d | %d | %s | %.3f | %.3f | %.3f | %.3f | "
                       "%.3f | %.0f |\n", run.cols, run.rows, run.tiles,
                       mode_name[m], res.render_p50, res.render_p99,
                       res.pump_p50, res.pump_p99, res.total_p99,
                       1e3 / res.total_mean);
            }
    return 0;
}
