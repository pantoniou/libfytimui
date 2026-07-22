/*
 * agent_inline.c - the coding-agent band through the PUBLIC interface only.
 *
 * Everything agent_layout_inline does by hand against the vendored core,
 * expressed in fytim_* calls alone: this file includes libfytimui.h and
 * links the library -- no core headers, no internal sources. The library
 * owns the band, the prompt editor, history navigation and completion
 * cycling; this host owns the event loop, the commands and the fake reply
 * stream.
 *
 * Usage: agent_inline        (Esc or /quit quits; Enter submits; Tab
 *                             completes; Up/Down browse history; ^G edits
 *                             the input in $EDITOR; "/demo N" streams a
 *                             child process into its own work-band --
 *                             several run concurrently and commit in
 *                             finish order)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <libfytimui.h>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const commands[] = {
    "/clear", "/demo", "/help", "/history", "/model", "/quit", "/status",
};
#define COMMANDS_N ((int)(sizeof commands / sizeof commands[0]))

static const char *const reply_pool[] = {
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do",
    "eiusmod tempor incididunt ut labore et dolore magna aliqua.",
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris",
    "nisi ut aliquip ex ea commodo consequat.",
};
#define REPLY_POOL_N ((int)(sizeof reply_pool / sizeof reply_pool[0]))

static void complete_cb(void *user, const char *text,
                        struct fytim_completions *c)
{
    int i;
    (void)user;
    for(i = 0; i < COMMANDS_N; i++)
        if(strncmp(commands[i], text, strlen(text)) == 0)
            fytim_completion_add(c, commands[i]);
}

/* A /demo job: a child process whose stdout streams into its own work-band.
 * Several run concurrently, each in an independent band; whichever finishes
 * first commits first, regardless of stacking order. */
#define JOBS_MAX 4
struct job {
    struct fytim_workband *wb;
    FILE *fp;                      /* popen stream, non-blocking */
    char acc[4096];
    int id;
};

struct app {
    struct fytim_workband *wb;     /* the reply streaming live */
    char acc[1024];                /* its accumulated content */
    int streaming;                 /* reply lines still to emit */
    int reply_next;
    int submitted;
    unsigned long committed;
    struct job jobs[JOBS_MAX];
    int job_seq;
};

static void job_spawn(struct fytim *ft, struct app *a, int count)
{
    char cmd[128], bot[64];
    int i;
    struct job *j = NULL;
    for(i = 0; i < JOBS_MAX; i++)
        if(!a->jobs[i].fp){ j = &a->jobs[i]; break; }
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
    j->id = ++a->job_seq;
    j->acc[0] = '\0';
    fytim_workband_set_top(j->wb, "");             /* a rule above the band */
    snprintf(bot, sizeof bot, " \x1b[2mdemo #%d (%ds) running\x1b[0m",
             j->id, count);
    fytim_workband_set_bottom(j->wb, bot);
}

static void job_tick(struct fytim *ft, struct app *a)
{
    int i;
    for(i = 0; i < JOBS_MAX; i++){
        struct job *j = &a->jobs[i];
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
        if(n == 0){                                /* EOF: the job is done */
            char done[64];
            pclose(j->fp);
            j->fp = NULL;
            snprintf(done, sizeof done, "\x1b[2mdemo #%d done\x1b[0m", j->id);
            fytim_workband_commit(j->wb);          /* finish order, not stack order */
            j->wb = NULL;
            fytim_commit(ft, done, strlen(done));
        }
    }
}

/* ^G: run $EDITOR over the input while the library has released the
 * terminal, then load the result back. */
static void edit_input(struct fytim *ft)
{
    char path[] = "/tmp/agent_inline.XXXXXX";
    char cmd[256], text[4096];
    const char *ed = getenv("EDITOR");
    int fd = mkstemp(path);
    FILE *fp;
    size_t n;
    if(fd < 0) return;
    dprintf(fd, "%s", fytim_input(ft));
    close(fd);
    if(!ed || !*ed) ed = "vi";
    if(fytim_suspend(ft) == FYTIM_OK){
        int rc;
        snprintf(cmd, sizeof cmd, "%s %s", ed, path);
        rc = system(cmd);
        (void)rc;
        fytim_resume(ft);
    }
    fp = fopen(path, "r");
    if(fp){
        n = fread(text, 1, sizeof text - 1, fp);
        fclose(fp);
        while(n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
        text[n] = '\0';
        fytim_set_input(ft, text);
    }
    unlink(path);
}

static void stream_tick(struct fytim *ft, struct app *a)
{
    (void)ft;
    if(!a->wb || a->streaming <= 0) return;
    /* one line per tick grows the work-band; when the reply is done the
     * band commits wholesale into scrollback and retires */
    snprintf(a->acc + strlen(a->acc), sizeof a->acc - strlen(a->acc), "%s%s",
             a->acc[0] ? "\n" : "", reply_pool[a->reply_next % REPLY_POOL_N]);
    a->reply_next++;
    fytim_workband_set(a->wb, a->acc, strlen(a->acc));
    if(--a->streaming == 0){
        fytim_workband_commit(a->wb);
        a->wb = NULL;
        a->committed += REPLY_POOL_N;
    }
}

static int handle_line(struct fytim *ft, struct app *a,
                       const char *text, size_t len)
{
    char echo[512];
    snprintf(echo, sizeof echo, "> %.*s", (int)len, text);
    fytim_commit(ft, echo, strlen(echo));
    a->committed++;
    fytim_history_add(ft, text);
    a->submitted++;
    if(strcmp(text, "/quit") == 0) return 0;
    if(strncmp(text, "/demo", 5) == 0){
        int n = atoi(text + 5);
        if(n < 1)  n = 10;
        if(n > 60) n = 60;
        job_spawn(ft, a, n);
        return 1;
    }
    if(strcmp(text, "/help") == 0){
        int i;
        for(i = 0; i < COMMANDS_N; i++)
            fytim_commit(ft, commands[i], strlen(commands[i]));
        a->committed += COMMANDS_N;
    }else if(strcmp(text, "/status") == 0){
        snprintf(echo, sizeof echo, "submitted %d, committed %lu",
                 a->submitted, a->committed);
        fytim_commit(ft, echo, strlen(echo));
        a->committed++;
    }else if(text[0] != '/' && !a->wb){
        a->wb = fytim_workband_create(ft);
        if(a->wb){
            fytim_workband_set_bottom(a->wb, " \x1b[2mstreaming\x1b[0m");
            a->acc[0] = '\0';
            a->streaming = REPLY_POOL_N;
            a->reply_next = 0;
        }
    }
    return 1;
}

int main(void)
{
    struct fytim *ft;
    struct app a;
    int running = 1;

    memset(&a, 0, sizeof a);
    ft = fytim_create(NULL);               /* stdin/stdout, defaults */
    if(!ft){
        fprintf(stderr, "agent_inline: failed to open terminal\n");
        return 1;
    }
    fytim_set_complete_fn(ft, complete_cb, NULL);
    fytim_set_header(ft, " agent_inline -- public fytim_* surface only");
    fytim_set_status_row(ft, 1,
        " scroll/select/copy: use the terminal | Tab completes | Esc quits");
    fytim_commit(ft, "agent_inline: committed lines live in your terminal's "
                     "own scrollback.", 76);

    while(running){
        struct pollfd pfd;
        struct fytim_event ev;
        char st[128];
        int timeout = fytim_poll_timeout_ms(ft);

        /* the HOST owns the loop: poll our fds, then pump */
        pfd.fd = fytim_poll_fd(ft);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if(timeout < 0 || timeout > 100) timeout = 100;
        if(pfd.fd >= 0) poll(&pfd, 1, timeout);

        stream_tick(ft, &a);
        job_tick(ft, &a);
        snprintf(st, sizeof st, " %s | submitted %d | committed %lu",
                 a.streaming ? "streaming" : "ready", a.submitted, a.committed);
        fytim_set_status_row(ft, 0, st);

        if(fytim_pump(ft) != FYTIM_OK) break;
        while(fytim_next_event(ft, &ev)){
            switch(ev.type){
                case FYTIM_EVENT_LINE:
                    running = handle_line(ft, &a, ev.text, ev.text_len);
                    break;
                case FYTIM_EVENT_EDIT:
                    edit_input(ft);
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

    fytim_destroy(ft);
    return 0;
}
