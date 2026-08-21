/*
 * test_fytim_surface_keys.c - keys handed to a surface.
 *
 * A surface holding the keys turns what the user types back into the bytes a
 * terminal would send, so the host can write them to the program it drives.
 * These cases pin the encoding and, as much, that the prompt stops seeing the
 * keys while a surface holds them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libfytimui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#define _GNU_SOURCE
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
    int in[2];
    int out[2];
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

static void h_keys(struct harness *h, const char *bytes, size_t len)
{
    CHECK(write(h->in[1], bytes, len) == (ssize_t)len);
}

static void h_drain(struct harness *h)
{
    char buf[65536];
    while(read(h->out[0], buf, sizeof buf) > 0)
        ;
}

/*
 * Type @in, pump, and collect every byte the surface was given. Returns the
 * length; @out is NUL terminated for the printable cases.
 */
static size_t type(struct harness *h, struct fytim_surface *s,
                   const char *in, size_t in_len, char *out, size_t cap)
{
    struct fytim_event ev;
    size_t n = 0;
    h_keys(h, in, in_len);
    CHECK(fytim_pump(h->ft) == FYTIM_OK);
    h_drain(h);
    while(fytim_next_event(h->ft, &ev)){
        if(ev.type != FYTIM_EVENT_SURFACE_KEYS) continue;
        CHECK(ev.surface == s);
        if(n + ev.text_len < cap){
            memcpy(out + n, ev.text, ev.text_len);
            n += ev.text_len;
        }
    }
    out[n] = '\0';
    return n;
}

static void test_typed_text_reaches_the_surface(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_surface_has_keys(s));
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    CHECK(type(&h, s, "abc", 3, got, sizeof got) == 3);
    CHECK(strcmp(got, "abc") == 0);
    fytim_surface_close(s);
    h_close(&h);
}

/* Enter is a carriage return, which is what a terminal sends. */
static void test_enter_is_a_return(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    CHECK(type(&h, s, "\r", 1, got, sizeof got) == 1);
    CHECK(got[0] == '\r');
    fytim_surface_close(s);
    h_close(&h);
}

/* A control chord is its control byte: ^C is 0x03, and it is NOT an
 * interrupt while the surface holds the keys. */
static void test_control_chord_is_a_control_byte(void)
{
    struct fytim_event ev;
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    int interrupts = 0;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    h_keys(&h, "\x03", 1);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    got[0] = '\0';
    while(fytim_next_event(h.ft, &ev)){
        if(ev.type == FYTIM_EVENT_INTERRUPT) interrupts++;
        if(ev.type == FYTIM_EVENT_SURFACE_KEYS && ev.text_len == 1)
            got[0] = ev.text[0];
    }
    CHECK(got[0] == '\x03');
    CHECK(interrupts == 0);
    fytim_surface_close(s);
    h_close(&h);
}

/* Every control byte reaches the program, not only the letters: ^\ (0x1c) is
 * a key a host reserves for itself, and it must arrive. */
static void test_every_control_byte_arrives(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    CHECK(type(&h, s, "\x1c", 1, got, sizeof got) == 1);
    CHECK(got[0] == '\x1c');
    CHECK(type(&h, s, "\x01", 1, got, sizeof got) == 1);
    CHECK(got[0] == '\x01');
    fytim_surface_close(s);
    h_close(&h);
}

/* An arrow key is the sequence a terminal sends for it. */
static void test_arrow_is_a_csi_sequence(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    size_t n;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    n = type(&h, s, "\x1b[A", 3, got, sizeof got);
    CHECK(n == 3);
    CHECK(memcmp(got, "\x1b[A", 3) == 0);
    n = type(&h, s, "\x1b[C", 3, got, sizeof got);
    CHECK(n == 3);
    CHECK(memcmp(got, "\x1b[C", 3) == 0);
    fytim_surface_close(s);
    h_close(&h);
}

/* Backspace, tab and delete keep the codes a terminal uses. */
static void test_editing_keys_keep_their_codes(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    CHECK(type(&h, s, "\x7f", 1, got, sizeof got) == 1);
    CHECK(got[0] == '\x7f');
    CHECK(type(&h, s, "\t", 1, got, sizeof got) == 1);
    CHECK(got[0] == '\t');
    CHECK(type(&h, s, "\x1b[3~", 4, got, sizeof got) == 4);
    CHECK(memcmp(got, "\x1b[3~", 4) == 0);
    fytim_surface_close(s);
    h_close(&h);
}

/* Text typed while a surface holds the keys must not land in the prompt. */
static void test_prompt_does_not_see_the_keys(void)
{
    struct harness h;
    struct fytim_surface *s;
    char got[64];
    const char *input;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    (void)type(&h, s, "hello", 5, got, sizeof got);
    input = fytim_input(h.ft);
    CHECK(input != NULL);
    CHECK(input[0] == '\0');
    fytim_surface_close(s);
    h_close(&h);
}

/* Giving the keys back restores the prompt. */
static void test_keys_return_to_the_prompt(void)
{
    struct harness h;
    struct fytim_surface *s;
    const char *input;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    CHECK(fytim_surface_set_keys(s, false) == FYTIM_OK);
    CHECK(!fytim_surface_has_keys(s));
    h_keys(&h, "hi", 2);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    input = fytim_input(h.ft);
    CHECK(input != NULL);
    CHECK(strcmp(input, "hi") == 0);
    fytim_surface_close(s);
    h_close(&h);
}

/* Closing a surface that holds the keys gives them back. */
static void test_close_returns_the_keys(void)
{
    struct harness h;
    struct fytim_surface *s;
    const char *input;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 2, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    fytim_surface_close(s);
    h_keys(&h, "ok", 2);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    input = fytim_input(h.ft);
    CHECK(input != NULL);
    CHECK(strcmp(input, "ok") == 0);
    h_close(&h);
}

/* One surface at a time: taking the keys takes them from the other. */
static void test_only_one_surface_holds_the_keys(void)
{
    struct harness h;
    struct fytim_surface *a, *b;
    char got[64];
    if(!h_open(&h)){ CHECK(0); return; }
    a = fytim_surface_open(h.ft, 1, 8);
    b = fytim_surface_open(h.ft, 1, 8);
    CHECK(fytim_surface_set_keys(a, true) == FYTIM_OK);
    CHECK(fytim_surface_set_keys(b, true) == FYTIM_OK);
    CHECK(!fytim_surface_has_keys(a));
    CHECK(fytim_surface_has_keys(b));
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    /* type() checks the surface on every event it sees. */
    CHECK(type(&h, b, "z", 1, got, sizeof got) == 1);
    fytim_surface_close(a);
    fytim_surface_close(b);
    h_close(&h);
}

/* A surface holding the keys is still drawn: taking the keys must not take
 * the frame with them. */
static void test_content_paints_while_holding_keys(void)
{
    struct fytim_cell cells[8];
    struct harness h;
    struct fytim_surface *s;
    char buf[16384];
    size_t n = 0;
    ssize_t r;
    int i;
    if(!h_open(&h)){ CHECK(0); return; }
    s = fytim_surface_open(h.ft, 3, 8);
    CHECK(fytim_surface_set_keys(s, true) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    h_drain(&h);
    memset(cells, 0, sizeof cells);
    for(i = 0; i < 8; i++){
        cells[i].chars[0] = 'K';
        cells[i].fg = FYTIM_COLOR_DEFAULT;
        cells[i].bg = FYTIM_COLOR_DEFAULT;
        cells[i].width = 1;
    }
    CHECK(fytim_surface_put_row(s, 0, cells, 8) == FYTIM_OK);
    CHECK(fytim_pump(h.ft) == FYTIM_OK);
    while(n < sizeof buf - 1 &&
          (r = read(h.out[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = '\0';
    CHECK(n > 0);
    CHECK(memmem(buf, n, "KKKKKKKK", 8) != NULL);
    fytim_surface_close(s);
    h_close(&h);
}

static void test_null_safety(void)
{
    CHECK(fytim_surface_set_keys(NULL, true) == FYTIM_ERR_INVALID);
    CHECK(!fytim_surface_has_keys(NULL));
}

struct case_ent { const char *name; void (*fn)(void); };
static const struct case_ent cases[] = {
    { "typed_text_reaches_the_surface",  test_typed_text_reaches_the_surface },
    { "enter_is_a_return",               test_enter_is_a_return },
    { "control_chord_is_a_control_byte", test_control_chord_is_a_control_byte },
    { "every_control_byte_arrives",      test_every_control_byte_arrives },
    { "arrow_is_a_csi_sequence",         test_arrow_is_a_csi_sequence },
    { "editing_keys_keep_their_codes",   test_editing_keys_keep_their_codes },
    { "prompt_does_not_see_the_keys",    test_prompt_does_not_see_the_keys },
    { "keys_return_to_the_prompt",       test_keys_return_to_the_prompt },
    { "close_returns_the_keys",          test_close_returns_the_keys },
    { "only_one_surface_holds_the_keys", test_only_one_surface_holds_the_keys },
    { "content_paints_while_holding_keys", test_content_paints_while_holding_keys },
    { "null_safety",                     test_null_safety },
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
