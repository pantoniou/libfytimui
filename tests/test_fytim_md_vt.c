/*
 * test_fytim_md_vt.c - the bubble invariant against the REAL renderer.
 *
 * Streams an actual markdown document through libfymd4c's progressive
 * renderer into the public fytim_tail_apply path, replays every emitted
 * byte into libvterm, and asserts the chrome row after EVERY pump:
 *
 *   - with the screen full, the bubble NEVER moves during the stream --
 *     growth scrolls text, freezes cancel against their commits, and an
 *     unmatched shrink (a heal retracting) is held;
 *   - the stream-end settle leaves exactly one line of breath.
 *
 * The synthetic tests pin hand-crafted updates; this one pins whatever
 * the renderer actually produces, in byte and line chunking both, so a
 * renderer-side change that breaks the invariant is caught here.
 *
 * Only built when libvterm AND libfymd4c are available.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"
#include <libfymd4c.h>
#include <vterm.h>

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

/* agent_md's canned reply, verbatim: heading, wrapped paragraphs, a
 * list, a fenced block (freezes line by line), a table (block
 * lookahead), inline emphasis under healing -- the exact stream whose
 * live trace showed the unmatched-shrink bounce. */
static const char doc[] =
    "# Streaming markdown\n"
    "\n"
    "This reply is rendered by **libfymd4c** and published through the\n"
    "public `fytim_*` surface only. Frozen lines land in *native\n"
    "scrollback*; the active region re-renders in the transcript's live\n"
    "tail -- work-bands are for tools, this stream never touches them.\n"
    "\n"
    "## What to look for\n"
    "\n"
    "- the band grows while a block is still **open**\n"
    "- finished blocks scroll away and are selectable with the mouse\n"
    "- links stay clickable: [libfyaml](https://github.com/pantoniou/libfyaml)\n"
    "\n"
    "```c\n"
    "int main(void)\n"
    "{\n"
    "    printf(\"code blocks freeze line by line\\n\");\n"
    "    return 0;\n"
    "}\n"
    "```\n"
    "\n"
    "| stage  | where it renders     |\n"
    "|--------|----------------------|\n"
    "| frozen | terminal scrollback  |\n"
    "| active | transcript tail      |\n"
    "\n"
    "And a closing paragraph so every block above freezes.\n";

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
    h->vt = vterm_new(24, 80);
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
}

static int marker_row(struct vth *h)
{
    int r, c;
    for(r = 0; r < 24; r++){
        char line[96];
        int o = 0;
        for(c = 0; c < 80 && o < 90; c++){
            VTermScreenCell cell;
            VTermPos p = { r, c };
            vterm_screen_get_cell(h->vs, p, &cell);
            line[o++] = (cell.chars[0] >= 32 && cell.chars[0] < 127)
                        ? (char)cell.chars[0] : ' ';
        }
        line[o] = '\0';
        if(strstr(line, "HDRMARK")) return r;
    }
    return -1;
}

static int screen_find_row(VTermScreen *vs, int rows, int cols,
                           const char *needle)
{
    int r, c;
    for(r = 0; r < rows; r++){
        char line[128];
        int o = 0;
        for(c = 0; c < cols && o < (int)sizeof line - 1; c++){
            VTermScreenCell cell;
            VTermPos p = { r, c };
            vterm_screen_get_cell(vs, p, &cell);
            line[o++] = (cell.chars[0] >= 32 && cell.chars[0] < 127)
                        ? (char)cell.chars[0] : ' ';
        }
        line[o] = '\0';
        if(strstr(line, needle)) return r;
    }
    return -1;
}

/*
 * The direct libfymd4c render is the oracle: committing those exact bytes
 * through fytim must produce the same cells, including reverse background
 * across both blank fence rows and from content end to the right edge.
 */
static void test_reverse_commit_matches_direct(void)
{
    static const char md[] = "> hello\n";
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct vth h;
    VTerm *direct;
    VTermScreen *ds;
    char *out = NULL;
    char *framed = NULL;
    size_t out_len = 0, framed_len, commit_len;
    int dy, x, yd, yc, mismatches, first_dy, first_x;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 80;
    cfg.flags = FYMD_RF_REVERSE;
    r = fymd_renderer_create(&cfg);
    CHECK(r != NULL);
    if(!r) return;
    CHECK(fymd_render(r, md, sizeof md - 1, &out, &out_len) == 0);
    CHECK(out != NULL && out_len > 0);
    if(!out || !out_len){ fymd_renderer_destroy(r); return; }
    framed_len = 13 + out_len + 13;
    framed = malloc(framed_len);
    CHECK(framed != NULL);
    if(!framed){ fymd_free(out); fymd_renderer_destroy(r); return; }
    memcpy(framed, "\x1b[40m\x1b[K\x1b[0m\n", 13);
    memcpy(framed + 13, out, out_len);
    memcpy(framed + 13 + out_len, "\x1b[40m\x1b[K\x1b[0m\n", 13);

    direct = vterm_new(24, 80);
    CHECK(direct != NULL);
    if(!direct){
        free(framed);
        fymd_free(out);
        fymd_renderer_destroy(r);
        return;
    }
    vterm_set_utf8(direct, 1);
    ds = vterm_obtain_screen(direct);
    vterm_screen_reset(ds, 1);
    vterm_input_write(direct, framed, framed_len);
    yd = screen_find_row(ds, 24, 80, "hello");
    CHECK(yd > 0 && yd + 1 < 24);

    if(!vth_open(&h)){
        CHECK(0);
        vterm_free(direct);
        free(framed);
        fymd_free(out);
        fymd_renderer_destroy(r);
        return;
    }
    commit_len = framed_len;
    while(commit_len && (framed[commit_len - 1] == '\n' ||
                         framed[commit_len - 1] == '\r'))
        commit_len--;
    CHECK(fytim_commit(h.ft, framed, commit_len) == FYTIM_OK);
    vth_pump(&h);
    yc = screen_find_row(h.vs, 24, 80, "hello");
    CHECK(yc > 0 && yc + 1 < 24);

    if(yd > 0 && yc > 0){
        mismatches = 0;
        first_dy = first_x = -1;
        for(dy = -1; dy <= 1; dy++){
            for(x = 0; x < 80; x++){
                VTermScreenCell a, b;
                VTermPos pa = { yd + dy, x };
                VTermPos pb = { yc + dy, x };
                vterm_screen_get_cell(ds, pa, &a);
                vterm_screen_get_cell(h.vs, pb, &b);
                vterm_screen_convert_color_to_rgb(ds, &a.bg);
                vterm_screen_convert_color_to_rgb(h.vs, &b.bg);
                if(!vterm_color_is_equal(&a.bg, &b.bg)){
                    if(!mismatches){ first_dy = dy; first_x = x; }
                    mismatches++;
                }
            }
        }
        if(mismatches)
            printf("    reverse-card bg mismatch: %d cells, first row %+d col %d\n",
                   mismatches, first_dy, first_x);
        CHECK(mismatches == 0);
    }
    vth_close(&h);
    vterm_free(direct);
    free(framed);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

static void direct_apply(VTerm *vt, const struct fymd_update *upd)
{
    char ctl[64];
    int n;
    if(upd->backtrack){
        n = snprintf(ctl, sizeof ctl, "\x1b[%zuA\r\x1b[J", upd->backtrack);
        vterm_input_write(vt, ctl, (size_t)n);
    }
    if(upd->content_len)
        vterm_input_write(vt, upd->content, upd->content_len);
}

static void screen_row_ascii(VTermScreen *vs, int row, int cols,
                             char *out, size_t cap)
{
    int c, o = 0;
    for(c = 0; c < cols && o < (int)cap - 1; c++){
        VTermScreenCell cell;
        VTermPos p = { row, c };
        vterm_screen_get_cell(vs, p, &cell);
        out[o++] = (cell.chars[0] >= 32 && cell.chars[0] < 127)
                    ? (char)cell.chars[0] : ' ';
    }
    out[o] = '\0';
}

static int screen_find_number(VTermScreen *vs, int rows, int cols, int number)
{
    char line[128], want[16], *p, *end;
    int r;
    snprintf(want, sizeof want, "%d", number);
    for(r = 0; r < rows; r++){
        screen_row_ascii(vs, r, cols, line, sizeof line);
        p = line;
        while(*p == ' ') p++;
        end = p + strlen(p);
        while(end > p && end[-1] == ' ') *--end = '\0';
        if(strcmp(p, want) == 0) return r;
    }
    return -1;
}

static void test_fenced_stream_matches_direct(void)
{
    static const char *chunks[] = {
        "A fenced block follows:\n\n```text\n",
        "0\n", "1\n", "2\n", "3\n", "4\n", "```\n"
    };
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct fymd_update upd;
    struct vth h;
    VTerm *direct;
    VTermScreen *ds;
    size_t i;
    int n, rd, rc, prev_d = -1, prev_c = -1;

    if(!vth_open(&h)){ CHECK(0); return; }
    direct = vterm_new(24, 80);
    CHECK(direct != NULL);
    if(!direct){ vth_close(&h); return; }
    vterm_set_utf8(direct, 1);
    ds = vterm_obtain_screen(direct);
    vterm_screen_reset(ds, 1);

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 80;
    cfg.flags = FYMD_RF_HEAL;
    cfg.max_active_lines = 12;
    r = fymd_renderer_create(&cfg);
    CHECK(r != NULL);
    if(!r){ vterm_free(direct); vth_close(&h); return; }

    for(i = 0; i < sizeof chunks / sizeof chunks[0]; i++){
        CHECK(fymd_render_push(r, chunks[i], strlen(chunks[i]), &upd) == 0);
        direct_apply(direct, &upd);
        CHECK(fytim_tail_apply(h.ft, upd.backtrack, upd.content,
                               upd.content_len, upd.freeze) == FYTIM_OK);
        vth_pump(&h);
    }

    for(n = 0; n <= 4; n++){
        rd = screen_find_number(ds, 24, 80, n);
        rc = screen_find_number(h.vs, 24, 80, n);
        CHECK(rd >= 0);
        CHECK(rc >= 0);
        if(prev_d >= 0 && rd >= 0) CHECK(rd == prev_d + 1);
        if(prev_c >= 0 && rc >= 0) CHECK(rc == prev_c + 1);
        prev_d = rd;
        prev_c = rc;
    }
    fymd_renderer_destroy(r);
    vterm_free(direct);
    vth_close(&h);
}

static void test_emoji_table_matches_direct(void)
{
    static const char md[] =
        "| Status | Value |\n"
        "|---|---|\n"
        "| ✅ OK | 42 |\n"
        "| ⚠ Warn | 7 |\n"
        "| ❌ Fail | 0 |\n";
    static const char *needles[] = { "OK", "Warn" };
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct vth h;
    VTerm *direct;
    VTermScreen *ds;
    char *out = NULL;
    size_t out_len = 0, commit_len, i;
    int yd, yc, x;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 40;
    r = fymd_renderer_create(&cfg);
    CHECK(r != NULL);
    if(!r) return;
    CHECK(fymd_render(r, md, sizeof md - 1, &out, &out_len) == 0);
    CHECK(out != NULL && out_len > 0);
    if(!out || !out_len){ fymd_renderer_destroy(r); return; }

    direct = vterm_new(24, 80);
    CHECK(direct != NULL);
    if(!direct){
        fymd_free(out);
        fymd_renderer_destroy(r);
        return;
    }
    vterm_set_utf8(direct, 1);
    ds = vterm_obtain_screen(direct);
    vterm_screen_reset(ds, 1);
    {
        const char *p = out, *end = out + out_len, *nl;
        while(p < end){
            nl = memchr(p, '\n', (size_t)(end - p));
            if(!nl){
                vterm_input_write(direct, p, (size_t)(end - p));
                break;
            }
            vterm_input_write(direct, p, (size_t)(nl - p));
            vterm_input_write(direct, "\r\n", 2);
            p = nl + 1;
        }
    }

    if(!vth_open(&h)){
        CHECK(0);
        vterm_free(direct);
        fymd_free(out);
        fymd_renderer_destroy(r);
        return;
    }
    commit_len = out_len;
    while(commit_len && (out[commit_len - 1] == '\n' ||
                         out[commit_len - 1] == '\r'))
        commit_len--;
    CHECK(fytim_commit(h.ft, out, commit_len) == FYTIM_OK);
    vth_pump(&h);

    for(i = 0; i < sizeof needles / sizeof needles[0]; i++){
        yd = screen_find_row(ds, 24, 80, needles[i]);
        yc = screen_find_row(h.vs, 24, 80, needles[i]);
        CHECK(yd >= 0 && yc >= 0);
        if(yd < 0 || yc < 0) continue;
        for(x = 0; x < 40; x++){
            VTermScreenCell a, b;
            VTermPos pa = { yd, x };
            VTermPos pb = { yc, x };
            vterm_screen_get_cell(ds, pa, &a);
            vterm_screen_get_cell(h.vs, pb, &b);
            CHECK(a.chars[0] == b.chars[0]);
        }
    }

    vth_close(&h);
    vterm_free(direct);
    fymd_free(out);
    fymd_renderer_destroy(r);
}

/* Stream `doc` through the real renderer in `chunk`-byte pieces (whole
 * lines when chunk == 0), pumping and checking the bubble after every
 * push. */
static void run_stream(int chunk)
{
    struct vth h;
    struct fymd_renderer_cfg cfg;
    struct fymd_renderer *r;
    struct fymd_update upd;
    size_t off = 0, n;
    char line[32];
    int i, y0;

    if(!vth_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, " HDRMARK") == FYTIM_OK);
    for(i = 0; i < 30; i++){           /* fill: the band pins to the bottom */
        snprintf(line, sizeof line, "F%02d", i);
        CHECK(fytim_commit(h.ft, line, strlen(line)) == FYTIM_OK);
    }
    vth_pump(&h);
    y0 = marker_row(&h);
    CHECK(y0 > 0);

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 80;
    cfg.flags = FYMD_RF_HEAL;
    cfg.max_active_lines = 8;
    r = fymd_renderer_create(&cfg);
    CHECK(r != NULL);
    if(!r){ vth_close(&h); return; }

    while(off < sizeof doc - 1){
        if(chunk > 0){
            n = sizeof doc - 1 - off;
            if(n > (size_t)chunk) n = (size_t)chunk;
        }else{
            const char *nl = memchr(doc + off, '\n', sizeof doc - 1 - off);
            n = nl ? (size_t)(nl - (doc + off)) + 1 : sizeof doc - 1 - off;
        }
        CHECK(fymd_render_push(r, doc + off, n, &upd) == 0);
        off += n;
        CHECK(fytim_tail_apply(h.ft, upd.backtrack, upd.content,
                               upd.content_len, upd.freeze) == FYTIM_OK);
        vth_pump(&h);
        if(marker_row(&h) != y0){
            CHECK(marker_row(&h) == y0);   /* report the offset it broke at */
            printf("    (chunk=%d, %zu/%zu bytes pushed)\n",
                   chunk, off, sizeof doc - 1);
            break;
        }
    }

    /* finish: healed remainder commits, tail clears, ONE settle at most,
     * ending in the one-line-of-breath idle layout */
    {
        const char *fin = NULL;
        size_t fin_len = 0;
        CHECK(fymd_render_finish(r, &fin, &fin_len) == 0);
        if(fin_len > 0){
            size_t l = fin_len;
            while(l && fin[l - 1] == '\n') l--;
            CHECK(fytim_commit(h.ft, fin, l) == FYTIM_OK);
        }
        CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);
        vth_pump(&h);
        CHECK(marker_row(&h) >= y0 - 1 && marker_row(&h) <= y0);
        /* the breath: the doc's last text sits two rows above the header
         * (its own row, one blank line, chrome) */
        {
            int yl = -1, rr, c;
            for(rr = 0; rr < 24; rr++){
                char t[96];
                int o = 0;
                for(c = 0; c < 80 && o < 90; c++){
                    VTermScreenCell cell;
                    VTermPos p = { rr, c };
                    vterm_screen_get_cell(h.vs, p, &cell);
                    t[o++] = (cell.chars[0] >= 32 && cell.chars[0] < 127)
                             ? (char)cell.chars[0] : ' ';
                }
                t[o] = '\0';
                if(strstr(t, "every block above freezes")) yl = rr;
            }
            CHECK(yl > 0 && marker_row(&h) == yl + 2);
        }
    }
    fymd_renderer_destroy(r);
    vth_close(&h);
}

static void test_bubble_pinned_bytes(void){ run_stream(7); }
static void test_bubble_pinned_lines(void){ run_stream(0); }
static void test_bubble_pinned_single(void){ run_stream(1); }

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "reverse_commit_matches_direct", test_reverse_commit_matches_direct },
        { "fenced_stream_matches_direct", test_fenced_stream_matches_direct },
        { "emoji_table_matches_direct", test_emoji_table_matches_direct },
        { "bubble_pinned_bytes",  test_bubble_pinned_bytes },
        { "bubble_pinned_lines",  test_bubble_pinned_lines },
        { "bubble_pinned_single", test_bubble_pinned_single },
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
