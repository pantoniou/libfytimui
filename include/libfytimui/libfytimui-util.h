/*
 * libfytimui-util.h - export macros, result codes, version.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_UTIL_H
#define LIBFYTIMUI_UTIL_H

#include <stddef.h>

#ifndef FYTIM_EXPORT
  #if defined(_WIN32)
    #if defined(FYTIM_BUILDING_DLL)
      #define FYTIM_EXPORT __declspec(dllexport)
    #elif defined(FYTIM_USING_DLL)
      #define FYTIM_EXPORT __declspec(dllimport)
    #else
      #define FYTIM_EXPORT
    #endif
  #elif defined(__GNUC__) || defined(__clang__)
    #define FYTIM_EXPORT __attribute__((visibility("default")))
  #else
    #define FYTIM_EXPORT
  #endif
#endif

/* Negative on failure, so callers can test < 0 uniformly. */
enum fytim_result {
    FYTIM_OK              =  0,
    FYTIM_ERR_INVALID     = -1,   /* bad argument */
    FYTIM_ERR_NOMEM       = -2,
    FYTIM_ERR_IO          = -3,
    FYTIM_ERR_CLOSED      = -4,   /* the UI has shut down */
    FYTIM_ERR_UNSUPPORTED = -5
};

const char *fytim_result_string(enum fytim_result r) FYTIM_EXPORT;
const char *fytim_version_string(void) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_UTIL_H */
