/*
 * libfytimui-style.h - colours and attributes shared by content and cells.
 *
 * Styled content reaches the library two ways: as text carrying SGR escapes,
 * and as cells a host publishes to a surface. Both name the same colours and
 * the same attributes, so they are defined once, here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_STYLE_H
#define LIBFYTIMUI_STYLE_H

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
 * so the backend maps them to the active palette rather than guessing RGB:
 * the theme of the user then applies, which is the point of an index. */
#define FYTIM_COLOR_DEFAULT  0xFF000000u
#define FYTIM_COLOR_INDEXED  0x01000000u

#endif /* LIBFYTIMUI_STYLE_H */
