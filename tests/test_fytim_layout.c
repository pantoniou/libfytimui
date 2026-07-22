/*
 * test_fytim_layout.c - vertical band geometry.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <libfytimui/libfytimui-layout.h>

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

static const struct fytim_rect *band(const struct fytim_layout *l, enum fytim_band b)
{
    return &l->band[b];
}

/* Bands must tile the screen exactly: no gap, no overlap, no row past the
 * bottom edge. This is the invariant every other case leans on, so it is
 * checked for each height rather than asserted once. */
static void check_tiles(const struct fytim_layout *l, int w, int h)
{
    int i, y = 0;
    for(i = 0; i < FYTIM_BAND_COUNT; ++i){
        const struct fytim_rect *r = band(l, (enum fytim_band)i);
        if(r->h == 0) continue;          /* hidden bands take no rows */
        CHECK(r->x == 0);
        CHECK(r->w == w);
        CHECK(r->h > 0);
        CHECK(r->y == y);
        y += r->h;
    }
    CHECK(y == h);                       /* covers the screen exactly */
}

/* A roomy terminal: full chrome, transcript absorbs the remainder. */
static void test_full_size(void)
{
    struct fytim_layout l;
    CHECK(fytim_layout_compute(80, 24, &l));
    CHECK(l.width == 80 && l.height == 24);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 24 - FYTIM_CHROME_ROWS);
    CHECK(band(&l, FYTIM_BAND_HEADER)->h == FYTIM_HEADER_ROWS);
    CHECK(band(&l, FYTIM_BAND_SEP_TOP)->h == FYTIM_SEP_ROWS);
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == FYTIM_PROMPT_ROWS);
    CHECK(band(&l, FYTIM_BAND_SEP_BOTTOM)->h == FYTIM_SEP_ROWS);
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == FYTIM_STATUS_ROWS);
    check_tiles(&l, 80, 24);

    /* The transcript is on top and the status is flush with the bottom. */
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->y == 0);
    CHECK(band(&l, FYTIM_BAND_STATUS)->y +
          band(&l, FYTIM_BAND_STATUS)->h == 24);
}

/* Only the transcript grows with the terminal; chrome is fixed. */
static void test_only_transcript_grows(void)
{
    struct fytim_layout a, b;
    int i;
    CHECK(fytim_layout_compute(80, 24, &a));
    CHECK(fytim_layout_compute(80, 200, &b));
    CHECK(band(&b, FYTIM_BAND_TRANSCRIPT)->h ==
          band(&a, FYTIM_BAND_TRANSCRIPT)->h + 176);
    for(i = FYTIM_BAND_HEADER; i < FYTIM_BAND_COUNT; ++i)
        CHECK(band(&a, (enum fytim_band)i)->h ==
              band(&b, (enum fytim_band)i)->h);
    check_tiles(&b, 80, 200);
}

/* Width is carried through to every visible band, including narrow ones. */
static void test_width_propagates(void)
{
    struct fytim_layout l;
    int i;
    CHECK(fytim_layout_compute(1, 24, &l));
    CHECK(l.width == 1);
    for(i = 0; i < FYTIM_BAND_COUNT; ++i)
        if(band(&l, (enum fytim_band)i)->h > 0)
            CHECK(band(&l, (enum fytim_band)i)->w == 1);
    check_tiles(&l, 1, 24);
}

/* The exact height at which the transcript is squeezed to its last row. */
static void test_minimum_untruncated(void)
{
    struct fytim_layout l;
    CHECK(fytim_layout_compute(80, FYTIM_CHROME_ROWS + 1, &l));
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == FYTIM_STATUS_ROWS);
    check_tiles(&l, 80, FYTIM_CHROME_ROWS + 1);
}

/* Below that, chrome sheds least-important-first. The transcript keeps its
 * single row until there is nothing left to shed. */
static void test_degrades_in_priority_order(void)
{
    struct fytim_layout l;

    /* 6: one status row goes. */
    CHECK(fytim_layout_compute(80, 6, &l));
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == 1);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 6);

    /* 5: status gone entirely -- and the trailing separator goes with it
     * rather than ruling the bottom edge. Its row falls to the transcript. */
    CHECK(fytim_layout_compute(80, 5, &l));
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == 0);
    CHECK(band(&l, FYTIM_BAND_SEP_BOTTOM)->h == 0);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 2);
    check_tiles(&l, 80, 5);

    /* 4: header still present, transcript back to its last row. */
    CHECK(fytim_layout_compute(80, 4, &l));
    CHECK(band(&l, FYTIM_BAND_SEP_BOTTOM)->h == 0);
    CHECK(band(&l, FYTIM_BAND_HEADER)->h == 1);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 4);

    /* 3: header goes. */
    CHECK(fytim_layout_compute(80, 3, &l));
    CHECK(band(&l, FYTIM_BAND_HEADER)->h == 0);
    CHECK(band(&l, FYTIM_BAND_SEP_TOP)->h == 1);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 3);

    /* 2: the last separator goes, leaving transcript + prompt. */
    CHECK(fytim_layout_compute(80, 2, &l));
    CHECK(band(&l, FYTIM_BAND_SEP_TOP)->h == 0);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 1);
    check_tiles(&l, 80, 2);

    /* 1: the prompt is the last band standing -- not the transcript. */
    CHECK(fytim_layout_compute(80, 1, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 1);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 0);
    check_tiles(&l, 80, 1);
}

/* No separator may be the last visible band: a rule flush against the bottom
 * edge borders nothing. Checked at every height, not just where it first bit.
 * (Regression: the example rendered exactly this at h == 5.) */
static void test_no_trailing_separator(void)
{
    struct fytim_layout l;
    int h, i, last;
    for(h = 1; h <= 40; ++h){
        CHECK(fytim_layout_compute(80, h, &l));
        last = -1;
        for(i = 0; i < FYTIM_BAND_COUNT; ++i)
            if(band(&l, (enum fytim_band)i)->h > 0) last = i;
        CHECK(last != FYTIM_BAND_SEP_BOTTOM);
        CHECK(last != FYTIM_BAND_SEP_TOP);
        /* and the trailing rule never outlives what it separates */
        if(band(&l, FYTIM_BAND_STATUS)->h == 0)
            CHECK(band(&l, FYTIM_BAND_SEP_BOTTOM)->h == 0);
    }
}

/* The prompt survives every height the solver accepts. */
static void test_prompt_always_present(void)
{
    struct fytim_layout l;
    int h;
    for(h = 1; h <= 120; ++h){
        CHECK(fytim_layout_compute(80, h, &l));
        CHECK(band(&l, FYTIM_BAND_PROMPT)->h == FYTIM_PROMPT_ROWS);
        check_tiles(&l, 80, h);
    }
}

/* Degenerate geometry is rejected, and *out is left safe to draw from: every
 * band hidden, so a caller ignoring the return value draws nothing. */
static void test_degenerate_geometry_rejected(void)
{
    struct fytim_layout l;
    int i;
    const int cases[][2] = { {0, 24}, {80, 0}, {0, 0}, {-1, 24}, {80, -5} };
    size_t c;

    for(c = 0; c < sizeof cases / sizeof cases[0]; ++c){
        memset(&l, 0xff, sizeof l);
        CHECK(!fytim_layout_compute(cases[c][0], cases[c][1], &l));
        CHECK(l.width == 0 && l.height == 0);
        for(i = 0; i < FYTIM_BAND_COUNT; ++i){
            CHECK(band(&l, (enum fytim_band)i)->h == 0);
            CHECK(band(&l, (enum fytim_band)i)->w == 0);
        }
    }
}

/* A NULL out must not be dereferenced. */
static void test_null_out_safe(void)
{
    CHECK(!fytim_layout_compute(80, 24, NULL));
    CHECK(!fytim_layout_compute(0, 0, NULL));
}

/* Extreme dimensions must not overflow into negative or wrapped heights. */
static void test_extreme_dimensions_safe(void)
{
    struct fytim_layout l;
    int i;
    const int big = 1 << 24;

    CHECK(fytim_layout_compute(big, big, &l));
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == big - FYTIM_CHROME_ROWS);
    for(i = 0; i < FYTIM_BAND_COUNT; ++i){
        CHECK(band(&l, (enum fytim_band)i)->h >= 0);
        CHECK(band(&l, (enum fytim_band)i)->y >= 0);
    }
    check_tiles(&l, big, big);
}

/* The solver is pure: same inputs, same bytes out, no retained state. */
static void test_deterministic(void)
{
    struct fytim_layout a, b;
    memset(&a, 0x11, sizeof a);
    memset(&b, 0x22, sizeof b);
    CHECK(fytim_layout_compute(100, 37, &a));
    CHECK(fytim_layout_compute(1, 1, &b));       /* perturb between calls */
    CHECK(fytim_layout_compute(100, 37, &b));
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}

/* Multi-row prompt (fytim_layout_compute_ex): the prompt band grows for a
 * multiline edit, everything else keeps its place, and rows == 1 reproduces
 * fytim_layout_compute exactly. */
static void test_multirow_prompt_grows(void)
{
    struct fytim_layout l, one, plain;
    CHECK(fytim_layout_compute_ex(80, 24, 3, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 3);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 24 - (FYTIM_CHROME_ROWS + 2));
    CHECK(band(&l, FYTIM_BAND_HEADER)->h == FYTIM_HEADER_ROWS);
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == FYTIM_STATUS_ROWS);
    check_tiles(&l, 80, 24);

    CHECK(fytim_layout_compute_ex(80, 24, 1, &one));
    CHECK(fytim_layout_compute(80, 24, &plain));
    CHECK(memcmp(&one, &plain, sizeof one) == 0);

    /* degenerate row counts clamp to a single row */
    CHECK(fytim_layout_compute_ex(80, 24, 0, &one));
    CHECK(memcmp(&one, &plain, sizeof one) == 0);
    CHECK(fytim_layout_compute_ex(80, 24, -5, &one));
    CHECK(memcmp(&one, &plain, sizeof one) == 0);
}

/* Extra prompt rows are shed only after all other chrome is gone, and the
 * prompt never drops below one row; the transcript keeps its final row
 * against even an outsized request. */
static void test_multirow_prompt_sheds_last(void)
{
    struct fytim_layout l;
    int h;

    /* h=5: status+sep_bottom+header shed, prompt keeps all 3 rows */
    CHECK(fytim_layout_compute_ex(80, 5, 3, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 3);
    CHECK(band(&l, FYTIM_BAND_STATUS)->h == 0);
    CHECK(band(&l, FYTIM_BAND_HEADER)->h == 0);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 5);

    /* h=3: all other chrome gone, prompt finally gives a row */
    CHECK(fytim_layout_compute_ex(80, 3, 3, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 2);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 3);

    /* h=1: prompt alone, one row */
    CHECK(fytim_layout_compute_ex(80, 1, 3, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 1);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 0);
    check_tiles(&l, 80, 1);

    /* outsized request: prompt is capped so the transcript keeps one row */
    CHECK(fytim_layout_compute_ex(80, 24, 100, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 23);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 24);

    /* an extreme row count must not overflow the chrome arithmetic (UB
     * found by UBSan: INT_MAX + 5); it clamps like any outsized request */
    CHECK(fytim_layout_compute_ex(80, 24, 0x7fffffff, &l));
    CHECK(band(&l, FYTIM_BAND_PROMPT)->h == 23);
    CHECK(band(&l, FYTIM_BAND_TRANSCRIPT)->h == 1);
    check_tiles(&l, 80, 24);

    /* the tiling invariant holds for every height at rows == 4 */
    for(h = 1; h <= 40; ++h){
        CHECK(fytim_layout_compute_ex(80, h, 4, &l));
        CHECK(band(&l, FYTIM_BAND_PROMPT)->h >= 1);
        check_tiles(&l, 80, h);
    }
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "full_size", test_full_size },
        { "only_transcript_grows", test_only_transcript_grows },
        { "width_propagates", test_width_propagates },
        { "minimum_untruncated", test_minimum_untruncated },
        { "degrades_in_priority_order", test_degrades_in_priority_order },
        { "no_trailing_separator", test_no_trailing_separator },
        { "prompt_always_present", test_prompt_always_present },
        { "degenerate_geometry_rejected", test_degenerate_geometry_rejected },
        { "null_out_safe", test_null_out_safe },
        { "extreme_dimensions_safe", test_extreme_dimensions_safe },
        { "deterministic", test_deterministic },
        { "multirow_prompt_grows", test_multirow_prompt_grows },
        { "multirow_prompt_sheds_last", test_multirow_prompt_sheds_last },
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
