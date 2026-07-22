/*
 * test_mouse_drag.c - drag tracking and transport access.
 *
 * Covers two local-only core deltas (docs/vendor-deltas.md):
 *
 *   - TIMUI_FLAG_MOUSE_DRAG, which adds ?1002 button-event tracking so an
 *     application can implement its own selection. Plain TIMUI_FLAG_MOUSE
 *     emits ?1000 only, which reports press/release but never motion, so a
 *     drag cannot be followed.
 *   - timui_transport(), which exposes the transport a Timui already owns so
 *     timui_clipboard_set (OSC 52) is reachable without opening one manually.
 *
 * Kept out of the upstream test files on purpose: a re-vendor overwrites
 * those, and these cases must survive it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "timui.h"

#include <string.h>
#include <unistd.h>

static int contains_(TimuiStr s, const char *needle){
    size_t n = strlen(needle);
    size_t i;
    if(n == 0) return 1;
    if(s.len < n) return 0;
    for(i = 0; i <= s.len - n; i++)
        if(memcmp(s.ptr + i, needle, n) == 0) return 1;
    return 0;
}

/* Negative control: plain mouse mode must NOT ask for motion reporting. This
 * pins the pre-existing behaviour, so the new flag is proven to be the thing
 * that changes it rather than something already true. */
TIMUI_TEST(test_mouse_without_drag_emits_no_1002){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_MOUSE, TIMUI_STR_LIT(""));
    out = timui_fake_output(&f);
    TIMUI_CHECK(contains_(out, "\x1b[?1000h"));
    TIMUI_CHECK(contains_(out, "\x1b[?1006h"));
    TIMUI_CHECK(!contains_(out, "\x1b[?1002h"));
    timui_fake_destroy(&f);
}

/* Positive: the drag flag adds ?1002 on top of the ordinary mouse modes. */
TIMUI_TEST(test_mouse_drag_emits_1002){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_MOUSE | TIMUI_FLAG_MOUSE_DRAG,
                       TIMUI_STR_LIT(""));
    out = timui_fake_output(&f);
    TIMUI_CHECK(contains_(out, "\x1b[?1000h"));
    TIMUI_CHECK(contains_(out, "\x1b[?1002h"));
    TIMUI_CHECK(contains_(out, "\x1b[?1006h"));
    timui_fake_destroy(&f);
}

/* Drag tracking must be turned off again on the way out, or it leaks into
 * the user's shell and breaks native selection after exit. */
TIMUI_TEST(test_mouse_drag_disabled_on_exit){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_MOUSE | TIMUI_FLAG_MOUSE_DRAG,
                       TIMUI_STR_LIT(""));
    timui_fake_clear_output(&f);
    timui_screen_exit(&t, &m);
    out = timui_fake_output(&f);
    TIMUI_CHECK(contains_(out, "\x1b[?1002l"));
    TIMUI_CHECK(contains_(out, "\x1b[?1000l"));
    timui_fake_destroy(&f);
}

/* The drag flag alone (no TIMUI_FLAG_MOUSE) must not enable tracking: ?1002
 * without ?1000/?1006 is a half-configured mouse whose reports nothing
 * decodes. */
TIMUI_TEST(test_mouse_drag_alone_is_inert){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_MOUSE_DRAG, TIMUI_STR_LIT(""));
    out = timui_fake_output(&f);
    TIMUI_CHECK(!contains_(out, "\x1b[?1002h"));
    TIMUI_CHECK(!contains_(out, "\x1b[?1000h"));
    timui_fake_destroy(&f);
}

/* timui_transport returns the live transport, and tolerates a NULL ui.
 *
 * Known gap: the !have_transport branch is not covered -- the public API has
 * no way to open a Timui without a transport, so removing that check does not
 * fail this test. It is kept because the field exists and a future opener
 * (timui_open_for_test paths) can leave it clear. */
TIMUI_TEST(test_transport_accessor){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds[2];

    TIMUI_CHECK(timui_transport(NULL) == NULL);

    if(pipe(fds) != 0) return;
    cfg.input_fd = fds[0];
    cfg.output_fd = fds[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    if(timui_open(&cfg, &ui) == TIMUI_OK){
        TimuiTransport *t = timui_transport(ui);
        TIMUI_CHECK(t != NULL);
        /* usable: an OSC 52 write through it must not fault */
        if(t) timui_clipboard_set(t, TIMUI_STR_LIT("lorem"));
        timui_close(ui);
    }
    close(fds[0]);
    close(fds[1]);
}

/* timui_mouse_state exposes the position and held-button state the frame
 * already tracks, which is what a drag selection needs: timui_mouse_clicked
 * reports only the press edge, and timui_poll_event sees nothing because
 * timui_begin has already drained every mouse event into its aggregators.
 *
 * With no input fed there is no button held, so this asserts the quiet
 * default and NULL-tolerance rather than a synthesized drag; the drag path
 * itself is exercised by hand through examples/agent_layout_tui. */
TIMUI_TEST(test_mouse_state_accessor){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds[2];

    TIMUI_CHECK(timui_mouse_state(NULL, NULL, NULL, NULL) == 0);

    if(pipe(fds) != 0) return;
    cfg.input_fd = fds[0];
    cfg.output_fd = fds[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    if(timui_open(&cfg, &ui) == TIMUI_OK){
        TimuiFrame *f = NULL;
        if(timui_begin_result(ui, &f) == TIMUI_OK){
            int x = -7, y = -7, down = -7;
            TIMUI_CHECK(timui_mouse_state(f, &x, &y, &down) == 1);
            TIMUI_CHECK(down == 0);          /* nothing held */
            TIMUI_CHECK(x != -7 && y != -7); /* outputs written */
            /* every out-pointer is optional */
            TIMUI_CHECK(timui_mouse_state(f, NULL, NULL, NULL) == 1);
            timui_end(f);
        }
        timui_close(ui);
    }
    close(fds[0]);
    close(fds[1]);
}
