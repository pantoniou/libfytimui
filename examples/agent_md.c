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
 *                             main stream; a plain line typed MID-stream is
 *                             a new turn: it is buffered in a pending
 *                             work-band and replayed when the turn ends;
 *                             Esc or /quit quits)
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
    struct fymd_renderer *r;       /* streams the output as a fenced block */
    char ren[16384];               /* the rendered block: frozen + active */
    size_t ren_len;
    size_t frozen_len;             /* prefix the renderer will not touch */
    int id;
};

static struct job jobs[JOBS_MAX];
static int job_seq;

/* Apply one progressive update to the job's rendered block -- the same
 * replay fytim_tail_apply does for the transcript, but into the band's
 * own buffer: drop the cursor-row residue plus `backtrack` rows from the
 * active region, append the new content, then freeze rows into the
 * untouchable prefix. */
static void job_apply(struct job *j, const struct fymd_update *u)
{
    size_t pos = j->ren_len, k, n;

    if(u->backtrack){
        while(pos > j->frozen_len && j->ren[pos - 1] != '\n') pos--;
        for(k = 0; k < u->backtrack && pos > j->frozen_len; k++){
            pos--;
            while(pos > j->frozen_len && j->ren[pos - 1] != '\n') pos--;
        }
    }
    j->ren_len = pos;
    n = u->content_len;
    if(n > sizeof j->ren - j->ren_len) n = sizeof j->ren - j->ren_len;
    memcpy(j->ren + j->ren_len, u->content, n);
    j->ren_len += n;
    for(k = 0; k < u->freeze; k++){
        while(j->frozen_len < j->ren_len && j->ren[j->frozen_len] != '\n')
            j->frozen_len++;
        if(j->frozen_len < j->ren_len) j->frozen_len++;   /* the '\n' too */
    }
}

/* Push tool bytes through the job's renderer and refresh the band with
 * the LIVE fenced-block render (trailing newline shed for display). */
static void job_push(struct job *j, const char *buf, size_t len)
{
    struct fymd_update upd;
    size_t show;
    if(!j->r) return;
    if(fymd_render_push(j->r, buf, len, &upd) != 0) return;
    job_apply(j, &upd);
    show = j->ren_len;
    while(show && j->ren[show - 1] == '\n') show--;
    fytim_workband_set(j->wb, j->ren, show);
}

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
    j->ren_len = 0;
    j->frozen_len = 0;
    /* the tool's output streams through its OWN renderer as a live fenced
     * markdown block; the finished render is the commit payload */
    {
        struct fymd_renderer_cfg mc;
        int w = 80;
        fytim_size(ft, &w, NULL);
        memset(&mc, 0, sizeof mc);
        mc.width = w;
        j->r = fymd_renderer_create(&mc);
        if(j->r) job_push(j, "```text\n", 8);
    }
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
        while((n = read(fileno(j->fp), chunk, sizeof chunk)) > 0)
            job_push(j, chunk, (size_t)n);          /* live fenced render */
        if(n == 0){                                /* EOF: the tool is done */
            char done[64];
            const char *fin = NULL;
            size_t fin_len = 0;
            (void)ft;
            pclose(j->fp);
            j->fp = NULL;
            job_push(j, "```\n", 4);               /* close the fence */
            /* the healed remainder replaces the active region; the full
             * render is BOTH the final live view and the commit payload */
            if(j->r && fymd_render_finish(j->r, &fin, &fin_len) == 0){
                len = fin_len;
                if(len > sizeof j->ren - j->frozen_len)
                    len = sizeof j->ren - j->frozen_len;
                memcpy(j->ren + j->frozen_len, fin, len);
                j->ren_len = j->frozen_len + len;
            }
            if(j->r){ fymd_renderer_destroy(j->r); j->r = NULL; }
            while(j->ren_len && j->ren[j->ren_len - 1] == '\n') j->ren_len--;
            fytim_workband_set(j->wb, j->ren, j->ren_len);
            fytim_workband_set_commit(j->wb, j->ren, j->ren_len);
            snprintf(done, sizeof done, " \x1b[2mdemo #%d done\x1b[0m", j->id);
            fytim_workband_set_bottom(j->wb, done);
            /* mid-stream this DEFERS: the final render stays until the
             * transcript stream ends, then commits in finish order */
            fytim_workband_commit(j->wb);
            j->wb = NULL;
        }
    }
}

/* Prompts submitted while a turn is still streaming: slash commands run
 * immediately (tools are concurrent by design), anything else is a NEW
 * turn and must not interrupt the current one. It is buffered here, shown
 * in its own work-band, and replayed in order once the stream ends. */
#define PENDING_MAX 8
struct pending {
    struct fytim_workband *wb;
    char *lines[PENDING_MAX];
    int n;
};

static void pending_show(struct fytim *ft, struct pending *p)
{
    char buf[2048], bot[64];
    size_t off = 0;
    int i, r;

    if(!p->n){
        if(p->wb){ fytim_workband_destroy(p->wb); p->wb = NULL; }
        return;
    }
    if(!p->wb){
        p->wb = fytim_workband_create(ft);
        if(!p->wb) return;
        fytim_workband_set_top(p->wb, "");         /* a rule above the band */
        fytim_workband_set_max_rows(p->wb, PENDING_MAX);
    }
    for(i = 0; i < p->n; i++){
        r = snprintf(buf + off, sizeof buf - off, "%s \x1b[2m>\x1b[0m %s",
                     i ? "\n" : "", p->lines[i]);
        if(r < 0 || off + (size_t)r >= sizeof buf) break;
        off += (size_t)r;
    }
    fytim_workband_set(p->wb, buf, off);
    snprintf(bot, sizeof bot, " \x1b[2m%d queued, plays when the turn ends\x1b[0m",
             p->n);
    fytim_workband_set_bottom(p->wb, bot);
}

static void pending_push(struct fytim *ft, struct pending *p, const char *line)
{
    if(p->n >= PENDING_MAX){
        /* full: drop the newest, the band bottom says so */
        fytim_workband_set_bottom(p->wb,
            " \x1b[2mqueue full, input dropped\x1b[0m");
        return;
    }
    p->lines[p->n] = strdup(line);
    if(!p->lines[p->n]) return;
    p->n++;
    pending_show(ft, p);
}

/* Pop the oldest queued prompt; caller frees. */
static char *pending_pop(struct fytim *ft, struct pending *p)
{
    char *line;
    int i;
    if(!p->n) return NULL;
    line = p->lines[0];
    for(i = 1; i < p->n; i++)
        p->lines[i - 1] = p->lines[i];
    p->n--;
    pending_show(ft, p);
    return line;
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
    struct pending pq;
    int running = 1;

    memset(&s, 0, sizeof s);
    memset(&pq, 0, sizeof pq);
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
                    /* a non-slash line is a NEW turn: while one is still
                     * streaming it is buffered and replayed when the turn
                     * ends -- committing it now would split the reply */
                    if(s.r && ev.text[0] != '/'){
                        pending_push(ft, &pq, ev.text);
                        break;
                    }
                    if(!s.r){
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

        /* the turn ended (and this pump flushed any deferred tool
         * commits): replay the oldest buffered prompt as the next turn */
        if(!s.r && pq.n){
            char *line = pending_pop(ft, &pq);
            if(line){
                char echo[512];
                snprintf(echo, sizeof echo, "\x1b[1m>\x1b[0m %s", line);
                fytim_commit(ft, echo, strlen(echo));
                md_start(ft, &s, md_doc, sizeof md_doc - 1, NULL, 60);
                free(line);
            }
        }
    }

    while(pq.n)
        free(pending_pop(ft, &pq));
    if(s.r) fymd_renderer_destroy(s.r);
    free(s.owned);
    fytim_destroy(ft);
    return 0;
}
