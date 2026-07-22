/*
 * agent_md.c - libfymd4c markdown streamed into the inline band.
 *
 * The full fyai stack in miniature: the HOST renders markdown with
 * libfymd4c and publishes the result through the public fytim_ surface.
 * A canned markdown reply is pushed chunk-by-chunk through
 * fymd_render_push(); each update is handed to fytim_tail_apply, which
 * routes the frozen rows into native scrollback and re-renders the active
 * region in the transcript's live tail. fymd_render_finish() commits the
 * healed remainder and clears the tail.
 *
 * The renderer is sized to the terminal width (fytim_size), so every
 * committed line arrives hard-wrapped and never soft-wraps -- the layout
 * contract the band model relies on.
 *
 * Usage: agent_md            (type anything and press Enter to stream the
 *                             canned markdown; "/stream FILE [DELAY_MS]"
 *                             streams a markdown file with DELAY_MS between
 *                             chunks, default 60; "/demo N" runs a tool call
 *                             into its own work-band, concurrently with the
 *                             main stream; Esc or /quit quits)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <libfytimui.h>
#include <libfymd4c.h>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char md_doc[] =
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
    "And a closing paragraph so the table above freezes too.\n";

/* One markdown reply streaming through the renderer into the transcript:
 * the agent's STANDARD output goes to the transcript pane immediately --
 * frozen lines commit into scrollback, the active region is the
 * transcript's live tail. Work-bands are a different path (tools,
 * subagents); this stream never touches them. */
struct mdstream {
    struct fymd_renderer *r;
    const char *doc;               /* the markdown being streamed */
    size_t doc_len;
    char *owned;                   /* freed when the stream ends (file case) */
    size_t off;                    /* bytes already pushed */
    int delay_ms;                  /* pacing between chunks */
    long long next_ms;             /* next push not before this */
};

/* A /demo tool call: a child process whose stdout streams into its OWN
 * work-band, concurrently with the main transcript stream in the tail --
 * the two paths are independent and may interleave freely. */
#define JOBS_MAX 4
struct job {
    struct fytim_workband *wb;
    FILE *fp;                      /* popen stream, non-blocking */
    char acc[4096];
    int id;
};

static struct job jobs[JOBS_MAX];
static int job_seq;

static void job_spawn(struct fytim *ft, int count)
{
    char cmd[128], bot[64];
    int i;
    struct job *j = NULL;
    for(i = 0; i < JOBS_MAX; i++)
        if(!jobs[i].fp){ j = &jobs[i]; break; }
    if(!j){
        fytim_commit(ft, "demo: all job slots busy", 24);
        return;
    }
    snprintf(cmd, sizeof cmd,
             "for i in $(seq 1 %d); do echo \"tick $i/%d\"; sleep 1; done",
             count, count);
    j->fp = popen(cmd, "r");
    if(!j->fp) return;
    fcntl(fileno(j->fp), F_SETFL, O_NONBLOCK);
    j->wb = fytim_workband_create(ft);
    j->id = ++job_seq;
    j->acc[0] = '\0';
    fytim_workband_set_top(j->wb, "");             /* a rule above the band */
    snprintf(bot, sizeof bot, " \x1b[2mdemo #%d (%ds) running\x1b[0m",
             j->id, count);
    fytim_workband_set_bottom(j->wb, bot);
}

static void job_tick(struct fytim *ft)
{
    int i;
    for(i = 0; i < JOBS_MAX; i++){
        struct job *j = &jobs[i];
        size_t len;
        char chunk[512];
        ssize_t n;
        if(!j->fp) continue;
        while((n = read(fileno(j->fp), chunk, sizeof chunk - 1)) > 0){
            chunk[n] = '\0';
            len = strlen(j->acc);
            snprintf(j->acc + len, sizeof j->acc - len, "%s", chunk);
        }
        len = strlen(j->acc);
        if(len){                                   /* drop the trailing '\n' */
            size_t show = j->acc[len - 1] == '\n' ? len - 1 : len;
            fytim_workband_set(j->wb, j->acc, show);
        }
        if(n == 0){                                /* EOF: the tool is done */
            char done[64];
            (void)ft;
            pclose(j->fp);
            j->fp = NULL;
            snprintf(done, sizeof done, " \x1b[2mdemo #%d done\x1b[0m", j->id);
            fytim_workband_set_bottom(j->wb, done);
            /* mid-stream this DEFERS: the final render stays until the
             * transcript stream ends, then commits in finish order */
            fytim_workband_commit(j->wb);
            j->wb = NULL;
        }
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void md_start(struct fytim *ft, struct mdstream *s,
                     const char *doc, size_t len, char *owned, int delay_ms)
{
    struct fymd_renderer_cfg cfg;
    int w = 80;
    if(s->r){ free(owned); return; }             /* one stream at a time */
    s->doc = doc;
    s->doc_len = len;
    s->owned = owned;
    s->delay_ms = delay_ms > 0 ? delay_ms : 60;
    s->next_ms = 0;
    fytim_size(ft, &w, NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.width = w;
    cfg.flags = FYMD_RF_HEAL;                    /* heal the streaming tail */
    /* Constrain the streaming ACTIVE region in the renderer: rows past the
     * cap are frozen out early, so the transcript stays complete and the
     * work-band stays bounded. (The line-limit viewport modes are display
     * windows -- they DROP rows, which would hole the transcript.) */
    cfg.max_active_lines = 8;
    s->r = fymd_renderer_create(&cfg);
    if(!s->r) return;
    s->off = 0;
}

static void md_tick(struct fytim *ft, struct mdstream *s)
{
    struct fymd_update upd;
    size_t n;
    if(!s->r) return;
    if(now_ms() < s->next_ms) return;
    s->next_ms = now_ms() + s->delay_ms;

    if(s->off < s->doc_len){
        /* a few bytes per tick, like a token stream; the LIBRARY applies
         * the update -- frozen rows to scrollback, the rest to the tail */
        n = s->doc_len - s->off;
        if(n > 7) n = 7;
        if(fymd_render_push(s->r, s->doc + s->off, n, &upd) != 0) goto fail;
        s->off += n;
        if(fytim_tail_apply(ft, upd.backtrack, upd.content,
                            upd.content_len, upd.freeze) != FYTIM_OK)
            goto fail;
        return;
    }

    /* the stream is exhausted: commit the healed remainder in place of the
     * active region and clear the tail */
    {
        const char *fin = NULL;
        size_t fin_len = 0;
        if(fymd_render_finish(s->r, &fin, &fin_len) == 0 && fin_len > 0){
            while(fin_len && (fin[fin_len - 1] == '\n')) fin_len--;
            fytim_commit(ft, fin, fin_len);
        }
        fytim_tail_set(ft, NULL, 0);
    }
    fymd_renderer_destroy(s->r);
    s->r = NULL;
    free(s->owned);
    s->owned = NULL;
    return;

fail:
    fytim_tail_set(ft, NULL, 0);
    fymd_renderer_destroy(s->r);
    s->r = NULL;
    free(s->owned);
    s->owned = NULL;
    fytim_commit(ft, "render error", 12);
}

/* /stream FILE [DELAY_MS]: stream a markdown file through the renderer. */
static void cmd_stream(struct fytim *ft, struct mdstream *s, const char *args)
{
    char path[512], msg[600];
    int delay = 0;
    FILE *fp;
    char *doc = NULL;
    long fl;
    size_t rd;

    while(*args == ' ') args++;
    if(sscanf(args, "%511s %d", path, &delay) < 1){
        fytim_commit(ft, "usage: /stream FILE [DELAY_MS]", 30);
        return;
    }
    fp = fopen(path, "r");
    if(!fp || fseek(fp, 0, SEEK_END) != 0 || (fl = ftell(fp)) < 0){
        if(fp) fclose(fp);
        snprintf(msg, sizeof msg, "cannot read %s", path);
        fytim_commit(ft, msg, strlen(msg));
        return;
    }
    rewind(fp);
    doc = malloc((size_t)fl + 1);
    if(!doc){ fclose(fp); return; }
    rd = fread(doc, 1, (size_t)fl, fp);
    fclose(fp);
    doc[rd] = '\0';
    snprintf(msg, sizeof msg, "\x1b[2mstreaming %s (%zu bytes, %d ms)\x1b[0m",
             path, rd, delay > 0 ? delay : 60);
    fytim_commit(ft, msg, strlen(msg));
    md_start(ft, s, doc, rd, doc, delay);
}

static void complete_cb(void *user, const char *text,
                        struct fytim_completions *c)
{
    static const char *const cmds[] = { "/demo", "/quit", "/stream" };
    size_t i;
    (void)user;
    for(i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        if(strncmp(cmds[i], text, strlen(text)) == 0)
            fytim_completion_add(c, cmds[i]);
}

int main(void)
{
    struct fytim *ft;
    struct mdstream s;
    int running = 1;

    memset(&s, 0, sizeof s);
    ft = fytim_create(NULL);
    if(!ft){
        fprintf(stderr, "agent_md: failed to open terminal\n");
        return 1;
    }
    fytim_set_header(ft, " agent_md -- libfymd4c through the fytim_ surface");
    fytim_set_status_row(ft, 1,
        " Enter: canned markdown | /stream FILE [MS] | /demo N | Esc quits");
    fytim_set_complete_fn(ft, complete_cb, NULL);

    while(running){
        struct pollfd pfd;
        struct fytim_event ev;

        pfd.fd = fytim_poll_fd(ft);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if(pfd.fd >= 0)
            poll(&pfd, 1, s.r ? (s.delay_ms < 100 ? s.delay_ms : 100) : 100);

        md_tick(ft, &s);
        job_tick(ft);
        fytim_set_status_row(ft, 0, s.r ? " streaming" : " ready");

        if(fytim_pump(ft) != FYTIM_OK) break;
        while(fytim_next_event(ft, &ev)){
            switch(ev.type){
                case FYTIM_EVENT_LINE:
                    if(strcmp(ev.text, "/quit") == 0){ running = 0; break; }
                    fytim_history_add(ft, ev.text);
                    {
                        char echo[512];
                        snprintf(echo, sizeof echo, "\x1b[1m>\x1b[0m %.*s",
                                 (int)ev.text_len, ev.text);
                        fytim_commit(ft, echo, strlen(echo));
                    }
                    if(strncmp(ev.text, "/stream", 7) == 0){
                        cmd_stream(ft, &s, ev.text + 7);
                    }else if(strncmp(ev.text, "/demo", 5) == 0){
                        int dn = atoi(ev.text + 5);
                        if(dn < 1)  dn = 10;
                        if(dn > 60) dn = 60;
                        job_spawn(ft, dn);
                    }else{
                        md_start(ft, &s, md_doc, sizeof md_doc - 1, NULL, 60);
                    }
                    break;
                case FYTIM_EVENT_QUIT:
                case FYTIM_EVENT_INTERRUPT:
                    running = 0;
                    break;
                default:
                    break;
            }
        }
    }

    if(s.r) fymd_renderer_destroy(s.r);
    free(s.owned);
    fytim_destroy(ft);
    return 0;
}
