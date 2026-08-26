/*
 * libfytimui.h - pane-oriented terminal UI for coding harnesses.
 *
 * A minimal, fully opaque surface over a vendored timui core. The core's
 * types and symbols are an implementation detail: nothing from timui.h
 * appears here, and the core is linked with hidden visibility so timui_*
 * does not reach this library's ABI either.
 *
 * The model is deliberately small. The host does not draw; it publishes
 * panes and their content, drains events, and drives the loop. Layout,
 * cursor management, terminal capabilities, scrolling, and repainting are
 * owned by the library.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_H
#define LIBFYTIMUI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libfytimui/libfytimui-util.h>
#include <libfytimui/libfytimui-layout.h>
#include <libfytimui/libfytimui-pane.h>
#include <libfytimui/libfytimui-style.h>
#include <libfytimui/libfytimui-band.h>
#include <libfytimui/libfytimui-surface.h>
#include <libfytimui/libfytimui-workpane.h>
#include <libfytimui/libfytimui-event.h>

#ifdef __cplusplus
}
#endif

#endif /* LIBFYTIMUI_H */
