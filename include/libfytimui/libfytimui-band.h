/*
 * libfytimui-band.h - the inline coding-agent surface.
 *
 * The library manages a small band at the bottom of the NORMAL screen (no
 * alt screen). Top to bottom:
 *
 *     [ transcript  ]   native scrollback: committed, stable lines
 *     [ tail        ]   the transcript's LIVE tail: the agent's own
 *                       streaming output, updated immediately
 *     [ work-band#1 ]   live progressive regions, oldest first; each is
 *     [ work-band#N ]   independent and retires into the transcript
 *     [ status-band ]   header, separators, prompt, status rows
 *
 * Finished lines are committed above the band into the terminal's native
 * scrollback, so scrolling, mouse selection, copy and search stay with the
 * terminal. The host publishes content and drains events; layout,
 * repainting, cursor, line editing, history navigation and completion
 * cycling are owned by the library.
 *
 * The prompt is a full readline-style editor: in-place cursor, mid-string
 * insertion, Left/Right/Home/End, Ctrl-A/E/B/F/D/K/U/W, Alt-b/f/d, Ctrl-T,
 * multiline via Shift+Enter (kitty keyboard protocol; degrades to submit),
 * history over Up/Ctrl-P and Down/Ctrl-N with the live draft preserved, and
 * Tab completion driven by the host callback below. ^L repaints the band.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIBFYTIMUI_BAND_H
#define LIBFYTIMUI_BAND_H

#include <libfytimui/libfytimui-util.h>

#include <stddef.h>

struct fytim;

/* ---- transcript --------------------------------------------------------- *
 * Commit finished lines into native scrollback. The bytes may carry SGR,
 * OSC-8 links, and libfymd4c's bare erase-to-EOL reverse-card fill. Cursor
 * movement, parameterized erase, and screen-mode sequences are rejected with
 * FYTIM_ERR_INVALID and nothing is committed. Lines must already be
 * hard-wrapped to the terminal width.
 * Embedded '\n' separates lines. Commits are batched and flushed by the
 * next fytim_pump as one update with the band repaint. */
enum fytim_result fytim_commit(struct fytim *ft, const char *buf, size_t len) FYTIM_EXPORT;

/* The transcript's live tail: the agent's standard output while it is
 * still being produced. It flows straight into the transcript -- frozen
 * lines go through fytim_commit as they stabilize and the tail is replaced
 * wholesale (a progressive markdown renderer's active region maps directly
 * onto it). Drawn under the scrollback and above every work-band, with no
 * chrome of its own; content-sized (the host caps it at the renderer).
 * Same rendered-text contract as fytim_commit; NULL or empty clears.
 *
 * The stream-in-progress signal is EXPLICIT: any tail update (set or
 * apply) marks a stream in flight, and only fytim_tail_set(NULL) ends it
 * -- a freeze can empty the tail at a block boundary mid-stream. Deferred
 * work-band commits hold until the stream ends, so a host that streams
 * MUST clear the tail when the reply completes. */
enum fytim_result fytim_tail_set(struct fytim *ft, const char *buf, size_t len) FYTIM_EXPORT;

/* Apply one progressive-renderer update to the tail (the exact shape of
 * libfymd4c's fymd_render_push): drop the cursor-row residue plus
 * `backtrack` full rows from the end of the tail, append `content`
 * ('\n'-separated rows, possibly ending in a zero-width SGR carry-over),
 * then commit the first `freeze` rows of the result into the transcript.
 * The equivalent of replaying CUU(backtrack) + "\r" + erase-down +
 * content on a terminal, with the frozen prefix routed to scrollback.
 * Same rendered-text contract; on rejection the tail is unchanged. */
enum fytim_result fytim_tail_apply(struct fytim *ft, size_t backtrack,
                                   const char *content, size_t len,
                                   size_t freeze) FYTIM_EXPORT;

/* ---- work-bands --------------------------------------------------------- *
 * A work-band is a live progressive region above the status-band: the
 * streaming render of a tool, an agent turn, anything still in flight.
 * Bands stack in creation order, oldest nearest the transcript, but their
 * position is not a promise about the transcript: tasks finish in arbitrary
 * order, and whichever band commits first lands in the transcript first,
 * even while an older band streams on above it. Each band is independent:
 * its content is replaced wholesale while streaming, and when the work is
 * done the band is committed -- its content moves into the transcript's
 * native scrollback and the band retires.
 *
 * A band shows the LAST rows of its content, up to its max_rows cap
 * (host-settable, a small default otherwise). When the terminal is too
 * short for every band, the oldest bands lose rows first; the status-band
 * chrome then degrades as before, prompt last. */
struct fytim_workband;

/* Append a new band below any existing ones. NULL on failure. The handle
 * stays valid until fytim_workband_commit/destroy or fytim_destroy. */
struct fytim_workband *fytim_workband_create(struct fytim *ft) FYTIM_EXPORT;

/* Replace the band's live content. Same rendered-text contract as
 * fytim_commit; on rejection the previous content is retained. Embedded '\n'
 * separates rows; NULL or empty clears (the band keeps one blank row). */
enum fytim_result fytim_workband_set(struct fytim_workband *wb,
                                     const char *buf, size_t len) FYTIM_EXPORT;

/* Cap the rows of content the band may occupy (>= 1). */
enum fytim_result fytim_workband_set_max_rows(struct fytim_workband *wb,
                                              int rows) FYTIM_EXPORT;

/* Optional single chrome rows framing the band's content; plain text draws
 * dim, and the text may carry SGR styling (rendered markdown) under the
 * same contract as fytim_workband_set. An empty string draws a full-width
 * separator rule; NULL removes the row. These frame the LIVE band only --
 * they are not committed. */
enum fytim_result fytim_workband_set_top(struct fytim_workband *wb,
                                         const char *text) FYTIM_EXPORT;
enum fytim_result fytim_workband_set_bottom(struct fytim_workband *wb,
                                            const char *text) FYTIM_EXPORT;

/* What the band commits can differ from what it shows live: an optional
 * commit payload replaces the live content in fytim_workband_commit --
 * e.g. the tool's output re-rendered as a fenced markdown block while the
 * band showed a live progressive view. Same rendered-text contract as
 * fytim_workband_set; on rejection the previous payload is retained.
 * NULL (or empty) clears it, falling back to committing the live content. */
enum fytim_result fytim_workband_set_commit(struct fytim_workband *wb,
                                            const char *buf, size_t len) FYTIM_EXPORT;

/* Commit the band's content into the transcript (batched like
 * fytim_commit) and retire the band. The handle is invalid after. While
 * the transcript tail is streaming, the commit is DEFERRED so it cannot
 * split the streaming reply: the band's final render stays on screen and
 * flushes into the transcript -- in finish order -- once the stream ends
 * and the tail clears. */
enum fytim_result fytim_workband_commit(struct fytim_workband *wb) FYTIM_EXPORT;

/* Retire the band without committing anything. */
void fytim_workband_destroy(struct fytim_workband *wb) FYTIM_EXPORT;

/* ---- chrome ------------------------------------------------------------- *
 * Header and status text may carry SGR styling escapes (rendered markdown)
 * under the same contract as fytim_commit; disallowed sequences are
 * rejected with FYTIM_ERR_INVALID. Plain text draws in the default chrome
 * style (bold header, dim status). */
enum fytim_result fytim_set_header(struct fytim *ft, const char *text) FYTIM_EXPORT;
/* Status rows under the prompt; row is 0 or 1. */
enum fytim_result fytim_set_status_row(struct fytim *ft, int row, const char *text) FYTIM_EXPORT;
/* The prompt marker drawn ahead of the input ("> " by default). May
 * carry SGR styling (a colored activity dot) under the same contract as
 * fytim_set_header. */
enum fytim_result fytim_set_marker(struct fytim *ft, const char *marker) FYTIM_EXPORT;

/* ---- input -------------------------------------------------------------- */

/* Replace the edit buffer (programmatic recall; user editing is library-
 * owned). NULL clears. */
enum fytim_result fytim_set_input(struct fytim *ft, const char *text) FYTIM_EXPORT;
/* The current edit buffer; valid until the next fytim_pump. */
const char *fytim_input(const struct fytim *ft) FYTIM_EXPORT;

/* ---- external editor ---------------------------------------------------- *
 * ^G emits FYTIM_EVENT_EDIT. The host then releases the terminal with
 * fytim_suspend (band erased, raw mode left), runs its editor over the
 * input text, takes the terminal back with fytim_resume (raw mode and a
 * full repaint), and loads the result with fytim_set_input. Pumps between
 * suspend and resume are inert and return FYTIM_OK. */
enum fytim_result fytim_suspend(struct fytim *ft) FYTIM_EXPORT;
enum fytim_result fytim_resume(struct fytim *ft) FYTIM_EXPORT;

/* ---- history ------------------------------------------------------------ *
 * The host records submitted lines it wants recallable (typically from its
 * FYTIM_EVENT_LINE handler); the library owns navigation: Up/Ctrl-P walk
 * older entries, Down/Ctrl-N walk back to the live draft, which is
 * preserved while browsing. Consecutive duplicates collapse; the oldest
 * entry is dropped when max_len is reached. */
enum fytim_result fytim_history_add(struct fytim *ft, const char *line) FYTIM_EXPORT;
enum fytim_result fytim_history_set_max_len(struct fytim *ft, int max_len) FYTIM_EXPORT;

/* ---- completion --------------------------------------------------------- *
 * linenoise-style: on Tab the library calls the host back with the current
 * input; the host adds candidates. A single candidate completes outright;
 * several first extend to the longest common prefix, then Tab cycles the
 * candidates and the original line (shown in a status-row ribbon, windowed
 * on the selection). Typing accepts and leaves completion. */
struct fytim_completions;   /* valid only during the callback */

typedef void (*fytim_complete_fn)(void *user, const char *text,
                                  struct fytim_completions *c);

enum fytim_result fytim_set_complete_fn(struct fytim *ft,
                                        fytim_complete_fn fn, void *user) FYTIM_EXPORT;
enum fytim_result fytim_completion_add(struct fytim_completions *c,
                                       const char *candidate) FYTIM_EXPORT;

#endif /* LIBFYTIMUI_BAND_H */
