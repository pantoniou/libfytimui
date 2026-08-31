/*
 * test_fytim_resize.c - the terminal changed size.
 *
 * The library samples the terminal on every pump, so a window that is resized
 * must be seen without a signal: the host is told, and what it publishes is
 * laid out at the new width. A pipe cannot answer TIOCGWINSZ, so these cases
 * need a real pseudo-terminal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "test_pty.h"

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

struct pty_h {
    struct fytim *ft;
    int master;
    int slave;
    pthread_t drainer;
    int drainer_live;
    volatile int stop;
};

/* A pty's output buffer is small -- 1023 bytes on macOS against ~64K on
 * Linux -- so a single frame can fill it. The library then blocks in write()
 * waiting for a reader, inside fytim_pump(), and a test that only drains
 * after the pump returns deadlocks. Drain continuously instead. */
static void *drain_thread(void *arg)
{
    struct pty_h *h = arg;
    char buf[4096];

    while(!h->stop){
        struct pollfd pfd;
        pfd.fd = h->master; pfd.events = POLLIN; pfd.revents = 0;
        if(poll(&pfd, 1, 20) <= 0) continue;
        while(read(h->master, buf, sizeof buf) > 0)
            ;
    }
    return NULL;
}

static void set_size(int fd, int rows, int cols)
{
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    CHECK(ioctl(fd, TIOCSWINSZ, &ws) == 0);
}

static int h_open(struct pty_h *h, const char *name)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(!timui_test_open_pty_pair(name, &h->master, &h->slave)) return 0;
    set_size(h->master, 24, 80);
    fcntl(h->master, F_SETFL, O_NONBLOCK);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->slave;
    cfg.output_fd = h->slave;
    h->ft = fytim_create(&cfg);
    if(!h->ft){ close(h->master); close(h->slave); return 0; }
    h->drainer_live = pthread_create(&h->drainer, NULL, drain_thread, h) == 0;
    CHECK(h->drainer_live);
    return 1;
}

static void h_close(struct pty_h *h)
{
    if(h->drainer_live){
        h->stop = 1;
        pthread_join(h->drainer, NULL);
        h->drainer_live = 0;
    }
    fytim_destroy(h->ft);
    close(h->master);
    close(h->slave);
}

/* Stop the drainer so the test can read what is written next itself. What
 * the drainer already took is gone, which is what these cases want: the
 * reading starts at the action under test. */
static void h_drain_stop(struct pty_h *h)
{
    if(!h->drainer_live) return;
    h->stop = 1;
    pthread_join(h->drainer, NULL);
    h->drainer_live = 0;
}

/* Read what is on the master, up to @cap - 1 bytes, NUL terminated. */
static size_t h_read(struct pty_h *h, char *buf, size_t cap)
{
    struct pollfd pfd;
    size_t used = 0;

    pfd.fd = h->master; pfd.events = POLLIN; pfd.revents = 0;
    while(used + 1 < cap && poll(&pfd, 1, 100) > 0){
        ssize_t n = read(h->master, buf + used, cap - 1 - used);
        if(n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = 0;
    return used;
}

/* The size is sampled on a pump, and the host is told what it became. */
static void test_resize_is_seen(void)
{
    struct fytim_event ev;
    struct pty_h h;
    int w = 0, ht = 0;
    int resized = 0;

    if(!h_open(&h, "resize_is_seen")) return;
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_size(h.ft, &w, &ht) == FYTIM_OK);
    CHECK(w == 80);
    CHECK(ht == 24);
    while(fytim_next_event(h.ft, &ev))
        ;

    set_size(h.master, 30, 100);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_size(h.ft, &w, &ht) == FYTIM_OK);
    CHECK(w == 100);
    CHECK(ht == 30);
    while(fytim_next_event(h.ft, &ev))
        if(ev.type == FYTIM_EVENT_RESIZE){
            resized = 1;
            CHECK(ev.width == 100);
            CHECK(ev.height == 30);
        }
    CHECK(resized);
    h_close(&h);
}

/* A surface holding the keys must not stop the size being sampled: a full
 * screen terminal is exactly the case where a resize matters. */
static void test_resize_is_seen_while_a_surface_holds_the_keys(void)
{
    struct fytim_surface *s;
    struct pty_h h;
    int w = 0, ht = 0;

    if(!h_open(&h, "resize_with_keys")) return;
    s = fytim_surface_open(h.ft, 4, 8);
    CHECK(s != NULL);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);

    set_size(h.master, 30, 100);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_size(h.ft, &w, &ht) == FYTIM_OK);
    CHECK(w == 100);
    CHECK(ht == 30);
    fytim_surface_close(s);
    h_close(&h);
}

/* The rows a surface is granted follow the window: a taller terminal shows
 * more of the program, which is what the host sizes it to. */
static void test_granted_rows_follow_the_window(void)
{
    struct pty_h h;
    struct fytim_surface *s;
    int small = 0, large = 0;

    if(!h_open(&h, "granted_rows_follow")) return;
    s = fytim_surface_open(h.ft, 40, 8);
    CHECK(s != NULL);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(s, &small) == FYTIM_OK);

    set_size(h.master, 40, 100);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_surface_granted_rows(s, &large) == FYTIM_OK);
    CHECK(small > 0);
    CHECK(large > small);
    fytim_surface_close(s);
    h_close(&h);
}

/* Resizing replaces the retained frame buffers. The replacement must still
 * be invalidated: the real terminal may have reflowed nonblank cells into
 * positions where the new frame contains blanks. */
static void test_resize_repaints_blank_cells(void)
{
    struct pty_h h;
    char buf[16384];

    if(!h_open(&h, "resize_repaints_blanks")) return;
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain_stop(&h);
    (void)h_read(&h, buf, sizeof buf);

    set_size(h.master, 24, 52);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_read(&h, buf, sizeof buf); /* frame made before host sees event */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(h_read(&h, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "\x1b[J") != NULL);
    h_close(&h);
}

/*
 * A host that keeps the source of what it committed can write those rows
 * again after a width change. It asks for a clear screen first, and the band
 * is anchored at the top of it.
 */
static void test_clear_screen_erases_the_screen(void)
{
    struct pty_h h;
    char buf[4096];

    if(!h_open(&h, "clear_screen")) return;
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain_stop(&h);
    (void)h_read(&h, buf, sizeof buf);      /* the frame just painted */

    CHECK(fytim_clear_screen(h.ft) == FYTIM_OK);
    CHECK(h_read(&h, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "\x1b[H\x1b[2J") != NULL);

    /* The screen is blank, so the next frame claims it with an erase of its
     * own instead of overwriting rows it no longer has. */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(h_read(&h, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "\x1b[J") != NULL);
    h_close(&h);
}

/* Ctrl-L repaints the band of the library. A host with rows of its own on the
 * screen is told, so that it can write them again beside it. */
static void test_ctrl_l_is_reported(void)
{
    struct fytim_event ev;
    struct pty_h h;
    int redraw = 0;
    int i;

    if(!h_open(&h, "ctrl_l_reported")) return;
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    while(fytim_next_event(h.ft, &ev))
        ;
    CHECK(write(h.master, "\x0c", 1) == 1);
    /* The key has to arrive before the pump that reads it. */
    for(i = 0; i < 20 && !redraw; i++){
        CHECK(fytim_pump(h.ft) == FYTIM_OK);
        while(fytim_next_event(h.ft, &ev))
            if(ev.type == FYTIM_EVENT_REDRAW)
                redraw = 1;
        if(!redraw){
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    CHECK(redraw);
    h_close(&h);
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "resize_is_seen",              test_resize_is_seen },
    { "resize_is_seen_with_keys",
      test_resize_is_seen_while_a_surface_holds_the_keys },
    { "granted_rows_follow_the_window", test_granted_rows_follow_the_window },
    { "resize_repaints_blank_cells", test_resize_repaints_blank_cells },
    { "clear_screen_erases_the_screen", test_clear_screen_erases_the_screen },
    { "ctrl_l_is_reported",            test_ctrl_l_is_reported },
};

int main(int argc, char **argv)
{
    size_t i;
    if(argc > 1 && strcmp(argv[1], "--list") == 0){
        for(i = 0; i < sizeof cases / sizeof cases[0]; i++)
            printf("%s\n", cases[i].name);
        return 0;
    }
    for(i = 0; i < sizeof cases / sizeof cases[0]; i++){
        if(argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        printf("== %s\n", cases[i].name);
        cases[i].fn();
    }
    return failures ? 1 : 0;
}
