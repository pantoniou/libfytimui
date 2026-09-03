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

#include <limits.h>
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

struct fytim_workpane;

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
    /* Set when this band composes a pane of tiles instead of one screen. */
    struct fytim_workpane *pane;
    /* Set when this band IS a tile: the pane that composes it, which is also
     * the list it has to be unlinked from when it retires. */
    struct fytim_workpane *in_pane;
    /* Columns the tile was given at the last frame. A band of its own has
     * the width of the terminal and does not need telling; a tile has a
     * share of it, and a host that hard-wraps its rows has to know which. */
    int granted_cols;
    /* Where the tile sits in an explicit grid. A row of -1 means the host
     * placed nothing and the pane gives it the next free cell. */
    int cell_row, cell_col, cell_row_span, cell_col_span;
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
    char *margin;                  /* chrome at the left of every row */
    uint32_t wash;                 /* ground under the program, or DEFAULT */
    int wash_mix;                  /* percent of the wash mixed into a colour */
    int rows, cols;
    int requested_rows;            /* layout request; 0 follows grid rows */
    int granted_cols;              /* columns the grid was given last frame */
    int cur_row, cur_col;
    bool cur_visible;
    int granted;                   /* content rows drawn at the last frame */
    int scroll_total, scroll_top;  /* what the HOST's scrollback holds */
    /* Where the tile was drawn at the last frame, so that a click can be
     * given to the thing under it. A column of -1 means the control was not
     * drawn, and a click there is a click on nothing. */
    int rect_x, rect_y, rect_w, rect_h;
    int bar_x, zoom_x, close_x, ctl_y;
};

/*
 * One region that tiles the screens it holds. It rides on a work-band node
 * for the same reason a surface does: the order, the row granting and the
 * shedding are the band rules, and a pane is not a second set of them.
 */
struct fytim_workpane {
    struct fytim_workband *wb;
    struct fytim          *owner;
    /*
     * The tiles are band nodes of their own, off the instance's list. A band
     * node is already either styled text or a grid of cells, and it already
     * carries the chrome and the row cap that go with one: a tile needs to be
     * nothing more than that, wherever it is composed.
     */
    struct fytim_workband *tiles;  /* oldest first */
    struct fytim_workband *zoom;   /* the tile that takes the pane, or NULL */
    char *sep;                     /* rule between adjacent columns */
    int columns;                   /* 0 selects the automatic grid */
    enum fytim_workpane_place place;
    int min_tile_cols;
    unsigned int controls;
    /* The explicit grid, or 0 x 0 for the arrangement the pane solves. */
    int grid_rows, grid_cols;
    int row_size[FYTIM_GRID_MAX];  /* cells, or 0 for an equal share */
    int col_size[FYTIM_GRID_MAX];
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
    bool               mouse;      /* the grab the host asked for */
    int                out_fd;

    /* band chrome */
    char *header;
    char *status[2];
    char *marker;
    struct fytim_sgr_style prompt_style;
    bool prompt_style_set;
    uint32_t prompt_bg;            /* the prompt's ground, or DEFAULT */
    bool no_prompt;      /* nobody types here: draw no prompt band */
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
    cfg->mouse     = false;   /* selection and copy stay with the terminal */
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
    /*
     * The grab is the host's call and it is not free: with it, selection and
     * copy stop being the terminal's. A host asks for it to give a work pane
     * its controls, and it lasts as long as this instance - the terminal
     * enables the mode at startup and leaves it at teardown.
     */
    if(cfg->mouse) tcfg.flags |= TIMUI_FLAG_MOUSE;
    ft->mouse = cfg->mouse;

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
    ft->prompt_bg = FYTIM_COLOR_DEFAULT;
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

/* Release one surface. The band or the pane that held it has let it go. */
static void sf_free(struct fytim_surface *sf)
{
    if(!sf) return;
    if(sf->owner->keys == sf) sf->owner->keys = NULL;
    free(sf->grid);
    free(sf->margin);
    free(sf);
}

static void wb_free(struct fytim_workband *wb)
{
    if(!wb) return;
    if(wb->pane){
        /* The pane owns its tiles: a host handle can outlive neither. */
        struct fytim_workband *t, *tnext;
        for(t = wb->pane->tiles; t; t = tnext){
            tnext = t->next;
            t->in_pane = NULL;      /* the list is going with the pane */
            wb_free(t);
        }
        free(wb->pane->sep);
        free(wb->pane);
        wb->pane = NULL;
    }
    if(wb->surface){
        /* The band owns the surface once it is attached: the UI can outlive
         * the host's handle, and destroying the UI must free both. */
        sf_free(wb->surface);
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

bool fytim_mouse_enabled(const struct fytim *ft)
{
    return ft && ft->mouse;
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

enum fytim_result fytim_clear_screen(struct fytim *ft)
{
    if(!ft || !ft->ui) return FYTIM_ERR_INVALID;
    if(ft->closed) return FYTIM_ERR_CLOSED;
    timui_inline_clear_screen(ft->ui);
    return FYTIM_OK;
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
    /* Unplaced: an explicit grid gives it the next free cell. */
    wb->cell_row = -1;
    tail = &ft->wbands;
    while(*tail) tail = &(*tail)->next;
    *tail = wb;
    return wb;
}

/* Unlink wb from its owner and free it. */
static void wb_retire(struct fytim_workband *wb)
{
    struct fytim_workband **p;

    /* A tile is on its pane's list; every other band is on the instance's. */
    p = wb->in_pane ? &wb->in_pane->tiles : &wb->owner->wbands;
    if(wb->in_pane && wb->in_pane->zoom == wb) wb->in_pane->zoom = NULL;
    while(*p && *p != wb) p = &(*p)->next;
    if(*p) *p = wb->next;
    wb->in_pane = NULL;
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

enum fytim_result fytim_workband_granted_cols(const struct fytim_workband *wb,
                                              int *cols)
{
    if(!wb || !cols) return FYTIM_ERR_INVALID;
    /* A band of its own has the whole width; only a tile has a share. */
    *cols = wb->in_pane ? wb->granted_cols : wb->owner->term_w;
    return FYTIM_OK;
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

/* The tiles one frame draws. A pane past this shows the oldest of
 * them: a screen cannot usefully hold more anyway. */
#define FYTIM_PANE_TILES_MAX 64

/* ---- the cell surface -------------------------------------------------- */

/* Rows a surface asks for: its grid, capped by the band's own cap. */
static int surface_content_rows(const struct fytim_surface *sf)
{
    int n = sf->requested_rows > 0 ? sf->requested_rows : sf->rows;
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
    sf->wash = FYTIM_COLOR_DEFAULT;
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
    /* Retiring the band frees the surface with it (see wb_free). A tile's
     * band leaves the pane's list; every other leaves the instance's. */
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
    sf->wb->max_rows = rows > 0 ? rows : INT_MAX;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_request_rows(struct fytim_surface *sf,
                                             int rows)
{
    if(!sf || rows < 0) return FYTIM_ERR_INVALID;
    sf->requested_rows = rows;
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
        /* The alignment is part of the screen: keep it in the transcript. */
        if(sf->margin && sf->margin[0] &&
           rt_add(&out, sf->margin, strlen(sf->margin))) goto nomem;
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

/* ---- the work pane ------------------------------------------------------ */

/* A pane solves its geometry from what its tiles ask for, and a tile is a
 * band node, so the band's own row rules answer that. */
static int wb_rows(const struct fytim_workband *wb);
static int styled_rows(const char *s);
static int chrome_rows(const char *s);

int fytim_workpane_count(const struct fytim_workpane *wp)
{
    const struct fytim_workband *t;
    int n = 0;

    if(!wp) return 0;
    for(t = wp->tiles; t; t = t->next) n++;
    return n;
}

/*
 * The grid for @n tiles across @w columns: as many columns as fit while every
 * tile keeps the minimum width, and never more than a square arrangement
 * needs. A region too narrow for two tiles gives one column, which is the
 * stack of full-width screens a narrow terminal has to fall back to.
 */
static void pane_grid(const struct fytim_workpane *wp, int n, int w,
                      int *colsp, int *rowsp)
{
    int cols, fit, sq;

    if(n < 1){
        *colsp = *rowsp = 0;
        return;
    }
    if(wp->columns > 0){
        cols = wp->columns;
    }else{
        fit = wp->min_tile_cols > 0 ? w / wp->min_tile_cols : n;
        if(fit < 1) fit = 1;
        for(sq = 1; sq * sq < n; sq++)
            ;
        cols = fit < sq ? fit : sq;
    }
    if(cols < 1) cols = 1;
    if(cols > n) cols = n;
    *colsp = cols;
    *rowsp = (n + cols - 1) / cols;
}



/*
 * Divide @total cells over @n tracks. A track with a size of its own keeps
 * it. The rest take what @natural says their tiles need, sharing any surplus
 * and giving up any shortfall in proportion, so that a one-row report beside
 * an eight-row screen is not half the region. A NULL @natural asks for an
 * equal share instead, which is what a column of tiles wants: a tile states
 * a height and never a width.
 *
 * Every track keeps at least one cell, sized or not: a track nobody can see
 * is not a track.
 */
static void tracks_solve(const int *size, const int *natural, int n, int total,
                         int *out)
{
    int i, fixed = 0, flex = 0, want = 0, left, base, extra;

    for(i = 0; i < n; i++){
        if(size[i] == FYTIM_TRACK_FIT && natural){
            /* Sized from its content, and taken before the rest share. */
            out[i] = natural[i] > 0 ? natural[i] : 1;
            fixed += out[i];
        }else if(size[i] > 0){
            out[i] = size[i];
            fixed += size[i];
        }else{
            out[i] = natural && natural[i] > 0 ? natural[i] : 1;
            want += out[i];
            flex++;
        }
    }
    /* Sized tracks that overrun the region are cut back in place, newest
     * first, so that what is left still holds one cell per track. */
    for(i = n - 1; i >= 0 && fixed + flex > total; i--)
        while(out[i] > 1 && fixed + flex > total){
            if(size[i] != 0){ out[i]--; fixed--; }
            else break;
        }
    if(flex < 1) return;
    left = total - fixed;
    if(left < flex) left = flex;

    if(!natural){
        /* No natural size to honour: an equal share, spare to the left. */
        base = left / flex;
        if(base < 1) base = 1;
        extra = left - base * flex;
        if(extra < 0) extra = 0;
        for(i = 0; i < n; i++){
            if(size[i] != 0) continue;
            out[i] = base + (extra > 0 ? 1 : 0);
            if(extra > 0) extra--;
        }
        return;
    }
    if(want == left) return;

    /* Scale what the tracks want to what there is, keeping a cell each. */
    for(i = 0; i < n; i++){
        if(size[i] != 0) continue;
        out[i] = want > 0 ? (int)(((long)out[i] * left) / want) : 1;
        if(out[i] < 1) out[i] = 1;
    }
    /* Rounding leaves a remainder either way; settle it on the tallest
     * track, which is the one a cell matters least to. */
    for(;;){
        int sum = 0, big = -1;

        for(i = 0; i < n; i++)
            if(size[i] == 0){
                sum += out[i];
                if(big < 0 || out[i] > out[big]) big = i;
            }
        if(big < 0 || sum == left) break;
        if(sum < left){
            out[big]++;
        }else{
            /* Take from the tallest that can still give a cell. */
            int give = -1;

            for(i = 0; i < n; i++)
                if(size[i] == 0 && out[i] > 1 &&
                   (give < 0 || out[i] > out[give]))
                    give = i;
            if(give < 0) break;
            out[give]--;
        }
    }
}

/* Which grid row @t occupies, and over how many. @idxp counts the tiles the
 * host did not place, which fill the cells in reading order. */
static void tile_grid_row(const struct fytim_workpane *wp,
                          const struct fytim_workband *t, int *idxp,
                          int *rowp, int *spanp)
{
    if(t->cell_row >= 0){
        *rowp = t->cell_row;
        *spanp = t->cell_row_span;
        return;
    }
    *rowp = wp->grid_cols > 0 ? *idxp / wp->grid_cols : 0;
    *spanp = 1;
    (*idxp)++;
}

/* The natural height of one explicit grid row: its tallest tile. */
static int grid_row_rows(const struct fytim_workpane *wp, int row)
{
    const struct fytim_workband *t;
    int idx = 0, tall = 0, r, rs, h;

    for(t = wp->tiles; t; t = t->next){
        tile_grid_row(wp, t, &idx, &r, &rs);
        if(row < r || row >= r + rs) continue;
        h = wb_rows(t);
        /* A tile spanning rows asks each of them for its share. */
        if(rs > 1) h = (h + rs - 1) / rs;
        if(h > tall) tall = h;
    }
    return tall > 0 ? tall : 1;
}

/*
 * Rows the pane asks for. Every tile of a grid row is drawn to the same
 * height, so the tallest tile sets it; the pane's own chrome is added, and
 * its cap - the band node's - bounds the result.
 */
static int pane_rows(const struct fytim_workpane *wp)
{
    const struct fytim_workband *t;
    int n, cols, rows, tall = 0, want;

    n = wp->zoom ? 1 : fytim_workpane_count(wp);
    if(n < 1) return 0;
    /* An explicit grid asks for the sum of its rows: a sized track for its
     * size, and any other for what its tallest tile needs. */
    if(!wp->zoom && wp->grid_rows > 0){
        int gr, want_rows = 0;

        for(gr = 0; gr < wp->grid_rows; gr++){
            /* A fitted row asks for what its tiles need, as a sharing one
             * does; only a row given a size of its own overrides them. */
            want_rows += wp->row_size[gr] > 0 ? wp->row_size[gr]
                                              : grid_row_rows(wp, gr);
        }
        want_rows += chrome_rows(wp->wb->top) + chrome_rows(wp->wb->bottom);
        if(wp->wb->max_rows > 0 && want_rows > wp->wb->max_rows)
            want_rows = wp->wb->max_rows;
        return want_rows;
    }
    pane_grid(wp, n, wp->owner->term_w, &cols, &rows);
    if(wp->zoom){
        tall = wb_rows(wp->zoom);
    }else{
        for(t = wp->tiles; t; t = t->next){
            int h = wb_rows(t);
            if(h > tall) tall = h;
        }
    }
    if(tall < 1) tall = 1;
    want = rows * tall + chrome_rows(wp->wb->top) +
           chrome_rows(wp->wb->bottom);
    if(wp->wb->max_rows > 0 && want > wp->wb->max_rows)
        want = wp->wb->max_rows;
    return want;
}

struct fytim_workpane *fytim_workpane_create(struct fytim *ft)
{
    struct fytim_workpane *wp;
    struct fytim_workband *wb;

    if(!ft) return NULL;
    wp = calloc(1, sizeof *wp);
    if(!wp) return NULL;
    wb = fytim_workband_create(ft);
    if(!wb){ free(wp); return NULL; }

    wp->wb = wb;
    wp->owner = ft;
    wp->min_tile_cols = FYTIM_TILE_MIN_COLS;
    /* The tiles are the content: a pane is not capped unless asked. */
    wb->max_rows = 0;
    wb->pane = wp;
    return wp;
}

void fytim_workpane_destroy(struct fytim_workpane *wp)
{
    if(!wp) return;
    /* Retiring the band frees the pane and its tiles with it (see wb_free). */
    wb_retire(wp->wb);
}

enum fytim_result fytim_workpane_set_top(struct fytim_workpane *wp,
                                         const char *text)
{
    if(!wp) return FYTIM_ERR_INVALID;
    return fytim_workband_set_top(wp->wb, text);
}

enum fytim_result fytim_workpane_set_bottom(struct fytim_workpane *wp,
                                            const char *text)
{
    if(!wp) return FYTIM_ERR_INVALID;
    return fytim_workband_set_bottom(wp->wb, text);
}

enum fytim_result fytim_workpane_set_max_rows(struct fytim_workpane *wp,
                                              int rows)
{
    if(!wp || rows < 0) return FYTIM_ERR_INVALID;
    wp->wb->max_rows = rows;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_grid(struct fytim_workpane *wp, int rows,
                                          int cols)
{
    if(!wp || rows < 0 || cols < 0) return FYTIM_ERR_INVALID;
    if(rows > FYTIM_GRID_MAX || cols > FYTIM_GRID_MAX)
        return FYTIM_ERR_INVALID;
    /* Either dimension absent means no explicit grid at all. */
    if(rows < 1 || cols < 1) rows = cols = 0;
    wp->grid_rows = rows;
    wp->grid_cols = cols;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_row_size(struct fytim_workpane *wp,
                                              int row, int cells)
{
    if(!wp || cells < FYTIM_TRACK_FIT || row < 0 || row >= FYTIM_GRID_MAX)
        return FYTIM_ERR_INVALID;
    wp->row_size[row] = cells;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_col_size(struct fytim_workpane *wp,
                                              int col, int cells)
{
    if(!wp || cells < FYTIM_TRACK_FIT || col < 0 || col >= FYTIM_GRID_MAX)
        return FYTIM_ERR_INVALID;
    wp->col_size[col] = cells;
    return FYTIM_OK;
}

/* Place @wb, which must be a tile, in the explicit grid of its pane. */
static enum fytim_result wb_set_cell(struct fytim_workband *wb, int row,
                                     int col, int row_span, int col_span)
{
    const struct fytim_workpane *wp;

    if(!wb || !wb->in_pane) return FYTIM_ERR_INVALID;
    wp = wb->in_pane;
    if(row < 0 || col < 0 || row_span < 1 || col_span < 1)
        return FYTIM_ERR_INVALID;
    if(wp->grid_rows < 1 || wp->grid_cols < 1) return FYTIM_ERR_INVALID;
    if(row + row_span > wp->grid_rows || col + col_span > wp->grid_cols)
        return FYTIM_ERR_INVALID;
    wb->cell_row = row;
    wb->cell_col = col;
    wb->cell_row_span = row_span;
    wb->cell_col_span = col_span;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_set_cell(struct fytim_surface *s, int row,
                                         int col, int row_span, int col_span)
{
    if(!s) return FYTIM_ERR_INVALID;
    return wb_set_cell(s->wb, row, col, row_span, col_span);
}

enum fytim_result fytim_workband_set_cell(struct fytim_workband *wb, int row,
                                          int col, int row_span, int col_span)
{
    return wb_set_cell(wb, row, col, row_span, col_span);
}

enum fytim_result fytim_workpane_set_place(struct fytim_workpane *wp,
                                           enum fytim_workpane_place place)
{
    if(!wp) return FYTIM_ERR_INVALID;
    if(place != FYTIM_WORKPANE_ABOVE_PROMPT &&
       place != FYTIM_WORKPANE_BELOW_PROMPT)
        return FYTIM_ERR_INVALID;
    wp->place = place;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_columns(struct fytim_workpane *wp,
                                             int cols)
{
    if(!wp || cols < 0) return FYTIM_ERR_INVALID;
    wp->columns = cols;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_min_tile_cols(struct fytim_workpane *wp,
                                                   int cols)
{
    if(!wp || cols < 0) return FYTIM_ERR_INVALID;
    wp->min_tile_cols = cols > 0 ? cols : FYTIM_TILE_MIN_COLS;
    return FYTIM_OK;
}

enum fytim_result fytim_workpane_set_tile_sep(struct fytim_workpane *wp,
                                              const char *text)
{
    if(!wp) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&wp->sep, text);
}

enum fytim_result fytim_workpane_set_controls(struct fytim_workpane *wp,
                                              unsigned int flags)
{
    if(!wp) return FYTIM_ERR_INVALID;
    wp->controls = flags;
    return FYTIM_OK;
}

unsigned int fytim_workpane_controls(const struct fytim_workpane *wp)
{
    return wp ? wp->controls : 0;
}

enum fytim_result fytim_workpane_set_zoom(struct fytim_workpane *wp,
                                          struct fytim_surface *sf)
{
    if(!wp) return FYTIM_ERR_INVALID;
    if(sf && sf->wb->in_pane != wp) return FYTIM_ERR_INVALID;
    wp->zoom = sf ? sf->wb : NULL;
    return FYTIM_OK;
}

struct fytim_surface *fytim_workpane_zoomed(const struct fytim_workpane *wp)
{
    return wp && wp->zoom ? wp->zoom->surface : NULL;
}

/* Append an empty tile to @wp. NULL on failure. */
struct fytim_workband *fytim_workband_create_in(struct fytim_workpane *wp)
{
    struct fytim_workband *t, **tail;

    if(!wp) return NULL;
    t = calloc(1, sizeof *t);
    if(!t) return NULL;
    t->owner = wp->owner;
    t->in_pane = wp;
    t->max_rows = wp->owner->wb_default_max;
    /* Unplaced: an explicit grid gives it the next free cell. */
    t->cell_row = -1;
    tail = &wp->tiles;
    while(*tail) tail = &(*tail)->next;
    *tail = t;
    return t;
}

struct fytim_surface *fytim_surface_open_in(struct fytim_workpane *wp,
                                            int rows, int cols)
{
    struct fytim_surface *sf;
    struct fytim_workband *t;

    if(!wp || rows < 1 || cols < 1) return NULL;
    if((size_t)rows > SIZE_MAX / sizeof(struct fytim_cell) / (size_t)cols)
        return NULL;

    sf = calloc(1, sizeof *sf);
    if(!sf) return NULL;
    sf->grid = calloc((size_t)rows * (size_t)cols, sizeof *sf->grid);
    if(!sf->grid){ free(sf); return NULL; }

    /* A tile is a band node of the pane, so everything that reads sf->wb -
     * the commit path, the chrome, the row rules - keeps working, and the
     * grid composes beside a tile that is text. */
    t = fytim_workband_create_in(wp);
    if(!t){ free(sf->grid); free(sf); return NULL; }

    sf->wb = t;
    sf->owner = wp->owner;
    sf->wash = FYTIM_COLOR_DEFAULT;
    sf->rows = rows;
    sf->cols = cols;
    sf->cur_row = sf->cur_col = -1;
    sf->bar_x = sf->zoom_x = sf->close_x = sf->ctl_y = -1;
    /* The grid is the content: a surface is not capped below its own size
     * unless the host asks for it. */
    t->max_rows = rows;
    t->surface = sf;
    return sf;
}

enum fytim_result fytim_surface_set_scroll_extent(struct fytim_surface *sf,
                                                  int total_rows, int top_row)
{
    if(!sf || total_rows < 0 || top_row < 0) return FYTIM_ERR_INVALID;
    sf->scroll_total = total_rows;
    sf->scroll_top = top_row;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_set_margin(struct fytim_surface *sf,
                                           const char *text)
{
    if(!sf) return FYTIM_ERR_INVALID;
    return set_dup_sgr(&sf->margin, text);
}

enum fytim_result fytim_surface_set_bg(struct fytim_surface *sf, uint32_t bg,
                                       int mix)
{
    if(!sf) return FYTIM_ERR_INVALID;
    if(mix < 0) mix = 0;
    if(mix > 100) mix = 100;
    sf->wash = bg;
    sf->wash_mix = mix;
    return FYTIM_OK;
}

enum fytim_result fytim_surface_granted_cols(const struct fytim_surface *sf,
                                             int *cols)
{
    if(!sf || !cols) return FYTIM_ERR_INVALID;
    *cols = sf->granted_cols;
    return FYTIM_OK;
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

/*
 * The tail of a key chunk the host did not consume. It is put back in front
 * of the input stream, so the next pump decodes it as the terminal's own
 * bytes and gives it to whoever holds the keys then.
 */
enum fytim_result fytim_keys_return(struct fytim *ft, const char *buf,
                                    size_t len)
{
    if(!ft || (!buf && len)) return FYTIM_ERR_INVALID;
    if(!len) return FYTIM_OK;
    if(timui_input_push(ft->ui, buf, len) != TIMUI_OK)
        return FYTIM_ERR_INVALID;
    return FYTIM_OK;
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

enum fytim_result fytim_set_prompt_bg(struct fytim *ft, uint32_t bg)
{
    if(!ft) return FYTIM_ERR_INVALID;
    ft->prompt_bg = bg;
    return FYTIM_OK;
}

enum fytim_result fytim_set_prompt_enabled(struct fytim *ft, bool enabled)
{
    if(!ft) return FYTIM_ERR_INVALID;
    ft->no_prompt = !enabled;
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
/*
 * A ground is a colour, or FYTIM_COLOR_REVERSED for the one the terminal
 * draws text in. These take the value, not the thing wearing it: a tile and
 * the prompt stand on the same ground and are drawn by the same rules.
 */
struct ground {
    const struct fytim *ft;
    uint32_t bg;                   /* FYTIM_COLOR_DEFAULT: no ground */
    int mix;                       /* percent of it in a colour of the cell */
};

struct draw_run_ctx {
    TimuiCellBuffer *buf;
    /* @x is where the next glyph goes and @origin_x is where a row starts.
     * They differ for anything drawn away from the left edge - a tile of a
     * work pane - and a newline has to return to the region's edge and not
     * to the terminal's, or every row after the first lands on whatever
     * stands to the left. */
    int x, y, max_x, origin_x;
    /* Colors and attributes an SGR run leaves unset inherit the base style,
     * so inline chrome (for example a colored status margin) does not cancel
     * the row's dim/bold/reverse presentation. */
    TimuiStyle base;
    /* The ground the run stands on. A row of chrome is a row of what owns
     * it and does not keep a background of its own: the renderer that drew
     * it painted the theme's ground, which is what this replaces. */
    struct ground ground;
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

/*
 * The tile's own ground, under a cell that has @bg. A cell with no colour of
 * its own takes the wash; one the program coloured keeps that colour, mixed
 * toward the wash. An indexed colour is a palette entry this library does not
 * know the value of, so there is nothing to mix and it is left alone.
 */
/* The ground under a cell that has @bg of its own. */
static uint32_t ground_bg_(const struct ground *g, uint32_t bg)
{
    uint32_t out = 0;
    int i, a, b;

    if(g->bg == FYTIM_COLOR_DEFAULT) return bg;
    if(bg == FYTIM_COLOR_DEFAULT) return g->bg;
    if(bg & FYTIM_COLOR_INDEXED) return bg;
    if(g->mix <= 0) return bg;
    if(g->mix >= 100) return g->bg;
    if(!timui_caps_has(timui_caps(g->ft->ui), TIMUI_CAP_TRUECOLOR))
        return bg;
    for(i = 16; i >= 0; i -= 8){
        a = (int)((bg >> i) & 0xff);
        b = (int)((g->bg >> i) & 0xff);
        out |= (uint32_t)(a + (b - a) * g->mix / 100) << i;
    }
    return out;
}

/*
 * Put @st on @g. @fg and @bg are what the cell or the run asked for, and @st
 * already carries them.
 *
 * A ground the terminal names is applied by reversing: what the cell said in
 * its foreground becomes the colour of its text, because the ground is now
 * the one the terminal draws text in. A cell that has a ground of its own
 * keeps it, as it keeps a colour there is nothing to mix.
 */
static TimuiStyle ground_on_(const struct ground *g, TimuiStyle st,
                             uint32_t fg, uint32_t bg)
{
    if(g->bg == FYTIM_COLOR_DEFAULT) return st;
    /*
     * Dim darkens the whole cell, ground and all, so chrome drawn dim would
     * lay a grey band across a tile whose body is not grey. A ground is one
     * colour or it is not a ground; what a row says is still its own.
     */
    st.attrs &= ~(unsigned)TIMUI_ATTR_DIM;
    if(g->bg == FYTIM_COLOR_REVERSED){
        /* A cell already reversed shows its foreground as its ground. */
        if((st.attrs & TIMUI_ATTR_REVERSE) || bg != FYTIM_COLOR_DEFAULT)
            return st;
        st.bg = sgr_color_(fg);
        st.fg = TIMUI_COLOR_DEFAULT;
        st.attrs |= TIMUI_ATTR_REVERSE;
        return st;
    }
    if(st.attrs & TIMUI_ATTR_REVERSE) st.fg = sgr_color_(ground_bg_(g, fg));
    else                              st.bg = sgr_color_(ground_bg_(g, bg));
    return st;
}

/*
 * Put a run of chrome on @g. A row of chrome belongs to what owns it, so
 * whatever ground the renderer that drew it painted - a colour it declared,
 * or a reverse that makes its foreground one - is not a ground it keeps.
 * What it says survives as the colour of its text.
 */
static TimuiStyle ground_run_(const struct ground *g, TimuiStyle st)
{
    uint32_t fg;

    if(g->bg == FYTIM_COLOR_DEFAULT) return st;
    /* Dim darkens the ground with the text; the ground is not the row's. */
    st.attrs &= ~(unsigned)TIMUI_ATTR_DIM;
    if(st.attrs & TIMUI_ATTR_REVERSE){
        fg = st.fg;
        st.fg = st.bg;
        st.bg = fg;
        st.attrs &= ~(unsigned)TIMUI_ATTR_REVERSE;
    }
    if(g->bg == FYTIM_COLOR_REVERSED){
        st.bg = st.fg;
        st.fg = TIMUI_COLOR_DEFAULT;
        st.attrs |= TIMUI_ATTR_REVERSE;
        return st;
    }
    st.bg = sgr_color_(g->bg);
    return st;
}

/* The style @g itself is drawn in, for the rows nothing else covers. */
static TimuiStyle ground_style_(const struct ground *g, TimuiStyle st)
{
    if(g->bg == FYTIM_COLOR_DEFAULT) return st;
    st.attrs &= ~(unsigned)TIMUI_ATTR_DIM;
    if(g->bg == FYTIM_COLOR_REVERSED){
        st.fg = TIMUI_COLOR_DEFAULT;
        st.bg = TIMUI_COLOR_DEFAULT;
        st.attrs |= TIMUI_ATTR_REVERSE;
        return st;
    }
    st.bg = sgr_color_(g->bg);
    return st;
}

/* The ground of a tile. */
static struct ground surface_ground_(const struct fytim_surface *sf)
{
    struct ground g = { sf->owner, sf->wash, sf->wash_mix };

    return g;
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
    /*
     * A row of chrome belongs to the tile: whatever ground the renderer that
     * drew it painted is the theme's, not the tile's, so it is given the
     * tile's instead and only what it says is kept.
     */
    st = ground_run_(&ctx->ground, st);
    for(i = 0; i <= len; i++){
        if(i == len || text[i] == '\n'){
            if(ctx->x < ctx->max_x && i > start){
                /*
                 * Draw only what fits in the region, measured cluster by
                 * cluster. A row wider than the region is cut at its edge:
                 * the columns past it belong to whatever stands there.
                 */
                size_t off = 0, fit = 0;
                bool cut = false;
                int w = 0;
                s.ptr = text + start;
                s.len = i - start;
                while(off < s.len){
                    size_t nx = timui_grapheme_next(s.ptr, s.len, off);
                    int gw;
                    if(nx <= off) break;
                    gw = timui_grapheme_width(s.ptr + off, nx - off);
                    if(ctx->x + w + gw > ctx->max_x){ cut = true; break; }
                    w += gw;
                    off = nx;
                    fit = nx;
                }
                if(fit){
                    s.len = fit;
                    timui_draw_text(ctx->buf, ctx->x, ctx->y, s, st);
                    ctx->x += w;
                }
                /*
                 * Say that the row goes on. A row that simply stops at the
                 * edge of a tile reads as damage; one that ends in an
                 * ellipsis reads as a row too long for the space.
                 */
                if(cut && ctx->max_x > ctx->origin_x){
                    TimuiStr e = { "\xe2\x80\xa6", 3 };
                    timui_draw_text(ctx->buf, ctx->max_x - 1, ctx->y, e, st);
                    ctx->x = ctx->max_x;
                }
            }
            /* A newline starts a new row at the region's left edge. */
            if(i < len){ ctx->x = ctx->origin_x; ctx->y++; }
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

static void draw_row_styled_bg(TimuiFrame *f, TimuiCellBuffer *buf,
                               int x, int y, int w,
                               const char *text, TimuiStyle plain,
                               struct ground g)
{
    if(strchr(text, '\x1b')){
        struct draw_run_ctx ctx;
        struct fytim_sgr_parser sp;
        ctx.buf = buf; ctx.x = x; ctx.y = y; ctx.max_x = x + w;
        ctx.origin_x = x;
        ctx.base = plain;
        ctx.ground = g;
        fytim_sgr_init(&sp);
        fytim_sgr_feed(&sp, text, strlen(text), draw_run_, &ctx);
    }else{
        timui_label(f, x, y, (TimuiStr){ text, strlen(text) }, plain);
    }
}

static void draw_row_styled(TimuiFrame *f, TimuiCellBuffer *buf,
                            int x, int y, int w,
                            const char *text, TimuiStyle plain)
{
    struct ground none = { NULL, FYTIM_COLOR_DEFAULT, 0 };

    draw_row_styled_bg(f, buf, x, y, w, text, plain, none);
}

/*
 * Rows a chrome slot draws. A slot holds a head, and a head can be more than
 * one row: a shell says what it is and then what it was asked to run. NULL
 * draws none.
 */
static int chrome_rows(const char *s)
{
    int n = 1;

    if(!s) return 0;
    for(; *s; s++) if(*s == '\n') n++;
    return n;
}

/*
 * Draw a chrome slot at @y, one row for each of its lines, at most @max of
 * them. An empty slot draws a rule. Returns the rows drawn.
 */
static int draw_chrome_bg(TimuiFrame *f, TimuiCellBuffer *buf, int x, int y,
                          int w, const char *s, TimuiStyle st, int max,
                          struct ground g)
{
    const char *nl;
    char *line;
    size_t len;
    int n = 0;

    if(!s || max < 1) return 0;
    if(!s[0]){ timui_draw_hline(buf, x, y, w, st); return 1; }
    /*
     * @st dims chrome, which is right for a rule or a caption: chrome that
     * frames the work is not the work. A slot that carries its own styling
     * was styled by whoever set it - a rendered head naming the call - and
     * dimming it a second time takes the emphasis it was given. Such a slot
     * is drawn on a plain base and keeps what it asked for.
     */
    if(strchr(s, '\x1b'))
        st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
    while(*s && n < max){
        nl = strchr(s, '\n');
        len = nl ? (size_t)(nl - s) : strlen(s);
        line = malloc(len + 1);
        if(!line) break;
        memcpy(line, s, len);
        line[len] = '\0';
        draw_row_styled_bg(f, buf, x, y + n, w, line, st, g);
        free(line);
        n++;
        if(!nl) break;
        s = nl + 1;
    }
    return n;
}

static int draw_chrome(TimuiFrame *f, TimuiCellBuffer *buf, int x, int y,
                       int w, const char *s, TimuiStyle st, int max)
{
    struct ground none = { NULL, FYTIM_COLOR_DEFAULT, 0 };

    return draw_chrome_bg(f, buf, x, y, w, s, st, max, none);
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
static void draw_surface(TimuiFrame *f, TimuiCellBuffer *buf,
                         struct fytim_surface *sf, TimuiStyle chrome,
                         int x, int y, int w, int rows)
{
    char utf8[FYTIM_CELL_CHARS * 4];
    const struct fytim_cell *cell;
    struct ground g = surface_ground_(sf);
    TimuiStyle st;
    TimuiStr str;
    int first, row, col, i;
    int margin_w;
    size_t len;

    first = sf->rows - rows;
    if(first < 0) first = 0;

    /* The margin is chrome drawn at the left of every row; the grid gets what
     * is left of the width, which is what the host is told it has. */
    margin_w = sf->margin ? sgr_disp_width(sf->margin) : 0;
    if(margin_w > w) margin_w = w;
    sf->granted_cols = w - margin_w;

    /* The ground of the tile, so that the part of it the grid does not cover
     * carries it as the grid does. */
    if(sf->wash != FYTIM_COLOR_DEFAULT)
        timui_draw_fill(buf, TIMUI_RECT(x, y, w, rows),
                        ground_style_(&g, chrome));

    for(row = first; row < sf->rows; row++, y++){
        if(margin_w)
            draw_row_styled_bg(f, buf, x, y, margin_w, sf->margin,
                               ground_style_(&g, chrome), g);
        for(col = 0; col < sf->cols && col < w - margin_w; col++){
            bool cursor;

            cell = &sf->grid[(size_t)row * (size_t)sf->cols + (size_t)col];
            st = timui_style_make(sgr_color_(cell->fg), sgr_color_(cell->bg),
                                  timui_attrs_from_fytim_(cell->attrs));
            /* The cursor is a reverse-video cell: the cursor of the terminal
             * belongs to the prompt, and a surface is watched while the user
             * types somewhere else. */
            cursor = sf->cur_visible && row == sf->cur_row &&
                     col == sf->cur_col;
            /*
             * The ground first, then the cursor, which is the one cell that
             * is NOT on it: reversing a ground the terminal names takes the
             * cell off it, and reversing a coloured one shows the colour the
             * ground was drawn against.
             */
            st = ground_on_(&g, st, cell->fg, cell->bg);
            if(cursor)
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
            timui_draw_text(buf, x + margin_w + col, y, str, st);
            /* A double-width glyph owns the cell after it. */
            if(cell->width > 1) col++;
        }
    }
}

/* Draw surface chrome beside the same margin as the program grid. */
static int draw_surface_chrome(TimuiFrame *f, TimuiCellBuffer *buf,
                               struct fytim_surface *sf, int x, int y, int w,
                               const char *text, TimuiStyle chrome, int max)
{
    struct ground g = surface_ground_(sf);
    int margin_w, n, row;

    margin_w = sf->margin ? sgr_disp_width(sf->margin) : 0;
    if(margin_w > w) margin_w = w;
    chrome = ground_style_(&g, chrome);
    if(sf->wash != FYTIM_COLOR_DEFAULT && max > 0)
        timui_draw_fill(buf, TIMUI_RECT(x, y, w, max), chrome);
    n = draw_chrome_bg(f, buf, x + margin_w, y, w - margin_w, text, chrome,
                       max, g);
    for(row = 0; margin_w > 0 && row < n; row++)
        draw_row_styled_bg(f, buf, x, y + row, margin_w, sf->margin, chrome,
                           g);
    return n;
}

/*
 * The marks a tile carries at the top right, and the bar down its right edge.
 * They are drawn only when the host grabbed the mouse: a control the user
 * cannot reach is a column taken from the program for nothing.
 */
#define FYTIM_TILE_ZOOM_MARK   "\xe2\xa4\xa2"   /* U+2922 */
#define FYTIM_TILE_CLOSE_MARK  "\xc3\x97"       /* U+00D7 */
#define FYTIM_TILE_ARROW_UP    "\xe2\x96\xb4"   /* U+25B4 */
#define FYTIM_TILE_ARROW_DOWN  "\xe2\x96\xbe"   /* U+25BE */
#define FYTIM_TILE_BAR_TRACK   "\xe2\x94\x82"   /* U+2502 */
#define FYTIM_TILE_BAR_THUMB   "\xe2\x96\x88"   /* U+2588 */

static bool pane_controls_live(const struct fytim_workpane *wp)
{
    return wp->controls && wp->owner->mouse;
}

/* Columns the bar takes from the grid of a tile. */
static int pane_bar_cols(const struct fytim_workpane *wp)
{
    return (pane_controls_live(wp) &&
            (wp->controls & FYTIM_WORKPANE_SCROLLBAR)) ? 1 : 0;
}

/*
 * Draw the bar for @sf down the column at @x, @h rows tall. The thumb says
 * where the host's scrollback is: with nothing behind the screen it fills the
 * track, which is how a bar says there is nowhere to go.
 */
static void draw_tile_bar(TimuiCellBuffer *buf, struct fytim_surface *sf,
                          unsigned int controls, TimuiStyle st, int x, int y,
                          int h)
{
    int track_y = y, track_h = h, thumb_h, thumb_y, i, total, top;
    TimuiStr s;

    if(h < 1) return;
    sf->bar_x = x;
    if((controls & FYTIM_WORKPANE_ARROWS) && h >= 3){
        s.ptr = FYTIM_TILE_ARROW_UP; s.len = strlen(s.ptr);
        timui_draw_text(buf, x, y, s, st);
        s.ptr = FYTIM_TILE_ARROW_DOWN; s.len = strlen(s.ptr);
        timui_draw_text(buf, x, y + h - 1, s, st);
        track_y = y + 1;
        track_h = h - 2;
    }
    total = sf->scroll_total > 0 ? sf->scroll_total : sf->rows;
    if(total < sf->rows) total = sf->rows;
    top = sf->scroll_top;
    if(top > total - sf->rows) top = total - sf->rows;
    if(top < 0) top = 0;

    thumb_h = (int)(((long)track_h * sf->rows + total - 1) / total);
    if(thumb_h < 1) thumb_h = 1;
    if(thumb_h > track_h) thumb_h = track_h;
    thumb_y = total > sf->rows
            ? (int)(((long)(track_h - thumb_h) * top) / (total - sf->rows))
            : 0;

    for(i = 0; i < track_h; i++){
        bool on = i >= thumb_y && i < thumb_y + thumb_h;
        s.ptr = on ? FYTIM_TILE_BAR_THUMB : FYTIM_TILE_BAR_TRACK;
        s.len = strlen(s.ptr);
        timui_draw_text(buf, x, track_y + i, s, st);
    }
}

/*
 * Draw the zoom and close marks at the right of the tile's first row and
 * record where they went. They sit on the tile's own chrome row when it has
 * one, so that they cost the program nothing.
 */
static void draw_tile_marks(TimuiCellBuffer *buf, struct fytim_surface *sf,
                            unsigned int controls, TimuiStyle st, int x,
                            int y, int w)
{
    TimuiStr s;
    int cx = x + w;

    if(w < 1) return;
    if(controls & FYTIM_WORKPANE_CLOSE){
        cx--;
        if(cx < x) return;
        s.ptr = FYTIM_TILE_CLOSE_MARK; s.len = strlen(s.ptr);
        timui_draw_text(buf, cx, y, s, st);
        sf->close_x = cx;
    }
    if(controls & FYTIM_WORKPANE_ZOOM){
        cx--;
        if(cx < x) return;
        s.ptr = FYTIM_TILE_ZOOM_MARK; s.len = strlen(s.ptr);
        timui_draw_text(buf, cx, y, s, st);
        sf->zoom_x = cx;
    }
    sf->ctl_y = y;
}

/*
 * Draw the tiles of @wp into the region at @y, @rows high. Every tile of a
 * grid row is drawn to the same height and every tile of a column to the same
 * width, so the screens read as one thing; a short region takes rows from the
 * grid rows evenly, and the tile sheds its content before its chrome, as a
 * surface standing alone does.
 */
/*
 * Draw one tile into the cell it holds. @shared_top and @shared_bottom are
 * the chrome its neighbours reserve, so that every screen beside it is given
 * the same content height.
 */
static void draw_tile(TimuiFrame *f, TimuiCellBuffer *buf,
                      struct fytim_workpane *wp, struct fytim_workband *t,
                      TimuiStyle chrome, int tx, int y, int tw, int th,
                      int shared_top, int shared_bottom)
{
    struct fytim_surface *sf;
    int ty = y, top, bottom, content, lines;
    int reserve_top = shared_top, reserve_bottom = shared_bottom;

    {
        {
            if(tw < 1 || th < 1) return;
            sf = t->surface;
            if(sf){
                /* Last frame's placement says nothing about this one. */
                sf->rect_x = tx; sf->rect_y = y;
                sf->rect_w = tw; sf->rect_h = th;
                sf->bar_x = sf->zoom_x = sf->close_x = sf->ctl_y = -1;
            }
            lines = sf ? surface_content_rows(sf) : styled_rows(t->content);
            content = lines;
            if(content < 1)
                content = (sf || !(t->top || t->bottom)) ? 1 : 0;
            if(content > t->max_rows) content = t->max_rows;
            top = chrome_rows(t->top);
            bottom = chrome_rows(t->bottom);
            /* A surface reserves the tallest chrome in its grid row, so all
             * equal-sized screens receive the same content height. */
            while((sf ? reserve_top + content + reserve_bottom
                      : top + content + bottom) > th){
                if(sf && content > 1) content--;
                else if(sf && reserve_top > 0) reserve_top--;
                else if(sf && reserve_bottom > 0) reserve_bottom--;
                else if(top) top--;
                else if(bottom) bottom = 0;
                else content--;
            }
            if(sf){
                if(top > reserve_top) top = reserve_top;
                if(bottom > reserve_bottom) bottom = reserve_bottom;
            }
            if(top){
                if(sf && pane_controls_live(wp))
                    draw_tile_marks(buf, sf, wp->controls, chrome, tx, ty, tw);
                if(sf)
                    ty += draw_surface_chrome(f, buf, sf, tx, ty, tw,
                                              t->top, chrome, top);
                else
                    ty += draw_chrome(f, buf, tx, ty, tw, t->top, chrome,
                                      top);
            }else if(sf && pane_controls_live(wp)){
                /* With no chrome row of its own the tile carries the marks
                 * on its first row: a cell of the program is a smaller cost
                 * than a row taken from every tile. */
                draw_tile_marks(buf, sf, wp->controls, chrome, tx, ty, tw);
            }
            if(sf){
                /* The bar is chrome down the right edge: the grid gets what
                 * is left, which is what the host is told it has. */
                int bar = pane_bar_cols(wp);
                if(bar >= tw) bar = 0;
                if(content > 0)
                    draw_surface(f, buf, sf, chrome, tx, ty, tw - bar,
                                 content);
                if(bar)
                    draw_tile_bar(buf, sf, wp->controls, chrome,
                                  tx + tw - bar, ty, content);
                sf->granted = content > 0 ? content : 0;
            }else{
                t->granted_cols = tw;
            }
            if(!sf && content > 0 && t->content){
                /* A tile of styled text shows its LAST rows, as a band of
                 * its own does: the end of a report is what is being made. */
                const char *p = t->content;
                struct draw_run_ctx ctx;
                struct fytim_sgr_parser sp;
                int skip = lines - content;
                while(skip > 0 && (p = strchr(p, '\n')) != NULL){ p++; skip--; }
                if(p){
                    ctx.buf = buf; ctx.x = tx; ctx.y = ty;
                    ctx.max_x = tx + tw;
                    ctx.origin_x = ctx.x;
                    ctx.ground = (struct ground){ NULL,
                                                  FYTIM_COLOR_DEFAULT, 0 };
                    ctx.base = timui_style_make(TIMUI_COLOR_DEFAULT,
                                                TIMUI_COLOR_DEFAULT, 0);
                    fytim_sgr_init(&sp);
                    fytim_sgr_feed(&sp, p, strlen(p), draw_run_, &ctx);
                }
            }
            ty += content;
            if(bottom){
                if(t->bottom[0] && sf)
                    (void)draw_surface_chrome(f, buf, sf, tx, ty, tw,
                                              t->bottom, chrome, 1);
                else if(t->bottom[0])
                    draw_row_styled(f, buf, tx, ty, tw, t->bottom, chrome);
                else
                    timui_draw_hline(buf, tx, ty, tw, chrome);
            }
        }
    }
}

/* The chrome the tiles of one grid row reserve between them. */
static void row_chrome(struct fytim_workband **arr, int first, int last,
                       int *topp, int *bottomp)
{
    int i;

    *topp = *bottomp = 0;
    for(i = first; i < last; i++){
        if(!arr[i]->surface) continue;
        if(chrome_rows(arr[i]->top) > *topp)
            *topp = chrome_rows(arr[i]->top);
        if(chrome_rows(arr[i]->bottom) > *bottomp)
            *bottomp = chrome_rows(arr[i]->bottom);
    }
}

/* A tile no arrangement could place shows nothing, and must say so: a host
 * sizing a program to its granted rows would otherwise draw into rows
 * nobody has. */
static void tile_unplaced(struct fytim_workband *t)
{
    struct fytim_surface *sf = t->surface;

    t->granted_cols = 0;
    if(!sf) return;
    sf->granted = 0;
    sf->rect_w = sf->rect_h = 0;
    sf->bar_x = sf->zoom_x = sf->close_x = -1;
}

/*
 * Place the tiles of an explicit grid and draw each in the cell it holds.
 * The tracks are solved once, so a tile that spans cells takes their sizes
 * and the rules between them.
 */
static void draw_pane_grid(TimuiFrame *f, TimuiCellBuffer *buf,
                           struct fytim_workpane *wp, TimuiStyle chrome,
                           int x, int y, int w, int rows)
{
    /* Where each tile of a grid row was placed, resolved before anything is
     * drawn: the chrome a tile reserves is what its whole row reserves. */
    struct placed { struct fytim_workband *t; int col, row_span, col_span; };
    struct placed row_tiles[FYTIM_GRID_MAX][FYTIM_GRID_MAX];
    int cw[FYTIM_GRID_MAX], rh[FYTIM_GRID_MAX];
    int cx[FYTIM_GRID_MAX], cy[FYTIM_GRID_MAX];
    int row_n[FYTIM_GRID_MAX];
    bool taken[FYTIM_GRID_MAX][FYTIM_GRID_MAX];
    struct fytim_workband *t;
    int nr = wp->grid_rows, nc = wp->grid_cols;
    int sep_w, i, j, next = 0;

    memset(taken, 0, sizeof taken);
    memset(row_n, 0, sizeof row_n);
    sep_w = (wp->sep && wp->sep[0]) ? sgr_disp_width(wp->sep) : 0;
    if(nc > 1 && w - (nc - 1) * sep_w < nc) sep_w = 0;

    {
        int nat[FYTIM_GRID_MAX];

        for(i = 0; i < nr; i++) nat[i] = grid_row_rows(wp, i);
        tracks_solve(wp->col_size, NULL, nc, w - (nc - 1) * sep_w, cw);
        tracks_solve(wp->row_size, nat, nr, rows, rh);
    }
    for(i = 0, j = x; i < nc; i++){ cx[i] = j; j += cw[i] + sep_w; }
    for(i = 0, j = y; i < nr; i++){ cy[i] = j; j += rh[i]; }

    /* Claim the placed cells first: an unplaced tile takes what is left. */
    for(t = wp->tiles; t; t = t->next){
        if(t->cell_row < 0) continue;
        for(i = t->cell_row; i < t->cell_row + t->cell_row_span; i++)
            for(j = t->cell_col; j < t->cell_col + t->cell_col_span; j++)
                if(i < nr && j < nc) taken[i][j] = true;
    }

    /* The rule between adjacent columns runs the height of the pane. */
    if(sep_w)
        for(i = 1; i < nc; i++){
            int sy;
            for(sy = y; sy < y + rows; sy++)
                draw_row_styled(f, buf, cx[i] - sep_w, sy, sep_w, wp->sep,
                                chrome);
        }

    /* Resolve every placement first: the chrome a tile reserves is what its
     * whole grid row reserves, so the row has to be known before it draws. */
    for(t = wp->tiles; t; t = t->next){
        int r, c, rs, cs;

        if(t->cell_row >= 0){
            r = t->cell_row; c = t->cell_col;
            rs = t->cell_row_span; cs = t->cell_col_span;
        }else{
            while(next < nr * nc && taken[next / nc][next % nc]) next++;
            if(next >= nr * nc){ tile_unplaced(t); continue; }
            r = next / nc; c = next % nc;
            rs = cs = 1;
            taken[r][c] = true;
            next++;
        }
        if(r >= nr || c >= nc || row_n[r] >= FYTIM_GRID_MAX){
            tile_unplaced(t);
            continue;
        }
        if(r + rs > nr) rs = nr - r;
        if(c + cs > nc) cs = nc - c;
        row_tiles[r][row_n[r]].t = t;
        row_tiles[r][row_n[r]].col = c;
        row_tiles[r][row_n[r]].row_span = rs;
        row_tiles[r][row_n[r]].col_span = cs;
        row_n[r]++;
    }

    for(i = 0; i < nr; i++){
        int shared_top = 0, shared_bottom = 0;

        for(j = 0; j < row_n[i]; j++){
            struct fytim_workband *rt = row_tiles[i][j].t;

            if(!rt->surface) continue;
            if(chrome_rows(rt->top) > shared_top)
                shared_top = chrome_rows(rt->top);
            if(chrome_rows(rt->bottom) > shared_bottom)
                shared_bottom = chrome_rows(rt->bottom);
        }
        for(j = 0; j < row_n[i]; j++){
            int c = row_tiles[i][j].col;
            int rs = row_tiles[i][j].row_span;
            int cs = row_tiles[i][j].col_span;
            int tw = 0, th = 0, k;

            for(k = 0; k < cs; k++) tw += cw[c + k];
            /* A span swallows the rules it crosses. */
            tw += (cs - 1) * sep_w;
            for(k = 0; k < rs; k++) th += rh[i + k];
            draw_tile(f, buf, wp, row_tiles[i][j].t, chrome, cx[c], cy[i],
                      tw, th, shared_top, shared_bottom);
        }
    }
}

static void draw_pane(TimuiFrame *f, TimuiCellBuffer *buf,
                      struct fytim_workpane *wp, TimuiStyle chrome,
                      int x, int y, int w, int rows)
{
    struct fytim_workband *arr[FYTIM_PANE_TILES_MAX];
    struct fytim_workband *t;
    int n = 0, cols, grows, sep_w, i, gr, gc;
    int base_h, extra_h, base_w, extra_w;

    if(wp->zoom){
        arr[n++] = wp->zoom;
    }else{
        for(t = wp->tiles; t && n < FYTIM_PANE_TILES_MAX; t = t->next)
            arr[n++] = t;
    }
    if(n < 1 || rows < 1 || w < 1) return;

    /* A host that declared an arrangement gets it; a zoomed tile is one
     * screen and needs none. */
    if(!wp->zoom && wp->grid_rows > 0){
        draw_pane_grid(f, buf, wp, chrome, x, y, w, rows);
        return;
    }

    pane_grid(wp, n, w, &cols, &grows);
    sep_w = (wp->sep && wp->sep[0]) ? sgr_disp_width(wp->sep) : 0;
    /* A separator that leaves no room for the screens is not drawn. */
    if(cols > 1 && w - (cols - 1) * sep_w < cols) sep_w = 0;

    base_h = rows / grows;
    extra_h = rows - base_h * grows;
    base_w = (w - (cols - 1) * sep_w) / cols;
    extra_w = (w - (cols - 1) * sep_w) - base_w * cols;

    for(i = 0, gr = 0; gr < grows; gr++){
        /* The spare rows go to the newest grid row, which is the one the
         * user is most likely watching. */
        int th = base_h + (gr >= grows - extra_h ? 1 : 0);
        int shared_top = 0, shared_bottom = 0;
        int last = i + cols;
        int tx = x;

        if(last > n) last = n;
        /* Surface chrome is a grid-row reservation.  Otherwise a command
         * that wraps in only one tile makes only that tile's PTY shorter;
         * a width remainder can then make the short PTY jump between tiles. */
        row_chrome(arr, i, last, &shared_top, &shared_bottom);
        for(gc = 0; gc < cols; gc++, i++){
            int tw = base_w + (gc < extra_w ? 1 : 0);

            if(gc && sep_w){
                int sy;
                for(sy = y; sy < y + th; sy++)
                    draw_row_styled(f, buf, tx, sy, sep_w, wp->sep, chrome);
                tx += sep_w;
            }
            if(i < n)
                draw_tile(f, buf, wp, arr[i], chrome, tx, y, tw, th,
                          shared_top, shared_bottom);
            tx += tw;
        }
        y += th;
    }
    for(; i < n; i++)
        tile_unplaced(arr[i]);
}

/* Rows the tail occupies: its '\n'-terminated rows, plus the trailing
 * partial row only when it holds VISIBLE text. The cursor row after a
 * final '\n' -- empty, or only an SGR carry-over residue -- must not
 * count: it is a phantom row with no matching commit, and every miscount
 * moves the bubble (the frame resize it forces has no commit to cancel
 * against). */
static bool sgr_probe_(void *user, const char *text, size_t len,
                       const struct fytim_sgr_style *style)
{
    (void)text; (void)len; (void)style;
    *(bool *)user = true;      /* a run was delivered: visible bytes exist */
    return false;
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

/* Rows a work-band wants: its content up to max_rows (at least one row, so
 * an idle band still shows), plus the optional top/bottom chrome rows. */
static int wb_rows(const struct fytim_workband *wb)
{
    int n;

    /* A pane solves its own geometry, chrome included, and an empty one
     * takes nothing: a region with no program in it is not a region. */
    if(wb->pane) return pane_rows(wb->pane);

    n = wb->surface ? surface_content_rows(wb->surface)
                    : styled_rows(wb->content);
    /* An idle band still shows: one row for it, unless it has chrome that
     * already says what it is. A screen always keeps a row of its own. */
    if(n < 1) n = (wb->surface || !(wb->top || wb->bottom)) ? 1 : 0;
    if(n > wb->max_rows) n = wb->max_rows;
    return n + chrome_rows(wb->top) + chrome_rows(wb->bottom);
}

/* Rows above the chrome: the transcript's live tail, then the work-bands;
 * one spare row when nothing is live -- the layout always reserves a
 * transcript row, and sizing the band to exactly the chrome would shed a
 * status row to pay for it. */
/*
 * The band that asked for the rows below the prompt, if there is one. It is
 * drawn after the chrome, so it is left out of the region the transcript and
 * the other bands share.
 */
static struct fytim_workband *wb_footer(const struct fytim *ft)
{
    struct fytim_workband *wb;

    for(wb = ft->wbands; wb; wb = wb->next)
        if(wb->pane && wb->pane->place == FYTIM_WORKPANE_BELOW_PROMPT)
            return wb;
    return NULL;
}

static int wb_footer_rows(const struct fytim *ft)
{
    const struct fytim_workband *wb = wb_footer(ft);

    return wb ? wb_rows(wb) : 0;
}

/*
 * The rows the band chrome is solved in. A pane below the prompt keeps the
 * last rows of the band for itself, and the chrome stands above them. It
 * never takes the whole band: the prompt outranks it.
 */
static int layout_height(const struct fytim *ft, int height)
{
    int foot = wb_footer_rows(ft);

    if(foot >= height) foot = 0;
    return height - foot;
}

static int wb_rows_total(const struct fytim *ft)
{
    const struct fytim_workband *foot = wb_footer(ft);
    const struct fytim_workband *wb;
    int n = styled_rows(ft->tail);
    for(wb = ft->wbands; wb; wb = wb->next)
        if(wb != foot) n += wb_rows(wb);
    return n > 0 ? n : 1;
}

/*
 * Rows the prompt asks for, and none at all while a surface holds the keys:
 * a prompt that cannot be typed into is a row taken from the program.
 */
static int prompt_lines(const struct fytim *ft)
{
    const char *marker = ft->marker ? ft->marker : "> ";
    /*
     * A tile that took the keys did not take the prompt: it is where the
     * user goes back to, and a row that is there but not lit says so. Only
     * a host that asked for no prompt has none, and one row is all a prompt
     * nobody is typing into needs.
     */
    if(ft->no_prompt) return 0;
    if(ft->keys) return 1;
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

    /*
     * Only a host that asked for no prompt takes these rows. Giving them to
     * a tile while it holds the keys would move everything on the screen the
     * moment the keys went to it, and give them back when they came home.
     */
    if(!ft->no_prompt) return;

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

/*
 * One band that holds a pane, in a region of @rows. The pane places its own
 * tiles; the band gives it the region and its own frame. Returns the rows
 * taken, which is @rows.
 */
static int draw_pane_band(TimuiFrame *f, TimuiCellBuffer *buf,
                          struct fytim_workband *wb, TimuiStyle wb_st,
                          int x, int y, int w, int rows)
{
    int top = chrome_rows(wb->top);
    int bottom = chrome_rows(wb->bottom);
    int content = rows - top - bottom;

    if(content < 1){ top = bottom = 0; content = rows; }
    if(top) y += draw_chrome(f, buf, x, y, w, wb->top, wb_st, top);
    draw_pane(f, buf, wb->pane, wb_st, x, y, w, content);
    y += content;
    if(bottom) draw_chrome(f, buf, x, y, w, wb->bottom, wb_st, bottom);
    return rows;
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
    /* The prompt is one filled card of three rows, rather than an editor
     * between two rules, when it was given a style or a ground of its own. */
    bool card = ft->prompt_style_set;
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
    /*
     * A ground under the prompt block replaces the style of its rows: it is
     * the same ground a focused tile stands on, so that the place the keys
     * are reads the same wherever they are. The marker is drawn on it and
     * keeps what it says, as the head of a tile does.
     */
    if(ft->prompt_bg != FYTIM_COLOR_DEFAULT && !ft->keys){
        struct ground g = { ft, ft->prompt_bg, 100 };

        in_st = ground_style_(&g, in_st);
        sep_st = in_st;
        card = true;
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
        struct fytim_workband *foot = wb_footer(ft);
        struct fytim_workband *wb;
        int nb = 0, i, y, avail, tl = styled_rows(ft->tail);
        for(wb = ft->wbands; wb; wb = wb->next) if(wb != foot) nb++;
        {
            struct fytim_workband *arr[nb > 0 ? nb : 1];
            int give[nb > 0 ? nb : 1];
            for(wb = ft->wbands, i = 0; wb; wb = wb->next)
                if(wb != foot) arr[i++] = wb;
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
                    ctx.origin_x = ctx.x;
                    ctx.ground = (struct ground){ NULL,
                                                  FYTIM_COLOR_DEFAULT, 0 };
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
                if(wb->pane){
                    y += draw_pane_band(f, buf, wb, wb_st, r->x, y, r->w,
                                        rows);
                    continue;
                }
                lines = wb->surface ? surface_content_rows(wb->surface)
                                    : styled_rows(wb->content);
                /* The band's own cap applies FIRST: past it, content shows
                 * its last max_rows lines and the chrome stays -- the rule
                 * and status row are what keep adjacent bands readable.
                 * Only a genuine row shortage (give < wb_rows) then sheds
                 * chrome: top rule first, bottom status next, content last. */
                content = lines;
                if(content < 1)
                    content = (wb->surface || !(wb->top || wb->bottom)) ?
                              1 : 0;
                if(content > wb->max_rows) content = wb->max_rows;
                top = chrome_rows(wb->top);
                bottom = chrome_rows(wb->bottom);
                /*
                 * A band sheds its chrome first, because its content is the
                 * report. A surface sheds content first: its chrome is the
                 * state row of a running program, and a screen one row
                 * shorter costs less than losing what the program is doing.
                 */
                while(top + content + bottom > rows){
                    if(wb->surface && content > 1) content--;
                    else if(top) top--;
                    else if(bottom) bottom = 0;
                    else content--;
                }
                if(top)
                    y += draw_chrome(f, buf, r->x, y, r->w, wb->top, wb_st,
                                     top);
                if(wb->surface){
                    if(content > 0)
                        draw_surface(f, buf, wb->surface, wb_st, r->x, y,
                                     r->w, content);
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
                    ctx.origin_x = ctx.x;
                    ctx.ground = (struct ground){ NULL,
                                                  FYTIM_COLOR_DEFAULT, 0 };
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
        if(card)
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
         * The editor is drawn only when the prompt has the keys: a focused
         * text area would eat the text the surface was just given. The row
         * still shows what was typed, so the user sees the line waiting for
         * them.
         */
        if(ft->keys && ft->input[0])
            draw_row_styled(f, buf, r->x + sgr_disp_width(marker), r->y,
                            r->w - sgr_disp_width(marker), ft->input, in_st);
        if(!ft->keys){
            timui_set_focus(f, id);   /* no focus model: the prompt owns keys */
            if(card)
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
        if(card)
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
    /*
     * A pane that asked for the rows below the prompt takes the last rows of
     * the band. The layout above was solved for what is left, so nothing it
     * placed reaches them.
     */
    {
        struct fytim_workband *foot = wb_footer(ft);
        int rows = foot ? wb_rows(foot) : 0;
        int h = timui_height(f), w = timui_width(f);

        if(rows > h) rows = h;
        if(foot && rows > 0)
            draw_pane_band(f, buf, foot, wb_st, 0, h - rows, w, rows);
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

static void surface_keys_emit(struct fytim *ft, struct key_out *k)
{
    char *dup;

    if(!k->len) return;
    dup = malloc(k->len + 1);
    if(!dup){ k->len = 0; return; }
    memcpy(dup, k->buf, k->len);
    dup[k->len] = '\0';
    ev_push(ft, FYTIM_EVENT_SURFACE_KEYS, dup, k->len, 0, 0);
    ft->evq[(ft->ev_head + ft->ev_n - 1) % FYTIM_EVQ_CAP].surface = ft->keys;
    k->len = 0;
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
        if(!rec.is_text && rec.key == TIMUI_KEY_UNKNOWN &&
           rec.codepoint == 't' &&
           (rec.mods & (TIMUI_MOD_CTRL | TIMUI_MOD_SHIFT)) ==
               (TIMUI_MOD_CTRL | TIMUI_MOD_SHIFT)){
            surface_keys_emit(ft, &k);
            ev_push(ft, FYTIM_EVENT_ZOOM_ROWS_NEXT, NULL, 0, 0, 0);
            continue;
        }
        if(!rec.is_text && rec.key == TIMUI_KEY_TAB &&
           (rec.mods & TIMUI_MOD_CTRL)){
            surface_keys_emit(ft, &k);
            ev_push(ft, FYTIM_EVENT_FOCUS_NEXT, NULL, 0, 0, 0);
            continue;
        }
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

    surface_keys_emit(ft, &k);
}

/* ---- the mouse on a tile ------------------------------------------------ */

static void ev_push_surface(struct fytim *ft, enum fytim_event_type type,
                            struct fytim_surface *sf, int delta)
{
    struct fytim_event *ev;

    ev_push(ft, type, NULL, 0, 0, 0);
    ev = &ft->evq[(ft->ev_head + ft->ev_n - 1) % FYTIM_EVQ_CAP];
    ev->surface = sf;
    ev->delta = delta;
}

/* The tile drawn over (@x, @y) at the last frame, or NULL. */
static struct fytim_surface *tile_at(struct fytim *ft, int x, int y)
{
    struct fytim_workband *wb, *t;
    struct fytim_surface *sf;

    for(wb = ft->wbands; wb; wb = wb->next){
        if(!wb->pane || !pane_controls_live(wb->pane)) continue;
        for(t = wb->pane->tiles; t; t = t->next){
            sf = t->surface;
            if(!sf || sf->rect_w < 1 || sf->rect_h < 1) continue;
            if(x >= sf->rect_x && x < sf->rect_x + sf->rect_w &&
               y >= sf->rect_y && y < sf->rect_y + sf->rect_h)
                return sf;
        }
    }
    return NULL;
}

/*
 * Give a click or a wheel turn to the tile under it. Returns true when the
 * wheel was a tile's, so that it does not also reach the transcript: the
 * user was pointing at one screen and meant that one.
 */
static bool pane_mouse(struct fytim *ft, TimuiFrame *f)
{
    struct fytim_surface *sf;
    int x = 0, y = 0, wheel;
    bool took = false;

    if(!ft->mouse) return false;

    if(timui_mouse_clicked(f, &x, &y)){
        sf = tile_at(ft, x, y);
        if(sf){
            unsigned int ctl = sf->wb->in_pane->controls;
            if(y == sf->ctl_y && sf->close_x >= 0 && x == sf->close_x){
                ev_push_surface(ft, FYTIM_EVENT_SURFACE_CLOSE, sf, 0);
            }else if(y == sf->ctl_y && sf->zoom_x >= 0 && x == sf->zoom_x){
                ev_push_surface(ft, FYTIM_EVENT_SURFACE_ZOOM, sf, 0);
            }else if(sf->bar_x >= 0 && x == sf->bar_x){
                /* On the bar: the ends step a row when they are arrows, and
                 * the track pages toward where the user pointed. */
                int top = sf->rect_y, bot = sf->rect_y + sf->rect_h - 1;
                int page = sf->granted > 0 ? sf->granted : 1;
                if((ctl & FYTIM_WORKPANE_ARROWS) && y == top)
                    ev_push_surface(ft, FYTIM_EVENT_SURFACE_SCROLL, sf, 1);
                else if((ctl & FYTIM_WORKPANE_ARROWS) && y == bot)
                    ev_push_surface(ft, FYTIM_EVENT_SURFACE_SCROLL, sf, -1);
                else if(y < top + sf->rect_h / 2)
                    ev_push_surface(ft, FYTIM_EVENT_SURFACE_SCROLL, sf, page);
                else
                    ev_push_surface(ft, FYTIM_EVENT_SURFACE_SCROLL, sf, -page);
            }
        }
    }

    wheel = timui_mouse_wheel(f);
    if(wheel){
        int down = 0;
        timui_mouse_state(f, &x, &y, &down);
        sf = tile_at(ft, x, y);
        if(sf){
            ev_push_surface(ft, FYTIM_EVENT_SURFACE_SCROLL, sf, wheel);
            took = true;
        }
    }
    return took;
}

/* ---- the pump ----------------------------------------------------------- */

enum fytim_result fytim_pump(struct fytim *ft)
{
    TimuiFrame *f = NULL;
    struct fytim_layout lay;
    bool submitted = false;
    bool resized = false;
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
            resized = true;
            ev_push(ft, FYTIM_EVENT_RESIZE, NULL, 0, nw, nh);
        }
        /*
         * With no prompt the separators that frame it go too, so the band
         * asks only for what is left: the header, the status and the work
         * bands. The layout drops an empty header or status after that.
         */
        if(prompt_lines(ft) > 0)
            want = FYTIM_CHROME_ROWS + wb_rows_total(ft) +
                   (prompt_lines(ft) - 1);
        else
            want = FYTIM_HEADER_ROWS + FYTIM_STATUS_ROWS + wb_rows_total(ft);
        want += wb_footer_rows(ft);
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
                /* Resize replaces both retained frame buffers. Invalidate
                 * the replacement, not the buffer that was just discarded:
                 * the terminal may have rewrapped any cell, blanks included. */
                timui_full_redraw(ft->ui);
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
        if(fytim_layout_compute_ex(timui_width(f),
                                   layout_height(ft, timui_height(f)),
                                   prompt_lines(ft), &lay)){
            layout_drop_empty_chrome(ft, &lay);
            draw_band(ft, f, &lay, &submitted);
        }
        timui_end(f);
        /* The host applies FYTIM_EVENT_RESIZE after this pump. The frame just
         * drawn still used its old surfaces, and can itself have wrapped on
         * the new physical width. Force the host-updated next frame too. */
        if(resized) timui_full_redraw(ft->ui);
        return FYTIM_OK;
    }

    if(timui_key_pressed(f, TIMUI_KEY_ESCAPE))
        ev_push(ft, FYTIM_EVENT_INTERRUPT, NULL, 0, 0, 0);
    if(!pane_mouse(ft, f) &&
       (timui_mouse_wheel(f) ||
        timui_key_pressed(f, TIMUI_KEY_PAGE_UP) ||
        timui_key_pressed(f, TIMUI_KEY_PAGE_DOWN)))
        ev_push(ft, FYTIM_EVENT_SCROLLBACK, NULL, 0, 0, 0);
    {
        int ctrl = timui_key_pressed_mods(f, TIMUI_KEY_UNKNOWN, TIMUI_MOD_CTRL);
        uint32_t cp = timui_key_codepoint(f);
        bool zoom_rows_next = ctrl && cp == 't' &&
            timui_key_pressed_mods(f, TIMUI_KEY_UNKNOWN,
                                   TIMUI_MOD_CTRL | TIMUI_MOD_SHIFT);
        bool focus_next = (!zoom_rows_next && ctrl && cp == 't') ||
            timui_key_pressed_mods(f, TIMUI_KEY_TAB, TIMUI_MOD_CTRL);
        if(ctrl && cp == 'c')
            ev_push(ft, FYTIM_EVENT_INTERRUPT, NULL, 0, 0, 0);
        if(ctrl && cp == 'd' && !ft->input[0])
            ev_push(ft, FYTIM_EVENT_QUIT, NULL, 0, 0, 0);
        if(ctrl && cp == 'l'){
            timui_full_redraw(ft->ui);
            ev_push(ft, FYTIM_EVENT_REDRAW, NULL, 0, 0, 0);
        }
        if(ctrl && cp == 'g')
            ev_push(ft, FYTIM_EVENT_EDIT, NULL, 0, 0, 0);
        if(focus_next)
            ev_push(ft, FYTIM_EVENT_FOCUS_NEXT, NULL, 0, 0, 0);
        if(zoom_rows_next)
            ev_push(ft, FYTIM_EVENT_ZOOM_ROWS_NEXT, NULL, 0, 0, 0);
        if(!focus_next && timui_key_pressed(f, TIMUI_KEY_TAB))
            complete_tab(ft);
        else if(!focus_next && (timui_text_input(f).len > 0 ||
                timui_key_pressed(f, TIMUI_KEY_ENTER)))
            complete_leave(ft);          /* typing accepts and exits */
        if((ctrl && cp == 'p') ||
           (timui_key_pressed(f, TIMUI_KEY_UP) && cursor_on_first_line(ft)))
            hist_prev(ft);
        else if((ctrl && cp == 'n') ||
                (timui_key_pressed(f, TIMUI_KEY_DOWN) && cursor_on_last_line(ft)))
            hist_next(ft);
    }

    if(fytim_layout_compute_ex(timui_width(f),
                               layout_height(ft, timui_height(f)),
                               prompt_lines(ft), &lay)){
        layout_drop_empty_chrome(ft, &lay);
        draw_band(ft, f, &lay, &submitted);
    }

    if(submitted){
        char *text = strdup(ft->input);
        if(text)
            ev_push(ft, FYTIM_EVENT_LINE, text, strlen(text), 0, 0);
        input_load(ft, NULL);
        free(ft->draft);
        ft->draft = NULL;
    }

    timui_end(f);
    if(resized) timui_full_redraw(ft->ui);
    return FYTIM_OK;
}
