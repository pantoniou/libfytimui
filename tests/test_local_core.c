/*
 * test_local_core.c - local-only core cases that fit no other local file.
 *
 * Kept out of the upstream test files on purpose: a re-vendor overwrites
 * those, and these cases must survive it (docs/vendor-deltas.md).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"
#include "timui.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* timui_key_codepoint: the parser turns a control byte into KEY_UNKNOWN +
 * MOD_CTRL + a codepoint, but the codepoint never reached the application,
 * so unhandled chords like Ctrl-L (refresh) were undetectable. The accessor
 * reports the frame's key codepoint and resets between frames. */
void test_key_codepoint_accessor(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];

    TIMUI_CHECK(timui_key_codepoint(NULL) == 0);

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TIMUI_CHECK(write(fds_in[1], "\x0c", 1) == 1);   /* Ctrl-L */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        TIMUI_CHECK(timui_key_pressed_mods(f, TIMUI_KEY_UNKNOWN, TIMUI_MOD_CTRL));
        TIMUI_CHECK(timui_key_codepoint(f) == (uint32_t)'l');
        timui_end(f);
        /* a quiet frame reports no codepoint */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        TIMUI_CHECK(timui_key_codepoint(f) == 0);
        timui_end(f);
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* Shared harness: open a pipe-backed ui, feed raw bytes, run one focused
 * text-area frame over the given state. */
static void ta_frame_(Timui *ui, int in_fd, const char *bytes,
                      TimuiTextAreaState *st, TimuiTextAreaResult *out){
    TimuiFrame *f = NULL;
    if(bytes && *bytes)
        TIMUI_CHECK(write(in_fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
    TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
    timui_set_focus(f, TIMUI_ID("ta"));
    *out = timui_text_area_mut(f, TIMUI_ID("ta"), TIMUI_RECT(0, 0, 40, 4), st, 0);
    timui_end(f);
}
static Timui *ta_open_(int fds_in[2], int fds_out[2]){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    if(pipe(fds_in) != 0) return NULL;
    if(pipe(fds_out) != 0){ close(fds_in[0]); close(fds_in[1]); return NULL; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    if(timui_open(&cfg, &ui) != TIMUI_OK) ui = NULL;
    return ui;
}
static void ta_close_(Timui *ui, int fds_in[2], int fds_out[2]){
    if(ui) timui_close(ui);
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* readline kills in the text area: Ctrl-K to end of line, Ctrl-U to line
 * start, Ctrl-W kills the word before the cursor. Line-scoped in a
 * multiline buffer -- text on other lines survives. */
void test_textarea_kill_ops(void){
    int fds_in[2], fds_out[2];
    Timui *ui = ta_open_(fds_in, fds_out);
    char text[64];
    TimuiTextAreaState st = { text, sizeof text, 0, 0 };
    TimuiTextAreaResult res;

    TIMUI_CHECK(ui != NULL);
    if(!ui){ return; }

    strcpy(text, "abc def\nxyz"); st.cursor = 5; st.scroll_y = 0;
    ta_frame_(ui, fds_in[1], "\x0b", &st, &res);          /* Ctrl-K */
    TIMUI_CHECK(strcmp(text, "abc d\nxyz") == 0);
    TIMUI_CHECK(st.cursor == 5);
    TIMUI_CHECK(res.changed);

    strcpy(text, "abc def\nxyz"); st.cursor = 5; st.scroll_y = 0;
    ta_frame_(ui, fds_in[1], "\x15", &st, &res);          /* Ctrl-U */
    TIMUI_CHECK(strcmp(text, "ef\nxyz") == 0);
    TIMUI_CHECK(st.cursor == 0);

    strcpy(text, "abc def"); st.cursor = 7; st.scroll_y = 0;
    ta_frame_(ui, fds_in[1], "\x17", &st, &res);          /* Ctrl-W */
    TIMUI_CHECK(strcmp(text, "abc ") == 0);
    TIMUI_CHECK(st.cursor == 4);

    /* Ctrl-K at end of line eats the newline (joins the lines) */
    strcpy(text, "ab\ncd"); st.cursor = 2; st.scroll_y = 0;
    ta_frame_(ui, fds_in[1], "\x0b", &st, &res);
    TIMUI_CHECK(strcmp(text, "abcd") == 0);

    ta_close_(ui, fds_in, fds_out);
}

/* Alt-b / Alt-f word motion (ESC-prefixed letters -> TIMUI_MOD_ALT). */
void test_textarea_word_motion(void){
    int fds_in[2], fds_out[2];
    Timui *ui = ta_open_(fds_in, fds_out);
    char text[64];
    TimuiTextAreaState st = { text, sizeof text, 0, 0 };
    TimuiTextAreaResult res;

    TIMUI_CHECK(ui != NULL);
    if(!ui){ return; }

    strcpy(text, "foo bar baz"); st.cursor = 11;
    ta_frame_(ui, fds_in[1], "\x1b" "b", &st, &res);      /* Alt-b */
    TIMUI_CHECK(st.cursor == 8);                          /* start of "baz" */
    ta_frame_(ui, fds_in[1], "\x1b" "b", &st, &res);
    TIMUI_CHECK(st.cursor == 4);                          /* start of "bar" */
    ta_frame_(ui, fds_in[1], "\x1b" "f", &st, &res);      /* Alt-f */
    TIMUI_CHECK(st.cursor == 7);                          /* end of "bar" */
    ta_frame_(ui, fds_in[1], "\x1b" "f", &st, &res);
    TIMUI_CHECK(st.cursor == 11);                         /* end of "baz" */
    ta_frame_(ui, fds_in[1], "\x1b" "f", &st, &res);
    TIMUI_CHECK(st.cursor == 11);                         /* end of text: stays */

    ta_close_(ui, fds_in, fds_out);
}

/* Ctrl-T transpose and Alt-d forward word kill. */
void test_textarea_transpose_and_kill_fwd(void){
    int fds_in[2], fds_out[2];
    Timui *ui = ta_open_(fds_in, fds_out);
    char text[64];
    TimuiTextAreaState st = { text, sizeof text, 0, 0 };
    TimuiTextAreaResult res;

    TIMUI_CHECK(ui != NULL);
    if(!ui){ return; }

    /* mid-string: swap the chars around the cursor and advance */
    strcpy(text, "abcd"); st.cursor = 2;
    ta_frame_(ui, fds_in[1], "\x14", &st, &res);          /* Ctrl-T */
    TIMUI_CHECK(strcmp(text, "acbd") == 0);
    TIMUI_CHECK(st.cursor == 3);

    /* at end of text: swap the last two, cursor stays at the end */
    strcpy(text, "ab"); st.cursor = 2;
    ta_frame_(ui, fds_in[1], "\x14", &st, &res);
    TIMUI_CHECK(strcmp(text, "ba") == 0);
    TIMUI_CHECK(st.cursor == 2);

    /* at start: nothing to swap */
    strcpy(text, "ab"); st.cursor = 0;
    ta_frame_(ui, fds_in[1], "\x14", &st, &res);
    TIMUI_CHECK(strcmp(text, "ab") == 0);

    /* Alt-d kills from the cursor through the end of the next word */
    strcpy(text, "foo bar"); st.cursor = 0;
    ta_frame_(ui, fds_in[1], "\x1b" "d", &st, &res);
    TIMUI_CHECK(strcmp(text, " bar") == 0);
    TIMUI_CHECK(st.cursor == 0);
    ta_frame_(ui, fds_in[1], "\x1b" "d", &st, &res);      /* eats " bar" too */
    TIMUI_CHECK(strcmp(text, "") == 0);

    ta_close_(ui, fds_in, fds_out);
}

/* timui_slot_style: the theme the ui actually resolved, so application
 * chrome (band fills, separators) can match widget styling exactly instead
 * of re-deriving the theme from the config by hand. */
void test_slot_style_accessor(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];

    {   /* NULL ui: zero style */
        TimuiStyle z = timui_slot_style(NULL, TIMUI_SLOT_INPUT);
        TIMUI_CHECK(z.fg == 0 && z.bg == 0 && z.attrs == 0);
    }

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    cfg.theme = TIMUI_THEME_DOS_BLUE;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiTheme th = timui_theme_builtin(TIMUI_THEME_DOS_BLUE);
        TimuiStyle want = timui_theme_style(&th, TIMUI_SLOT_INPUT_FOCUSED);
        TimuiStyle got = timui_slot_style(ui, TIMUI_SLOT_INPUT_FOCUSED);
        TIMUI_CHECK(got.fg == want.fg && got.bg == want.bg &&
                    got.attrs == want.attrs);
        {   /* out-of-range slot: zero style, not a read past the table */
            TimuiStyle z = timui_slot_style(ui, (TimuiStyleSlot)9999);
            TIMUI_CHECK(z.fg == 0 && z.bg == 0 && z.attrs == 0);
        }
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}

/* The text area paints its whole rect in the widget style, not only the
 * cells that carry glyphs -- an input box reads as a box because its empty
 * cells share the background. */
void test_textarea_fills_background(void){
    int fds_in[2], fds_out[2];
    Timui *ui = ta_open_(fds_in, fds_out);
    char text[16] = "a";
    TimuiTextAreaState st = { text, sizeof text, 1, 0 };
    TimuiTextAreaResult res;

    TIMUI_CHECK(ui != NULL);
    if(!ui){ return; }
    {
        TimuiFrame *f = NULL;
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        res = timui_text_area_mut(f, TIMUI_ID("ta"), TIMUI_RECT(1, 0, 8, 2),
                                  &st, 0);
        (void)res;
        {
            TimuiCellBuffer *buf = timui_frame_buffer(f);
            TimuiCell *glyph  = timui_cells_get(buf, 1, 0);   /* the 'a' */
            TimuiCell *blank  = timui_cells_get(buf, 5, 0);   /* same row, empty */
            TimuiCell *below  = timui_cells_get(buf, 1, 1);   /* empty row */
            TimuiCell *out    = timui_cells_get(buf, 0, 0);   /* outside the rect */
            TIMUI_CHECK(glyph && blank && below && out);
            if(glyph && blank && below && out){
                TIMUI_CHECK(blank->bg == glyph->bg);   /* box, not ragged text */
                TIMUI_CHECK(below->bg == glyph->bg);
                TIMUI_CHECK(out->bg != glyph->bg);     /* fill stays inside */
            }
        }
        timui_end(f);
        timui_close(ui);
    }
    ta_close_(NULL, fds_in, fds_out);   /* ui already closed above */
}

/* regression/textarea-scroll-on-grow: the text area's vertical scroll chases
 * the cursor downward but never returned when the viewport grew back. A
 * 1-row viewport on "a\nb" with the cursor on line 1 pushes scroll_y to 1;
 * handing the SAME state to a 2-row viewport must scroll back to 0 so both
 * lines are visible -- instead line 0 stayed hidden forever. Seen live in
 * agent_layout_inline: after Shift+Enter the previous prompt line vanished
 * even though the prompt band had grown a row for it. */
void test_regression_textarea_scroll_on_grow(void){
    TimuiConfig cfg = TIMUI_CONFIG_INIT;
    Timui *ui = NULL;
    int fds_in[2], fds_out[2];
    char text[32] = "a\nb";
    TimuiTextAreaState st = { text, sizeof text, 3, 0 };

    if(pipe(fds_in) != 0) { TIMUI_CHECK(0); return; }
    if(pipe(fds_out) != 0) { TIMUI_CHECK(0); close(fds_in[0]); close(fds_in[1]); return; }
    cfg.input_fd = fds_in[0];
    cfg.output_fd = fds_out[1];
    cfg.flags |= TIMUI_FLAG_EXTERNAL_POLL;
    TIMUI_CHECK(timui_open(&cfg, &ui) == TIMUI_OK);
    if(ui){
        TimuiFrame *f = NULL;
        TimuiTextAreaResult res;

        /* 1-row viewport: the cursor is on line 1, so the view scrolls */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        res = timui_text_area_mut(f, TIMUI_ID("ta"), TIMUI_RECT(0, 0, 10, 1),
                                  &st, 0);
        TIMUI_CHECK(res.state.scroll_y == 1);
        timui_end(f);

        /* 2-row viewport, same state: everything fits, scroll must return */
        TIMUI_CHECK(timui_begin_result(ui, &f) == TIMUI_OK);
        res = timui_text_area_mut(f, TIMUI_ID("ta"), TIMUI_RECT(0, 0, 10, 2),
                                  &st, 0);
        TIMUI_CHECK(res.state.scroll_y == 0);
        {   /* and line 0 is actually drawn again */
            TimuiCellBuffer *buf = timui_frame_buffer(f);
            TimuiCell *c = timui_cells_get(buf, 0, 0);
            TIMUI_CHECK(c && c->codepoint == 'a');
        }
        timui_end(f);
        timui_close(ui);
    }
    close(fds_in[0]); close(fds_in[1]);
    close(fds_out[0]); close(fds_out[1]);
}
