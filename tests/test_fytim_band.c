/*
 * test_fytim_band.c - the public inline band surface.
 *
 * Drives libfytimui through the PUBLIC interface only: pipes for the
 * terminal, raw key bytes in, escape stream out. What the agent example
 * does by hand against the core, these cases prove the fytim_ surface can
 * do alone -- prompt editing, history, completion, commits, chrome.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

struct harness {
    struct fytim *ft;
    int in[2];    /* we write keys into in[1] */
    int out[2];   /* we read escapes from out[0] */
};

static int h_open(struct harness *h)
{
    struct fytim_cfg cfg;
    memset(h, 0, sizeof *h);
    if(pipe(h->in) != 0) return 0;
    if(pipe(h->out) != 0){ close(h->in[0]); close(h->in[1]); return 0; }
    fcntl(h->out[0], F_SETFL, O_NONBLOCK);
    fytim_cfg_default(&cfg);
    cfg.input_fd  = h->in[0];
    cfg.output_fd = h->out[1];
    h->ft = fytim_create(&cfg);
    return h->ft != NULL;
}
static void h_close(struct harness *h)
{
    fytim_destroy(h->ft);
    close(h->in[0]); close(h->in[1]);
    close(h->out[0]); close(h->out[1]);
}
static void h_keys(struct harness *h, const char *bytes)
{
    CHECK(write(h->in[1], bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
}
/* Drain everything currently in the output pipe into buf; returns length. */
static size_t h_out(struct harness *h, char *buf, size_t cap)
{
    size_t n = 0;
    ssize_t r;
    while(n < cap - 1 && (r = read(h->out[0], buf + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = '\0';
    return n;
}
static int contains(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if(nl == 0 || n < nl) return 0;
    for(i = 0; i + nl <= n; i++)
        if(memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

/* create + pump paints an inline band on the normal screen: erase-with-
 * closed-style, never the alt screen. */
static void test_create_paints_inline(void)
{
    struct harness h;
    char buf[8192];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(contains(buf, n, "\x1b[0m\r\x1b[J"));
    CHECK(!contains(buf, n, "\x1b[?1049h"));
    h_close(&h);
}

/* Commits are batched: nothing on the wire until the next pump, then the
 * lines land CRLF-terminated ahead of the band repaint. */
static void test_commit_batches_into_pump(void)
{
    struct harness h;
    char buf[8192];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_commit(h.ft, "hello", 5) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(n == 0);                              /* batched, not immediate */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "hello\x1b[0m\x1b[K"));
    h_close(&h);
}

/* The SGR-only contract: styling passes, cursor/erase control is rejected
 * and nothing reaches the terminal. */
static void test_commit_rejects_disallowed(void)
{
    struct harness h;
    char buf[8192];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_commit(h.ft, "\x1b[1mbold\x1b[0m", 12) == FYTIM_OK);
    CHECK(fytim_commit(h.ft, "\x1b[40mcard\x1b[K\x1b[0m",
                       sizeof("\x1b[40mcard\x1b[K\x1b[0m") - 1) == FYTIM_OK);
    CHECK(fytim_commit(h.ft, "bad\x1b[2Jworse", 12) == FYTIM_ERR_INVALID);
    CHECK(fytim_commit(h.ft, "bad\x1b[1Kworse", 12) == FYTIM_ERR_INVALID);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "bold"));
    CHECK(!contains(buf, n, "worse"));
    h_close(&h);
}

/* Enter submits: a FYTIM_EVENT_LINE with the text, and the input clears. */
static void test_line_event_on_enter(void)
{
    struct harness h;
    struct fytim_event ev;
    if(!h_open(&h)){ CHECK(0); return; }
    h_keys(&h, "hi\r");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_next_event(h.ft, &ev));
    CHECK(ev.type == FYTIM_EVENT_LINE);
    CHECK(ev.text_len == 2 && memcmp(ev.text, "hi", 2) == 0);
    CHECK(strcmp(fytim_input(h.ft), "") == 0);
    CHECK(!fytim_next_event(h.ft, &ev));
    h_close(&h);
}

/* Esc cancels outstanding work without exiting the session. */
static void test_esc_emits_interrupt(void)
{
    struct harness h;
    struct fytim_event ev;
    if(!h_open(&h)){ CHECK(0); return; }
    h_keys(&h, "\x1b\x1b");            /* ESC ESC decodes as one Escape */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_next_event(h.ft, &ev));
    CHECK(ev.type == FYTIM_EVENT_INTERRUPT);
    h_close(&h);
}

/* Scrollback keys that reach inline mode let the host pause animation. */
static void test_page_key_emits_scrollback(void)
{
    struct harness h;
    struct fytim_event ev;
    if(!h_open(&h)){ CHECK(0); return; }
    h_keys(&h, "\x1b[5~");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_next_event(h.ft, &ev));
    CHECK(ev.type == FYTIM_EVENT_SCROLLBACK);
    h_close(&h);
}

/* History: the host records lines; Up recalls, Down returns to the draft. */
static void test_history_recall(void)
{
    struct harness h;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_history_add(h.ft, "older") == FYTIM_OK);
    CHECK(fytim_history_add(h.ft, "newer") == FYTIM_OK);
    h_keys(&h, "draft");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "draft") == 0);
    h_keys(&h, "\x1b[A");              /* Up */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "newer") == 0);
    h_keys(&h, "\x1b[A");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "older") == 0);
    h_keys(&h, "\x1b[B");              /* one key per pump: key state is
                                          per-frame, like real typing */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_keys(&h, "\x1b[B");              /* second Down: back to the draft */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "draft") == 0);
    h_close(&h);
}

/* Completion: the host callback supplies candidates; a unique match
 * completes outright, several extend to the common prefix then cycle. */
static void comp_cb(void *user, const char *text, struct fytim_completions *c)
{
    (void)user;
    if(strncmp("/help", text, strlen(text)) == 0)
        CHECK(fytim_completion_add(c, "/help") == FYTIM_OK);
    if(strncmp("/history", text, strlen(text)) == 0)
        CHECK(fytim_completion_add(c, "/history") == FYTIM_OK);
}
static void test_completion(void)
{
    struct harness h;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_complete_fn(h.ft, comp_cb, NULL) == FYTIM_OK);

    /* "/he" matches both -> extends to the common prefix "/h" is already
     * there; common of /help,/history is "/h"; typed "/he" -> only /help */
    h_keys(&h, "/he");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);   /* the text lands first ... */
    h_keys(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);   /* ... then Tab completes it */
    CHECK(strcmp(fytim_input(h.ft), "/help") == 0);

    /* reset the line, then "/h" matches both: Tab extends nothing beyond
     * "/h", so it starts cycling: /help -> /history -> original -> ... */
    CHECK(fytim_set_input(h.ft, "/h") == FYTIM_OK);
    h_keys(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "/help") == 0);
    h_keys(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "/history") == 0);
    h_keys(&h, "\t");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(strcmp(fytim_input(h.ft), "/h") == 0);     /* the original */
    h_close(&h);
}

/* Chrome and live work-band content reach the band. */
static void test_chrome_and_workband(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_header(h.ft, "HDR") == FYTIM_OK);
    /* status rows are SGR-enabled markdown too: styling passes, control is
     * rejected */
    CHECK(fytim_set_status_row(h.ft, 0, "\x1b[32mST0\x1b[0m") == FYTIM_OK);
    CHECK(fytim_set_status_row(h.ft, 0, "no\x1b[2Jway") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_status_row(h.ft, 1, "ST1") == FYTIM_OK);
    CHECK(fytim_set_marker(h.ft, ">> ") == FYTIM_OK);
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "LIVE", 4) == FYTIM_OK);
    CHECK(fytim_set_status_row(h.ft, 2, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "HDR"));
    CHECK(contains(buf, n, "ST0"));
    CHECK(contains(buf, n, "ST1"));
    CHECK(contains(buf, n, ">> "));
    CHECK(contains(buf, n, "LIVE"));
    h_close(&h);
}

/* A work-band lives above the chrome while streaming; committing it moves
 * its content into native scrollback and retires the band. */
static void test_workband_lifecycle(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "wb-live", 7) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "wb-live"));
    CHECK(!contains(buf, n, "wb-live\x1b[0m\x1b[K"));     /* live, not committed */

    CHECK(fytim_workband_commit(wb) == FYTIM_OK);   /* wb is gone after */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "wb-live\x1b[0m\x1b[K"));      /* now in scrollback */
    h_close(&h);
}

/* Content beyond max_rows shows only its LAST rows. */
static void test_workband_caps_last_rows(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set_max_rows(wb, 2) == FYTIM_OK);
    CHECK(fytim_workband_set(wb, "L1\nL2\nL3\nL4", 11) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "L3"));
    CHECK(contains(buf, n, "L4"));
    CHECK(!contains(buf, n, "L1"));
    CHECK(!contains(buf, n, "L2"));
    CHECK(fytim_workband_set_max_rows(wb, 0) == FYTIM_ERR_INVALID);
    h_close(&h);
}

/* A trailing newline does not cost the band a row: rendered content always
 * ends on one, and counting the empty row after it as content pushes the
 * newest real row out of a capped window. */
static void test_workband_trailing_newline_not_a_row(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set_max_rows(wb, 2) == FYTIM_OK);
    CHECK(fytim_workband_set(wb, "L1\nL2\nL3\nL4\n", 12) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "L3"));
    CHECK(contains(buf, n, "L4"));
    CHECK(!contains(buf, n, "L1"));
    CHECK(!contains(buf, n, "L2"));
    h_close(&h);
}

/* Optional per-band top and bottom rows frame the live content. */
static void test_workband_top_bottom(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set_top(wb, "\x1b[1mTOPROW\x1b[0m") == FYTIM_OK);
    CHECK(fytim_workband_set_top(wb, "no\x1b[2Jway") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_set_bottom(wb, "BOTROW") == FYTIM_OK);
    CHECK(fytim_workband_set(wb, "MIDROW", 6) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TOPROW"));
    CHECK(contains(buf, n, "MIDROW"));
    CHECK(contains(buf, n, "BOTROW"));
    {   /* top-to-bottom order on the wire */
        const char *t = strstr(buf, "TOPROW"), *m = strstr(buf, "MIDROW"),
                   *b = strstr(buf, "BOTROW");
        CHECK(t && m && b && t < m && m < b);
    }
    CHECK(fytim_workband_set_top(wb, NULL) == FYTIM_OK);   /* clears */
    h_close(&h);
}

/* Bands stack oldest-first above the chrome, but tasks finish in arbitrary
 * order: whichever band commits first lands in the transcript first, even
 * if an older band is still live above it. */
static void test_workband_order_and_independence(void)
{
    struct harness h;
    struct fytim_workband *w1, *w2;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    w1 = fytim_workband_create(h.ft);
    w2 = fytim_workband_create(h.ft);
    CHECK(w1 && w2);
    CHECK(fytim_workband_set(w1, "FIRSTB", 6) == FYTIM_OK);
    CHECK(fytim_workband_set(w2, "SECONDB", 7) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    {
        const char *a = strstr(buf, "FIRSTB"), *b = strstr(buf, "SECONDB");
        CHECK(a && b && a < b);          /* #1 above #2 while live */
    }
    /* the NEWER band finishes first: it commits while the older streams on */
    CHECK(fytim_workband_commit(w2) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "SECONDB\x1b[0m\x1b[K"));  /* committed */
    CHECK(contains(buf, n, "FIRSTB"));       /* still live */
    CHECK(!contains(buf, n, "FIRSTB\x1b[0m\x1b[K"));
    CHECK(fytim_workband_commit(w1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "FIRSTB\x1b[0m\x1b[K"));   /* transcript order = commit order */
    h_close(&h);
}

/* While the transcript tail is streaming, a work-band commit must NOT
 * interleave into the transcript (it would split the streaming reply):
 * the band's final render persists on screen and commits -- in finish
 * order -- once the stream ends and the tail clears. */
static void test_workband_commit_defers_during_stream(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_tail_set(h.ft, "STREAMING", 9) == FYTIM_OK);
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "TOOLOUT", 7) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_workband_commit(wb) == FYTIM_OK);   /* mid-stream: deferred */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "TOOLOUT\x1b[0m\x1b[K"));   /* not committed */

    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);   /* stream done */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TOOLOUT\x1b[0m\x1b[K"));    /* now it lands */
    h_close(&h);
}

/* regression: the stream-in-progress signal is EXPLICIT, not inferred from
 * tail emptiness -- a freeze can consume the whole active region at a
 * block boundary, momentarily emptying the tail mid-stream, and deferred
 * work-band commits must keep holding until the host clears the tail with
 * fytim_tail_set(NULL). */
static void test_workband_defer_survives_empty_tail(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    /* one update whose freeze consumes everything: the tail is now empty
     * but the stream is still in flight */
    CHECK(fytim_tail_apply(h.ft, 0, "ROW1\n", 5, 1) == FYTIM_OK);
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "TOOLOUT", 7) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_workband_commit(wb) == FYTIM_OK);   /* must defer */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "TOOLOUT\x1b[0m\x1b[K"));

    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);   /* stream ended */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TOOLOUT\x1b[0m\x1b[K"));
    h_close(&h);
}

/* The SGR-only contract holds for live work-band content too. */
/* What a band commits can differ from what it shows live: the host sets a
 * commit payload (e.g. the tool output re-rendered as a fenced markdown
 * block) and the live render never reaches the transcript. */
static void test_workband_commit_payload(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "LIVEROW", 7) == FYTIM_OK);
    CHECK(fytim_workband_set_commit(wb, "FENCEDPAY", 9) == FYTIM_OK);
    /* the payload has the same SGR-only contract; rejection keeps the old */
    CHECK(fytim_workband_set_commit(wb, "x\x1b[2Jy", 6) == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_commit(wb) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "FENCEDPAY\x1b[0m\x1b[K"));  /* payload committed */
    CHECK(!contains(buf, n, "LIVEROW\x1b[0m\x1b[K"));   /* live render not */
    h_close(&h);
}

/* The payload survives a deferred commit, and clearing it (NULL) falls
 * back to committing the live content. */
static void test_workband_commit_payload_defers(void)
{
    struct harness h;
    struct fytim_workband *wb, *wb2;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_tail_set(h.ft, "STREAMING", 9) == FYTIM_OK);
    wb = fytim_workband_create(h.ft);
    wb2 = fytim_workband_create(h.ft);
    CHECK(wb != NULL && wb2 != NULL);
    CHECK(fytim_workband_set(wb, "LIVEROW", 7) == FYTIM_OK);
    CHECK(fytim_workband_set_commit(wb, "FENCEDPAY", 9) == FYTIM_OK);
    CHECK(fytim_workband_set(wb2, "OTHERLIVE", 9) == FYTIM_OK);
    CHECK(fytim_workband_set_commit(wb2, "GONE", 4) == FYTIM_OK);
    CHECK(fytim_workband_set_commit(wb2, NULL, 0) == FYTIM_OK); /* clear */
    CHECK(fytim_workband_commit(wb) == FYTIM_OK);    /* mid-stream: defer */
    CHECK(fytim_workband_commit(wb2) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);    /* stream done */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "FENCEDPAY\x1b[0m\x1b[K"));
    CHECK(!contains(buf, n, "LIVEROW\x1b[0m\x1b[K"));
    CHECK(contains(buf, n, "OTHERLIVE\x1b[0m\x1b[K"));  /* fallback: live */
    CHECK(!contains(buf, n, "GONE\x1b[0m\x1b[K"));
    h_close(&h);
}

/* The transcript is ONE continuous SGR stream: a style opened in one
 * commit is still open when the next commit's first row lands (a fenced
 * block freezes row by row, each row a separate commit). The running
 * state must be re-opened at the head of every commit, not only after
 * row breaks within one. */
static void test_sgr_carries_across_commits(void)
{
    struct harness h;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_commit(h.ft, "\x1b[2mCC1", 7) == FYTIM_OK);   /* dim opened */
    CHECK(fytim_commit(h.ft, "CC2", 3) == FYTIM_OK);          /* still dim  */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "\x1b[2mCC2"));
    CHECK(fytim_commit(h.ft, "CC3\x1b[0m", 7) == FYTIM_OK);   /* closes    */
    CHECK(fytim_commit(h.ft, "CC4", 3) == FYTIM_OK);          /* plain now */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "\x1b[2mCC3"));
    CHECK(!contains(buf, n, "\x1b[2mCC4"));
    h_close(&h);
}

/* Output is buffered and flushed once per frame (a frame split across
 * many small writes flickers on terminals without DEC 2026). A commit
 * larger than the transport buffer takes the flush-then-spill path and
 * must arrive complete and in order. */
static void test_large_commit_spills_intact(void)
{
    struct harness h;
    static char big[40960], out[65536];
    size_t n, o = 0;
    int i;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, out, sizeof out);

    for(i = 0; i < 500; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "row%03d 456789012345678901234567890123456789012345678901234567890123456789012\n",
                              i);
    CHECK(fytim_commit(h.ft, big, o) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, out, sizeof out);
    CHECK(contains(out, n, "row000 "));
    CHECK(contains(out, n, "row250 "));
    CHECK(contains(out, n, "row499 "));
    /* ordering survived the buffer boundary */
    CHECK(strstr(out, "row000") < strstr(out, "row499"));
    /* and EXACTLY once each: a flush that forgets to reset duplicates
     * rows, a skipped flush drops them */
    {
        int cnt = 0;
        const char *p = out;
        while((p = strstr(p, "row")) != NULL){ cnt++; p += 3; }
        if(cnt != 500){
            char needle[16];
            printf("    (rows seen: %d, bytes: %zu)\n", cnt, n);
            for(i = 0; i < 500; i++){
                snprintf(needle, sizeof needle, "row%03d ", i);
                if(!contains(out, n, needle)) printf("    missing %s\n", needle);
            }
        }
        CHECK(cnt == 500);
    }
    h_close(&h);
}

static void test_workband_rejects_disallowed(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "GOODWB", 6) == FYTIM_OK);
    CHECK(fytim_workband_set(wb, "bad\x1b[2Jworse", 12) == FYTIM_ERR_INVALID);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "GOODWB"));       /* previous content retained */
    CHECK(!contains(buf, n, "worse"));
    h_close(&h);
}

/* When the terminal is too short for every band, the OLDEST loses rows
 * first (its earliest lines go); the newest band keeps everything. */
static void test_workband_shed_oldest(void)
{
    struct harness h;
    struct fytim_workband *w1, *w2;
    char a[256], b[256], buf[32768];
    size_t n;
    int i;
    if(!h_open(&h)){ CHECK(0); return; }
    a[0] = b[0] = '\0';
    for(i = 1; i <= 10; i++){
        snprintf(a + strlen(a), sizeof a - strlen(a), "A%02d%s", i,
                 i < 10 ? "\n" : "");
        snprintf(b + strlen(b), sizeof b - strlen(b), "B%02d%s", i,
                 i < 10 ? "\n" : "");
    }
    w1 = fytim_workband_create(h.ft);
    w2 = fytim_workband_create(h.ft);
    CHECK(w1 && w2);
    CHECK(fytim_workband_set_max_rows(w1, 12) == FYTIM_OK);
    CHECK(fytim_workband_set_max_rows(w2, 12) == FYTIM_OK);
    CHECK(fytim_workband_set(w1, a, strlen(a)) == FYTIM_OK);
    CHECK(fytim_workband_set(w2, b, strlen(b)) == FYTIM_OK);
    /* pipes keep the 80x24 default: chrome 6 + 20 wanted rows > 24 */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "B01"));          /* newest keeps everything */
    CHECK(contains(buf, n, "B10"));
    CHECK(contains(buf, n, "A10"));          /* oldest keeps its tail ... */
    CHECK(!contains(buf, n, "A01"));         /* ... sheds its head */
    CHECK(!contains(buf, n, "A02"));
    h_close(&h);
}

/* Every band update is wrapped in DEC 2026 synchronized output, UNCONDITIONALLY:
 * capability detection misses plenty of terminals (and multiplexers strip
 * it), but the escape is a private mode that unsupporting terminals ignore
 * -- while supporting ones render the whole update atomically, which is
 * the last line of defense against flicker. */
static void test_sync_brackets_updates(void)
{
    struct harness h;
    char buf[16384];
    size_t n;
    int hi, lo;
    /* a deliberately non-modern terminal: detection must not matter */
    setenv("TERM", "xterm", 1);
    unsetenv("TERM_PROGRAM");
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_commit(h.ft, "syncline", 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    hi = contains(buf, n, "\x1b[?2026h");
    lo = contains(buf, n, "\x1b[?2026l");
    CHECK(hi && lo);
    {   /* the whole update sits inside the bracket */
        const char *b = strstr(buf, "\x1b[?2026h"), *c = strstr(buf, "syncline"),
                   *e = strstr(buf, "\x1b[?2026l");
        CHECK(b && c && e && b < c && c < e);
    }
    h_close(&h);
}

/* The transcript's live tail: the agent's own streaming output, updated
 * immediately, drawn directly under the scrollback and ABOVE every
 * work-band -- a different path from the work panes. Same SGR contract. */
static void test_transcript_tail(void)
{
    struct harness h;
    struct fytim_workband *wb;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    wb = fytim_workband_create(h.ft);
    CHECK(wb != NULL);
    CHECK(fytim_workband_set(wb, "WBROW", 5) == FYTIM_OK);
    CHECK(fytim_tail_set(h.ft, "TAILA\nTAILB", 11) == FYTIM_OK);
    CHECK(fytim_tail_set(h.ft, "no\x1b[2Jway", 9) == FYTIM_ERR_INVALID);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "TAILA"));
    CHECK(contains(buf, n, "TAILB"));
    CHECK(contains(buf, n, "WBROW"));
    {   /* the tail paints above the work-band */
        const char *t = strstr(buf, "TAILB"), *w = strstr(buf, "WBROW");
        CHECK(t && w && t < w);
    }
    /* clearing the tail shrinks the band back */
    CHECK(fytim_tail_set(h.ft, NULL, 0) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(!contains(buf, n, "TAILA"));
    h_close(&h);
}

/* Progressive updates applied by the LIBRARY: fytim_tail_apply takes a
 * renderer's (backtrack, content, freeze) triple -- fymd_render_push's
 * update shape -- and maintains the tail itself: drop the cursor-row
 * residue plus `backtrack` full rows, append, commit the first `freeze`
 * rows. regression: content rows arrive as "text\n" + an SGR carry-over
 * tail; counting that residue as a backtracked row under-dropped by one
 * and every partial rendition of a line staircased into the transcript. */
static void test_tail_apply(void)
{
    struct harness h;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    /* three pushes re-rendering one growing line, renderer-shaped */
    CHECK(fytim_tail_apply(h.ft, 0, "L1a\n\x1b[1m", 8, 0) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_tail_apply(h.ft, 1, "\x1b[0mL1ab\n\x1b[1m", 13, 0) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);
    CHECK(fytim_tail_apply(h.ft, 1, "\x1b[0mL1abc\nL2\n", 13, 1) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    /* the final rendition commits exactly once; no staircase */
    CHECK(contains(buf, n, "L1abc\x1b[0m\x1b[K"));
    CHECK(!contains(buf, n, "L1ab\x1b[0m\x1b[K"));
    CHECK(!contains(buf, n, "L1a\x1b[0m\x1b[K"));
    CHECK(contains(buf, n, "L2"));               /* still live in the tail */

    /* the SGR contract holds; a rejected push leaves the tail unchanged */
    CHECK(fytim_tail_apply(h.ft, 0, "no\x1b[2Jway", 9, 0) == FYTIM_ERR_INVALID);
    CHECK(fytim_tail_apply(h.ft, 0, "\x1b[40mcard\x1b[K\x1b[0m\n",
                           sizeof("\x1b[40mcard\x1b[K\x1b[0m\n") - 1, 0)
          == FYTIM_OK);
    h_close(&h);
}

/* The terminal geometry accessor: the host sizes its markdown renderer to
 * the terminal width, so hard-wrapped lines never soft-wrap. */
static void test_size_accessor(void)
{
    struct harness h;
    int w = 0, hh = 0;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_size(h.ft, &w, &hh) == FYTIM_OK);
    CHECK(w == 80 && hh == 24);            /* pipes keep the default */
    CHECK(fytim_size(h.ft, &w, NULL) == FYTIM_OK);
    CHECK(fytim_size(NULL, &w, &hh) == FYTIM_ERR_INVALID);
    h_close(&h);
}

/* regression: with NO work-bands live, the full status-band chrome must
 * still fit -- both status rows included. (Bug: the idle band was sized to
 * exactly the chrome, but the layout reserves a transcript row, so the
 * second status row was shed whenever no work-band existed.) */
static void test_regression_idle_band_keeps_status(void)
{
    struct harness h;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_status_row(h.ft, 0, "ST0") == FYTIM_OK);
    CHECK(fytim_set_status_row(h.ft, 1, "ST1") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "ST0"));
    CHECK(contains(buf, n, "ST1"));
    h_close(&h);
}

/* ^G asks for an external editor; the host brackets it with suspend and
 * resume. Suspended pumps are inert; resume repaints in full. */
static void test_ctrl_g_edit_and_suspend(void)
{
    struct harness h;
    struct fytim_event ev;
    char buf[16384];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    (void)h_out(&h, buf, sizeof buf);

    h_keys(&h, "\x07");                        /* ^G */
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_next_event(h.ft, &ev));
    CHECK(ev.type == FYTIM_EVENT_EDIT);
    (void)h_out(&h, buf, sizeof buf);

    CHECK(fytim_resume(h.ft) == FYTIM_ERR_INVALID);   /* not suspended */
    CHECK(fytim_suspend(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "\x1b[0m\r\x1b[J"));       /* band erased */
    CHECK(fytim_suspend(h.ft) == FYTIM_ERR_INVALID);  /* double */

    CHECK(fytim_pump(h.ft) == FYTIM_OK);              /* inert pump */
    n = h_out(&h, buf, sizeof buf);
    CHECK(n == 0);

    CHECK(fytim_resume(h.ft) == FYTIM_OK);
    CHECK(fytim_set_input(h.ft, "edited text") == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    n = h_out(&h, buf, sizeof buf);
    CHECK(contains(buf, n, "edited text"));           /* full repaint */
    h_close(&h);
}

/* ^T belongs to the host so it can move focus between its work tiles. */
static void test_ctrl_t_focus_next(void)
{
    struct harness h;
    struct fytim_event ev;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_keys(&h, "\x14");
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    CHECK(fytim_next_event(h.ft, &ev));
    CHECK(ev.type == FYTIM_EVENT_FOCUS_NEXT);
    CHECK(!fytim_next_event(h.ft, &ev));
    CHECK(!strcmp(fytim_input(h.ft), ""));
    h_close(&h);
}

/* Every entry point survives NULL/degenerate arguments. */
static void test_null_safety(void)
{
    struct fytim_event ev;
    CHECK(fytim_pump(NULL) == FYTIM_ERR_INVALID);
    CHECK(!fytim_next_event(NULL, &ev));
    CHECK(fytim_commit(NULL, "x", 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_create(NULL) == NULL);
    CHECK(fytim_workband_set(NULL, "x", 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_set_max_rows(NULL, 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_set_top(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_set_bottom(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_workband_commit(NULL) == FYTIM_ERR_INVALID);
    fytim_workband_destroy(NULL);
    CHECK(fytim_tail_set(NULL, "x", 1) == FYTIM_ERR_INVALID);
    CHECK(fytim_suspend(NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_resume(NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_set_header(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_status_row(NULL, 0, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_marker(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_chrome_style(NULL, FYTIM_CHROME_HEADER,
                                 "\x1b[1m") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_input(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_input(NULL) != NULL);            /* "" for a NULL ft */
    CHECK(fytim_history_add(NULL, "x") == FYTIM_ERR_INVALID);
    CHECK(fytim_history_set_max_len(NULL, 8) == FYTIM_ERR_INVALID);
    CHECK(fytim_set_complete_fn(NULL, NULL, NULL) == FYTIM_ERR_INVALID);
    CHECK(fytim_completion_add(NULL, "x") == FYTIM_ERR_INVALID);
    fytim_destroy(NULL);
}

static void test_chrome_style_contract(void)
{
    struct harness h;

    if(!h_open(&h)){ CHECK(0); return; }
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_HEADER,
                                 "\x1b[31;1m") == FYTIM_OK);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_STATUS,
                                 "\x1b[2m") == FYTIM_OK);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_WORKBAND,
                                 "\x1b[34m") == FYTIM_OK);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_MARKER,
                                 "\x1b[32m") == FYTIM_OK);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_STYLE_COUNT,
                                 "\x1b[1m") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_HEADER,
                                 "visible") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_HEADER,
                                 "\x1b[2J") == FYTIM_ERR_INVALID);
    CHECK(fytim_set_chrome_style(h.ft, FYTIM_CHROME_HEADER,
                                 NULL) == FYTIM_OK);
    h_close(&h);
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "create_paints_inline", test_create_paints_inline },
        { "commit_batches_into_pump", test_commit_batches_into_pump },
        { "commit_rejects_disallowed", test_commit_rejects_disallowed },
        { "line_event_on_enter", test_line_event_on_enter },
        { "esc_emits_interrupt", test_esc_emits_interrupt },
        { "page_key_emits_scrollback", test_page_key_emits_scrollback },
        { "history_recall", test_history_recall },
        { "completion", test_completion },
        { "chrome_and_workband", test_chrome_and_workband },
        { "workband_lifecycle", test_workband_lifecycle },
        { "workband_caps_last_rows", test_workband_caps_last_rows },
        { "workband_trailing_newline_not_a_row",
          test_workband_trailing_newline_not_a_row },
        { "workband_top_bottom", test_workband_top_bottom },
        { "workband_order_and_independence", test_workband_order_and_independence },
        { "workband_rejects_disallowed", test_workband_rejects_disallowed },
        { "large_commit_spills_intact", test_large_commit_spills_intact },
        { "sgr_carries_across_commits", test_sgr_carries_across_commits },
        { "workband_commit_payload", test_workband_commit_payload },
        { "workband_commit_payload_defers", test_workband_commit_payload_defers },
        { "workband_commit_defers_during_stream",
          test_workband_commit_defers_during_stream },
        { "workband_defer_survives_empty_tail",
          test_workband_defer_survives_empty_tail },
        { "workband_shed_oldest", test_workband_shed_oldest },
        { "ctrl_g_edit_and_suspend", test_ctrl_g_edit_and_suspend },
        { "ctrl_t_focus_next", test_ctrl_t_focus_next },
        { "regression_idle_band_keeps_status",
          test_regression_idle_band_keeps_status },
        { "size_accessor", test_size_accessor },
        { "sync_brackets_updates", test_sync_brackets_updates },
        { "transcript_tail", test_transcript_tail },
        { "tail_apply", test_tail_apply },
        { "chrome_style_contract", test_chrome_style_contract },
        { "null_safety", test_null_safety },
    };
    size_t i, n = sizeof(tests) / sizeof(tests[0]);

    if(argc == 2 && strcmp(argv[1], "--list") == 0){
        for(i = 0; i < n; ++i) printf("%s\n", tests[i].name);
        return 0;
    }
    if(argc == 2){
        for(i = 0; i < n; ++i)
            if(strcmp(argv[1], tests[i].name) == 0){ tests[i].fn(); return failures ? 1 : 0; }
        fprintf(stderr, "no such test: %s\n", argv[1]);
        return 2;
    }
    for(i = 0; i < n; ++i) tests[i].fn();
    printf(failures ? "%d failure(s)\n" : "all %d ok\n", failures ? failures : (int)n);
    return failures ? 1 : 0;
}
