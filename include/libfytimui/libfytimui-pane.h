/*
 * libfytimui-pane.h - lifecycle, host-owned loop, panes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_PANE_H
#define LIBFYTIMUI_PANE_H

#include <libfytimui/libfytimui-util.h>

#include <stdbool.h>
#include <stddef.h>

struct fytim;        /* opaque UI instance */
struct fytim_pane;   /* opaque pane; owned by the fytim it was opened on */

struct fytim_cfg {
    size_t struct_size;      /* sizeof(struct fytim_cfg); forward-compat guard */
    int    input_fd;         /* terminal input; -1 selects stdin */
    int    output_fd;        /* terminal output; -1 selects stdout */
    const char *title;       /* window/terminal title; may be NULL */
    bool   mouse;            /* grab the mouse. Off by default, and the
                                default is the right answer for almost every
                                host: without the grab, selection and copy
                                stay with the terminal, which is where the
                                user expects them. A host asks for it only to
                                give a work pane its controls (scroll bars and
                                the marks that zoom or close a tile), and the
                                grab then lasts as long as the UI does. */
    bool   clipboard;        /* OSC 52 copy out of a pane (pane model) */
    int    workband_rows;    /* default max content rows per work-band;
                                0 selects the default (4) */
    bool   intr_signal;      /* leave ^C generating SIGINT instead of
                                delivering it as input. FYTIM_EVENT_INTERRUPT
                                then fires only for Escape, and the host must
                                handle SIGINT itself -- which is the point: a
                                host whose loop is wedged cannot read ^C,
                                because reading it needs that same loop. ^\
                                and ^Z stay application keys. */
};

/* Fill cfg with defaults (stdin/stdout). */
void fytim_cfg_default(struct fytim_cfg *cfg) FYTIM_EXPORT;

struct fytim *fytim_create(const struct fytim_cfg *cfg) FYTIM_EXPORT;
void          fytim_destroy(struct fytim *ft) FYTIM_EXPORT;

/* The terminal geometry in cells, as last sampled (see FYTIM_EVENT_RESIZE
 * for changes). Hosts size their markdown renderer to this width so that
 * committed lines arrive hard-wrapped and never soft-wrap. Either out
 * pointer may be NULL. */
enum fytim_result fytim_size(const struct fytim *ft, int *w, int *h) FYTIM_EXPORT;

/* Whether this instance grabbed the mouse (see fytim_cfg.mouse). A work
 * pane draws no control the user could not reach. */
bool fytim_mouse_enabled(const struct fytim *ft) FYTIM_EXPORT;

/* ---- host-owned event loop --------------------------------------------- */

/* The descriptor the host should add to its own poll set, or -1 if there is
 * nothing pollable. */
int fytim_poll_fd(const struct fytim *ft) FYTIM_EXPORT;

/* Maximum milliseconds the host may block before calling fytim_pump again and
 * still have animation and escape-sequence timeouts advance correctly. */
int fytim_poll_timeout_ms(const struct fytim *ft) FYTIM_EXPORT;

/* Drain pending input, update state, repaint if anything changed. Never
 * blocks: the host's loop owns waiting. Call when poll reports the descriptor
 * readable, or when the timeout above expires. */
enum fytim_result fytim_pump(struct fytim *ft) FYTIM_EXPORT;

/* ---- panes -------------------------------------------------------------- */

/* A pane's lifecycle state drives its default presentation (spinner while
 * running, collapsed on success, retained and marked on failure). */
enum fytim_pane_state {
    FYTIM_PANE_RUNNING = 0,
    FYTIM_PANE_DONE,
    FYTIM_PANE_FAILED,
    FYTIM_PANE_CANCELLED
};

/* The transcript pane exists for the lifetime of the UI and is where ordered
 * conversation output goes. Task/agent panes are opened and closed around
 * concurrent work. */
struct fytim_pane *fytim_transcript(struct fytim *ft) FYTIM_EXPORT;
struct fytim_pane *fytim_pane_open(struct fytim *ft, const char *title) FYTIM_EXPORT;
enum fytim_result  fytim_pane_close(struct fytim_pane *p) FYTIM_EXPORT;

enum fytim_result fytim_pane_set_title(struct fytim_pane *p, const char *title) FYTIM_EXPORT;
enum fytim_result fytim_pane_set_state(struct fytim_pane *p, enum fytim_pane_state st) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_PANE_H */
