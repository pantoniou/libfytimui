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
#include <sys/wait.h>
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
    int byte_mode;                 /* chunk pushes instead of whole lines */
    int delay_ms;                  /* pacing between chunks */
    long long next_ms;             /* next push not before this */
};

/* Tool calls: a child process whose stdout streams into its OWN
 * work-band, concurrently with the main transcript stream in the tail.
 * The presentation is the agent shape:
 *
 *     [dot] **tool** command-line            <- live: yellow activity dot
 *       styled output rows (windowed)
 *
 *     [dot] **tool** command-line OK|ERROR   <- final: white or red dot,
 *       truncated output                        committed to the transcript
 *
 * No fence chrome anywhere: the header line is the frame. File tools
 * fence with the file's language so the output is syntax highlighted. */
#define JOBS_MAX 4
#define JOB_LIVE_ROWS 6            /* live body rows */
#define JOB_DONE_ROWS 3            /* truncated output in the transcript */
struct job {
    struct fytim_workband *wb;
    FILE *fp;                      /* popen stream, non-blocking */
    struct fymd_renderer *r;       /* body renderer: styled, no decoration */
    char acc[8192];                /* raw tool output accumulated */
    size_t acc_len;
    char tool[16];                 /* "shell", "update", ... */
    char cmd[128];                 /* display command line */
    char lang[16];                 /* fence language for highlighting */
    int id;
};

static struct job jobs[JOBS_MAX];
static int job_seq;

/* The header row: activity dot, bold tool name, command, status. */
static size_t job_header(const struct job *j, char *buf, size_t cap,
                         const char *dot, const char *status)
{
    int n = snprintf(buf, cap, "%s \x1b[1m%s\x1b[0m %s%s",
                     dot, j->tool, j->cmd, status);
    return n < 0 ? 0 : (size_t)n;
}

/* Compose header + rendered body into the band. LINE mode: only complete
 * lines render while the tool runs -- a torn read flashes as artifacts;
 * the partial line waits in acc for its '\n' (whole renders everything).
 * The renderer's HEAD_TAIL limit windows the LIVE body around an
 * omission separator. */
static void job_show(struct job *j, const char *dot_sgr, const char *status)
{
    struct fymd_fenced_block_opts opts;
    char head[256], *full;
    char *out = NULL;
    size_t out_len = 0, hlen, len = j->acc_len;

    if(!j->wb) return;
    hlen = job_header(j, head, sizeof head, dot_sgr, status);
    while(len && j->acc[len - 1] != '\n') len--;   /* complete lines only */
    while(len && j->acc[len - 1] == '\n') len--;   /* terminator, not a row */
    if(len && j->r){
        memset(&opts, 0, sizeof opts);
        opts.language = j->lang;
        opts.flags = FYMD_FBF_STYLE | FYMD_FBF_HIGHLIGHT;
        if(fymd_render_fenced_block(j->r, j->acc, len, &opts,
                                    &out, &out_len) != 0)
            out = NULL;
        while(out && out_len && out[out_len - 1] == '\n') out_len--;
    }
    full = malloc(hlen + 1 + out_len + 1);
    if(full){
        memcpy(full, head, hlen);
        if(out_len){
            full[hlen] = '\n';
            memcpy(full + hlen + 1, out, out_len);
        }
        fytim_workband_set(j->wb, full, hlen + (out_len ? 1 + out_len : 0));
        free(full);
    }
    if(out) fymd_free(out);
}

/* The running dot BLINKS: bright and dim yellow alternate on a host-side
 * timer (SGR blink is unreliable across terminals). */
static long long now_ms(void);
static const char *job_dot_running(void)
{
    return (now_ms() / 500) & 1 ? "\x1b[2;33m\xe2\x97\x8f\x1b[0m"
                                : "\x1b[33m\xe2\x97\x8f\x1b[0m";
}

static void job_push(struct job *j, const char *buf, size_t len)
{
    if(len > sizeof j->acc - j->acc_len) len = sizeof j->acc - j->acc_len;
    memcpy(j->acc + j->acc_len, buf, len);
    j->acc_len += len;
    job_show(j, job_dot_running(), "");
}

static void job_spawn(struct fytim *ft, const char *tool, const char *display,
                      const char *shell_cmd, const char *lang)
{
    int i;
    struct job *j = NULL;
    for(i = 0; i < JOBS_MAX; i++)
        if(!jobs[i].fp){ j = &jobs[i]; break; }
    if(!j){
        fytim_commit(ft, "tool: all job slots busy", 24);
        return;
    }
    j->fp = popen(shell_cmd, "r");
    if(!j->fp) return;
    fcntl(fileno(j->fp), F_SETFL, O_NONBLOCK);
    j->wb = fytim_workband_create(ft);
    j->id = ++job_seq;
    j->acc_len = 0;
    snprintf(j->tool, sizeof j->tool, "%s", tool);
    snprintf(j->cmd, sizeof j->cmd, "%s", display);
    snprintf(j->lang, sizeof j->lang, "%s", lang && *lang ? lang : "text");
    /* body renderer: code styling (margin, prefix, dim/highlight) with
     * the decoration rules blanked -- the header line is the frame */
    {
        struct fymd_renderer_cfg mc;
        struct fymd_line_limit_opts ll;
        int w = 80;
        fytim_size(ft, &w, NULL);
        memset(&mc, 0, sizeof mc);
        mc.width = w;
        mc.style = "code:\n  decoration:\n    header: ''\n    footer: ''\n";
        j->r = fymd_renderer_create(&mc);
        memset(&ll, 0, sizeof ll);
        /* balanced head/tail around the omission separator */
        ll.mode = FYMD_LLM_HEAD_TAIL;
        ll.split = FYMD_LLS_BALANCED;
        ll.max_lines = JOB_LIVE_ROWS;
        if(j->r) fymd_renderer_set_line_limit(j->r, &ll);
    }
    fytim_workband_set_max_rows(j->wb, JOB_LIVE_ROWS + 1);   /* + header */
    job_show(j, job_dot_running(), "");
}

static void job_tick(struct fytim *ft)
{
    static long long last_phase = -1;
    long long phase = (now_ms() / 500) & 1;
    int i;
    for(i = 0; i < JOBS_MAX; i++){
        struct job *j = &jobs[i];
        char chunk[512];
        ssize_t n;
        if(!j->fp) continue;
        if(phase != last_phase)                    /* blink the activity dot */
            job_show(j, job_dot_running(), "");
        while((n = read(fileno(j->fp), chunk, sizeof chunk)) > 0)
            job_push(j, chunk, (size_t)n);
        if(n == 0){                                /* EOF: the tool is done */
            struct fymd_line_limit_opts ll;
            int rc = pclose(j->fp);
            int ok = rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
            (void)ft;
            j->fp = NULL;
            /* the FINAL form: white or red dot, status word, and the
             * output truncated to its newest rows -- this is both the
             * last live view and the commit payload (no fence chrome) */
            memset(&ll, 0, sizeof ll);
            ll.mode = FYMD_LLM_SCROLL;
            ll.max_lines = JOB_DONE_ROWS;
            if(j->r) fymd_renderer_set_line_limit(j->r, &ll);
            job_show(j, ok ? "\x1b[32m\xe2\x97\x8f\x1b[0m"
                           : "\x1b[31m\xe2\x97\x8f\x1b[0m", "");
            if(j->r){ fymd_renderer_destroy(j->r); j->r = NULL; }
            /* mid-stream this DEFERS: the final render stays until the
             * transcript stream ends, then commits in finish order */
            fytim_workband_commit(j->wb);
            j->wb = NULL;
        }
    }
    last_phase = phase;
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
                     const char *doc, size_t len, char *owned, int delay_ms,
                     int byte_mode)
{
    struct fymd_renderer_cfg cfg;
    int w = 80;
    if(s->r){ free(owned); return; }             /* one stream at a time */
    s->doc = doc;
    s->doc_len = len;
    s->owned = owned;
    s->byte_mode = byte_mode;
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
        if(s->byte_mode){
            /* BYTE mode: a few bytes per tick like a token stream, but
             * never ending inside a UTF-8 sequence -- the torn bytes
             * would flash as artifacts in the live render */
            n = s->doc_len - s->off;
            if(n > 7){
                n = 7;
                while(n > 1 && (s->doc[s->off + n] & 0xC0) == 0x80) n--;
                if((s->doc[s->off + n] & 0xC0) == 0x80)   /* long codepoint */
                    while(s->off + n < s->doc_len &&
                          (s->doc[s->off + n] & 0xC0) == 0x80) n++;
            }
        }else{
            /* LINE mode (default): one whole source line per tick */
            const char *nl = memchr(s->doc + s->off, '\n',
                                    s->doc_len - s->off);
            n = nl ? (size_t)(nl - (s->doc + s->off)) + 1
                   : s->doc_len - s->off;
        }
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

/* /stream FILE [DELAY_MS] [byte]: stream a markdown file through the
 * renderer; "byte" pushes UTF-8-safe byte chunks instead of whole lines. */
static void cmd_stream(struct fytim *ft, struct mdstream *s, const char *args)
{
    char path[512], msg[600], mode[16];
    int delay = 0, byte_mode;
    FILE *fp;
    char *doc = NULL;
    long fl;
    size_t rd;

    while(*args == ' ') args++;
    mode[0] = '\0';
    if(sscanf(args, "%511s %d %15s", path, &delay, mode) < 1){
        fytim_commit(ft, "usage: /stream FILE [DELAY_MS] [byte]", 37);
        return;
    }
    byte_mode = strcmp(mode, "byte") == 0;
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
    snprintf(msg, sizeof msg, "\x1b[2mstreaming %s (%zu bytes, %d ms, %s)\x1b[0m",
             path, rd, delay > 0 ? delay : 60, byte_mode ? "byte" : "line");
    fytim_commit(ft, msg, strlen(msg));
    md_start(ft, s, doc, rd, doc, delay, byte_mode);
}

static void complete_cb(void *user, const char *text,
                        struct fytim_completions *c)
{
    static const char *const cmds[] = { "/demo", "/edit", "/quit",
                                        "/run", "/stream" };
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
        " Enter: markdown | /stream FILE [MS] | /demo N | /run CMD | /edit FILE");
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
        /* the prompt marker doubles as the TURN indicator: a chevron when
         * idle, the blinking activity button while the reply streams */
        if(s.r){
            char mk[16];
            snprintf(mk, sizeof mk, "%s ", job_dot_running());
            fytim_set_marker(ft, mk);
        }else{
            fytim_set_marker(ft, "\xe2\x9d\xaf ");
        }

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
                    if(!s.r && ev.text[0] != '/'){
                        /* slash commands are not echoed: a tool call's own
                         * header line is its transcript record */
                        char echo[512];
                        snprintf(echo, sizeof echo, "\x1b[1m>\x1b[0m %.*s",
                                 (int)ev.text_len, ev.text);
                        fytim_commit(ft, echo, strlen(echo));
                    }
                    if(strncmp(ev.text, "/stream", 7) == 0){
                        cmd_stream(ft, &s, ev.text + 7);
                    }else if(strncmp(ev.text, "/demo", 5) == 0){
                        char cmd[160], disp[64];
                        int dn = atoi(ev.text + 5);
                        if(dn < 1)  dn = 10;
                        if(dn > 60) dn = 60;
                        snprintf(cmd, sizeof cmd,
                                 "for i in $(seq 1 %d); do echo \"tick $i/%d\";"
                                 " sleep 1; done", dn, dn);
                        snprintf(disp, sizeof disp, "demo %d", dn);
                        job_spawn(ft, "shell", disp, cmd, "text");
                    }else if(strncmp(ev.text, "/run ", 5) == 0){
                        char cmd[512];
                        snprintf(cmd, sizeof cmd, "%s 2>&1", ev.text + 5);
                        job_spawn(ft, "shell", ev.text + 5, cmd, "text");
                    }else if(strncmp(ev.text, "/edit ", 6) == 0){
                        /* a file tool: fence with the file's language so
                         * the render is syntax highlighted */
                        char cmd[512];
                        const char *path = ev.text + 6, *dot, *lang = "text";
                        dot = strrchr(path, '.');
                        if(dot){
                            if(!strcmp(dot, ".c") || !strcmp(dot, ".h"))
                                lang = "c";
                            else if(!strcmp(dot, ".py"))  lang = "python";
                            else if(!strcmp(dot, ".md"))  lang = "markdown";
                            else if(!strcmp(dot, ".sh"))  lang = "sh";
                        }
                        snprintf(cmd, sizeof cmd, "cat %s 2>&1", path);
                        job_spawn(ft, "update", path, cmd, lang);
                    }else{
                        md_start(ft, &s, md_doc, sizeof md_doc - 1, NULL, 60, 0);
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
                md_start(ft, &s, md_doc, sizeof md_doc - 1, NULL, 60, 0);
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
