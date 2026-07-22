/*
 * test_fytim_vt.c - public-surface visual tests through libvterm.
 *
 * Drives libfytimui through the PUBLIC interface and replays every emitted
 * byte into a real VT emulator, then asserts on the resulting screen grid.
 * This catches band-geometry and repaint bugs that byte-substring checks
 * cannot: overlapping work-bands, shed chrome rows, stale widths after a
 * resize.
 *
 * Only built when libvterm is available (see tests/CMakeLists.txt).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"
#include <vterm.h>

#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

/* ---- a fytim instance replayed into libvterm ---------------------------- */
struct vth {
    struct fytim *ft;
    VTerm *vt;
    VTermScreen *vs;
    int rows, cols;
    int in[2];     /* pipe pair: we write keys into in[1] */
    int out[2];
    int mfd, sfd;  /* pty pair when opened with vth_open_pty */
};

static int vth_open(struct vth *h)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    h->mfd = h->sfd = -1;
    h->rows = 24; h->cols = 80;
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    h->vt = vterm_new(h->rows, h->cols);
    vterm_set_utf8(h->vt, 1);
    h->vs = vterm_obtain_screen(h->vt);
    vterm_screen_reset(h->vs, 1);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

/* As above but over a real pty, so TIOCGWINSZ works and can be changed. */
static int vth_open_pty(struct vth *h, int rows, int cols)
{
    struct fytim_cfg cfg;
    struct winsize ws;
    memset(h, 0, sizeof *h);
    h->in[0] = h->in[1] = h->out[0] = h->out[1] = -1;
    h->rows = rows; h->cols = cols;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if(openpty(&h->mfd, &h->sfd, NULL, NULL, &ws) != 0) return 0;
    fcntl(h->mfd, F_SETFL, O_NONBLOCK);
    h->vt = vterm_new(rows, cols);
    vterm_set_utf8(h->vt, 1);
    h->vs = vterm_obtain_screen(h->vt);
    vterm_screen_reset(h->vs, 1);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->sfd;
    cfg.output_fd = h->sfd;
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}

static void vth_close(struct vth *h)
{
    fytim_destroy(h->ft);
    if(h->vt) vterm_free(h->vt);
    if(h->mfd >= 0) close(h->mfd);
    if(h->sfd >= 0) close(h->sfd);
    if(h->in[0]  > 0) close(h->in[0]);
    if(h->in[1]  > 0) close(h->in[1]);
    if(h->out[0] > 0) close(h->out[0]);
    if(h->out[1] > 0) close(h->out[1]);
}

/* Pump once and replay whatever reached the "terminal" into libvterm. */
static void vth_pump(struct vth *h)
{
    char buf[65536];
    ssize_t n;
    int fd = h->mfd >= 0 ? h->mfd : h->out[0];
    CHECK(fytim_pump(h->ft) == FYTIM_OK);
    while((n = read(fd, buf, sizeof buf)) > 0)
        vterm_input_write(h->vt, buf, (size_t)n);
}

/* Copy screen row y as ASCII text ('?' for non-ASCII, ' ' for blank). */
static void vth_row(struct vth *h, int y, char *out, size_t cap)
{
    int x;
    size_t o = 0;
    for(x = 0; x < h->cols && o + 1 < cap; x++){
        VTermScreenCell cell;
        VTermPos p = { y, x };
        vterm_screen_get_cell(h->vs, p, &cell);
        out[o++] = cell.chars[0] == 0 ? ' '
                 : (cell.chars[0] >= 32 && cell.chars[0] < 127)
                   ? (char)cell.chars[0] : '?';
    }
    out[o] = '\0';
}

/* Row index whose text contains needle, or -1. */
static int vth_find_row(struct vth *h, const char *needle)
{
    char line[512];
    int y;
    for(y = 0; y < h->rows; y++){
        vth_row(h, y, line, sizeof line);
        if(strstr(line, needle)) return y;
    }
    return -1;
}

/* Number of cells on row y holding the box-drawing rule U+2500. */
static int vth_rule_width(struct vth *h, int y)
{
    int x, n = 0;
    for(x = 0; x < h->cols; x++){
        VTermScreenCell cell;
        VTermPos p = { y, x };
        vterm_screen_get_cell(h->vs, p, &cell);
        if(cell.chars[0] == 0x2500) n++;
    }
    return n;
}

/* ---- regression: a work-band beyond its cap must keep its chrome -------- *
 * Content past max_rows shows only its LAST max_rows lines; the top rule
 * and the bottom status row stay. Two active bands must stay visually
 * separate. (Bug: the draw trim gave content priority over the band's own
 * chrome, so a band past its cap swallowed its rule and status row and two
 * streaming bands merged into one unreadable block.) */
static void test_regression_workband_cap_keeps_chrome(void)
{
    struct vth h;
    struct fytim_workband *w1, *w2;
    int y1, y2, yb1, yb2;
    if(!vth_open(&h)){ CHECK(0); return; }
    w1 = fytim_workband_create(h.ft);
    w2 = fytim_workband_create(h.ft);
    CHECK(w1 && w2);
    CHECK(fytim_workband_set_top(w1, "") == FYTIM_OK);
    CHECK(fytim_workband_set_bottom(w1, " RUN-A") == FYTIM_OK);
    CHECK(fytim_workband_set_top(w2, "") == FYTIM_OK);
    CHECK(fytim_workband_set_bottom(w2, " RUN-B") == FYTIM_OK);
    /* 6 content lines against the default cap of 4 */
    CHECK(fytim_workband_set(w1, "A1\nA2\nA3\nA4\nA5\nA6", 17) == FYTIM_OK);
    CHECK(fytim_workband_set(w2, "B1\nB2\nB3\nB4\nB5\nB6", 17) == FYTIM_OK);
    vth_pump(&h);

    /* last-4 windows only */
    CHECK(vth_find_row(&h, "A1") < 0);
    CHECK(vth_find_row(&h, "A2") < 0);
    CHECK(vth_find_row(&h, "B2") < 0);
    y1 = vth_find_row(&h, "A3");
    y2 = vth_find_row(&h, "B3");
    CHECK(vth_find_row(&h, "A6") == y1 + 3);
    CHECK(vth_find_row(&h, "B6") == y2 + 3);

    /* each band keeps its own chrome: rule above, status below */
    yb1 = vth_find_row(&h, "RUN-A");
    yb2 = vth_find_row(&h, "RUN-B");
    CHECK(yb1 == y1 + 4);
    CHECK(yb2 == y2 + 4);
    CHECK(y1 > 0 && vth_rule_width(&h, y1 - 1) == h.cols);
    CHECK(y2 > 0 && vth_rule_width(&h, y2 - 1) == h.cols);
    /* and the bands do not interleave: band 2 starts below band 1's bottom */
    CHECK(y2 - 1 == yb1 + 1);
    vth_close(&h);
}

/* True when the first cell of the row containing needle is drawn bold. */
static int vth_row_is_bold(struct vth *h, const char *needle)
{
    int y = vth_find_row(h, needle);
    VTermScreenCell cell;
    VTermPos p = { y, 0 };
    if(y < 0) return -1;
    /* the needle's own cell, not column 0 (rows may be indented) */
    {
        char line[512];
        char *at;
        vth_row(h, y, line, sizeof line);
        at = strstr(line, needle);
        p.col = (int)(at - line);
    }
    vterm_screen_get_cell(h->vs, p, &cell);
    return cell.attrs.bold ? 1 : 0;
}

/* ---- regression: SGR state must carry across content rows --------------- *
 * libfymd4c opens a style once and relies on SGR carry-over across '\n'
 * (e.g. a fenced block body is one leading \x1b[2m). Rows are painted and
 * committed independently, so the library must re-open the running state
 * at each row start. (Bug: a work-band or tail lost all styling after its
 * first row, and committed lines after the first lost it in scrollback.) */
static void test_regression_sgr_carries_across_rows(void)
{
    struct vth h;
    struct fytim_workband *wb;
    if(!vth_open(&h)){ CHECK(0); return; }

    /* work-band: dim opened on row 1 must still be live on rows 2 and 3 */
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "\x1b[1mWA1\nWA2\nWA3", 15) == FYTIM_OK);
    /* the transcript tail relies on the same carry-over */
    CHECK(fytim_tail_set(h.ft, "\x1b[1mTA1\nTA2", 11) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_row_is_bold(&h, "WA1") == 1);
    CHECK(vth_row_is_bold(&h, "WA2") == 1);
    CHECK(vth_row_is_bold(&h, "WA3") == 1);
    CHECK(vth_row_is_bold(&h, "TA1") == 1);
    CHECK(vth_row_is_bold(&h, "TA2") == 1);
    fytim_workband_destroy(wb);
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);

    /* committed lines: the per-line reset in scrollback must re-open the
     * carried state on every following line */
    CHECK(fytim_commit(h.ft, "\x1b[1mCA1\nCA2\nCA3\x1b[0m plain", 25) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_row_is_bold(&h, "CA1") == 1);
    CHECK(vth_row_is_bold(&h, "CA2") == 1);
    CHECK(vth_row_is_bold(&h, "CA3") == 1);
    CHECK(vth_row_is_bold(&h, "plain") == 0);   /* the reset still resets */
    vth_close(&h);
}

/* A freeze cut mid-style: the frozen rows keep the style in scrollback AND
 * the remaining tail re-opens it, even though the cut dropped the bytes
 * that carried it. */
static void test_regression_sgr_survives_freeze_cut(void)
{
    struct vth h;
    if(!vth_open(&h)){ CHECK(0); return; }
    /* one dim block of three rows; freeze the first two */
    CHECK(fytim_tail_apply(h.ft, 0, "\x1b[1mFA1\nFA2\nFA3", 15, 2) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_row_is_bold(&h, "FA1") == 1);     /* committed */
    CHECK(vth_row_is_bold(&h, "FA2") == 1);     /* committed */
    CHECK(vth_row_is_bold(&h, "FA3") == 1);     /* still the live tail */
    vth_close(&h);
}

/* ---- regression: the bubble never moves mid-stream ---------------------- *
 * At the screen bottom all motion is text scrolling, never chrome
 * movement: a freeze's commits (+k scroll) and the frame shrink (-k) land
 * in the same pump and cancel. The historical bounce was a counting
 * artifact -- the tail's trailing '\n'/SGR residue counted as a row, a
 * phantom with no matching commit. And the layout keeps exactly ONE line
 * of breath: after a full-block freeze the chrome sits at most one blank
 * row (plus the committed blank separator) below the text, not hanging at
 * a held high-water height. */
static void test_regression_bubble_pinned_while_streaming(void)
{
    struct vth h;
    char line[32];
    int i, y0;
    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, " HDRMARK") == FYTIM_OK);
    /* fill the screen so the band reaches the bottom and pins there */
    for(i = 0; i < 30; i++){
        snprintf(line, sizeof line, "F%02d", i);
        CHECK(fytim_commit(h.ft, line, strlen(line)) == FYTIM_OK);
    }
    vth_pump(&h);
    y0 = vth_find_row(&h, "HDRMARK");
    CHECK(y0 > 0);

    /* a line-mode renderer stream: grow, grow, grow with an SGR residue,
     * then a block boundary freezing everything including its blank
     * separator line -- the chrome must not move on ANY pump */
    CHECK(fytim_tail_apply(h.ft, 0, "A1\n", 3, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    CHECK(fytim_tail_apply(h.ft, 1, "A1\nA2\n\x1b[2m", 11, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    CHECK(fytim_tail_apply(h.ft, 2, "\x1b[0mA1\nA2\nA3\n", 14, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    /* the tail sits DIRECTLY above the chrome while streaming: counting
     * the trailing-'\n' cursor row would wedge a phantom blank row in */
    CHECK(vth_find_row(&h, "HDRMARK") == vth_find_row(&h, "A3") + 1);
    CHECK(fytim_tail_apply(h.ft, 3, "A1\nA2\nA3\n\n", 10, 4) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    /* one breath only: committed blank separator, one spare row, chrome */
    CHECK(vth_find_row(&h, "HDRMARK") == vth_find_row(&h, "A3") + 3);

    /* the next block starts: still pinned */
    CHECK(fytim_tail_apply(h.ft, 0, "B1\n", 3, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    CHECK(vth_find_row(&h, "B1") > 0);

    /* the stream ends with the tail already empty: nothing left to settle,
     * the chrome does not move */
    CHECK(fytim_tail_apply(h.ft, 1, "B1\n", 3, 1) == FYTIM_OK);
    vth_pump(&h);
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    CHECK(vth_find_row(&h, "HDRMARK") == vth_find_row(&h, "B1") + 2);
    vth_close(&h);
}

/* ---- regression: indexed SGR colors must reach the screen --------------- *
 * (Bug: the run-to-cell conversion mapped indexed (16/256-palette) colors
 * to the default, so \x1b[31m..\x1b[33m content rendered colorless --
 * an agent's red/green/yellow status dots all looked the same.) */
static void test_regression_indexed_colors_mapped(void)
{
    struct vth h;
    struct fytim_workband *wb;
    int y;
    VTermScreenCell cell;
    VTermPos p;
    if(!vth_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "\x1b[31mRED\x1b[0m \x1b[32mGRN", 21) == FYTIM_OK);
    vth_pump(&h);
    y = vth_find_row(&h, "RED");
    CHECK(y >= 0);
    p.row = y; p.col = 0;
    vterm_screen_get_cell(h.vs, p, &cell);
    CHECK(VTERM_COLOR_IS_RGB(&cell.fg));
    CHECK(cell.fg.rgb.red > 100 && cell.fg.rgb.green < 80);
    p.col = 4;                                       /* the GRN run */
    vterm_screen_get_cell(h.vs, p, &cell);
    CHECK(VTERM_COLOR_IS_RGB(&cell.fg));
    CHECK(cell.fg.rgb.green > 100 && cell.fg.rgb.red < 80);
    vth_close(&h);
}

/* ---- regression: an unmatched shrink is held, not resized --------------- *
 * The renderer's active region can lose rows WITHOUT freezing anything (a
 * heal retracting, a block re-wrapping). Such a shrink has no commit to
 * cancel against, so resizing to it moves the bubble up and back down.
 * The frame may only shrink by as many rows as were committed in the same
 * pump; the excess is held until commits (or the settle) cover it. */
static void test_regression_unmatched_shrink_held(void)
{
    struct vth h;
    char line[32];
    int i, y0;
    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, " HDRMARK") == FYTIM_OK);
    for(i = 0; i < 30; i++){
        snprintf(line, sizeof line, "F%02d", i);
        CHECK(fytim_commit(h.ft, line, strlen(line)) == FYTIM_OK);
    }
    vth_pump(&h);
    y0 = vth_find_row(&h, "HDRMARK");
    CHECK(y0 > 0);

    CHECK(fytim_tail_set(h.ft, "C1\nC2\nC3\nC4\nC5", 14) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);   /* growth scrolls, pinned */

    /* a heal retracts three rows: no commits, so the frame holds */
    CHECK(fytim_tail_set(h.ft, "C1\nC2", 5) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);

    /* commits arrive (a tool's deferred flush, frozen rows): they release
     * the hold row for row, still without moving the bubble */
    CHECK(fytim_commit(h.ft, "D1\nD2\nD3", 8) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0);
    CHECK(vth_find_row(&h, "D3") > 0);

    /* stream end: the two remaining tail rows give way to the single
     * spare breath row -- ONE settle hop, and only at stream end */
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);
    vth_pump(&h);
    CHECK(vth_find_row(&h, "HDRMARK") == y0 - 1);
    CHECK(vth_find_row(&h, "HDRMARK") == vth_find_row(&h, "D3") + 2);
    vth_close(&h);
}

/* ---- regression: the stream-end settle leaves no gap -------------------- *
 * While the tail streams the frame holds its high-water height; when the
 * stream ends it COMPACTS upward in one atomic hop. With nothing
 * committed, the settled layout must be exactly the pre-stream layout --
 * no blank rows left between the transcript and the chrome. */
static void test_regression_settle_compacts(void)
{
    struct vth h;
    int idle, streaming, after;
    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_status_row(h.ft, 0, " MARKER") == FYTIM_OK);
    vth_pump(&h);
    idle = vth_find_row(&h, "MARKER");
    CHECK(idle > 0);
    CHECK(fytim_tail_set(h.ft, "S1\nS2\nS3\nS4\nS5\nS6", 17) == FYTIM_OK);
    vth_pump(&h);
    streaming = vth_find_row(&h, "MARKER");
    CHECK(streaming > idle);             /* the band grew for the tail */
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);   /* stream ends */
    vth_pump(&h);
    after = vth_find_row(&h, "MARKER");
    CHECK(after == idle);                /* compacted, no residual gap */
    CHECK(vth_find_row(&h, "S1") < 0);   /* the tail area is really gone */
    vth_close(&h);
}

/* ---- regression: a width-only resize must repaint at the new width ------ *
 * (Bug: the pump resized the frame only when the ROW count changed, so a
 * width change left the band painted at the stale width and the status
 * line broke.) */
static void test_regression_resize_repaints_width(void)
{
    struct vth h;
    struct winsize ws;
    struct fytim_event ev;
    int found_resize = 0, y;
    if(!vth_open_pty(&h, 24, 80)){ CHECK(0); return; }
    CHECK(fytim_set_status_row(h.ft, 0, " ST0") == FYTIM_OK);
    CHECK(fytim_set_status_row(h.ft, 1, " ST1") == FYTIM_OK);
    vth_pump(&h);
    y = vth_find_row(&h, "ST1");    /* full chrome present before */
    CHECK(y >= 0);

    /* widen the terminal; same row count so only the width changes */
    memset(&ws, 0, sizeof ws);
    ws.ws_row = 24; ws.ws_col = 100;
    CHECK(ioctl(h.mfd, TIOCSWINSZ, &ws) == 0);
    h.cols = 100;
    vterm_set_size(h.vt, 24, 100);

    vth_pump(&h);
    while(fytim_next_event(h.ft, &ev))
        if(ev.type == FYTIM_EVENT_RESIZE && ev.width == 100 && ev.height == 24)
            found_resize = 1;
    CHECK(found_resize);
    vth_pump(&h);

    /* the separators must now span the full new width */
    for(y = 0; y < h.rows; y++)
        CHECK(vth_rule_width(&h, y) == 0 || vth_rule_width(&h, y) == 100);
    CHECK(vth_find_row(&h, "ST0") >= 0);
    CHECK(vth_find_row(&h, "ST1") >= 0);
    {
        int nrules = 0;
        for(y = 0; y < h.rows; y++)
            if(vth_rule_width(&h, y) == 100) nrules++;
        CHECK(nrules == 2);              /* sep_top and sep_bottom, full width */
    }
    vth_close(&h);
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "regression_workband_cap_keeps_chrome",
          test_regression_workband_cap_keeps_chrome },
        { "regression_resize_repaints_width",
          test_regression_resize_repaints_width },
        { "regression_bubble_pinned_while_streaming",
          test_regression_bubble_pinned_while_streaming },
        { "regression_indexed_colors_mapped",
          test_regression_indexed_colors_mapped },
        { "regression_unmatched_shrink_held",
          test_regression_unmatched_shrink_held },
        { "regression_settle_compacts",
          test_regression_settle_compacts },
        { "regression_sgr_carries_across_rows",
          test_regression_sgr_carries_across_rows },
        { "regression_sgr_survives_freeze_cut",
          test_regression_sgr_survives_freeze_cut },
    };
    size_t i, n = sizeof(tests) / sizeof(tests[0]);

    if(argc == 2 && strcmp(argv[1], "--list") == 0){
        for(i = 0; i < n; ++i) printf("%s\n", tests[i].name);
        return 0;
    }
    if(argc == 2){
        for(i = 0; i < n; ++i)
            if(strcmp(argv[1], tests[i].name) == 0){ tests[i].fn(); return failures ? 1 : 0; }
        fprintf(stderr, "no such test: %s\n", argv[1]);
        return 2;
    }
    for(i = 0; i < n; ++i) tests[i].fn();
    printf(failures ? "%d failure(s)\n" : "all %d ok\n", failures ? failures : (int)n);
    return failures ? 1 : 0;
}
