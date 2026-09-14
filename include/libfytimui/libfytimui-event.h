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
    FYTIM_EVENT_SCROLLBACK, /* wheel/PageUp/PageDown reached the application.
                               A turn of the wheel over a region of the page
                               names it in text/text_len, with the lifetime of
                               FYTIM_EVENT_LINE text: the last region that
                               holds the cell. A key names none. */
    FYTIM_EVENT_PANE_SELECT, /* the user selected a pane to expand */
    FYTIM_EVENT_EDIT,        /* ^G: the user asked for an external editor;
                                the host runs it between fytim_suspend and
                                fytim_resume, then fytim_set_input */
    FYTIM_EVENT_FOCUS_NEXT,  /* ^T or Kitty Ctrl-Tab: move keyboard focus */
    FYTIM_EVENT_ZOOM_ROWS_NEXT, /* Kitty Ctrl-Shift-T: cycle pane height */
    FYTIM_EVENT_SURFACE_KEYS, /* keys for the surface holding them, already
                                 encoded as the bytes a terminal would send */
    FYTIM_EVENT_REDRAW,      /* ^L: the user asked for a clean screen. The
                                library repaints its own band; a host that
                                keeps the source of what it committed can
                                make those rows again too */

    /* A tile of a work pane was operated with the mouse. The library owns
     * no scrollback and moves nothing itself: it says what was asked for,
     * and the host publishes the rows it wants seen. Each names the tile in
     * @surface, and the scroll carries its distance in @delta - positive is
     * back through the history, negative is toward the live screen. */
    FYTIM_EVENT_SURFACE_SCROLL,
    FYTIM_EVENT_SURFACE_ZOOM,   /* the user asked to zoom or unzoom it */
    FYTIM_EVENT_SURFACE_CLOSE,  /* the user asked to be rid of it */
    /* A click on the head of a tile, off its controls. @row and @col are
     * the cell of the head text that was clicked, from the first cell after
     * the margin of the tile: the host that wrote the head knows what is
     * there. */
    FYTIM_EVENT_SURFACE_CLICK,
    /* A click on an act region of the page. The id is in text/text_len,
     * with the lifetime of FYTIM_EVENT_LINE text. */
    FYTIM_EVENT_ACT,
    /* A key that the host bound with fytim_set_key_bindings, taken from the
     * prompt. Its name, as bound, is in text/text_len, with the lifetime of
     * FYTIM_EVENT_LINE text. */
    FYTIM_EVENT_KEY,
    /* A drag over a text region of the page ended. The id of the region is
     * in text/text_len, with the lifetime of FYTIM_EVENT_LINE text; @row and
     * @col are the cell it started on and @end_row and @end_col the cell it
     * ended on, counted from the region. The host knows the text there and
     * copies it with fytim_copy(). */
    FYTIM_EVENT_SELECT
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

    /* FYTIM_EVENT_SURFACE_SCROLL and FYTIM_EVENT_SCROLLBACK: rows asked
     * for, back through the history when positive and toward the live screen
     * when negative. A turn of the wheel is three rows, and a page is the
     * rows of the terminal less one. */
    int delta;

    /* FYTIM_EVENT_SURFACE_CLICK: the cell of the head text.
     * FYTIM_EVENT_SELECT: the cell the selection started on. */
    int row;
    int col;

    /* FYTIM_EVENT_SELECT: the cell the selection ended on. */
    int end_row;
    int end_col;
};

/* Pop one event. Returns false when the queue is empty. */
bool fytim_next_event(struct fytim *ft, struct fytim_event *out) FYTIM_EXPORT;

/*
 * Put the @len bytes of @text on the clipboard of the terminal with OSC 52.
 * FYTIM_ERR_UNSUPPORTED unless fytim_cfg.clipboard is set; FYTIM_ERR_INVALID
 * for no text.
 */
enum fytim_result fytim_copy(struct fytim *ft, const char *text,
                             size_t len) FYTIM_EXPORT;

/* Clear the selection the library draws over a text region, as a host does
 * when the text under it moves. */
void fytim_selection_clear(struct fytim *ft) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_EVENT_H */
