/*
 * fytim_sgr.h - SGR escape stream -> styled text runs (internal).
 *
 * libfymd4c emits Markdown as text carrying SGR styling escapes only; cursor,
 * erase, and screen-mode control belongs to the compositor. This splits such a
 * stream into runs of plain text each carrying a resolved style, so a pane can
 * be converted to cells once and retained rather than re-parsed per frame.
 *
 * Non-SGR CSI sequences are recognised so they can be skipped rather than
 * leaking their bytes into the text, and are reported as disallowed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FYTIM_SGR_H
#define FYTIM_SGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The colours and attributes are the public ones: content given as SGR text
 * and content given as cells must mean the same thing by them. */
#include <libfytimui/libfytimui-style.h>

struct fytim_sgr_style {
    uint32_t fg;
    uint32_t bg;
    uint32_t attrs;
};

/* Called for each run of plain text sharing one style. text is not NUL
 * terminated. Return false to stop parsing. */
typedef bool (*fytim_sgr_run_fn)(void *user, const char *text, size_t len,
                                 const struct fytim_sgr_style *style);

struct fytim_sgr_parser {
    struct fytim_sgr_style style;
    /* Set when input contained a non-SGR control sequence (cursor movement,
     * erase, screen mode). Those are not permitted in pane content. */
    bool disallowed_seen;
    /* Carry for an escape sequence split across feeds. */
    char   pending[64];
    size_t pending_len;
    /*
     * An OSC carries a payload of arbitrary length - an OSC 8 hyperlink holds a
     * whole URL - so it cannot be carried in a bounded buffer. Once its
     * introducer has been seen it is tracked as state instead: skip bytes until
     * the terminator arrives, however many feeds that takes.
     */
    bool   in_osc;
    bool   osc_is_link;
    bool   osc_saw_esc;      /* the ESC of a possible ST split across feeds */
    unsigned osc_seen;       /* bytes seen, to classify OSC 8 across a split */
};

void fytim_sgr_init(struct fytim_sgr_parser *p);

/* Feed bytes; runs are delivered via cb. Safe across arbitrary chunk
 * boundaries, including an escape sequence split mid-way. */
void fytim_sgr_feed(struct fytim_sgr_parser *p, const char *buf, size_t len,
                    fytim_sgr_run_fn cb, void *user);

/* Write the escape sequence that re-opens `style` from the default state
 * into out (NUL-terminated). Returns the length, 0 when the style IS the
 * default (nothing to re-open) or when cap cannot hold it. 64 bytes always
 * suffice. Used to keep SGR carry-over alive across per-row resets. */
size_t fytim_sgr_style_emit(const struct fytim_sgr_style *style,
                            char *out, size_t cap);

#endif /* FYTIM_SGR_H */
