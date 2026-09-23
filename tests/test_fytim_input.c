/*
 * test_fytim_input.c - terminal replies in the input stream.
 *
 * A reply that arrives after its query gave up must not reach the host as
 * keys or text.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timui.h"

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

/* The events of a feed as a string: text as itself, and a key as
 * <kKEY:MODS:CODEPOINT>. */
struct seen {
    char text[512];
};

static void seen_cb(void *ctx, const TimuiEvent *ev)
{
    struct seen *s = (struct seen *)ctx;
    size_t n = strlen(s->text);
    char buf[64];

    if(ev->kind == TIMUI_EVENT_TEXT){
        snprintf(buf, sizeof buf, "%.*s", (int)ev->as.text.len, ev->as.text.ptr);
    }else if(ev->kind == TIMUI_EVENT_KEY){
        snprintf(buf, sizeof buf, "<k%d:%u:%u>", (int)ev->as.key.key,
                 (unsigned)ev->as.key.mods, (unsigned)ev->as.key.codepoint);
    }else{
        snprintf(buf, sizeof buf, "<e%d>", (int)ev->kind);
    }
    snprintf(s->text + n, sizeof s->text - n, "%s", buf);
}

static void feed(TimuiInputParser *p, struct seen *s, const char *bytes)
{
    timui_input_feed(p, bytes, strlen(bytes), seen_cb, s);
}

/* Feed each string of a NULL-terminated list as one read. */
static const char *run(const char *const *reads)
{
    static struct seen s;
    TimuiInputParser p;

    memset(&s, 0, sizeof s);
    timui_input_init(&p);
    for(; *reads; reads++)
        feed(&p, &s, *reads);
    return s.text;
}

#define RUN(...) run((const char *const[]){ __VA_ARGS__, NULL })

static void test_regression_a_late_osc_reply_is_not_typed(void)
{
    CHECK(!strcmp(RUN("a\x1b]11;rgb:ffff/ffff/ffff\x1b\\z"), "az"));
    CHECK(!strcmp(RUN("\x1b]10;rgb:0/0/0\az"), "z"));
}

static void test_regression_a_split_osc_reply_is_not_typed(void)
{
    CHECK(!strcmp(RUN("\x1b", "]11;rgb:", "ffff/0/0\x1b", "\\z"), "z"));
}

static void test_regression_a_late_dcs_reply_is_not_typed(void)
{
    CHECK(!strcmp(RUN("\x1bP>|xterm(390)\x1b\\z"), "z"));
    CHECK(!strcmp(RUN("\x1bP1+r524742=38\x1b\\z"), "z"));
}

static void test_regression_a_late_apc_reply_is_not_typed(void)
{
    CHECK(!strcmp(RUN("\x1b_Gi=31;OK\x1b\\z"), "z"));
}

static void test_regression_a_late_kitty_flags_reply_is_not_a_key(void)
{
    CHECK(!strcmp(RUN("\x1b[?1uz"), "z"));
}

static void test_regression_a_late_mode_report_is_not_typed(void)
{
    CHECK(!strcmp(RUN("\x1b[?2026;2$yz"), "z"));
    CHECK(!strcmp(RUN("\x1b[4;1$yz"), "z"));
}

static void test_late_csi_replies_are_dropped(void)
{
    CHECK(!strcmp(RUN("\x1b[?62;4;22cz"), "z"));
    CHECK(!strcmp(RUN("\x1b[?1;0;256Sz"), "z"));
    CHECK(!strcmp(RUN("\x1b[6;20;10tz"), "z"));
}

/* An Alt key whose second byte cannot start a reply stays a key. */
static void test_alt_keys_stay_keys(void)
{
    char want[64];

    snprintf(want, sizeof want, "<k%d:%u:%u>a", (int)TIMUI_KEY_UNKNOWN,
             (unsigned)TIMUI_MOD_ALT, (unsigned)']');
    CHECK(!strcmp(RUN("\x1b]a"), want));
    snprintf(want, sizeof want, "<k%d:%u:%u>x", (int)TIMUI_KEY_UNKNOWN,
             (unsigned)TIMUI_MOD_ALT, (unsigned)'P');
    CHECK(!strcmp(RUN("\x1bPx"), want));
    /* the second byte of a reply is still to come */
    CHECK(!strcmp(RUN("\x1b_"), ""));
}

/* An Alt key at the end of the input is decided when the Escape timeout
 * passes. */
static void test_a_lone_alt_key_resolves(void)
{
    struct seen s;
    TimuiInputParser p;
    char want[64];

    memset(&s, 0, sizeof s);
    timui_input_init(&p);
    timui_input_set_now(&p, 1000);
    feed(&p, &s, "\x1b]");
    CHECK(!strcmp(s.text, ""));
    timui_input_flush_esc(&p, 2000, seen_cb, &s);
    snprintf(want, sizeof want, "<k%d:%u:%u>", (int)TIMUI_KEY_UNKNOWN,
             (unsigned)TIMUI_MOD_ALT, (unsigned)']');
    CHECK(!strcmp(s.text, want));
}

/* A string that does not end is given up after its byte or time limit. */
static void test_an_unterminated_string_ends(void)
{
    static char big[4300];
    struct seen s;
    TimuiInputParser p;

    memset(big, 'a', sizeof big - 1);
    memcpy(big, "\x1b]1", 3);
    CHECK(strchr(RUN(big, "z"), 'z') != NULL);

    memset(&s, 0, sizeof s);
    timui_input_init(&p);
    timui_input_set_now(&p, 1000);
    feed(&p, &s, "\x1b]11;rgb:");
    timui_input_flush_esc(&p, 5000, seen_cb, &s);
    feed(&p, &s, "z");
    CHECK(!strcmp(s.text, "z"));
}

/* Keys that end in the same CSI final byte as a reply still decode. */
static void test_keys_still_decode(void)
{
    char want[64];

    CHECK(strstr(RUN("\x1b[97u"), ":0:97>") != NULL);
    snprintf(want, sizeof want, "<k%d:%u:%u>", (int)TIMUI_KEY_UP, 0u, 0u);
    CHECK(!strcmp(RUN("\x1b[A"), want));
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "regression_a_late_osc_reply_is_not_typed",
          test_regression_a_late_osc_reply_is_not_typed },
        { "regression_a_split_osc_reply_is_not_typed",
          test_regression_a_split_osc_reply_is_not_typed },
        { "regression_a_late_dcs_reply_is_not_typed",
          test_regression_a_late_dcs_reply_is_not_typed },
        { "regression_a_late_apc_reply_is_not_typed",
          test_regression_a_late_apc_reply_is_not_typed },
        { "regression_a_late_kitty_flags_reply_is_not_a_key",
          test_regression_a_late_kitty_flags_reply_is_not_a_key },
        { "regression_a_late_mode_report_is_not_typed",
          test_regression_a_late_mode_report_is_not_typed },
        { "late_csi_replies_are_dropped", test_late_csi_replies_are_dropped },
        { "alt_keys_stay_keys", test_alt_keys_stay_keys },
        { "a_lone_alt_key_resolves", test_a_lone_alt_key_resolves },
        { "an_unterminated_string_ends", test_an_unterminated_string_ends },
        { "keys_still_decode", test_keys_still_decode },
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
    return failures ? 1 : 0;
}
