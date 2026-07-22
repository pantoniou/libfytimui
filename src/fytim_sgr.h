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

enum {
    FYTIM_ATTR_BOLD      = 1u << 0,
    FYTIM_ATTR_DIM       = 1u << 1,
    FYTIM_ATTR_ITALIC    = 1u << 2,
    FYTIM_ATTR_UNDERLINE = 1u << 3,
    FYTIM_ATTR_REVERSE   = 1u << 4,
    FYTIM_ATTR_STRIKE    = 1u << 5
};

/* Colours are 0xRRGGBB with FYTIM_COLOR_DEFAULT meaning "terminal default".
 * Indexed (16/256) colours are kept as an index with FYTIM_COLOR_INDEXED set,
 * so the backend can map them to the active palette rather than guessing RGB
 * here. */
#define FYTIM_COLOR_DEFAULT  0xFF000000u
#define FYTIM_COLOR_INDEXED  0x01000000u

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
