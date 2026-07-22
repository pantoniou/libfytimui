/*
 * test_external_poll.c — external-poll mode: the host owns poll()/select().
 *
 * When an embedding application (e.g. an existing event loop) already polls a
 * set of descriptors, timui must not perform its own blocking wait inside
 * timui_begin. These tests assert that property deterministically via an
 * input-wait counter rather than a wall-clock measurement, so they cannot go
 * flaky under load.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "timui.h"

#include <unistd.h>

/* Open a Timui over a pipe: a real fd, but not a tty, so termios_active is 0
 * and the non-tty throttle path is the one under test. */
static TimuiResult open_over_pipe_(Timui **out_ui, uint32_t extra_flags, int *rfd, int *wfd)
{
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    int fds[2];
    if(pipe(fds) != 0) return TIMUI_ERR_IO;
    *rfd = fds[0];
    *wfd = fds[1];
    cfg.input_fd = fds[0];
    cfg.output_fd = fds[1];
    cfg.flags |= extra_flags;
    return timui_open(&cfg, out_ui);
}

/* Baseline (negative control): without the flag, a begin with no input pending
 * performs exactly one internal wait. This pins the default behavior so the new
 * flag is proven to be the thing that changes it. */
TIMUI_TEST(test_default_mode_waits_for_input){
    Timui *ui = NULL;
    TimuiFrame *f = NULL;
    int rfd = -1, wfd = -1;
    if(open_over_pipe_(&ui, 0, &rfd, &wfd) != TIMUI_OK){
        if(rfd >= 0) close(rfd);
        if(wfd >= 0) close(wfd);
        return;   /* environment cannot open a terminal-less UI; nothing to assert */
    }
    TIMUI_CHECK(timui_input_waits_for_test(ui) == 0);
    if(timui_begin_result(ui, &f) == TIMUI_OK) timui_end(f);
    TIMUI_CHECK(timui_input_waits_for_test(ui) == 1);
    timui_close(ui);
    close(rfd);
    close(wfd);
}

/* Positive: with external-poll mode the frame never waits internally. */
TIMUI_TEST(test_external_poll_never_waits){
    Timui *ui = NULL;
    int rfd = -1, wfd = -1;
    int i;
    if(open_over_pipe_(&ui, TIMUI_FLAG_EXTERNAL_POLL, &rfd, &wfd) != TIMUI_OK){
        if(rfd >= 0) close(rfd);
        if(wfd >= 0) close(wfd);
        return;
    }
    for(i = 0; i < 8; ++i){
        TimuiFrame *f = NULL;
        if(timui_begin_result(ui, &f) == TIMUI_OK) timui_end(f);
    }
    TIMUI_CHECK(timui_input_waits_for_test(ui) == 0);
    timui_close(ui);
    close(rfd);
    close(wfd);
}

/* External-poll mode must still consume input that the host's poll reported as
 * readable — skipping the wait must not skip the read. */
TIMUI_TEST(test_external_poll_still_reads_ready_input){
    Timui *ui = NULL;
    TimuiFrame *f = NULL;
    int rfd = -1, wfd = -1;
    if(open_over_pipe_(&ui, TIMUI_FLAG_EXTERNAL_POLL, &rfd, &wfd) != TIMUI_OK){
        if(rfd >= 0) close(rfd);
        if(wfd >= 0) close(wfd);
        return;
    }
    TIMUI_CHECK(write(wfd, "q", 1) == 1);
    if(timui_begin_result(ui, &f) == TIMUI_OK){
        TIMUI_CHECK(timui_text_input(f).len == 1);
        timui_end(f);
    }
    TIMUI_CHECK(timui_input_waits_for_test(ui) == 0);
    timui_close(ui);
    close(rfd);
    close(wfd);
}

/* The host needs the descriptor and the cadence hint to add timui to its own
 * poll set. Negative cases: NULL must not crash and must report "nothing to
 * poll" rather than a plausible-looking fd 0. */
TIMUI_TEST(test_external_poll_exposes_fd_and_timeout){
    Timui *ui = NULL;
    int rfd = -1, wfd = -1;
    TIMUI_CHECK(timui_poll_fd(NULL) == -1);
    TIMUI_CHECK(timui_poll_timeout_ms(NULL) == -1);
    if(open_over_pipe_(&ui, TIMUI_FLAG_EXTERNAL_POLL, &rfd, &wfd) != TIMUI_OK){
        if(rfd >= 0) close(rfd);
        if(wfd >= 0) close(wfd);
        return;
    }
    TIMUI_CHECK(timui_poll_fd(ui) == rfd);
    TIMUI_CHECK(timui_poll_timeout_ms(ui) >= 0);
    timui_close(ui);
    close(rfd);
    close(wfd);
}
