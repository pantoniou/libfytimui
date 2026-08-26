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
#include <libfytimui/libfytimui-surface.h>

#include <stdbool.h>
#include <stddef.h>

enum fytim_event_type {
    FYTIM_EVENT_NONE = 0,
    FYTIM_EVENT_LINE,        /* the user submitted an input line */
    FYTIM_EVENT_INTERRUPT,   /* Escape/^C: cancel outstanding work */
    FYTIM_EVENT_QUIT,        /* ^D on an empty line, or a quit request */
    FYTIM_EVENT_RESIZE,
    FYTIM_EVENT_SCROLLBACK, /* wheel/PageUp/PageDown reached the application */
    FYTIM_EVENT_PANE_SELECT, /* the user selected a pane to expand */
    FYTIM_EVENT_EDIT,        /* ^G: the user asked for an external editor;
                                the host runs it between fytim_suspend and
                                fytim_resume, then fytim_set_input */
    FYTIM_EVENT_SURFACE_KEYS, /* keys for the surface holding them, already
                                 encoded as the bytes a terminal would send */

    /* A tile of a work pane was operated with the mouse. The library owns
     * no scrollback and moves nothing itself: it says what was asked for,
     * and the host publishes the rows it wants seen. Each names the tile in
     * @surface, and the scroll carries its distance in @delta - positive is
     * back through the history, negative is toward the live screen. */
    FYTIM_EVENT_SURFACE_SCROLL,
    FYTIM_EVENT_SURFACE_ZOOM,   /* the user asked to zoom or unzoom it */
    FYTIM_EVENT_SURFACE_CLOSE   /* the user asked to be rid of it */
};

struct fytim_event {
    enum fytim_event_type type;

    /* FYTIM_EVENT_LINE: the submitted text, valid until the next
     * fytim_next_event or fytim_pump on the same instance. */
    const char *text;
    size_t      text_len;

    /* FYTIM_EVENT_PANE_SELECT: the pane concerned. */
    struct fytim_pane *pane;

    /* FYTIM_EVENT_SURFACE_KEYS: the surface holding the keys. The bytes are
     * in text/text_len, with the same lifetime. */
    struct fytim_surface *surface;

    /* FYTIM_EVENT_RESIZE: the new terminal geometry, in cells. */
    int width;
    int height;

    /* FYTIM_EVENT_SURFACE_SCROLL: rows asked for, back through the history
     * when positive and toward the live screen when negative. */
    int delta;
};

/* Pop one event. Returns false when the queue is empty. */
bool fytim_next_event(struct fytim *ft, struct fytim_event *out) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_EVENT_H */
