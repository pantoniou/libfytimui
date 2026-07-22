/*
 * test_inline.c - inline (non-alt-screen) band rendering.
 *
 * Covers the TIMUI_FLAG_INLINE local-only core delta (docs/vendor-deltas.md):
 * instead of owning the whole alt screen, timui manages a band of N rows
 * anchored at the physical cursor. Finished transcript lines are committed
 * above the band into the terminal's native scrollback, so scrolling, mouse
 * selection and copy all belong to the terminal again.
 *
 * The contract under test: between frames the cursor sits at the band anchor
 * (column 0). A paint is "\r" + erase-down + the rows (CRLF-separated) + a
 * CUU back to the anchor; a commit is "\r" + erase-down + the finished lines,
 * each ending "\r\n", leaving the cursor at the new anchor.
 *
 * Kept out of the upstream test files on purpose: a re-vendor overwrites
 * those, and these cases must survive it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "timui.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int find_(TimuiStr s, size_t from, const char *needle){
    size_t n = strlen(needle);
    size_t i;
    if(n == 0 || s.len < n) return -1;
    for(i = from; i + n <= s.len; i++)
        if(memcmp(s.ptr + i, needle, n) == 0) return (int)i;
    return -1;
}
static int has_(TimuiStr s, const char *needle){ return find_(s, 0, needle) >= 0; }

/* Inline mode must never touch the alt screen, even when the caller also set
 * TIMUI_FLAG_ALT_SCREEN -- inline wins, or committed lines would land in a
 * buffer that vanishes on exit. */
void test_inline_enter_suppresses_alt_screen(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_INLINE | TIMUI_FLAG_ALT_SCREEN,
                       TIMUI_STR_LIT(""));
    out = timui_fake_output(&f);
    TIMUI_CHECK(!has_(out, "\x1b[?1049h"));
    TIMUI_CHECK(has_(out, "\x1b[?7l"));      /* auto-wrap still disabled */
    timui_fake_clear_output(&f);
    timui_screen_exit(&t, &m);
    out = timui_fake_output(&f);
    TIMUI_CHECK(!has_(out, "\x1b[?1049l"));
    timui_fake_destroy(&f);
}

/* On exit the band (prompt/status chrome) is erased so only committed history
 * remains -- the clean-CLI ending. The erase must precede the cursor show. */
void test_inline_exit_erases_band(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;
    int erase, show;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_INLINE, TIMUI_STR_LIT(""));
    timui_fake_clear_output(&f);
    timui_screen_exit(&t, &m);
    out = timui_fake_output(&f);
    erase = find_(out, 0, "\r\x1b[J");
    show  = find_(out, 0, "\x1b[?25h");
    TIMUI_CHECK(erase >= 0);
    TIMUI_CHECK(show > erase);
    timui_fake_destroy(&f);
}

/* A non-inline exit must NOT erase anything: pins that the erase is the new
 * flag's doing and not pre-existing behaviour. */
void test_plain_exit_does_not_erase(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiScreenMode m;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_screen_enter(&t, &m, TIMUI_FLAG_ALT_SCREEN, TIMUI_STR_LIT(""));
    timui_fake_clear_output(&f);
    timui_screen_exit(&t, &m);
    out = timui_fake_output(&f);
    TIMUI_CHECK(!has_(out, "\x1b[J"));
    timui_fake_destroy(&f);
}

/* Two-row paint: home, erase down, row 0, CRLF, row 1, then re-home with a
 * one-row cursor-up. Order is asserted, not just presence. */
void test_inline_paint_two_rows(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer buf;
    TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
    TimuiStr out;
    int home, ab, crlf, cd, up;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&buf, 5, 2, &al) == TIMUI_OK);
    timui_draw_text(&buf, 0, 0, TIMUI_STR_LIT("ab"), st);
    timui_draw_text(&buf, 0, 1, TIMUI_STR_LIT("cd"), st);
    timui_inline_paint(&t, &buf);
    out = timui_fake_output(&f);
    /* overwrite-in-place: each row is EL-cleared then written; the paint
     * NEVER erases the band wholesale (\x1b[J blanks everything below the
     * anchor for a frame -- that is the flicker this contract removes) */
    TIMUI_CHECK(!has_(out, "\x1b[J"));
    home = find_(out, 0, "\x1b[0m\r");
    TIMUI_CHECK(home >= 0);
    TIMUI_CHECK(find_(out, (size_t)home, "\x1b[K") >= 0);   /* row cleared */
    ab   = find_(out, (size_t)home, "ab");
    TIMUI_CHECK(ab > home);
    crlf = find_(out, (size_t)ab, "\r\n");
    TIMUI_CHECK(crlf > ab);
    cd   = find_(out, (size_t)crlf, "cd");
    TIMUI_CHECK(cd > crlf);
    up   = find_(out, (size_t)cd, "\x1b[1A");
    TIMUI_CHECK(up > cd);
    timui_cells_destroy(&buf);
    timui_fake_destroy(&f);
}

/* A one-row band needs no line feed and no cursor-up: a stray LF here would
 * scroll the terminal every frame. */
void test_inline_paint_single_row(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer buf;
    TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&buf, 5, 1, &al) == TIMUI_OK);
    timui_draw_text(&buf, 0, 0, TIMUI_STR_LIT("hi"), st);
    timui_inline_paint(&t, &buf);
    out = timui_fake_output(&f);
    TIMUI_CHECK(has_(out, "hi"));
    TIMUI_CHECK(!has_(out, "\n"));
    TIMUI_CHECK(!has_(out, "A"));   /* no CUU (and 'A' is not in the content) */
    TIMUI_CHECK(!has_(out, "\x1b[J"));   /* overwrite, never band-erase */
    timui_fake_destroy(&f);
    timui_cells_destroy(&buf);
}

/* Every row separator is emitted with the style closed: the LF may scroll
 * the screen, and BCE terminals fill the revealed line with the CURRENT
 * background -- an open row background would bleed into the next row. */
void test_inline_paint_resets_before_row_break(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer buf;
    TimuiStyle st = timui_style_make(0xFFFFFFu, 0x45475Au, 0);
    TimuiStr out;
    int i;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&buf, 4, 3, &al) == TIMUI_OK);
    for(i = 0; i < 4; i++){                    /* full-width coloured rows */
        timui_draw_text(&buf, i, 0, TIMUI_STR_LIT("x"), st);
        timui_draw_text(&buf, i, 1, TIMUI_STR_LIT("y"), st);
    }
    timui_inline_paint(&t, &buf);
    out = timui_fake_output(&f);
    {   /* every "\r\n" must be directly preceded by the SGR reset */
        size_t j;
        int breaks = 0;
        for(j = 1; j + 1 < out.len; j++){
            if(out.ptr[j] == '\r' && out.ptr[j+1] == '\n'){
                breaks++;
                TIMUI_CHECK(j >= 4 && memcmp(out.ptr + j - 4, "\x1b[0m", 4) == 0);
            }
        }
        TIMUI_CHECK(breaks == 2);
    }
    timui_cells_destroy(&buf);
    timui_fake_destroy(&f);
}

/* Styled cells emit SGR, and the paint ends with a full reset so committed
 * lines written afterwards do not inherit band colours. */
void test_inline_paint_resets_style(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer buf;
    TimuiStyle st = timui_style_make(0xFF0000u, TIMUI_COLOR_DEFAULT, 0);
    TimuiStr out;
    int text, reset;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&buf, 5, 1, &al) == TIMUI_OK);
    timui_draw_text(&buf, 0, 0, TIMUI_STR_LIT("x"), st);
    timui_inline_paint(&t, &buf);
    out = timui_fake_output(&f);
    TIMUI_CHECK(has_(out, "38;2;255;0;0"));
    text  = find_(out, 0, "x");
    reset = text >= 0 ? find_(out, (size_t)text, "\x1b[0m") : -1;
    TIMUI_CHECK(text >= 0);
    TIMUI_CHECK(reset > text);
    timui_cells_destroy(&buf);
    timui_fake_destroy(&f);
}

/* Degenerate input must be quietly safe and emit nothing. */
void test_inline_paint_null_safe(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer zero;
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    memset(&zero, 0, sizeof zero);
    timui_inline_paint(NULL, NULL);
    timui_inline_paint(&t, NULL);
    timui_inline_paint(NULL, &zero);
    timui_inline_paint(&t, &zero);            /* no cells: nothing to paint */
    out = timui_fake_output(&f);
    TIMUI_CHECK(out.len == 0);
    timui_fake_destroy(&f);
}

/* Commit: each finished line OVERWRITES a band row in place -- written,
 * style-closed, EL-cleared, then "\r\n" to scroll it into scrollback. No
 * band erase: the blank-then-repaint flash was the flicker. A trailing
 * newline in the input does not produce an extra blank line. */
void test_inline_commit_lines(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiStr out;
    int home, one, two;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_inline_commit_emit(&t, TIMUI_STR_LIT("one\ntwo\n"));
    out = timui_fake_output(&f);
    TIMUI_CHECK(!has_(out, "\x1b[J"));
    home = find_(out, 0, "\x1b[0m\r");           /* style closed, column 0 */
    TIMUI_CHECK(home >= 0);
    one = find_(out, (size_t)home, "one\x1b[0m\x1b[K\r\n");
    TIMUI_CHECK(one > home);
    two = find_(out, (size_t)one, "two\x1b[0m\x1b[K\r\n");
    TIMUI_CHECK(two > one);
    TIMUI_CHECK(find_(out, (size_t)two + 12, "\r\n") < 0);  /* no extra blank */
    timui_fake_destroy(&f);
}

/* An empty commit is a no-op: erasing the band for nothing would flicker. */
void test_inline_commit_empty_is_noop(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiStr empty = {NULL, 0};

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    timui_inline_commit_emit(&t, empty);
    timui_inline_commit_emit(NULL, TIMUI_STR_LIT("x"));
    TIMUI_CHECK(timui_fake_output(&f).len == 0);
    timui_fake_destroy(&f);
}

/* timui_open with the inline flag sizes the frame as width x inline_rows and
 * rejects a missing row count; the frame paint goes through the transport. */
void test_inline_open_sizes_band(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];

    /* inline without a row count is a configuration error */
    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 0;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_ERR_INVALID_ARGUMENT);

    cfg.inline_rows = 4;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        if(f){
            TIMUI_CHECK(timui_height(f) == 4);
            TIMUI_CHECK(timui_width(f) > 0);
            timui_label(f, 0, 0, TIMUI_STR_LIT("band"),
                        timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0));
            timui_end(f);
        }
        {   /* the frame must have been painted through the transport */
            char buf[4096];
            ssize_t n;
            fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
            n = read(fds_out[0], buf, sizeof buf - 1);
            TIMUI_CHECK(n > 0);
            if(n > 0){
                TimuiStr s = { buf, (size_t)n };
                TIMUI_CHECK(has_(s, "\r\x1b[J"));
                TIMUI_CHECK(has_(s, "band"));
            }
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Inline hardware cursor: a focused input field requests the cursor at its
 * cell; timui_end must park the terminal cursor there (relative moves from
 * the anchor + show), emit nothing when neither cells nor cursor changed,
 * move it without repainting when only the cursor changed, and un-park back
 * to the anchor before the erase on close -- otherwise band rows above the
 * parked cursor survive the exit erase. */
void test_inline_parks_cursor_for_focused_input(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    char text[32] = "ab";
    TimuiInputState st = { text, sizeof text, 2, 0 };
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 3;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiId id = TIMUI_ID("prompt");
        TimuiRect r = TIMUI_RECT(2, 1, 8, 1);

        /* frame 1: focused field, text "ab", cursor after it -> park at
         * (2 + 2, 1): down 1, right 4, show */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_set_focus(f, id);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            int down = find_(s, 0, "\x1b[1B");
            int right = down >= 0 ? find_(s, (size_t)down, "\x1b[4C") : -1;
            int show = right >= 0 ? find_(s, (size_t)right, "\x1b[?25h") : -1;
            TIMUI_CHECK(down >= 0);
            TIMUI_CHECK(right > down);
            TIMUI_CHECK(show > right);
        }

        /* frame 2: identical -> total silence */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n <= 0);

        /* frame 3: cursor moved left, cells identical -> a move, no repaint */
        st.cursor = 1;
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[1A"));   /* un-park to the anchor first */
            TIMUI_CHECK(has_(s, "\x1b[3C"));
            TIMUI_CHECK(!has_(s, "\x1b[J"));
            TIMUI_CHECK(!has_(s, "ab"));       /* no cell rewrites at all */
        }

        /* close while parked: the cursor must return to the anchor so the
         * exit erase (emitted only on a real tty, before which this un-park
         * sits in timui_restore_terminal) covers the whole band. On a pipe
         * only the un-park itself is observable. */
        timui_close(ui);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[1A"));
        }
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* While a repaint is in flight the parked cursor is hidden, then re-shown
 * after re-parking -- without ?2026 the cursor would otherwise be seen
 * jumping to the anchor and across the rewritten cells on every keystroke. */
void test_inline_cursor_hidden_across_repaint(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    char text[32] = "a";
    TimuiInputState st = { text, sizeof text, 1, 0 };
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiId id = TIMUI_ID("prompt");
        TimuiRect r = TIMUI_RECT(0, 1, 8, 1);

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_set_focus(f, id);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        /* a keystroke: cells change while the cursor is parked */
        text[1] = 'b'; text[2] = '\0'; st.cursor = 2;
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            int hide = find_(s, 0, "\x1b[?25l");
            int glyph = hide >= 0 ? find_(s, (size_t)hide, "b") : -1;
            int show = glyph >= 0 ? find_(s, (size_t)glyph, "\x1b[?25h") : -1;
            TIMUI_CHECK(hide >= 0);
            TIMUI_CHECK(glyph > hide);
            TIMUI_CHECK(show > glyph);
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Losing the cursor (focus cleared) hides it again and returns to the
 * anchor, so the next paint's row arithmetic stays valid. */
void test_inline_cursor_hidden_when_unfocused(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    char text[32] = "x";
    TimuiInputState st = { text, sizeof text, 1, 0 };
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiId id = TIMUI_ID("prompt");
        TimuiRect r = TIMUI_RECT(0, 1, 8, 1);

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_set_focus(f, id);
        timui_input_field(f, id, r, &st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_set_focus(f, 0);                  /* drop focus */
        timui_input_field(f, id, r, &st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[?25l"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Row-diff paint: when only one row changed, only that row is rewritten --
 * no band erase (\x1b[J would blank everything and shimmer), unchanged rows
 * untouched, the changed row cleared to EOL with EL, and the cursor moved
 * down and back up with relative steps that cancel out. */
void test_inline_diff_paint_touches_only_changed_row(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer prev, curr;
    TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
    TimuiStr out;
    int down, xy, el, up;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&prev, 8, 3, &al) == TIMUI_OK);
    TIMUI_CHECK(timui_cells_init(&curr, 8, 3, &al) == TIMUI_OK);
    timui_draw_text(&prev, 0, 0, TIMUI_STR_LIT("aaa"), st);
    timui_draw_text(&prev, 0, 1, TIMUI_STR_LIT("bbb"), st);
    timui_draw_text(&prev, 0, 2, TIMUI_STR_LIT("ccc"), st);
    timui_draw_text(&curr, 0, 0, TIMUI_STR_LIT("aaa"), st);
    timui_draw_text(&curr, 0, 1, TIMUI_STR_LIT("XY"), st);
    timui_draw_text(&curr, 0, 2, TIMUI_STR_LIT("ccc"), st);
    timui_inline_paint_diff(&t, &prev, &curr);
    out = timui_fake_output(&f);
    TIMUI_CHECK(!has_(out, "\x1b[J"));          /* never erase the band */
    TIMUI_CHECK(!has_(out, "\x1b[K"));          /* span rewrite needs no EL */
    TIMUI_CHECK(!has_(out, "aaa"));             /* unchanged rows untouched */
    TIMUI_CHECK(!has_(out, "ccc"));
    down = find_(out, 0, "\x1b[1B");            /* step to row 1 ... */
    TIMUI_CHECK(down >= 0);
    xy = down >= 0 ? find_(out, (size_t)down, "XY ") : -1;
    TIMUI_CHECK(xy > down);          /* 'bbb'->'XY': the blanked cell too */
    el = xy;                                    /* (name kept: span end) */
    up = el >= 0 ? find_(out, (size_t)el, "\x1b[1A") : -1;
    TIMUI_CHECK(up > el);                       /* ... and back to the anchor */
    timui_cells_destroy(&prev);
    timui_cells_destroy(&curr);
    timui_fake_destroy(&f);
}

/* A change in row 0 needs no cursor movement at all beyond "\r", and equal
 * or size-mismatched buffers emit nothing (mismatch means the band moved --
 * the full-repaint path owns that case). */
void test_inline_diff_paint_edges(void){
    TimuiAllocator al = timui_default_allocator();
    TimuiFakeTransport f;
    TimuiTransport t;
    TimuiCellBuffer prev, curr, small;
    TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
    TimuiStr out;

    TIMUI_CHECK(timui_fake_init(&f, &al) == TIMUI_OK);
    t = timui_fake_transport(&f);
    TIMUI_CHECK(timui_cells_init(&prev, 8, 2, &al) == TIMUI_OK);
    TIMUI_CHECK(timui_cells_init(&curr, 8, 2, &al) == TIMUI_OK);
    TIMUI_CHECK(timui_cells_init(&small, 8, 1, &al) == TIMUI_OK);

    /* equal -> silence */
    timui_draw_text(&prev, 0, 0, TIMUI_STR_LIT("p"), st);
    timui_draw_text(&curr, 0, 0, TIMUI_STR_LIT("p"), st);
    timui_inline_paint_diff(&t, &prev, &curr);
    TIMUI_CHECK(timui_fake_output(&f).len == 0);

    /* size mismatch -> silence (full repaint owns it) */
    timui_inline_paint_diff(&t, &small, &curr);
    TIMUI_CHECK(timui_fake_output(&f).len == 0);

    /* NULL-safety */
    timui_inline_paint_diff(NULL, &prev, &curr);
    timui_inline_paint_diff(&t, NULL, &curr);
    timui_inline_paint_diff(&t, &prev, NULL);
    TIMUI_CHECK(timui_fake_output(&f).len == 0);

    /* row 0, cell 0 change: no cursor steps at all, just "\r" + the cell */
    timui_draw_text(&curr, 0, 0, TIMUI_STR_LIT("q"), st);
    timui_inline_paint_diff(&t, &prev, &curr);
    out = timui_fake_output(&f);
    TIMUI_CHECK(has_(out, "q"));
    TIMUI_CHECK(!has_(out, "\x1b[K"));
    TIMUI_CHECK(!has_(out, "B"));
    TIMUI_CHECK(!has_(out, "A"));
    TIMUI_CHECK(!has_(out, "C"));

    /* mid-row change: skip the unchanged prefix with CUF, emit only the
     * changed cell, leave the unchanged suffix alone */
    timui_fake_clear_output(&f);
    timui_draw_text(&prev, 0, 1, TIMUI_STR_LIT("abcdef"), st);
    timui_draw_text(&curr, 0, 0, TIMUI_STR_LIT("q"), st);   /* row 0 equal now */
    timui_draw_text(&prev, 0, 0, TIMUI_STR_LIT("q"), st);
    timui_draw_text(&curr, 0, 1, TIMUI_STR_LIT("abXdef"), st);
    timui_inline_paint_diff(&t, &prev, &curr);
    out = timui_fake_output(&f);
    TIMUI_CHECK(has_(out, "\x1b[2C"));   /* skip "ab" */
    TIMUI_CHECK(has_(out, "X"));
    TIMUI_CHECK(!has_(out, "a"));
    TIMUI_CHECK(!has_(out, "d"));        /* suffix untouched */
    timui_cells_destroy(&prev);
    timui_cells_destroy(&curr);
    timui_cells_destroy(&small);
    timui_fake_destroy(&f);
}

/* Through the ui: a prompt-row edit must NOT erase the band. This is the
 * typing-shimmer regression -- every keystroke used to go erase-down + full
 * rewrite, blanking the band for a frame on terminals without ?2026. */
void test_inline_end_diffs_rows(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 3;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("head"), st);
        timui_label(f, 0, 1, TIMUI_STR_LIT("> a"), st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("head"), st);
        timui_label(f, 0, 1, TIMUI_STR_LIT("> ab"), st);   /* one keystroke */
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            int cuf, b;
            TIMUI_CHECK(!has_(s, "\x1b[J"));   /* no band erase */
            TIMUI_CHECK(!has_(s, "head"));     /* unchanged row untouched */
            /* span diff: one keystroke = skip "> a" (CUF 3), emit "b" */
            cuf = find_(s, 0, "\x1b[3C");
            TIMUI_CHECK(cuf >= 0);
            b = cuf >= 0 ? find_(s, (size_t)cuf, "b") : -1;
            TIMUI_CHECK(b > cuf);
            TIMUI_CHECK(!has_(s, "> a"));      /* prefix not retransmitted */
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* An unchanged band must emit NOTHING: repainting identical content every
 * frame is the flicker the inline mode exists to avoid. A changed band must
 * then paint again. */
void test_inline_end_skips_unchanged_frame(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 3;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);
        int i;

        /* frame 1: content -> paints */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("same"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);

        /* frames 2..4: identical content -> silence */
        for(i = 0; i < 3; i++){
            TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
            timui_label(f, 0, 0, TIMUI_STR_LIT("same"), st);
            timui_end(f);
        }
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n <= 0);

        /* frame 5: a change -> paints again */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("diff"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "diff"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Commits are batched into the next timui_end so the erase + committed lines
 * + band repaint reach the terminal as ONE update (inside one sync bracket):
 * nothing is written at commit time, and the following end repaints even if
 * the band content itself did not change. */
void test_inline_commit_batches_into_end(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);

        /* settle: paint the band once, drain the pipe */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        /* commit alone writes nothing... */
        timui_inline_commit(ui, TIMUI_STR_LIT("scrolled"));
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n <= 0);

        /* ...the next end emits the lines AND repaints the unchanged band */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            int lines = find_(s, 0, "scrolled\x1b[0m\x1b[K\r\n");
            int band;
            TIMUI_CHECK(lines >= 0);
            band = lines >= 0 ? find_(s, (size_t)lines, "band") : -1;
            TIMUI_CHECK(band > lines);   /* repaint follows the commit */
            /* the band was settled: the flush must overwrite in place,
             * never blank the band first (the flicker regression) */
            TIMUI_CHECK(!has_(s, "\x1b[J"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* timui_inline_commit routes through the ui's own transport. */
void test_inline_commit_via_ui(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];

    timui_inline_commit(NULL, TIMUI_STR_LIT("x"));   /* NULL ui is safe */

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        char buf[4096];
        ssize_t n;
        TimuiFrame *f = NULL;
        timui_inline_commit(ui, TIMUI_STR_LIT("hello"));
        /* commits are batched into the next end (one atomic update) */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_end(f);
        fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "hello\x1b[0m\x1b[K\r\n"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Suspend releases the terminal for a child process (an external editor):
 * the band is erased with the style closed, frames while suspended write
 * nothing, and resume forces a full repaint. Double suspend and resume
 * without suspend are invalid. */
void test_inline_suspend_resume(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_suspend(NULL) == TIMUI_ERR_INVALID_ARGUMENT);
    TIMUI_CHECK(timui_resume(NULL) == TIMUI_ERR_INVALID_ARGUMENT);
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);

        TIMUI_CHECK(timui_resume(ui) == TIMUI_ERR_INVALID_ARGUMENT);  /* not suspended */

        /* settle: paint once, drain */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        /* suspend: closes the style and erases the band */
        TIMUI_CHECK(timui_suspend(ui) == TIMUI_OK);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[0m\r\x1b[J"));
        }
        TIMUI_CHECK(timui_suspend(ui) == TIMUI_ERR_INVALID_ARGUMENT); /* double */

        /* a frame while suspended must not touch the terminal */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("hidden"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n <= 0);

        /* resume repaints in full even though the cells did not change */
        TIMUI_CHECK(timui_resume(ui) == TIMUI_OK);
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "band"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* regression: the trust protocol behind flicker-free streaming. The FIRST
 * paint after open claims the band with an erase (the screen below the
 * anchor is the shell's); once settled, commit flushes and repaints
 * OVERWRITE in place -- no \x1b[J anywhere. A full redraw (^L) or resume
 * distrusts the screen again and erases once. */
void test_inline_trust_protocol(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[8192];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 2;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);

        /* first paint: untrusted screen, erase-down once */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[0m\r\x1b[J"));
        }

        /* settled: a commit + repaint must not erase */
        timui_inline_commit(ui, TIMUI_STR_LIT("done"));
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "done\x1b[0m\x1b[K\r\n"));
            TIMUI_CHECK(!has_(s, "\x1b[J"));
        }

        /* ^L distrusts the screen: erase once more */
        timui_full_redraw(ui);
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("band"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            TIMUI_CHECK(has_(s, "\x1b[0m\r\x1b[J"));
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* regression: when the band SHRINKS (a work-band retired), the rows it no
 * longer covers are cleaned up with a targeted erase below the new band --
 * inside the same flush, after the repaint -- not by blanking everything. */
void test_inline_shrink_cleans_below(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char buf[16384];
    ssize_t n;

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    fcntl(fds_out[0], F_SETFL, O_NONBLOCK);
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_INLINE | TIMUI_FLAG_EXTERNAL_POLL;
    cfg.inline_rows = 4;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiStyle st = timui_style_make(TIMUI_COLOR_DEFAULT, TIMUI_COLOR_DEFAULT, 0);

        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("tall"), st);
        timui_label(f, 0, 3, TIMUI_STR_LIT("last"), st);
        timui_end(f);
        while(read(fds_out[0], buf, sizeof buf - 1) > 0){}

        TIMUI_CHECK(timui_ui_resize(ui, 20, 2) == TIMUI_OK);
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        timui_label(f, 0, 0, TIMUI_STR_LIT("short"), st);
        timui_end(f);
        n = read(fds_out[0], buf, sizeof buf - 1);
        TIMUI_CHECK(n > 0);
        if(n > 0){
            TimuiStr s = { buf, (size_t)n };
            int paint = find_(s, 0, "short");
            int shift;
            TIMUI_CHECK(paint >= 0);
            /* BOTTOM-anchored shrink: the 2 stale rows are the TOP of the
             * old extent -- each is cleared and stepped over BEFORE the
             * paint, advancing the anchor so the band's bottom row (the
             * chrome) never moves. Nothing is erased below. */
            shift = find_(s, 0, "\r\x1b[0m\x1b[K\x1b[B\r\x1b[0m\x1b[K\x1b[B");
            TIMUI_CHECK(shift >= 0 && shift < paint);
            TIMUI_CHECK(find_(s, 0, "\x1b[2B\r\x1b[J\x1b[2A") < 0);
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}
