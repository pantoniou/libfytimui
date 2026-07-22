/*
 * test_fytim_sgr.c - SGR stream -> styled runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fytim_sgr.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond)) {                                                       \
            ++failures;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                   \
    } while(0)

#define MAX_RUNS 16
struct capture {
    char text[MAX_RUNS][256];
    struct fytim_sgr_style style[MAX_RUNS];
    int n;
};

static bool cap_run(void *user, const char *text, size_t len,
                    const struct fytim_sgr_style *style)
{
    struct capture *c = (struct capture *)user;
    if(c->n >= MAX_RUNS) return false;
    if(len >= sizeof c->text[0]) len = sizeof c->text[0] - 1;
    memcpy(c->text[c->n], text, len);
    c->text[c->n][len] = '\0';
    c->style[c->n] = *style;
    ++c->n;
    return true;
}

static void feed(struct capture *c, struct fytim_sgr_parser *p, const char *s)
{
    fytim_sgr_feed(p, s, strlen(s), cap_run, c);
}

/* Plain text with no escapes is one default-styled run. */
static void test_plain_text(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "hello world");
    CHECK(c.n == 1);
    CHECK(strcmp(c.text[0], "hello world") == 0);
    CHECK(c.style[0].attrs == 0);
    CHECK(c.style[0].fg == FYTIM_COLOR_DEFAULT);
    CHECK(!p.disallowed_seen);
}

/* Escapes must not leak into the text, and must change the style of what
 * follows them. */
static void test_bold_run(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "a\x1b[1mb\x1b[0mc");
    CHECK(c.n == 3);
    CHECK(strcmp(c.text[0], "a") == 0 && c.style[0].attrs == 0);
    CHECK(strcmp(c.text[1], "b") == 0 && (c.style[1].attrs & FYTIM_ATTR_BOLD));
    CHECK(strcmp(c.text[2], "c") == 0 && c.style[2].attrs == 0);
}

/* Multiple parameters in one sequence all apply. */
static void test_combined_params(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b[1;3;4mx");
    CHECK(c.n == 1);
    CHECK((c.style[0].attrs & FYTIM_ATTR_BOLD));
    CHECK((c.style[0].attrs & FYTIM_ATTR_ITALIC));
    CHECK((c.style[0].attrs & FYTIM_ATTR_UNDERLINE));
}

static void test_truecolor(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b[38;2;18;52;86mx");
    CHECK(c.n == 1);
    CHECK(c.style[0].fg == 0x123456u);
}

static void test_indexed_color(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b[38;5;42mx");
    CHECK(c.n == 1);
    CHECK(c.style[0].fg == (FYTIM_COLOR_INDEXED | 42u));
}

/* State persists across feeds: a style set in one chunk still applies to the
 * next, since a pane is appended to incrementally. */
static void test_style_persists_across_feeds(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b[1ma");
    feed(&c, &p, "b");
    CHECK(c.n == 2);
    CHECK((c.style[1].attrs & FYTIM_ATTR_BOLD));
}

/* Negative: an escape sequence split across feeds must not emit its bytes as
 * text, and must still take effect once completed. */
static void test_escape_split_across_feeds(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    int i;
    fytim_sgr_init(&p);
    feed(&c, &p, "a\x1b");
    feed(&c, &p, "[1");
    feed(&c, &p, "mb");
    for(i = 0; i < c.n; ++i)
        CHECK(strchr(c.text[i], '\x1b') == NULL && strchr(c.text[i], '[') == NULL);
    CHECK(c.n == 2);
    CHECK(strcmp(c.text[0], "a") == 0);
    CHECK(strcmp(c.text[1], "b") == 0 && (c.style[1].attrs & FYTIM_ATTR_BOLD));
}

/* Negative: cursor/erase/screen-mode controls are not styling. They must be
 * swallowed rather than rendered, and flagged so the caller can reject the
 * content outright. */
static void test_disallowed_sequences_flagged(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "a\x1b[2Jb\x1b[10;5Hc");
    CHECK(p.disallowed_seen);
    CHECK(c.n == 3);
    CHECK(strcmp(c.text[0], "a") == 0);
    CHECK(strcmp(c.text[1], "b") == 0);
    CHECK(strcmp(c.text[2], "c") == 0);
}

/* OSC 8 hyperlinks are the ONE non-SGR escape that passes: they carry no
 * cursor/erase semantics, and committed lines keep their links clickable
 * in native scrollback (libfymd4c emits them for markdown links). The
 * link bytes are swallowed, never rendered as text. */
static void test_osc8_hyperlink_allowed(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    int i;
    fytim_sgr_init(&p);
    feed(&c, &p, "a\x1b]8;;http://x.y\x1b\\link\x1b]8;;\x1b\\b");
    CHECK(!p.disallowed_seen);
    for(i = 0; i < c.n; ++i)
        CHECK(strstr(c.text[i], "http") == NULL);
    CHECK(c.n == 3);
    CHECK(strcmp(c.text[0], "a") == 0);
    CHECK(strcmp(c.text[1], "link") == 0);
    CHECK(strcmp(c.text[2], "b") == 0);

    /* BEL-terminated form too */
    feed(&c, &p, "\x1b]8;;http://z\x07t\x1b]8;;\x07");
    CHECK(!p.disallowed_seen);

    /* split across feeds */
    feed(&c, &p, "\x1b]8;;ht");
    feed(&c, &p, "tp://q\x1b");
    feed(&c, &p, "\\ok");
    CHECK(!p.disallowed_seen);
    CHECK(strcmp(c.text[c.n - 1], "ok") == 0);
}

/* Every OTHER OSC stays disallowed: titles, clipboard, palette. */
static void test_other_osc_still_disallowed(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "a\x1b]0;title\x07z");
    CHECK(p.disallowed_seen);
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b]52;c;aGk=\x07");    /* OSC 52 clipboard */
    CHECK(p.disallowed_seen);
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b]88;;x\x07");        /* 8-prefixed but not OSC 8 */
    CHECK(p.disallowed_seen);
}

/* Negative: malformed and truncated input must terminate, not run away. */
static void test_malformed_input_safe(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    feed(&c, &p, "\x1b");            /* bare ESC, never completed */
    feed(&c, &p, "\x1b[");           /* truncated CSI */
    feed(&c, &p, "\x1b[999999999m"); /* absurd parameter */
    feed(&c, &p, "\x1b[;;;m");       /* empty parameters */
    feed(&c, &p, "\x1b[38;2;1mx");   /* truncated truecolor */
    CHECK(!0);                        /* reaching here without hanging is the assertion */
}

/* An over-long escape sequence must not overflow the carry buffer. */
static void test_overlong_escape_safe(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    char big[512];
    size_t i;
    fytim_sgr_init(&p);
    big[0] = '\x1b'; big[1] = '[';
    for(i = 2; i < sizeof big - 1; ++i) big[i] = '1';
    big[sizeof big - 1] = 'm';
    fytim_sgr_feed(&p, big, sizeof big, cap_run, &c);
    CHECK(!0);
}

/* Parse -> emit -> parse must reach the same style: the emitter is what
 * re-opens carried state after a per-row reset. */
static void test_style_emit_roundtrip(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p, q;
    char seq[64];
    size_t n;

    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, "\x1b[1;3;38;5;196;48;2;10;20;30mX", 30, cap_run, &c);
    n = fytim_sgr_style_emit(&p.style, seq, sizeof seq);
    CHECK(n > 0 && n == strlen(seq));
    fytim_sgr_init(&q);
    fytim_sgr_feed(&q, seq, n, cap_run, &c);
    CHECK(q.style.attrs == p.style.attrs);
    CHECK(q.style.fg == p.style.fg);
    CHECK(q.style.bg == p.style.bg);
}

static void test_style_emit_default_and_tiny(void)
{
    struct fytim_sgr_parser p;
    char seq[64];
    fytim_sgr_init(&p);
    /* the default state emits nothing */
    CHECK(fytim_sgr_style_emit(&p.style, seq, sizeof seq) == 0);
    /* a would-truncate cap emits nothing rather than a broken escape */
    p.style.attrs = FYTIM_ATTR_BOLD | FYTIM_ATTR_UNDERLINE;
    CHECK(fytim_sgr_style_emit(&p.style, seq, 4) == 0);
    CHECK(fytim_sgr_style_emit(&p.style, NULL, 0) == 0);
    CHECK(fytim_sgr_style_emit(NULL, seq, sizeof seq) == 0);
    /* and with room it round-trips the attrs */
    CHECK(fytim_sgr_style_emit(&p.style, seq, sizeof seq) > 0);
    CHECK(strcmp(seq, "\x1b[1;4m") == 0);
}

/* Runs must never split a UTF-8 sequence: the internal chunking (256
 * bytes per pass) previously cut a 3-byte box-drawing glyph mid-sequence
 * and the consumer drew the torn bytes as garbage cells. Every delivered
 * run must start on a lead byte and end on a complete codepoint. */
static bool utf8_run(void *user, const char *text, size_t len,
                     const struct fytim_sgr_style *style)
{
    int *bad = user;
    size_t i = 0;
    (void)style;
    if(len && ((unsigned char)text[0] & 0xC0) == 0x80) (*bad)++;
    while(i < len){
        unsigned char c = (unsigned char)text[i];
        size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                    : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if(i + need > len){ (*bad)++; break; }
        i += need;
    }
    return true;
}

static void test_runs_never_split_utf8(void)
{
    struct fytim_sgr_parser p;
    char buf[1024];
    size_t o = 0;
    int i, bad = 0;
    /* a rule row: escapes plus 300 U+2500, crossing several 256-byte
     * chunk boundaries at every possible phase */
    o += (size_t)snprintf(buf + o, sizeof buf - o, "\x1b[2m");
    for(i = 0; i < 300 && o + 3 < sizeof buf; i++){
        memcpy(buf + o, "\xe2\x94\x80", 3);
        o += 3;
    }
    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, buf, o, utf8_run, &bad);
    CHECK(bad == 0);
}

static void test_null_and_empty_safe(void)
{
    struct capture c = {0};
    struct fytim_sgr_parser p;
    fytim_sgr_init(&p);
    fytim_sgr_feed(&p, NULL, 0, cap_run, &c);
    fytim_sgr_feed(&p, "", 0, cap_run, &c);
    fytim_sgr_feed(&p, "x", 1, NULL, &c);
    CHECK(c.n == 0);
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "plain_text", test_plain_text },
        { "bold_run", test_bold_run },
        { "combined_params", test_combined_params },
        { "truecolor", test_truecolor },
        { "indexed_color", test_indexed_color },
        { "style_persists_across_feeds", test_style_persists_across_feeds },
        { "escape_split_across_feeds", test_escape_split_across_feeds },
        { "disallowed_sequences_flagged", test_disallowed_sequences_flagged },
        { "osc8_hyperlink_allowed", test_osc8_hyperlink_allowed },
        { "other_osc_still_disallowed", test_other_osc_still_disallowed },
        { "malformed_input_safe", test_malformed_input_safe },
        { "overlong_escape_safe", test_overlong_escape_safe },
        { "null_and_empty_safe", test_null_and_empty_safe },
        { "runs_never_split_utf8", test_runs_never_split_utf8 },
        { "style_emit_roundtrip", test_style_emit_roundtrip },
        { "style_emit_default_and_tiny", test_style_emit_default_and_tiny },
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
