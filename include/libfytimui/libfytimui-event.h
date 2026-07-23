/*
 * libfytimui-event.h - events drained by the host loop.
 *
 * A pull queue rather than callbacks: the host already owns polling, so it
 * drains events at a point of its choosing instead of having control
 * inverted into a callback during fytim_pump.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_EVENT_H
#define LIBFYTIMUI_EVENT_H

#include <libfytimui/libfytimui-util.h>
#include <libfytimui/libfytimui-pane.h>

#include <stdbool.h>
#include <stddef.h>

enum fytim_event_type {
    FYTIM_EVENT_NONE = 0,
    FYTIM_EVENT_LINE,        /* the user submitted an input line */
    FYTIM_EVENT_INTERRUPT,   /* ^C: cancel outstanding work */
    FYTIM_EVENT_QUIT,        /* ^D on an empty line, or a quit request */
    FYTIM_EVENT_RESIZE,
    FYTIM_EVENT_SCROLLBACK, /* wheel/PageUp/PageDown reached the application */
    FYTIM_EVENT_PANE_SELECT, /* the user selected a pane to expand */
    FYTIM_EVENT_EDIT         /* ^G: the user asked for an external editor;
                                the host runs it between fytim_suspend and
                                fytim_resume, then fytim_set_input */
};

struct fytim_event {
    enum fytim_event_type type;

    /* FYTIM_EVENT_LINE: the submitted text, valid until the next
     * fytim_next_event or fytim_pump on the same instance. */
    const char *text;
    size_t      text_len;

    /* FYTIM_EVENT_PANE_SELECT: the pane concerned. */
    struct fytim_pane *pane;

    /* FYTIM_EVENT_RESIZE: the new terminal geometry, in cells. */
    int width;
    int height;
};

/* Pop one event. Returns false when the queue is empty. */
bool fytim_next_event(struct fytim *ft, struct fytim_event *out) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_EVENT_H */
