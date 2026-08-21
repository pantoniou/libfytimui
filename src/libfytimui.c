/*
 * libfytimui.c - lifecycle, host-owned loop, pane model.
 *
 * The vendored timui core is included here and nowhere in the public headers,
 * so no timui type or symbol reaches a consumer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <libfytimui/libfytimui-layout.h>
#include "fytim_sgr.h"
#include "timui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FYTIM_VERSION_STRING
#define FYTIM_VERSION_STRING "0.0.0"
#endif

#define FYTIM_INPUT_CAP    4096
#define FYTIM_EVQ_CAP      16
#define FYTIM_HIST_DEFAULT 64
#define FYTIM_WORKBAND_DEFAULT 4  /* default max content rows per work-band */
#define FYTIM_PROMPT_MAX   6      /* rows the prompt band may grow to */

struct fytim_pane {
    struct fytim_pane *next;
    struct fytim      *owner;
    char              *title;
    enum fytim_pane_state state;

    /* Retained styled content. Appended bytes are parsed once, here, so a
     * repaint does not re-run the SGR parser over the whole pane. */
    struct fytim_sgr_parser sgr;
};

struct fytim_workband {
    struct fytim_workband *next;
    struct fytim          *owner;
    char *content;                 /* live SGR-styled rows, '\n'-separated */
    char *commit;                  /* commits instead of content when set */
    char *top, *bottom;            /* optional chrome rows; "" draws a rule */
    int   max_rows;                /* content-row cap, >= 1 */
    /* committed while the transcript tail was streaming: the final render
     * stays on screen and the commit flushes -- in finish order -- when
     * the stream ends (interleaving would split the streaming reply) */
    int   finished;
    int   finish_seq;
    /* Set when this band draws a grid of cells instead of styled text. The
     * band node is what composes; the surface is what it holds. */
    struct fytim_surface *surface;
};

/*
 * A grid of cells the host publishes. It rides on a work-band node, so
 * ordering, row granting and chrome are the band rules and not a second set
 * of them: a surface and a band shed rows the same way.
 */
struct fytim_surface {
    struct fytim_workband *wb;
    struct fytim          *owner;
    struct fytim_cell     *grid;   /* rows * cols, row-major */
    int rows, cols;
    int cur_row, cur_col;
    bool cur_visible;
    int granted;                   /* content rows drawn at the last frame */
};

struct fytim_completions {
    struct fytim *owner;
    /* candidates collected during the host callback; retained while
     * completion mode is active so Tab can keep cycling */
};

struct fytim {
    Timui             *ui;
    struct fytim_pane *panes;      /* transcript is the head and is never closed */
    struct fytim_pane *transcript;
    bool               closed;
    bool               suspended;  /* terminal released to a child process */
    int                out_fd;

    /* band chrome */
    char *header;
    char *status[2];
    char *marker;
    struct fytim_sgr_style prompt_style;
    bool prompt_style_set;
    struct fytim_sgr_style chrome_style[FYTIM_CHROME_STYLE_COUNT];
    bool chrome_style_set[FYTIM_CHROME_STYLE_COUNT];

    /* the transcript's live tail: the agent's streaming output, above
     * every work-band and below the scrollback. tail_streaming is the
     * EXPLICIT stream-in-progress signal: set by any tail update, cleared
     * only by fytim_tail_set(NULL) -- a freeze can empty the tail at a
     * block boundary with the stream still in flight. */
    char *tail;
    bool  tail_streaming;

    /* live work-bands, oldest first (nearest the transcript). Creation
     * order fixes the stacking only; bands commit in completion order. */
    struct fytim_workband *wbands;
    struct fytim_surface  *keys;   /* the surface the keys go to, or NULL */
    int   wb_default_max;
    int   wb_finish_seq;

    /* prompt editor (library-owned; the host only reads/replaces) */
    char               input[FYTIM_INPUT_CAP];
    TimuiTextAreaState pst;

    /* history: Up/Ctrl-P .. Down/Ctrl-N with a preserved live draft */
    char **hist;
    int    hist_n, hist_cap, hist_max, hist_pos;
    char  *draft;

    /* completion */
    fytim_complete_fn complete_fn;
    void             *complete_user;
    struct fytim_completions comps;
    char **comp;                   /* retained candidates while cycling */
    int    comp_n, comp_cap;
    int    comp_active, comp_idx, comp_collecting;
    char  *comp_saved;

    /* event queue (FIFO); text ownership travels with the entry and parks
     * in ev_last after a pop, per the header's lifetime contract */
    struct fytim_event evq[FYTIM_EVQ_CAP];
    int    ev_head, ev_n;
    char  *ev_last;

    /* geometry */
    int term_w, term_h;
    int band_rows;
    int band_w;                    /* width the frame was last sized to */
    int pending_commit_rows;       /* rows committed since the last pump */
    /* running SGR state of the transcript stream, carried across commits */
    struct fytim_sgr_parser commit_sgr;
};

const char *fytim_version_string(void)
{
    return FYTIM_VERSION_STRING;
}

const char *fytim_result_string(enum fytim_result r)
{
    switch(r){
        case FYTIM_OK:              return "ok";
        case FYTIM_ERR_INVALID:     return "invalid argument";
        case FYTIM_ERR_NOMEM:       return "out of memory";
        case FYTIM_ERR_IO:          return "I/O error";
        case FYTIM_ERR_CLOSED:      return "closed";
        case FYTIM_ERR_UNSUPPORTED: return "unsupported";
    }
    return "unknown error";
}

void fytim_cfg_default(struct fytim_cfg *cfg)
{
    if(!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->struct_size = sizeof *cfg;
    cfg->input_fd  = -1;
    cfg->output_fd = -1;
    cfg->mouse     = false;   /* the inline band never grabs the mouse */
    cfg->clipboard = false;
    cfg->workband_rows = 0;   /* 0 selects the default */
    cfg->intr_signal   = false;
}

static struct fytim_pane *pane_new(struct fytim *ft, const char *title)
{
    struct fytim_pane *p = calloc(1, sizeof *p);
    if(!p) return NULL;
    p->owner = ft;
    p->state = FYTIM_PANE_RUNNING;
    fytim_sgr_init(&p->sgr);
    if(title){
        p->title = strdup(title);
        if(!p->title){ free(p); return NULL; }
    }
    return p;
}

static void pane_free(struct fytim_pane *p)
{
    if(!p) return;
    free(p->title);
    free(p);
}

struct fytim *fytim_create(const struct fytim_cfg *cfg)
{
    struct fytim_cfg defaults;
    struct fytim *ft;
    TimuiConfig tcfg = TIMUI_CONFIG_INIT;

    if(!cfg){
        fytim_cfg_default(&defaults);
        cfg = &defaults;
    }else if(cfg->struct_size != sizeof *cfg){
        return NULL;   /* built against a different header revision */
    }

    ft = calloc(1, sizeof *ft);
    if(!ft) return NULL;

    tcfg.input_fd  = (cfg->input_fd  >= 0) ? cfg->input_fd  : STDIN_FILENO;
    tcfg.output_fd = (cfg->output_fd >= 0) ? cfg->output_fd : STDOUT_FILENO;
    tcfg.title     = cfg->title;
    /* The host owns poll(); timui must never block inside a pump. The band
     * lives on the normal screen (no alt screen), never grabs the mouse,
     * and the kitty keyboard protocol makes Shift+Enter distinguishable
     * (harmlessly ignored elsewhere). Synchronized output (DEC 2026) is
     * FORCED, not capability-gated: unsupporting terminals ignore the
     * private mode, supporting ones render each update atomically -- the
     * last line of defense against streaming flicker. */
    tcfg.flags |= TIMUI_FLAG_EXTERNAL_POLL | TIMUI_FLAG_INLINE |
                  TIMUI_FLAG_HIDE_CURSOR |
                  TIMUI_FLAG_KITTY_KEYBOARD | TIMUI_FLAG_SYNC_OUTPUT;
    if(cfg->intr_signal) tcfg.flags |= TIMUI_FLAG_INTR_SIGNAL;

    ft->wb_default_max = (cfg->workband_rows > 0) ? cfg->workband_rows
                                                  : FYTIM_WORKBAND_DEFAULT;
    ft->hist_max  = FYTIM_HIST_DEFAULT;
    ft->hist_pos  = 0;
    ft->out_fd    = tcfg.output_fd;
    ft->pst.text  = ft->input;
    ft->pst.cap   = sizeof ft->input;
    ft->comps.owner = ft;
    fytim_sgr_init(&ft->commit_sgr);
    ft->term_w = 80;
    ft->term_h = 24;
    (void)timui_term_size(tcfg.output_fd, &ft->term_w, &ft->term_h);
    ft->band_rows = FYTIM_CHROME_ROWS + 1;   /* no work-bands yet: one spare
                                                transcript row keeps the full
                                                chrome (see wb_rows_total) */
    if(ft->band_rows > ft->term_h) ft->band_rows = ft->term_h;
    ft->band_w = ft->term_w;
    tcfg.inline_rows = ft->band_rows;

    if(timui_open(&tcfg, &ft->ui) != TIMUI_OK){
        free(ft);
        return NULL;
    }

    ft->transcript = pane_new(ft, NULL);
    if(!ft->transcript){
        timui_close(ft->ui);
        free(ft);
        return NULL;
    }
    ft->panes = ft->transcript;
    return ft;
}

static void comp_free_candidates(struct fytim *ft)
{
    int i;
    for(i = 0; i < ft->comp_n; i++) free(ft->comp[i]);
    free(ft->comp);
    ft->comp = NULL;
    ft->comp_n = ft->comp_cap = 0;
}

static void wb_free(struct fytim_workband *wb)
{
    if(!wb) return;
    if(wb->surface){
        /* The band owns the surface once it is attached: the UI can outlive
         * the host's handle, and destroying the UI must free both. */
        free(wb->surface->grid);
        free(wb->surface);
        wb->surface = NULL;
    }
    free(wb->content);
    free(wb->commit);
    free(wb->top);
    free(wb->bottom);
    free(wb);
}

void fytim_destroy(struct fytim *ft)
{
    struct fytim_pane *p, *next;
    struct fytim_workband *wb, *wnext;
    int i;
    if(!ft) return;
    for(p = ft->panes; p; p = next){
        next = p->next;
        pane_free(p);
    }
    for(wb = ft->wbands; wb; wb = wnext){
        wnext = wb->next;
        wb_free(wb);
    }
    if(ft->ui) timui_close(ft->ui);
    free(ft->header);
    free(ft->status[0]);
    free(ft->status[1]);
    free(ft->marker);
    free(ft->tail);
    for(i = 0; i < ft->hist_n; i++) free(ft->hist[i]);
    free(ft->hist);
    free(ft->draft);
    comp_free_candidates(ft);
    free(ft->comp_saved);
    for(i = 0; i < ft->ev_n; i++)
        free((char *)ft->evq[(ft->ev_head + i) % FYTIM_EVQ_CAP].text);
    free(ft->ev_last);
    free(ft);
}

enum fytim_result fytim_size(const struct fytim *ft, int *w, int *h)
{
    if(!ft) return FYTIM_ERR_INVALID;
    if(w) *w = ft->term_w;
    if(h) *h = ft->term_h;
    return FYTIM_OK;
}

int fytim_poll_fd(const struct fytim *ft)
{
    return ft ? timui_poll_fd(ft->ui) : -1;
}

int fytim_poll_timeout_ms(const struct fytim *ft)
{
    return ft ? timui_poll_timeout_ms(ft->ui) : -1;
}

struct fytim_pane *fytim_transcript(struct fytim *ft)
{
    return ft ? ft->transcript : NULL;
}

struct fytim_pane *fytim_pane_open(struct fytim *ft, const char *title)
{
    struct fytim_pane *p;
    if(!ft || ft->closed) return NULL;
    p = pane_new(ft, title);
    if(!p) return NULL;
    /* Append so pane order is stable and matches creation order. */
    {
        struct fytim_pane **tail = &ft->panes;
        while(*tail) tail = &(*tail)->next;
        *tail = p;
    }
    return p;
}

enum fytim_result fytim_pane_close(struct fytim_pane *p)
{
    struct fytim *ft;
    struct fytim_pane **link;

    if(!p || !p->owner) return FYTIM_ERR_INVALID;
    ft = p->owner;
    if(p == ft->transcript) return FYTIM_ERR_INVALID;   /* not closeable */

    for(link = &ft->panes; *link; link = &(*link)->next){
        if(*link == p){
            *link = p->next;
            pane_free(p);
            return FYTIM_OK;
        }
    }
    return FYTIM_ERR_INVALID;
}

enum fytim_result fytim_pane_set_title(struct fytim_pane *p, const char *title)
{
    char *dup;
    if(!p) return FYTIM_ERR_INVALID;
    dup = title ? strdup(title) : NULL;
    if(title && !dup) return FYTIM_ERR_NOMEM;
    free(p->title);
    p->title = dup;
    return FYTIM_OK;
}

enum fytim_result fytim_pane_set_state(struct fytim_pane *p, enum fytim_pane_state st)
{
    if(!p) return FYTIM_ERR_INVALID;
    switch(st){
        case FYTIM_PANE_RUNNING:
        case FYTIM_PANE_DONE:
        case FYTIM_PANE_FAILED:
        case FYTIM_PANE_CANCELLED:
            p->state = st;
            return FYTIM_OK;
    }
    return FYTIM_ERR_INVALID;
}

/* ---- small helpers ------------------------------------------------------ */

static enum fytim_result set_dup(char **slot, const char *s)
{
    char *dup = s ? strdup(s) : NULL;
    if(s && !dup) return FYTIM_ERR_NOMEM;
    free(*slot);
    *slot = dup;
    return FYTIM_OK;
}

static bool rendered_only(const char *buf, size_t len);

/* set_dup for rows that may carry SGR styling: same contract as content. */
static enum fytim_result set_dup_sgr(char **slot, const char *s)
{
    if(s && *s && !rendered_only(s, strlen(s))) return FYTIM_ERR_INVALID;
    return set_dup(slot, s);
}

/* Validate the SGR-only contract: styling escapes pass, anything the
 * compositor owns (cursor, erase, screen modes) is rejected. */
static bool sgr_only_run_(void *user, const char *text, size_t len,
                          const struct fytim_sgr_style *style)
{
    (void)user; (void)text; (void)len; (void)style;
    return true;
}
static bool rendered_only(const char *buf, size_t len)
{
    struct fytim_sgr_parser p;
    const char *el, *cur = buf;
    size_t left = len, chunk;

    fytim_sgr_init(&p);
    /*
     * libfymd4c reverse-card rows use bare EL as a structural fill while
     * their background SGR is active. It neither moves the cursor nor
     * addresses another row, and the inline transcript emitter consumes it
     * at the current row before advancing. Keep every other CSI/erase
     * sequence subject to the strict SGR/OSC-8 parser.
     */
    while(left > 0){
        el = memmem(cur, left, "\x1b[K", 3);
        chunk = el ? (size_t)(el - cur) : left;
        if(chunk)
            fytim_sgr_feed(&p, cur, chunk, sgr_only_run_, NULL);
        if(!el) break;
        cur = el + 3;
        left -= chunk + 3;
    }
    return !p.disallowed_seen;
}

/* ---- events ------------------------------------------------------------- */

static void ev_push(struct fytim *ft, enum fytim_event_type type,
                    char *owned_text, size_t text_len, int w, int h)
{
    struct fytim_event *ev;
    if(ft->ev_n == FYTIM_EVQ_CAP){        /* full: drop the oldest */
        free((char *)ft->evq[ft->ev_head].text);
        ft->ev_head = (ft->ev_head + 1) % FYTIM_EVQ_CAP;
        ft->ev_n--;
    }
    ev = &ft->evq[(ft->ev_head + ft->ev_n) % FYTIM_EVQ_CAP];
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->text = owned_text;
    ev->text_len = text_len;
    ev->width = w;
    ev->height = h;
    ft->ev_n++;
}

bool fytim_next_event(struct fytim *ft, struct fytim_event *out)
{
    if(!ft || !out || ft->ev_n == 0){
        if(out) memset(out, 0, sizeof *out);
        return false;
    }
    *out = ft->evq[ft->ev_head];
    ft->ev_head = (ft->ev_head + 1) % FYTIM_EVQ_CAP;
    ft->ev_n--;
    /* the text stays valid until the next pop or pump */
    free(ft->ev_last);
    ft->ev_last = (char *)out->text;
    return true;
}

/* ---- band content ------------------------------------------------------- */

/* Committed lines land in scrollback each ending in a hard SGR reset, but
 * libfymd4c opens a style once and relies on carry-over across '\n' (a
 * fenced block body is one leading escape). Re-open the running state at
 * every row start so the carry survives the per-row reset. Returns a
 * malloc'd normalized copy with *out_len set, or NULL when the input needs
 * no rewriting (single row, or no carried state at any row break). */
/* State-tracking feed only: runs are discarded (feed treats a NULL cb as
 * a no-op, so an explicit sink is needed). */
static bool sgr_sink_(void *user, const char *text, size_t len,
                      const struct fytim_sgr_style *style)
{
    (void)user; (void)text; (void)len; (void)style;
    return true;
}

static char *sgr_rowsafe(struct fytim_sgr_parser *p, const char *buf,
                         size_t len, size_t *out_len)
{
    char seq[64], *out = NULL, *grown;
    size_t cap = 0, o = 0, start = 0, in_done = 0, i, n;

    /* the parser PERSISTS across commits -- the transcript is one
     * continuous SGR stream, and a style opened by an earlier commit is
     * still open at this buffer's first row */
    n = fytim_sgr_style_emit(&p->style, seq, sizeof seq);
    if(n > 0){
        cap = (n + len + 64) * 2;
        out = malloc(cap);
        if(!out) n = 0;
        else { memcpy(out, seq, n); o = n; }
    }
    for(i = 0; i < len; i++){
        if(buf[i] != '\n') continue;
        fytim_sgr_feed(p, buf + start, i + 1 - start, sgr_sink_, NULL);
        start = i + 1;
        /* A style carry is needed only before another row in THIS commit.
         * Reopening it after the final newline leaves an escape-only partial
         * row; timui_inline_commit then supplies its missing newline and
         * accidentally commits a blank row after every styled streamed row.
         * The persistent parser already reopens the style at the head of the
         * next commit. */
        if(start == len) continue;
        n = fytim_sgr_style_emit(&p->style, seq, sizeof seq);
        if(n == 0 && !out) continue;       /* nothing carried, nothing yet */
        /* worst case: all pending input plus this re-open */
        if(cap < o + n + (len - in_done)){
            cap = (o + n + (len - in_done)) * 2;
            grown = realloc(out, cap);
            if(!grown){ free(out); return NULL; }
            out = grown;
        }
        /* everything up to the row break not yet materialized, then the
         * re-open (o counts output bytes; input bytes lag by the inserted
         * escapes, tracked via `start` against `in_done`) */
        memcpy(out + o, buf + in_done, start - in_done);
        o += start - in_done;
        in_done = start;
        memcpy(out + o, seq, n);
        o += n;
    }
    /* keep the state current through the final partial row, for the NEXT
     * commit's head */
    if(start < len)
        fytim_sgr_feed(p, buf + start, len - start, sgr_sink_, NULL);
    if(!out) return NULL;
    memcpy(out + o, buf + in_done, len - in_done);   /* cap reserved the tail */
    *out_len = o + (len - in_done);
    return out;
}

/* The single funnel into the transcript: normalize, then batch. Every
 * committed row is counted -- the pump may shrink the frame only by as
 * many rows as this pump commits, so shrink and scroll cancel and the
 * bubble stays pinned. */
static void commit_norm(struct fytim *ft, const char *buf, size_t len)
{
    size_t nlen = 0, i;
    char *norm = sgr_rowsafe(&ft->commit_sgr, buf, len, &nlen);
    TimuiStr s;
    s.ptr = norm ? norm : (char *)buf;
    s.len = norm ? nlen : len;
    for(i = 0; i < len; i++)
        if(buf[i] == '\n') ft->pending_commit_rows++;
    if(len && buf[len - 1] != '\n')
        ft->pending_commit_rows++;    /* the emit appends the final '\n' */
    timui_inline_commit(ft->ui, s);   /* copies; batched until the pump */
    free(norm);
}

enum fytim_result fytim_commit(struct fytim *ft, const char *buf, size_t len)
{
    if(!ft || (!buf && len)) return FYTIM_ERR_INVALID;
    if(len == 0) return FYTIM_OK;
    if(!rendered_only(buf, len)) return FYTIM_ERR_INVALID;
    commit_norm(ft, buf, len);
    return FYTIM_OK;
}

enum fytim_result fytim_tail_set(struct fytim *ft, const char *buf, size_t len)
{
    char *dup;
    if(!ft || (!buf && len)) return FYTIM_ERR_INVALID;
    if(buf && len && !rendered_only(buf, len)) return FYTIM_ERR_INVALID;
    if(!buf || len == 0){
        free(ft->tail);
        ft->tail = NULL;
        ft->tail_streaming = false;    /* the stream is over */
        return FYTIM_OK;
    }
    dup = malloc(len + 1);
    if(!dup) return FYTIM_ERR_NOMEM;
    memcpy(dup, buf, len);
    dup[len] = '\0';
    free(ft->tail);
    ft->tail = dup;
    ft->tail_streaming = true;
    return FYTIM_OK;
}

enum fytim_result fytim_tail_apply(struct fytim *ft, size_t backtrack,
                                   const char *content, size_t len,
                                   size_t freeze)
{
    size_t pos, k, tlen;
    char *grown;

    if(!ft || (!content && len)) return FYTIM_ERR_INVALID;
    if(content && len && !rendered_only(content, len)) return FYTIM_ERR_INVALID;
    ft->tail_streaming = true;         /* until fytim_tail_set(NULL) */

    /* Backtrack, terminal-style: the cursor-row residue after the last
     * '\n' (an SGR carry-over, or a partial row) is erased in ADDITION to
     * the `backtrack` full rows -- CUU moves over full rows only, and the
     * erase-down then takes everything below. Counting the residue as one
     * of the backtracked rows under-drops and staircases the stream. */
    pos = ft->tail ? strlen(ft->tail) : 0;
    while(pos > 0 && ft->tail[pos - 1] != '\n') pos--;
    for(k = 0; k < backtrack && pos > 0; k++){
        pos--;
        while(pos > 0 && ft->tail[pos - 1] != '\n') pos--;
    }
    if(ft->tail) ft->tail[pos] = '\0';

    if(len > 0){
        grown = realloc(ft->tail, pos + len + 1);
        if(!grown) return FYTIM_ERR_NOMEM;
        memcpy(grown + pos, content, len);
        grown[pos + len] = '\0';
        ft->tail = grown;
    }

    if(freeze > 0 && ft->tail){
        char *p = ft->tail;
        for(k = 0; k < freeze && *p; k++){
            char *nl = strchr(p, '\n');
            if(!nl) break;
            p = nl + 1;
        }
        if(p != ft->tail){
            /* the cut drops the bytes that carried the SGR state into the
             * remainder: re-open it at the remainder's head */
            struct fytim_sgr_parser sp;
            char seq[64];
            size_t slen, rlen = strlen(p);
            /* the full prefix, final '\n' included: trimming it swallowed
             * a frozen BLANK row (a block separator), losing the breath
             * line between blocks in the transcript */
            fytim_commit(ft, ft->tail, (size_t)(p - ft->tail));
            fytim_sgr_init(&sp);
            fytim_sgr_feed(&sp, ft->tail, (size_t)(p - ft->tail), sgr_sink_, NULL);
            slen = fytim_sgr_style_emit(&sp.style, seq, sizeof seq);
            if(slen > 0){
                char *nt = malloc(slen + rlen + 1);
                if(nt){
                    memcpy(nt, seq, slen);
                    memcpy(nt + slen, p, rlen + 1);
                    free(ft->tail);
                    ft->tail = nt;
                }else{
                    memmove(ft->tail, p, rlen + 1);
                }
            }else{
                memmove(ft->tail, p, rlen + 1);
            }
        }
    }
    tlen = ft->tail ? strlen(ft->tail) : 0;
    if(tlen == 0 && ft->tail){
        free(ft->tail);
        ft->tail = NULL;
    }
    return FYTIM_OK;
}

/* ---- external editor ---------------------------------------------------- */

enum fytim_result fytim_suspend(struct fytim *ft)
{
    if(!ft || ft->suspended) return FYTIM_ERR_INVALID;
    if(timui_suspend(ft->ui) != TIMUI_OK) return FYTIM_ERR_IO;
    ft->suspended = true;
    return FYTIM_OK;
}

enum fytim_result fytim_resume(struct fytim *ft)
{
    if(!ft || !ft->suspended) return FYTIM_ERR_INVALID;
    if(timui_resume(ft->ui) != TIMUI_OK) return FYTIM_ERR_IO;
    ft->suspended = false;
    return FYTIM_OK;
}

/* ---- work-bands --------------------------------------------------------- */

struct fytim_workband *fytim_workband_create(struct fytim *ft)
{
    struct fytim_workband *wb, **tail;
    if(!ft) return NULL;
    wb = calloc(1, sizeof *wb);
    if(!wb) return NULL;
    wb->owner = ft;
    wb->max_rows = ft->wb_default_max;
    tail = &ft->wbands;
    while(*tail) tail = &(*tail)->next;
    *tail = wb;
    return wb;
}

/* Unlink wb from its owner and free it. */
static void wb_retire(struct fytim_workband *wb)
{
    struct fytim_workband **p = &wb->owner->wbands;
    while(*p && *p != wb) p = &(*p)->next;
    if(*p) *p = wb->next;
    wb_free(wb);
}

/* Validate and dup an SGR-only byte range into *slot; NULL/empty clears.
 * On rejection the previous value is retained. */
static enum fytim_result wb_set_slot(char **slot, const char *buf, size_t len)
{
    char *dup;
    if(!buf && len) return FYTIM_ERR_INVALID;
    if(buf && len && !rendered_only(buf, len)) return FYTIM_ERR_INVALID;
    if(!buf || len == 0){
        free(*slot);
        *slot = NULL;
        return FYTIM_OK;
    }
    dup = malloc(len + 1);
    if(!dup) return FYTIM_ERR_NOMEM;
    memcpy(dup, buf, len);
    dup[len] = '\0';
    free(*slot);
    *slot = dup;
    return FYTIM_OK;
}

enum fytim_result fytim_workband_set(struct fytim_workband *wb,
                                     const char *buf, size_t len)
{
    if(!wb) return FYTIM_ERR_INVALID;
    return wb_set_slot(&wb->content, buf, len);
}

enum fytim_result fytim_workband_set_commit(struct fytim_workband *wb,
                                            const char *buf, size_t len)
{
    if(!wb) return FYTIM_ERR_INVALID;
    return wb_set_slot(&wb->commit, buf, len);
}

enum fytim_result fytim_workband_set_max_rows(struct fytim_workband *wb, int rows)
{
    if(!wb || rows < 1) return FYTIM_ERR_INVALID;
    wb->max_rows = rows;
    return FYTIM_OK;
}

enum fytim_result fytim_workband_set_top(struct fytim_workband *wb,
                                         const char *text)
{
    if(!wb) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&wb->top, text);
}

enum fytim_result fytim_workband_set_bottom(struct fytim_workband *wb,
                                            const char *text)
{
    if(!wb) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&wb->bottom, text);
}

enum fytim_result fytim_workband_commit(struct fytim_workband *wb)
{
    if(!wb) return FYTIM_ERR_INVALID;
    if(wb->owner->tail || wb->owner->tail_streaming){
        /* the transcript tail is streaming: committing now would split the
         * streaming reply. The final render stays on screen; the pump
         * flushes it once the tail clears. The handle is invalid to the
         * host either way. */
        wb->finished = 1;
        wb->finish_seq = ++wb->owner->wb_finish_seq;
        return FYTIM_OK;
    }
    {
        /* the commit payload, when set, replaces the live render */
        const char *out = wb->commit ? wb->commit : wb->content;
        if(out)
            commit_norm(wb->owner, out, strlen(out));  /* batched, like fytim_commit */
    }
    wb_retire(wb);
    return FYTIM_OK;
}

/* Flush work-bands that finished while the tail was streaming, oldest
 * finisher first, now that the stream is over. */
static void wb_flush_finished(struct fytim *ft)
{
    for(;;){
        struct fytim_workband *wb, *best = NULL;
        for(wb = ft->wbands; wb; wb = wb->next)
            if(wb->finished && (!best || wb->finish_seq < best->finish_seq))
                best = wb;
        if(!best) return;
        {
            const char *out = best->commit ? best->commit : best->content;
            if(out) commit_norm(ft, out, strlen(out));
        }
        wb_retire(best);
    }
}

void fytim_workband_destroy(struct fytim_workband *wb)
{
    if(!wb) return;
    wb_retire(wb);
}



static size_t fytim_utf8_put_(char *out, uint32_t cp);

/* A growable byte buffer, for serialising a grid into committed text. */
struct response_text { char *buf; size_t len, cap; };

static int rt_add(struct response_text *t, const char *data, size_t n)
{
    if(t->len + n + 1 > t->cap){
        size_t cap = t->cap ? t->cap * 2 : 256;
        char *nb;
        while(cap < t->len + n + 1) cap *= 2;
        nb = realloc(t->buf, cap);
        if(!nb) return -1;
        t->buf = nb;
        t->cap = cap;
    }
    memcpy(t->buf + t->len, data, n);
    t->len += n;
    t->buf[t->len] = '\0';
    return 0;
}

/* ---- the cell surface -------------------------------------------------- */

/* Rows a surface asks for: its grid, capped by the band's own cap. */
static int surface_content_rows(const struct fytim_surface *sf)
{
    int n = sf->rows;
    if(n < 1) n = 1;
    if(n > sf->wb->max_rows) n = sf->wb->max_rows;
    return n;
}

struct fytim_surface *fytim_surface_open(struct fytim *ft, int rows, int cols)
{
    struct fytim_surface *sf;
    struct fytim_workband *wb;

    if(!ft || rows < 1 || cols < 1) return NULL;
    /* A grid is rows * cols cells: refuse a size whose product overflows. */
    if((size_t)rows > SIZE_MAX / sizeof(struct fytim_cell) / (size_t)cols)
        return NULL;

    sf = calloc(1, sizeof *sf);
    if(!sf) return NULL;
    sf->grid = calloc((size_t)rows * (size_t)cols, sizeof *sf->grid);
    if(!sf->grid){ free(sf); return NULL; }

    wb = fytim_workband_create(ft);
    if(!wb){ free(sf->grid); free(sf); return NULL; }

    sf->wb = wb;
    sf->owner = ft;
    sf->rows = rows;
    sf->cols = cols;
    sf->cur_row = sf->cur_col = -1;
    /* The grid is the content: a surface is not capped below its own size
     * unless the host asks for it. */
    wb->max_rows = rows;
    wb->surface = sf;
    return sf;
}

void fytim_surface_close(struct fytim_surface *sf)
{
    if(!sf) return;
    /* The keys cannot stay with something that is gone. */
    if(sf->owner->keys == sf) sf->owner->keys = NULL;
    /* Retiring the band frees the surface with it (see wb_free). */
    wb_retire(sf->wb);
}

enum fytim_result fytim_surface_resize(struct fytim_surface *sf, int rows,
                                       int cols)
{
    struct fytim_cell *grid;
    int r, keep_rows, keep_cols;

    if(!sf || rows < 1 || cols < 1) return FYTIM_ERR_INVALID;
    if((size_t)rows > SIZE_MAX / sizeof *grid / (size_t)cols)
        return FYTIM_ERR_INVALID;
    if(rows == sf->rows && cols == sf->cols) return FYTIM_OK;

    grid = calloc((size_t)rows * (size_t)cols, sizeof *grid);
    if(!grid) return FYTIM_ERR_NOMEM;

    /* Keep what still fits. The host redraws anyway, but a resize that
     * blanked the grid would flash an empty pane in the meantime. */
    keep_rows = rows < sf->rows ? rows : sf->rows;
    keep_cols = cols < sf->cols ? cols : sf->cols;
    for(r = 0; r < keep_rows; r++)
        memcpy(grid + (size_t)r * (size_t)cols,
               sf->grid + (size_t)r * (size_t)sf->cols,
               (size_t)keep_cols * sizeof *grid);

    free(sf->grid);
    sf->grid = grid;
    if(sf->wb->max_rows == sf->rows) sf->wb->max_rows = rows;
    sf->rows = rows;
    sf->cols = cols;
    if(sf->cur_row >= rows || sf->cur_col >= cols){
        sf->cur_row = sf->cur_col = -1;
        sf->cur_visible = false;
    }
    return FYTIM_OK;
}

enum fytim_result fytim_surface_size(const struct fytim_surface *sf, int *rows,
                                     int *cols)
{
    if(!sf) return FYTIM_ERR_INVALID;
    if(rows) *rows = sf->rows;
    if(cols) *cols = sf->cols;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_granted_rows(const struct fytim_surface *sf,
                                             int *rows)
{
    if(!sf || !rows) return FYTIM_ERR_INVALID;
    *rows = sf->granted;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_set_max_rows(struct fytim_surface *sf, int rows)
{
    if(!sf || rows < 0) return FYTIM_ERR_INVALID;
    sf->wb->max_rows = rows > 0 ? rows : sf->rows;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_put_row(struct fytim_surface *sf, int row,
                                        const struct fytim_cell *cells, int n)
{
    if(!sf || !cells || n < 0) return FYTIM_ERR_INVALID;
    if(row < 0 || row >= sf->rows) return FYTIM_ERR_INVALID;
    /* More cells than the row holds is not an error: a host that publishes
     * a wider screen than the surface has is showing part of it. */
    if(n > sf->cols) n = sf->cols;

    memcpy(sf->grid + (size_t)row * (size_t)sf->cols, cells,
           (size_t)n * sizeof *cells);
    if(n < sf->cols)
        memset(sf->grid + (size_t)row * (size_t)sf->cols + n, 0,
               (size_t)(sf->cols - n) * sizeof *cells);
    return FYTIM_OK;
}

enum fytim_result fytim_surface_clear(struct fytim_surface *sf)
{
    if(!sf) return FYTIM_ERR_INVALID;
    memset(sf->grid, 0,
           (size_t)sf->rows * (size_t)sf->cols * sizeof *sf->grid);
    return FYTIM_OK;
}

enum fytim_result fytim_surface_set_cursor(struct fytim_surface *sf, int row,
                                           int col, bool visible)
{
    if(!sf) return FYTIM_ERR_INVALID;
    if(!visible){
        sf->cur_visible = false;
        return FYTIM_OK;
    }
    if(row < 0 || col < 0 || row >= sf->rows || col >= sf->cols)
        return FYTIM_ERR_INVALID;
    sf->cur_row = row;
    sf->cur_col = col;
    sf->cur_visible = true;
    return FYTIM_OK;
}


/*
 * Serialise one row of the grid as styled text: an SGR run is written when the
 * style changes, and the blank tail of the row is left off. This is the same
 * form a work band's content takes, so a committed surface reaches the
 * transcript through the one commit path.
 */
static int surface_row_text(const struct fytim_surface *sf, int row,
                            struct response_text *out)
{
    struct fytim_sgr_style st, prev;
    const struct fytim_cell *cell;
    char esc[64], utf8[4];
    bool have_prev = false;
    int col, last, i;
    size_t n;

    last = -1;
    for(col = 0; col < sf->cols; col++)
        if(sf->grid[(size_t)row * (size_t)sf->cols + (size_t)col].chars[0])
            last = col;

    memset(&prev, 0, sizeof prev);
    for(col = 0; col <= last; col++){
        cell = &sf->grid[(size_t)row * (size_t)sf->cols + (size_t)col];
        st.fg = cell->fg;
        st.bg = cell->bg;
        st.attrs = cell->attrs;
        if(!have_prev || st.fg != prev.fg || st.bg != prev.bg ||
           st.attrs != prev.attrs){
            n = fytim_sgr_style_emit(&st, esc, sizeof esc);
            if(n && rt_add(out, esc, n)) return -1;
            prev = st;
            have_prev = true;
        }
        if(!cell->chars[0]){
            if(rt_add(out, " ", 1)) return -1;
            continue;
        }
        for(i = 0; i < FYTIM_CELL_CHARS && cell->chars[i]; i++){
            n = fytim_utf8_put_(utf8, cell->chars[i]);
            if(rt_add(out, utf8, n)) return -1;
        }
        if(cell->width > 1) col++;
    }
    if(have_prev && rt_add(out, "\x1b[0m", 4)) return -1;
    return 0;
}

enum fytim_result fytim_surface_commit(struct fytim_surface *sf)
{
    struct response_text out;
    int row, last;

    if(!sf) return FYTIM_ERR_INVALID;

    memset(&out, 0, sizeof out);
    /* A screen is mostly blank at the bottom: commit what was drawn. */
    last = -1;
    for(row = 0; row < sf->rows; row++){
        int col;
        for(col = 0; col < sf->cols; col++)
            if(sf->grid[(size_t)row * (size_t)sf->cols + (size_t)col].chars[0])
                last = row;
    }
    /* The chrome is part of the screen: a title says what the screen was. */
    if(sf->wb->top && sf->wb->top[0] &&
       rt_add(&out, sf->wb->top, strlen(sf->wb->top))) goto nomem;

    for(row = 0; row <= last; row++){
        if(out.len && rt_add(&out, "\n", 1)) goto nomem;
        if(surface_row_text(sf, row, &out)) goto nomem;
    }

    if(sf->wb->bottom && sf->wb->bottom[0]){
        if(out.len && rt_add(&out, "\n", 1)) goto nomem;
        if(rt_add(&out, sf->wb->bottom, strlen(sf->wb->bottom))) goto nomem;
    }
    if(out.len) commit_norm(sf->owner, out.buf, out.len);
    free(out.buf);
    fytim_surface_close(sf);
    return FYTIM_OK;

nomem:
    free(out.buf);
    return FYTIM_ERR_NOMEM;
}

enum fytim_result fytim_surface_set_keys(struct fytim_surface *sf, bool take)
{
    if(!sf) return FYTIM_ERR_INVALID;
    if(take) sf->owner->keys = sf;
    else if(sf->owner->keys == sf) sf->owner->keys = NULL;
    return FYTIM_OK;
}

bool fytim_surface_has_keys(const struct fytim_surface *sf)
{
    return sf && sf->owner->keys == sf;
}

enum fytim_result fytim_surface_set_top(struct fytim_surface *sf,
                                        const char *text)
{
    if(!sf) return FYTIM_ERR_INVALID;
    return fytim_workband_set_top(sf->wb, text);
}

enum fytim_result fytim_surface_set_bottom(struct fytim_surface *sf,
                                           const char *text)
{
    if(!sf) return FYTIM_ERR_INVALID;
    return fytim_workband_set_bottom(sf->wb, text);
}

enum fytim_result fytim_set_header(struct fytim *ft, const char *text)
{
    if(!ft) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&ft->header, text);
}

enum fytim_result fytim_set_status_row(struct fytim *ft, int row, const char *text)
{
    if(!ft || row < 0 || row > 1) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&ft->status[row], text);
}

enum fytim_result fytim_set_marker(struct fytim *ft, const char *marker)
{
    if(!ft) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&ft->marker, marker);   /* SGR-capable, validated */
}

static bool style_only_(void *user, const char *text, size_t len,
                        const struct fytim_sgr_style *style)
{
    bool *visible = user;
    (void)text; (void)style;
    if(len) *visible = true;
    return true;
}

enum fytim_result fytim_set_prompt_style(struct fytim *ft, const char *sgr)
{
    struct fytim_sgr_parser p;
    bool visible = false;

    if(!ft) return FYTIM_ERR_INVALID;
    if(!sgr){
        memset(&ft->prompt_style, 0, sizeof ft->prompt_style);
        ft->prompt_style_set = false;
        return FYTIM_OK;
    }
    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, sgr, strlen(sgr), style_only_, &visible);
    if(visible || p.disallowed_seen)
        return FYTIM_ERR_INVALID;
    ft->prompt_style = p.style;
    ft->prompt_style_set = true;
    return FYTIM_OK;
}

enum fytim_result fytim_set_chrome_style(struct fytim *ft,
        enum fytim_chrome_style slot, const char *sgr)
{
    struct fytim_sgr_parser p;
    bool visible = false;

    if(!ft || slot < 0 || slot >= FYTIM_CHROME_STYLE_COUNT)
        return FYTIM_ERR_INVALID;
    if(!sgr){
        memset(&ft->chrome_style[slot], 0, sizeof ft->chrome_style[slot]);
        ft->chrome_style_set[slot] = false;
        return FYTIM_OK;
    }
    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, sgr, strlen(sgr), style_only_, &visible);
    if(visible || p.disallowed_seen)
        return FYTIM_ERR_INVALID;
    ft->chrome_style[slot] = p.style;
    ft->chrome_style_set[slot] = true;
    return FYTIM_OK;
}

/* ---- input -------------------------------------------------------------- */

static void input_load(struct fytim *ft, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if(n >= sizeof ft->input) n = sizeof ft->input - 1;
    if(n) memmove(ft->input, s, n);
    ft->input[n] = '\0';
    ft->pst.cursor = n;
    ft->pst.scroll_y = 0;
}

enum fytim_result fytim_set_input(struct fytim *ft, const char *text)
{
    if(!ft) return FYTIM_ERR_INVALID;
    input_load(ft, text);
    ft->comp_active = 0;
    return FYTIM_OK;
}

const char *fytim_input(const struct fytim *ft)
{
    return ft ? ft->input : "";
}

/* ---- history ------------------------------------------------------------ */

enum fytim_result fytim_history_add(struct fytim *ft, const char *line)
{
    char *dup;
    if(!ft || !line) return FYTIM_ERR_INVALID;
    if(ft->hist_n > 0 && strcmp(ft->hist[ft->hist_n - 1], line) == 0){
        ft->hist_pos = ft->hist_n;
        return FYTIM_OK;                 /* consecutive duplicates collapse */
    }
    if(ft->hist_n == ft->hist_max){      /* full: drop the oldest */
        free(ft->hist[0]);
        memmove(&ft->hist[0], &ft->hist[1],
                (size_t)(ft->hist_n - 1) * sizeof ft->hist[0]);
        ft->hist_n--;
    }
    if(ft->hist_n == ft->hist_cap){
        int ncap = ft->hist_cap ? ft->hist_cap * 2 : 16;
        char **nh = realloc(ft->hist, (size_t)ncap * sizeof *nh);
        if(!nh) return FYTIM_ERR_NOMEM;
        ft->hist = nh;
        ft->hist_cap = ncap;
    }
    dup = strdup(line);
    if(!dup) return FYTIM_ERR_NOMEM;
    ft->hist[ft->hist_n++] = dup;
    ft->hist_pos = ft->hist_n;
    return FYTIM_OK;
}

enum fytim_result fytim_history_set_max_len(struct fytim *ft, int max_len)
{
    if(!ft || max_len < 1) return FYTIM_ERR_INVALID;
    while(ft->hist_n > max_len){         /* shrink: oldest go first */
        free(ft->hist[0]);
        memmove(&ft->hist[0], &ft->hist[1],
                (size_t)(ft->hist_n - 1) * sizeof ft->hist[0]);
        ft->hist_n--;
    }
    ft->hist_max = max_len;
    if(ft->hist_pos > ft->hist_n) ft->hist_pos = ft->hist_n;
    return FYTIM_OK;
}

static void hist_prev(struct fytim *ft)
{
    if(ft->hist_pos == 0) return;
    if(ft->hist_pos == ft->hist_n){      /* leaving the live draft: keep it */
        free(ft->draft);
        ft->draft = strdup(ft->input);
        if(!ft->draft) return;
    }
    ft->hist_pos--;
    input_load(ft, ft->hist[ft->hist_pos]);
}

static void hist_next(struct fytim *ft)
{
    if(ft->hist_pos >= ft->hist_n) return;
    ft->hist_pos++;
    input_load(ft, ft->hist_pos == ft->hist_n ? (ft->draft ? ft->draft : "")
                                              : ft->hist[ft->hist_pos]);
}

/* Multiline guard: arrows browse history only from the edge lines. */
static int cursor_on_first_line(const struct fytim *ft)
{
    return memchr(ft->input, '\n', ft->pst.cursor) == NULL;
}
static int cursor_on_last_line(const struct fytim *ft)
{
    return strchr(ft->input + ft->pst.cursor, '\n') == NULL;
}

/* ---- completion --------------------------------------------------------- */

enum fytim_result fytim_set_complete_fn(struct fytim *ft,
                                        fytim_complete_fn fn, void *user)
{
    if(!ft) return FYTIM_ERR_INVALID;
    ft->complete_fn = fn;
    ft->complete_user = user;
    return FYTIM_OK;
}

enum fytim_result fytim_completion_add(struct fytim_completions *c,
                                       const char *candidate)
{
    struct fytim *ft;
    char *dup;
    if(!c || !c->owner || !candidate) return FYTIM_ERR_INVALID;
    ft = c->owner;
    if(!ft->comp_collecting) return FYTIM_ERR_INVALID;   /* outside the callback */
    if(ft->comp_n == ft->comp_cap){
        int ncap = ft->comp_cap ? ft->comp_cap * 2 : 8;
        char **nc = realloc(ft->comp, (size_t)ncap * sizeof *nc);
        if(!nc) return FYTIM_ERR_NOMEM;
        ft->comp = nc;
        ft->comp_cap = ncap;
    }
    dup = strdup(candidate);
    if(!dup) return FYTIM_ERR_NOMEM;
    ft->comp[ft->comp_n++] = dup;
    return FYTIM_OK;
}

static void complete_leave(struct fytim *ft)
{
    ft->comp_active = 0;
    comp_free_candidates(ft);
    free(ft->comp_saved);
    ft->comp_saved = NULL;
}

static void complete_tab(struct fytim *ft)
{
    if(!ft->complete_fn) return;
    if(!ft->comp_active){
        size_t plen = strlen(ft->input), common;
        int i;
        comp_free_candidates(ft);
        free(ft->comp_saved);
        ft->comp_saved = strdup(ft->input);
        if(!ft->comp_saved) return;
        ft->comp_collecting = 1;
        ft->complete_fn(ft->complete_user, ft->input, &ft->comps);
        ft->comp_collecting = 0;
        if(ft->comp_n == 0) return;               /* nothing matches: beepless */
        if(ft->comp_n == 1){                      /* unique: complete outright */
            input_load(ft, ft->comp[0]);
            complete_leave(ft);
            return;
        }
        /* bash-style: first Tab extends to the longest common prefix; only
         * when that adds nothing does Tab start cycling the candidates */
        common = strlen(ft->comp[0]);
        for(i = 1; i < ft->comp_n; i++){
            size_t j = 0;
            while(j < common && ft->comp[i][j] == ft->comp[0][j]) j++;
            common = j;
        }
        if(common > plen && common < sizeof ft->input){
            memcpy(ft->input, ft->comp[0], common);
            ft->input[common] = '\0';
            ft->pst.cursor = common;
            ft->pst.scroll_y = 0;
            complete_leave(ft);
            return;
        }
        ft->comp_active = 1;
        ft->comp_idx = 0;
    }else{
        ft->comp_idx = (ft->comp_idx + 1) % (ft->comp_n + 1);  /* + the original */
    }
    input_load(ft, ft->comp_idx == ft->comp_n ? ft->comp_saved
                                              : ft->comp[ft->comp_idx]);
}

/* ---- band drawing ------------------------------------------------------- */

/* Draw one SGR-styled string into the frame as cells, clipped to the row. */
struct draw_run_ctx {
    TimuiCellBuffer *buf;
    int x, y, max_x;
    /* Colors and attributes an SGR run leaves unset inherit the base style,
     * so inline chrome (for example a colored status margin) does not cancel
     * the row's dim/bold/reverse presentation. */
    TimuiStyle base;
};
/* Indexed (16/256-palette) colors pass through AS INDEXED: the core
 * emits the classic 30-37/90-97 codes, so the terminal's own theme
 * palette applies. (They were once dropped to the default -- colorless
 * -- and briefly mapped to hard-coded xterm RGB.) */
static uint32_t sgr_color_(uint32_t c)
{
    if(c == FYTIM_COLOR_DEFAULT) return TIMUI_COLOR_DEFAULT;
    if(c & FYTIM_COLOR_INDEXED)  return TIMUI_COLOR_ANSI | (c & 0xFFu);
    return c;
}

static uint32_t timui_attrs_from_fytim_(uint32_t attrs)
{
    uint32_t out = 0;
    if(attrs & FYTIM_ATTR_BOLD)      out |= TIMUI_ATTR_BOLD;
    if(attrs & FYTIM_ATTR_DIM)       out |= TIMUI_ATTR_DIM;
    if(attrs & FYTIM_ATTR_ITALIC)    out |= TIMUI_ATTR_ITALIC;
    if(attrs & FYTIM_ATTR_UNDERLINE) out |= TIMUI_ATTR_UNDERLINE;
    if(attrs & FYTIM_ATTR_REVERSE)   out |= TIMUI_ATTR_REVERSE;
    if(attrs & FYTIM_ATTR_STRIKE)    out |= TIMUI_ATTR_STRIKE;
    return out;
}

/* Encode one code point; @out holds at least 4 bytes. */
static size_t fytim_utf8_put_(char *out, uint32_t cp)
{
    if(cp < 0x80){ out[0] = (char)cp; return 1; }
    if(cp < 0x800){
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if(cp < 0x10000){
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static TimuiStyle timui_style_from_sgr_(const struct fytim_sgr_style *s,
                                        TimuiStyle base)
{
    TimuiStyle st;

    st.fg = s->fg == FYTIM_COLOR_DEFAULT ? base.fg : sgr_color_(s->fg);
    st.bg = s->bg == FYTIM_COLOR_DEFAULT ? base.bg : sgr_color_(s->bg);
    st.attrs = base.attrs;
    if(s->attrs & FYTIM_ATTR_BOLD)      st.attrs |= TIMUI_ATTR_BOLD;
    if(s->attrs & FYTIM_ATTR_DIM)       st.attrs |= TIMUI_ATTR_DIM;
    if(s->attrs & FYTIM_ATTR_ITALIC)    st.attrs |= TIMUI_ATTR_ITALIC;
    if(s->attrs & FYTIM_ATTR_UNDERLINE) st.attrs |= TIMUI_ATTR_UNDERLINE;
    if(s->attrs & FYTIM_ATTR_REVERSE)   st.attrs |= TIMUI_ATTR_REVERSE;
    if(s->attrs & FYTIM_ATTR_STRIKE)    st.attrs |= TIMUI_ATTR_STRIKE;
    return st;
}

static bool draw_run_(void *user, const char *text, size_t len,
                      const struct fytim_sgr_style *style)
{
    struct draw_run_ctx *ctx = user;
    TimuiStyle st;
    TimuiStr s;
    size_t i, start = 0;
    st.fg = style->fg == FYTIM_COLOR_DEFAULT ? ctx->base.fg
                                             : sgr_color_(style->fg);
    st.bg = style->bg == FYTIM_COLOR_DEFAULT ? ctx->base.bg
                                             : sgr_color_(style->bg);
    st.attrs = ctx->base.attrs;
    if(style->attrs & FYTIM_ATTR_BOLD)      st.attrs |= TIMUI_ATTR_BOLD;
    if(style->attrs & FYTIM_ATTR_DIM)       st.attrs |= TIMUI_ATTR_DIM;
    if(style->attrs & FYTIM_ATTR_ITALIC)    st.attrs |= TIMUI_ATTR_ITALIC;
    if(style->attrs & FYTIM_ATTR_UNDERLINE) st.attrs |= TIMUI_ATTR_UNDERLINE;
    if(style->attrs & FYTIM_ATTR_REVERSE)   st.attrs |= TIMUI_ATTR_REVERSE;
    if(style->attrs & FYTIM_ATTR_STRIKE)    st.attrs |= TIMUI_ATTR_STRIKE;
    for(i = 0; i <= len; i++){
        if(i == len || text[i] == '\n'){
            if(ctx->x < ctx->max_x && i > start){
                s.ptr = text + start;
                s.len = i - start;
                timui_draw_text(ctx->buf, ctx->x, ctx->y, s, st);
                {   /* advance by the run's display width, cluster-aware */
                    size_t off = 0;
                    while(off < s.len){
                        size_t nx = timui_grapheme_next(s.ptr, s.len, off);
                        if(nx <= off) break;
                        ctx->x += timui_grapheme_width(s.ptr + off, nx - off);
                        off = nx;
                    }
                }
            }
            if(i < len){ ctx->x = 0; ctx->y++; }   /* '\n' starts a new row */
            start = i + 1;
        }
    }
    return true;
}

/* Draw one chrome row that may carry SGR styling (rendered markdown).
 * Unstyled text keeps the row's plain style; styled text is parsed to
 * cells like work-band content. */
/* Display width of SGR-styled text: escapes are zero-width. */
static bool width_run_(void *user, const char *text, size_t len,
                       const struct fytim_sgr_style *style)
{
    int *w = user;
    size_t off = 0;
    (void)style;
    while(off < len){
        size_t nx = timui_grapheme_next(text, len, off);
        if(nx <= off) break;
        *w += timui_grapheme_width(text + off, nx - off);
        off = nx;
    }
    return true;
}

static int sgr_disp_width(const char *s)
{
    struct fytim_sgr_parser p;
    int w = 0;
    if(!strchr(s, '\x1b')) return timui_display_width(s);
    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, s, strlen(s), width_run_, &w);
    return w;
}

static void draw_row_styled(TimuiFrame *f, TimuiCellBuffer *buf,
                            int x, int y, int w,
                            const char *text, TimuiStyle plain)
{
    if(strchr(text, '\x1b')){
        struct draw_run_ctx ctx;
        struct fytim_sgr_parser sp;
        ctx.buf = buf; ctx.x = x; ctx.y = y; ctx.max_x = x + w;
        ctx.base = plain;
        fytim_sgr_init(&sp);
        fytim_sgr_feed(&sp, text, strlen(text), draw_run_, &ctx);
    }else{
        timui_label(f, x, y, (TimuiStr){ text, strlen(text) }, plain);
    }
}

/*
 * Draw @rows rows of the grid at @y, the LAST rows of it when the region is
 * short: the bottom of a screen is where a program is working, so that is
 * what a shortened surface keeps.
 *
 * One cell is drawn as text rather than written to the buffer directly,
 * because a cell here carries a base character and the characters that
 * combine with it, and the core resolves such a cluster - its width, and the
 * continuation cell a wide glyph needs - on the way in.
 */
static void draw_surface(TimuiCellBuffer *buf, const struct fytim_surface *sf,
                         int x, int y, int w, int rows)
{
    char utf8[FYTIM_CELL_CHARS * 4];
    const struct fytim_cell *cell;
    TimuiStyle st;
    TimuiStr str;
    int first, row, col, i;
    size_t len;

    first = sf->rows - rows;
    if(first < 0) first = 0;

    for(row = first; row < sf->rows; row++, y++){
        for(col = 0; col < sf->cols && col < w; col++){
            cell = &sf->grid[(size_t)row * (size_t)sf->cols + (size_t)col];
            st = timui_style_make(sgr_color_(cell->fg), sgr_color_(cell->bg),
                                  timui_attrs_from_fytim_(cell->attrs));
            /* The cursor is a reverse-video cell: the cursor of the terminal
             * belongs to the prompt, and a surface is watched while the user
             * types somewhere else. */
            if(sf->cur_visible && row == sf->cur_row && col == sf->cur_col)
                st.attrs ^= TIMUI_ATTR_REVERSE;
            len = 0;
            for(i = 0; i < FYTIM_CELL_CHARS && cell->chars[i]; i++)
                len += fytim_utf8_put_(utf8 + len, cell->chars[i]);
            if(!len){
                /* A blank cell still carries its colours. */
                utf8[0] = ' ';
                len = 1;
            }
            str.ptr = utf8;
            str.len = len;
            timui_draw_text(buf, x + col, y, str, st);
            /* A double-width glyph owns the cell after it. */
            if(cell->width > 1) col++;
        }
    }
}

/* Rows styled text occupies: its '\n'-terminated rows, plus the trailing
 * partial row only when it holds VISIBLE text. The row after a final '\n'
 * -- empty, or only an SGR carry-over residue -- must not count: it is a
 * phantom row. In the transcript tail it has no matching commit, and every
 * miscount moves the bubble (the frame resize it forces has no commit to
 * cancel against). In a work band, whose rendered content always ends on a
 * newline, it costs the band a row and pushes the newest real row out of a
 * capped window. */
static int styled_rows(const char *s)
{
    int n = 0;
    const char *p, *last;
    bool visible = false;
    if(!s || !*s) return 0;
    for(p = s, last = s; *p; p++)
        if(*p == '\n'){ n++; last = p + 1; }
    if(*last){
        struct fytim_sgr_parser sp;
        fytim_sgr_init(&sp);
        fytim_sgr_feed(&sp, last, strlen(last), sgr_probe_, &visible);
        if(visible) n++;
    }
    return n;
}

static bool sgr_probe_(void *user, const char *text, size_t len,
                       const struct fytim_sgr_style *style)
{
    (void)text; (void)len; (void)style;
    *(bool *)user = true;      /* a run was delivered: visible bytes exist */
    return false;
}

/* Rows a work-band wants: its content up to max_rows (at least one row, so
 * an idle band still shows), plus the optional top/bottom chrome rows. */
static int wb_rows(const struct fytim_workband *wb)
{
    int n = wb->surface ? surface_content_rows(wb->surface)
                        : styled_rows(wb->content);
    if(n < 1) n = 1;
    if(n > wb->max_rows) n = wb->max_rows;
    return n + (wb->top ? 1 : 0) + (wb->bottom ? 1 : 0);
}

/* Rows above the chrome: the transcript's live tail, then the work-bands;
 * one spare row when nothing is live -- the layout always reserves a
 * transcript row, and sizing the band to exactly the chrome would shed a
 * status row to pay for it. */
static int wb_rows_total(const struct fytim *ft)
{
    const struct fytim_workband *wb;
    int n = styled_rows(ft->tail);
    for(wb = ft->wbands; wb; wb = wb->next) n += wb_rows(wb);
    return n > 0 ? n : 1;
}

/*
 * Rows the prompt asks for, and none at all while a surface holds the keys:
 * a prompt that cannot be typed into is a row taken from the program.
 */
static int prompt_lines(const struct fytim *ft)
{
    const char *marker = ft->marker ? ft->marker : "> ";
    if(ft->keys) return 0;
    const char *p = ft->input;
    size_t len = strlen(p), i = 0, next;
    int width = ft->term_w - sgr_disp_width(marker);
    int n = 1, col = 0, gw;
    if(width < 1) width = 1;
    while(i < len && n < FYTIM_PROMPT_MAX){
        if(p[i] == '\r' || p[i] == '\n'){
            if(p[i] == '\r' && i + 1 < len && p[i + 1] == '\n') i++;
            i++;
            n++;
            col = 0;
            continue;
        }
        next = timui_grapheme_next(p, len, i);
        if(next <= i) next = i + 1;
        gw = timui_grapheme_width(p + i, next - i);
        if(gw < 1) gw = 1;
        if(col > 0 && col + gw > width){
            n++;
            col = 0;
        }
        col += gw;
        i = next;
        if(col >= width){
            n++;
            col = 0;
        }
    }
    return n < FYTIM_PROMPT_MAX ? n : FYTIM_PROMPT_MAX;
}

static void draw_completion_ribbon(struct fytim *ft, TimuiFrame *f,
                                   const struct fytim_rect *r, TimuiStyle st)
{
    char full[2048], line[512];
    int i, o = 0, sel_beg = 0, sel_end = 0, first = 0;
    for(i = 0; i <= ft->comp_n && o < (int)sizeof full - 1; i++){
        const char *c = i == ft->comp_n
                        ? (ft->comp_saved && ft->comp_saved[0] ? ft->comp_saved
                                                               : "(empty)")
                        : ft->comp[i];
        if(i == ft->comp_idx) sel_beg = o;
        o += snprintf(full + o, sizeof full - (size_t)o, "%c%s%c ",
                      i == ft->comp_idx ? '[' : ' ', c,
                      i == ft->comp_idx ? ']' : ' ');
        if(i == ft->comp_idx) sel_end = o;
    }
    {   /* window on the selection so it never leaves the screen */
        int avail = r->w - 2, len, truncated;
        if(avail < 4) avail = 4;
        if(sel_end > avail - 1 + first){
            first = sel_end - (avail - 1);
            if(first > sel_beg) first = sel_beg;
        }
        len = o - first;
        truncated = len > avail;
        if(truncated) len = avail - 1;
        if(len > (int)sizeof line - 8) len = (int)sizeof line - 8;
        snprintf(line, sizeof line, "%s%.*s%s",
                 first > 0 ? "\xe2\x80\xa6" : " ",
                 len, full + first,
                 truncated ? "\xe2\x80\xa6" : "");
    }
    timui_label(f, r->x, r->y, (TimuiStr){ line, strlen(line) }, st);
}

/*
 * A chrome row with nothing in it is not a row, while a surface holds the
 * keys.
 *
 * The layout solves geometry and cannot know whether the host set a header or
 * a status line, so it reserves them. Here the content is known. This applies
 * only to the full-screen case, though: the streaming transcript pins its
 * bubble against the chrome below it, and taking those rows away when they
 * happen to be empty would move it.
 */
static void layout_drop_empty_chrome(const struct fytim *ft,
                                     struct fytim_layout *lay)
{
    int freed = 0;
    size_t i;

    if(!ft->keys) return;

    if(!ft->header && lay->band[FYTIM_BAND_HEADER].h){
        freed += lay->band[FYTIM_BAND_HEADER].h;
        lay->band[FYTIM_BAND_HEADER].h = 0;
    }
    if(!ft->status[0] && !ft->status[1] && !ft->comp_active &&
       lay->band[FYTIM_BAND_STATUS].h){
        freed += lay->band[FYTIM_BAND_STATUS].h;
        lay->band[FYTIM_BAND_STATUS].h = 0;
        /* The trailing separator borders nothing once the status is gone. */
        freed += lay->band[FYTIM_BAND_SEP_BOTTOM].h;
        lay->band[FYTIM_BAND_SEP_BOTTOM].h = 0;
    }
    if(!freed) return;

    lay->band[FYTIM_BAND_TRANSCRIPT].h += freed;
    /* The bands below the transcript move up by what it took. */
    for(i = 0; i < FYTIM_BAND_COUNT; i++){
        int y = 0;
        size_t j;
        for(j = 0; j < i; j++)
            y += lay->band[j].h;
        lay->band[i].y = lay->band[i].h ? y : 0;
    }
}

static void draw_band(struct fytim *ft, TimuiFrame *f,
                      const struct fytim_layout *lay, bool *submitted)
{
    TimuiCellBuffer *buf = timui_frame_buffer(f);
    const struct fytim_rect *r;
    TimuiStyle in_st  = timui_slot_style(ft->ui, TIMUI_SLOT_INPUT_FOCUSED);
    TimuiStyle sep_st = timui_style_make(in_st.fg, in_st.bg, TIMUI_ATTR_DIM);
    TimuiStyle dim    = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT,
                                         TIMUI_ATTR_DIM);
    TimuiStyle bold   = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT,
                                         TIMUI_ATTR_BOLD);
    TimuiStyle wb_st, header_st, status_st, marker_st;
    const char *marker = ft->marker ? ft->marker : "> ";
    int marker_w;

    wb_st = ft->chrome_style_set[FYTIM_CHROME_WORKBAND] ?
            timui_style_from_sgr_(&ft->chrome_style[FYTIM_CHROME_WORKBAND],
                                  dim) : dim;
    header_st = ft->chrome_style_set[FYTIM_CHROME_HEADER] ?
            timui_style_from_sgr_(&ft->chrome_style[FYTIM_CHROME_HEADER],
                                  bold) : bold;
    status_st = ft->chrome_style_set[FYTIM_CHROME_STATUS] ?
            timui_style_from_sgr_(&ft->chrome_style[FYTIM_CHROME_STATUS],
                                  dim) : dim;
    if(ft->prompt_style_set){
        in_st.fg = sgr_color_(ft->prompt_style.fg);
        in_st.bg = sgr_color_(ft->prompt_style.bg);
        in_st.attrs = 0;
        if(ft->prompt_style.attrs & FYTIM_ATTR_BOLD)
            in_st.attrs |= TIMUI_ATTR_BOLD;
        if(ft->prompt_style.attrs & FYTIM_ATTR_DIM)
            in_st.attrs |= TIMUI_ATTR_DIM;
        if(ft->prompt_style.attrs & FYTIM_ATTR_ITALIC)
            in_st.attrs |= TIMUI_ATTR_ITALIC;
        if(ft->prompt_style.attrs & FYTIM_ATTR_UNDERLINE)
            in_st.attrs |= TIMUI_ATTR_UNDERLINE;
        if(ft->prompt_style.attrs & FYTIM_ATTR_REVERSE)
            in_st.attrs |= TIMUI_ATTR_REVERSE;
        if(ft->prompt_style.attrs & FYTIM_ATTR_STRIKE)
            in_st.attrs |= TIMUI_ATTR_STRIKE;
        sep_st = in_st;
    }
    marker_st = ft->chrome_style_set[FYTIM_CHROME_MARKER] ?
            timui_style_from_sgr_(&ft->chrome_style[FYTIM_CHROME_MARKER],
                    timui_style_make(in_st.fg, in_st.bg,
                                     in_st.attrs | TIMUI_ATTR_BOLD)) :
            timui_style_make(in_st.fg, in_st.bg, in_st.attrs | TIMUI_ATTR_BOLD);

    r = &lay->band[FYTIM_BAND_TRANSCRIPT];
    if(r->h > 0 && (ft->wbands || ft->tail)){
        /* The transcript's live tail sits at the TOP of the region, then
         * the work-bands stack oldest-first below it. Rows are granted
         * bottom-up (newest work-band first, the tail last), so when the
         * region is short the topmost content loses rows first; within a
         * shorted band the top row goes first, then the bottom, then the
         * earliest content lines. */
        struct fytim_workband *wb;
        int nb = 0, i, y, avail, tl = styled_rows(ft->tail);
        for(wb = ft->wbands; wb; wb = wb->next) nb++;
        {
            struct fytim_workband *arr[nb > 0 ? nb : 1];
            int give[nb > 0 ? nb : 1];
            for(wb = ft->wbands, i = 0; wb; wb = wb->next, i++) arr[i] = wb;
            avail = r->h;
            for(i = nb - 1; i >= 0; i--){
                int want = wb_rows(arr[i]);
                give[i] = want < avail ? want : avail;
                avail -= give[i];
            }
            y = r->y;
            if(tl > 0){
                /* the transcript tail: last rows that fit, no chrome */
                int tgive = tl < avail ? tl : avail;
                const char *p = ft->tail;
                int skip = tl - tgive;
                struct draw_run_ctx ctx;
                struct fytim_sgr_parser sp;
                while(skip > 0 && (p = strchr(p, '\n')) != NULL){ p++; skip--; }
                if(tgive > 0 && p){
                    ctx.buf = buf; ctx.x = r->x; ctx.y = y;
                    ctx.max_x = r->x + r->w;
                    ctx.base = timui_style_make(TIMUI_COLOR_DEFAULT,
                                                TIMUI_COLOR_DEFAULT, 0);
                    fytim_sgr_init(&sp);
                    fytim_sgr_feed(&sp, p, strlen(p), draw_run_, &ctx);
                }
                y += tgive;
            }
            for(i = 0; i < nb; i++){
                int rows = give[i], top, bottom, content, lines, skip;
                wb = arr[i];
                lines = wb->surface ? surface_content_rows(wb->surface)
                                    : styled_rows(wb->content);
                /* The band's own cap applies FIRST: past it, content shows
                 * its last max_rows lines and the chrome stays -- the rule
                 * and status row are what keep adjacent bands readable.
                 * Only a genuine row shortage (give < wb_rows) then sheds
                 * chrome: top rule first, bottom status next, content last. */
                content = lines < 1 ? 1 : lines;
                if(content > wb->max_rows) content = wb->max_rows;
                top = wb->top ? 1 : 0;
                bottom = wb->bottom ? 1 : 0;
                /*
                 * A band sheds its chrome first, because its content is the
                 * report. A surface sheds content first: its chrome is the
                 * state row of a running program, and a screen one row
                 * shorter costs less than losing what the program is doing.
                 */
                while(top + content + bottom > rows){
                    if(wb->surface && content > 1) content--;
                    else if(top) top = 0;
                    else if(bottom) bottom = 0;
                    else content--;
                }
                if(top){
                    if(wb->top[0])
                        draw_row_styled(f, buf, r->x, y, r->w, wb->top, wb_st);
                    else
                        timui_draw_hline(buf, r->x, y, r->w, wb_st);
                    y++;
                }
                if(wb->surface){
                    if(content > 0)
                        draw_surface(buf, wb->surface, r->x, y, r->w, content);
                    wb->surface->granted = content > 0 ? content : 0;
                }else if(content > 0 && wb->content){
                    const char *p = wb->content;
                    struct draw_run_ctx ctx;
                    struct fytim_sgr_parser sp;
                    skip = lines - content;
                    while(skip > 0 && (p = strchr(p, '\n')) != NULL){ p++; skip--; }
                    if(p){
                        ctx.buf = buf; ctx.x = r->x; ctx.y = y;
                        ctx.max_x = r->x + r->w;
                        ctx.base = timui_style_make(TIMUI_COLOR_DEFAULT,
                                                    TIMUI_COLOR_DEFAULT, 0);
                        fytim_sgr_init(&sp);
                        fytim_sgr_feed(&sp, p, strlen(p), draw_run_, &ctx);
                    }
                }
                y += content;
                if(bottom){
                    if(wb->bottom[0])
                        draw_row_styled(f, buf, r->x, y, r->w, wb->bottom, wb_st);
                    else
                        timui_draw_hline(buf, r->x, y, r->w, wb_st);
                    y++;
                }
            }
        }
    }
    r = &lay->band[FYTIM_BAND_HEADER];
    if(r->h > 0 && ft->header)
        draw_row_styled(f, buf, r->x, r->y, r->w, ft->header, header_st);
    r = &lay->band[FYTIM_BAND_SEP_TOP];
    if(r->h > 0){
        if(ft->prompt_style_set)
            timui_draw_fill(buf, TIMUI_RECT(r->x, r->y, r->w, r->h), sep_st);
        else
            timui_draw_hline(buf, r->x, r->y, r->w, sep_st);
    }
    r = &lay->band[FYTIM_BAND_PROMPT];
    if(r->h > 0){
        TimuiId id = TIMUI_ID("fytim.prompt");
        TimuiTextAreaResult res;
        timui_draw_fill(buf, TIMUI_RECT(r->x, r->y, r->w, r->h), in_st);
        /* the marker may carry SGR (a colored activity dot): draw it
         * through the styled path, width from visible glyphs only */
        draw_row_styled(f, buf, r->x, r->y, r->w, marker, marker_st);
        marker_w = sgr_disp_width(marker);
        /*
         * A surface holding the keys leaves no prompt band at all, so this
         * runs only when the prompt is the user's. Drawing a focused text
         * area then would also eat the text the surface was just given.
         */
        if(!ft->keys){
            timui_set_focus(f, id);   /* no focus model: the prompt owns keys */
            if(ft->prompt_style_set)
                res = timui_text_area_mut_styled(
                    f, id, TIMUI_RECT(r->x + marker_w, r->y,
                                      r->w - marker_w, r->h),
                    &ft->pst, TIMUI_TEXT_AREA_ENTER_SUBMITS, in_st);
            else
                res = timui_text_area_mut(
                    f, id, TIMUI_RECT(r->x + marker_w, r->y,
                                      r->w - marker_w, r->h),
                    &ft->pst, TIMUI_TEXT_AREA_ENTER_SUBMITS);
            if(res.submitted) *submitted = true;
        }
    }
    r = &lay->band[FYTIM_BAND_SEP_BOTTOM];
    if(r->h > 0){
        if(ft->prompt_style_set)
            timui_draw_fill(buf, TIMUI_RECT(r->x, r->y, r->w, r->h), sep_st);
        else
            timui_draw_hline(buf, r->x, r->y, r->w, sep_st);
    }
    r = &lay->band[FYTIM_BAND_STATUS];
    if(r->h > 0){
        if(ft->comp_active)
            draw_completion_ribbon(ft, f, r, status_st);
        else if(ft->status[0])
            draw_row_styled(f, buf, r->x, r->y, r->w,
                            ft->status[0], status_st);
        if(r->h > 1 && ft->status[1])
            draw_row_styled(f, buf, r->x, r->y + 1, r->w,
                            ft->status[1], status_st);
    }
}


/* ---- keys handed to a surface ------------------------------------------ */

struct key_out {
    char buf[512];
    size_t len;
};

static void key_put(struct key_out *k, const char *bytes, size_t n)
{
    if(k->len + n > sizeof k->buf) return;
    memcpy(k->buf + k->len, bytes, n);
    k->len += n;
}

static void key_put_str(struct key_out *k, const char *s)
{
    key_put(k, s, strlen(s));
}

/* The bytes a terminal sends for a named key, or NULL when it has none. */
static const char *key_sequence(TimuiKey key)
{
    switch(key){
    case TIMUI_KEY_ENTER:     return "\r";
    case TIMUI_KEY_TAB:       return "\t";
    case TIMUI_KEY_BACKSPACE: return "\x7f";
    case TIMUI_KEY_ESCAPE:    return "\x1b";
    case TIMUI_KEY_UP:        return "\x1b[A";
    case TIMUI_KEY_DOWN:      return "\x1b[B";
    case TIMUI_KEY_RIGHT:     return "\x1b[C";
    case TIMUI_KEY_LEFT:      return "\x1b[D";
    case TIMUI_KEY_HOME:      return "\x1b[H";
    case TIMUI_KEY_END:       return "\x1b[F";
    case TIMUI_KEY_INSERT:    return "\x1b[2~";
    case TIMUI_KEY_DELETE:    return "\x1b[3~";
    case TIMUI_KEY_PAGE_UP:   return "\x1b[5~";
    case TIMUI_KEY_PAGE_DOWN: return "\x1b[6~";
    case TIMUI_KEY_F1:        return "\x1bOP";
    case TIMUI_KEY_F2:        return "\x1bOQ";
    case TIMUI_KEY_F3:        return "\x1bOR";
    case TIMUI_KEY_F4:        return "\x1bOS";
    case TIMUI_KEY_F5:        return "\x1b[15~";
    case TIMUI_KEY_F6:        return "\x1b[17~";
    case TIMUI_KEY_F7:        return "\x1b[18~";
    case TIMUI_KEY_F8:        return "\x1b[19~";
    case TIMUI_KEY_F9:        return "\x1b[20~";
    case TIMUI_KEY_F10:       return "\x1b[21~";
    case TIMUI_KEY_F11:       return "\x1b[23~";
    case TIMUI_KEY_F12:       return "\x1b[24~";
    default:                  return NULL;
    }
}

/*
 * The control byte of a chord, or 0 when it is not one. A chord is reported
 * either as the letter it was typed with or as the control byte itself, and a
 * host reserving a key such as ^\ needs both to arrive.
 */
static char key_control_byte(uint32_t cp)
{
    if(cp >= 'a' && cp <= 'z') return (char)(cp - 'a' + 1);
    if(cp >= 'A' && cp <= 'Z') return (char)(cp - 'A' + 1);
    if(cp >= '@' && cp <= '_') return (char)(cp - '@');
    if(cp && (cp < 0x20 || cp == 0x7f)) return (char)cp;
    return 0;
}

/*
 * Encode the input of one frame for the surface holding the keys, in the order
 * it was typed. Order is the whole point: a host reserves a key for itself and
 * reads the key after it, so a chord and its command must not be swapped, and
 * two presses of one key are two presses.
 */
static void surface_keys_collect(struct fytim *ft, TimuiFrame *f)
{
    TimuiInputRecord rec;
    struct key_out k;
    const char *seq;
    char utf8[4];
    char ctl;
    int i, n;

    k.len = 0;
    n = timui_input_log_count(f);
    for(i = 0; i < n; i++){
        if(!timui_input_log_at(f, i, &rec)) break;
        if(rec.is_text){
            key_put(&k, utf8, fytim_utf8_put_(utf8, rec.codepoint));
            continue;
        }
        seq = key_sequence(rec.key);
        if(seq){
            key_put_str(&k, seq);
            continue;
        }
        /* A chord the key table cannot name: ^C is 0x03, which is the byte
         * the program wants and not an interrupt for this library to act on.
         * An Alt chord is the same byte with an escape before it, which is
         * how a terminal sends one. */
        ctl = (rec.mods & TIMUI_MOD_CTRL) ? key_control_byte(rec.codepoint) : 0;
        if(ctl){
            if(rec.mods & TIMUI_MOD_ALT) key_put_str(&k, "\x1b");
            key_put(&k, &ctl, 1);
            continue;
        }
        if(rec.codepoint && (rec.mods & TIMUI_MOD_ALT)){
            key_put_str(&k, "\x1b");
            key_put(&k, utf8, fytim_utf8_put_(utf8, rec.codepoint));
            continue;
        }
        if(rec.codepoint && !rec.mods)
            key_put(&k, utf8, fytim_utf8_put_(utf8, rec.codepoint));
    }

    if(!k.len) return;
    {
        char *dup = malloc(k.len + 1);
        if(!dup) return;
        memcpy(dup, k.buf, k.len);
        dup[k.len] = '\0';
        ev_push(ft, FYTIM_EVENT_SURFACE_KEYS, dup, k.len, 0, 0);
        ft->evq[(ft->ev_head + ft->ev_n - 1) % FYTIM_EVQ_CAP].surface =
            ft->keys;
    }
}

/* ---- the pump ----------------------------------------------------------- */

enum fytim_result fytim_pump(struct fytim *ft)
{
    TimuiFrame *f = NULL;
    struct fytim_layout lay;
    bool submitted = false;
    TimuiResult tr;

    if(!ft || !ft->ui) return FYTIM_ERR_INVALID;
    if(ft->closed) return FYTIM_ERR_CLOSED;
    if(ft->suspended) return FYTIM_OK;   /* terminal belongs to the child */

    /* the streaming reply ended: work-bands that finished during it may
     * now retire into the transcript, in finish order */
    if(!ft->tail && !ft->tail_streaming) wb_flush_finished(ft);

    /* geometry: sample the terminal (cheap ioctl; pipes keep the default)
     * and size the band for the work-bands and the prompt lines being
     * edited */
    {
        int nw = ft->term_w, nh = ft->term_h;
        int want;
        if(timui_term_size(ft->out_fd, &nw, &nh) == TIMUI_OK &&
           nw > 0 && nh > 0 && (nw != ft->term_w || nh != ft->term_h)){
            ft->term_w = nw;
            ft->term_h = nh;
            ev_push(ft, FYTIM_EVENT_RESIZE, NULL, 0, nw, nh);
            /* the terminal rewrapped/scrolled under the band: whatever is
             * on screen is stale regardless of our own geometry */
            timui_full_redraw(ft->ui);
        }
        want = FYTIM_CHROME_ROWS + wb_rows_total(ft) + (prompt_lines(ft) - 1);
        /* Growth is immediate; a shrink is allowed only up to the rows
         * COMMITTED in this same pump, so shrink and scroll cancel and the
         * bubble never moves -- all motion is text scrolling. An
         * unmatched shrink (a heal retracting rows: no commit to cancel
         * against) is HELD until later commits cover it; the stream-end
         * settle takes whatever remains in one atomic hop. styled_rows()
         * counting only visible rows is part of the same invariant. */
        if(ft->tail_streaming && want < ft->band_rows){
            int floor_rows = ft->band_rows - ft->pending_commit_rows;
            if(want < floor_rows) want = floor_rows;
        }
        ft->pending_commit_rows = 0;
        if(want > ft->term_h) want = ft->term_h;
        /* a WIDTH change must resize the frame too, not only a row change */
        if(want != ft->band_rows || ft->term_w != ft->band_w){
            if(timui_ui_resize(ft->ui, ft->term_w, want) == TIMUI_OK){
                ft->band_rows = want;
                ft->band_w = ft->term_w;
            }
        }
    }

    tr = timui_begin_result(ft->ui, &f);
    if(tr == TIMUI_ERR_EOF || tr == TIMUI_ERR_CLOSED){
        ft->closed = true;
        return FYTIM_ERR_CLOSED;
    }
    if(tr != TIMUI_OK) return FYTIM_ERR_IO;

    /* free the previous pop's text now that a pump invalidates it */
    free(ft->ev_last);
    ft->ev_last = NULL;

    /*
     * A surface holding the keys takes every one of them, Escape and ^C
     * included: they belong to the program it stands for. The host keeps a
     * key of its own and finds it in the bytes it is given.
     */
    if(ft->keys){
        surface_keys_collect(ft, f);
        if(fytim_layout_compute_ex(timui_width(f), timui_height(f),
                                   prompt_lines(ft), &lay)){
            layout_drop_empty_chrome(ft, &lay);
            draw_band(ft, f, &lay, &submitted);
        }
        timui_end(f);
        return FYTIM_OK;
    }

    if(timui_key_pressed(f, TIMUI_KEY_ESCAPE))
        ev_push(ft, FYTIM_EVENT_INTERRUPT, NULL, 0, 0, 0);
    if(timui_mouse_wheel(f) ||
       timui_key_pressed(f, TIMUI_KEY_PAGE_UP) ||
       timui_key_pressed(f, TIMUI_KEY_PAGE_DOWN))
        ev_push(ft, FYTIM_EVENT_SCROLLBACK, NULL, 0, 0, 0);
    {
        int ctrl = timui_key_pressed_mods(f, TIMUI_KEY_UNKNOWN, TIMUI_MOD_CTRL);
        uint32_t cp = timui_key_codepoint(f);
        if(ctrl && cp == 'c')
            ev_push(ft, FYTIM_EVENT_INTERRUPT, NULL, 0, 0, 0);
        if(ctrl && cp == 'd' && !ft->input[0])
            ev_push(ft, FYTIM_EVENT_QUIT, NULL, 0, 0, 0);
        if(ctrl && cp == 'l')
            timui_full_redraw(ft->ui);
        if(ctrl && cp == 'g')
            ev_push(ft, FYTIM_EVENT_EDIT, NULL, 0, 0, 0);
        if(timui_key_pressed(f, TIMUI_KEY_TAB))
            complete_tab(ft);
        else if(timui_text_input(f).len > 0 ||
                timui_key_pressed(f, TIMUI_KEY_ENTER))
            complete_leave(ft);          /* typing accepts and exits */
        if((ctrl && cp == 'p') ||
           (timui_key_pressed(f, TIMUI_KEY_UP) && cursor_on_first_line(ft)))
            hist_prev(ft);
        else if((ctrl && cp == 'n') ||
                (timui_key_pressed(f, TIMUI_KEY_DOWN) && cursor_on_last_line(ft)))
            hist_next(ft);
    }

    if(fytim_layout_compute_ex(timui_width(f), timui_height(f),
                               prompt_lines(ft), &lay))
        draw_band(ft, f, &lay, &submitted);

    if(submitted){
        char *text = strdup(ft->input);
        if(text)
            ev_push(ft, FYTIM_EVENT_LINE, text, strlen(text), 0, 0);
        input_load(ft, NULL);
        free(ft->draft);
        ft->draft = NULL;
    }

    timui_end(f);
    return FYTIM_OK;
}
