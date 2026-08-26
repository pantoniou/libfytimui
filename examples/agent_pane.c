/*
 * agent_pane.c - the tiled work pane through the PUBLIC interface only.
 *
 * A pane holds many screens and gives each of them a tile of one region, so
 * that parallel work reads as one thing instead of as a stack of screens
 * pushing each other off the top. This host drives that interface and
 * nothing else: it includes libfytimui.h and links the library.
 *
 * Each tile stands for a program. A real host runs one on a pseudo-terminal
 * and publishes what its emulator produced; here a tile draws a pattern of
 * its own, which is the same thing as far as the library is concerned - the
 * host publishes cells, and the library places them. The status rows report
 * what every tile was GRANTED, which is the number a real host sizes its
 * pseudo-terminal to, so the grid can be read off the screen.
 *
 * Usage: agent_pane [--mouse]
 *
 *   /add [rows]      open another tile          /close N   retire tile N
 *   /zoom N          one tile takes the pane    /unzoom    back to the grid
 *   /cols N          fix the columns (0 auto)   /min N     narrowest tile
 *   /sep TEXT        the rule between columns   /frame     pane frame on/off
 *   /cap N           rows the pane may take     /titles    tile chrome on/off
 *   /keys N          type into tile N (^\ to come back)
 *   /controls        cycle the mouse controls (needs --mouse)
 *   /quit            leave
 *
 * With --mouse the library grabs the mouse and the tiles carry a scroll bar
 * and marks that ask to zoom or close. That grab costs the terminal its
 * selection and copy, which is why it is asked for and never assumed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <libfytimui.h>

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TILES_MAX 8
#define TILE_COLS 200

/* A stand-in for a program on a pseudo-terminal. */
struct tile {
    struct fytim_surface *sf;
    int id;
    int rows;
    int tick;
    int scroll_top;                /* where its history is being read */
    char typed[64];                /* what was typed into it while it had
                                      the keys, which a real host would have
                                      written to the program instead */
    size_t typed_len;
};

struct app {
    struct fytim *ft;
    struct fytim_workpane *wp;
    struct tile tiles[TILES_MAX];
    int n;
    int next_id;
    int keys_on;                   /* the tile holding the keys, or -1 */
    unsigned int controls;
    int titles;
    int framed;
    int quit;
};

static const char *const commands[] = {
    "/add", "/cap", "/close", "/cols", "/controls", "/frame", "/help",
    "/keys", "/min", "/quit", "/sep", "/titles", "/unzoom", "/zoom",
};
#define COMMANDS_N ((int)(sizeof commands / sizeof commands[0]))

static void complete_cb(void *user, const char *text,
                        struct fytim_completions *c)
{
    int i;
    (void)user;
    for(i = 0; i < COMMANDS_N; i++)
        if(strncmp(commands[i], text, strlen(text)) == 0)
            fytim_completion_add(c, commands[i]);
}

/* A cell in the terminal's own colours unless a colour is asked for. */
static struct fytim_cell cell_of(uint32_t ch, uint32_t fg, uint32_t bg,
                                 uint32_t attrs)
{
    struct fytim_cell c;
    memset(&c, 0, sizeof c);
    c.chars[0] = ch;
    c.fg = fg;
    c.bg = bg;
    c.attrs = attrs;
    c.width = 1;
    return c;
}

static void put_text(struct fytim_cell *row, int cols, int at,
                     const char *s, uint32_t fg, uint32_t bg, uint32_t attrs)
{
    int i;
    for(i = 0; s[i] && at + i < cols; i++)
        row[at + i] = cell_of((unsigned char)s[i], fg, bg, attrs);
}

/*
 * Draw what this tile's program is doing. The pattern is deliberately wide
 * and tall: a tile shows the LAST rows of the grid and the leftmost columns
 * of it, so a screen bigger than its tile proves the clipping as well.
 */
static void tile_paint(struct tile *t)
{
    static const uint32_t palette[] = {
        0xff5f5f, 0x5fd75f, 0x5fafff, 0xffd75f, 0xd787ff, 0x5fffd7,
    };
    struct fytim_cell row[TILE_COLS];
    uint32_t fg = palette[t->id % (int)(sizeof palette / sizeof palette[0])];
    char text[TILE_COLS];
    int r, i, cols = TILE_COLS, bar;

    for(r = 0; r < t->rows; r++){
        for(i = 0; i < cols; i++)
            row[i] = cell_of(' ', FYTIM_COLOR_DEFAULT, FYTIM_COLOR_DEFAULT, 0);
        if(r == 0){
            snprintf(text, sizeof text,
                     "program %d  tick %d  history %d", t->id, t->tick,
                     t->scroll_top);
            put_text(row, cols, 0, text, fg, FYTIM_COLOR_DEFAULT,
                     FYTIM_ATTR_BOLD);
        }else if(r == 1 && t->typed_len){
            snprintf(text, sizeof text, "typed: %s", t->typed);
            put_text(row, cols, 0, text, FYTIM_COLOR_DEFAULT,
                     FYTIM_COLOR_DEFAULT, FYTIM_ATTR_REVERSE);
        }else{
            /* A bar that walks, so an idle tile is still visibly alive and
             * a clipped one is visibly clipped. */
            bar = (t->tick + r * 3) % 40;
            snprintf(text, sizeof text, "row %-3d", r);
            put_text(row, cols, 0, text, FYTIM_COLOR_DEFAULT,
                     FYTIM_COLOR_DEFAULT, FYTIM_ATTR_DIM);
            for(i = 0; i < 6 && 8 + bar + i < cols; i++)
                row[8 + bar + i] = cell_of(' ', FYTIM_COLOR_DEFAULT, fg, 0);
            /* Where the tile ends is worth seeing: mark far columns. */
            put_text(row, cols, 60, "|60", fg, FYTIM_COLOR_DEFAULT, 0);
            put_text(row, cols, 120, "|120", fg, FYTIM_COLOR_DEFAULT, 0);
        }
        fytim_surface_put_row(t->sf, r, row, cols);
    }
    /* The cursor a program left behind: the library draws it reversed and
     * leaves the terminal's own cursor with the prompt. */
    fytim_surface_set_cursor(t->sf, t->rows - 1, t->tick % 20, true);
    /* A pane with a scroll bar needs to know what is behind the screen. */
    fytim_surface_set_scroll_extent(t->sf, 500, t->scroll_top);
}

static void tile_title(struct app *a, struct tile *t)
{
    char text[160];
    int rows = 0, cols = 0;

    if(!a->titles){
        fytim_surface_set_top(t->sf, NULL);
        return;
    }
    fytim_surface_granted_rows(t->sf, &rows);
    fytim_surface_granted_cols(t->sf, &cols);
    snprintf(text, sizeof text, "\x1b[1mprogram %d\x1b[0m  granted %dx%d%s",
             t->id, rows, cols,
             a->keys_on == t->id ? "  \x1b[7m KEYS \x1b[0m" : "");
    fytim_surface_set_top(t->sf, text);
}

static struct tile *tile_by_id(struct app *a, int id)
{
    int i;
    for(i = 0; i < a->n; i++)
        if(a->tiles[i].id == id) return &a->tiles[i];
    return NULL;
}

static struct tile *tile_by_surface(struct app *a, struct fytim_surface *sf)
{
    int i;
    for(i = 0; i < a->n; i++)
        if(a->tiles[i].sf == sf) return &a->tiles[i];
    return NULL;
}

static void tile_add(struct app *a, int rows)
{
    struct tile *t;

    if(a->n == TILES_MAX) return;
    if(rows < 1) rows = 5;
    t = &a->tiles[a->n];
    memset(t, 0, sizeof *t);
    /* A screen wider and taller than any tile: the pane must clip it. */
    t->sf = fytim_surface_open_in(a->wp, rows, TILE_COLS);
    if(!t->sf) return;
    t->id = a->next_id++;
    t->rows = rows;
    a->n++;
    tile_paint(t);
    tile_title(a, t);
}

/* Retire a tile the way a finished program's screen is retired: its last
 * screen goes into the transcript and the tile leaves the pane. */
static void tile_drop(struct app *a, struct tile *t, int commit)
{
    int i = (int)(t - a->tiles);

    if(a->keys_on == t->id){
        fytim_surface_set_keys(t->sf, false);
        a->keys_on = -1;
    }
    if(commit) fytim_surface_commit(t->sf);
    else fytim_surface_close(t->sf);
    memmove(&a->tiles[i], &a->tiles[i + 1],
            (size_t)(a->n - i - 1) * sizeof *a->tiles);
    a->n--;
}

static void say(struct app *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void say(struct app *a, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fytim_commit(a->ft, buf, strlen(buf));
}

/* The status rows say what the library granted, which is the whole point. */
static void status_update(struct app *a)
{
    char top[512], bottom[512];
    size_t n = 0;
    int i, rows = 0, cols = 0;

    n = (size_t)snprintf(top, sizeof top, "tiles %d ", a->n);
    for(i = 0; i < a->n && n < sizeof top - 24; i++){
        fytim_surface_granted_rows(a->tiles[i].sf, &rows);
        fytim_surface_granted_cols(a->tiles[i].sf, &cols);
        n += (size_t)snprintf(top + n, sizeof top - n, " [%d: %dx%d]",
                              a->tiles[i].id, rows, cols);
    }
    fytim_set_status_row(a->ft, 0, top);

    snprintf(bottom, sizeof bottom,
             "zoom %s  controls %s%s  /help for the commands",
             fytim_workpane_zoomed(a->wp) ? "on" : "off",
             a->controls ? "on" : "off",
             fytim_mouse_enabled(a->ft) ? "" : " (no --mouse)");
    fytim_set_status_row(a->ft, 1, bottom);
}

static void do_command(struct app *a, const char *line)
{
    char verb[32] = "", arg[128] = "";
    struct tile *t;
    int n;

    if(line[0] != '/'){
        say(a, "type /help for the commands");
        return;
    }
    n = sscanf(line, "%31s %127[^\n]", verb, arg);
    if(n < 1) return;

    if(!strcmp(verb, "/quit")){
        a->quit = 1;
    }else if(!strcmp(verb, "/help")){
        say(a, "/add [rows]  /close N  /zoom N  /unzoom  /cols N  /min N");
        say(a, "/sep TEXT  /frame  /cap N  /titles  /keys N  /controls  /quit");
    }else if(!strcmp(verb, "/add")){
        tile_add(a, atoi(arg));
    }else if(!strcmp(verb, "/close")){
        t = tile_by_id(a, atoi(arg));
        if(t) tile_drop(a, t, 1);
        else say(a, "no tile %s", arg);
    }else if(!strcmp(verb, "/zoom")){
        t = tile_by_id(a, atoi(arg));
        if(t) fytim_workpane_set_zoom(a->wp, t->sf);
        else say(a, "no tile %s", arg);
    }else if(!strcmp(verb, "/unzoom")){
        fytim_workpane_set_zoom(a->wp, NULL);
    }else if(!strcmp(verb, "/cols")){
        fytim_workpane_set_columns(a->wp, atoi(arg));
    }else if(!strcmp(verb, "/min")){
        fytim_workpane_set_min_tile_cols(a->wp, atoi(arg));
    }else if(!strcmp(verb, "/sep")){
        fytim_workpane_set_tile_sep(a->wp, arg[0] ? arg : NULL);
    }else if(!strcmp(verb, "/cap")){
        fytim_workpane_set_max_rows(a->wp, atoi(arg));
    }else if(!strcmp(verb, "/frame")){
        a->framed = !a->framed;
        fytim_workpane_set_top(a->wp, a->framed ? "\x1b[2mwork\x1b[0m" : NULL);
        fytim_workpane_set_bottom(a->wp, a->framed ? "" : NULL);
    }else if(!strcmp(verb, "/titles")){
        a->titles = !a->titles;
    }else if(!strcmp(verb, "/controls")){
        /* none -> bar -> bar+arrows -> bar+arrows+zoom+close -> none */
        if(!a->controls)
            a->controls = FYTIM_WORKPANE_SCROLLBAR;
        else if(a->controls == FYTIM_WORKPANE_SCROLLBAR)
            a->controls |= FYTIM_WORKPANE_ARROWS;
        else if(!(a->controls & FYTIM_WORKPANE_ZOOM))
            a->controls |= FYTIM_WORKPANE_ZOOM | FYTIM_WORKPANE_CLOSE;
        else
            a->controls = 0;
        fytim_workpane_set_controls(a->wp, a->controls);
        if(a->controls && !fytim_mouse_enabled(a->ft))
            say(a, "the controls need --mouse: nothing will be drawn");
    }else if(!strcmp(verb, "/keys")){
        t = tile_by_id(a, atoi(arg));
        if(!t){ say(a, "no tile %s", arg); return; }
        fytim_surface_set_keys(t->sf, true);
        a->keys_on = t->id;
        say(a, "tile %d has the keys; ^\\ takes them back", t->id);
    }else{
        say(a, "unknown command %s", verb);
    }
}

/*
 * Bytes the user typed for the tile holding the keys. A real host writes
 * them to the program; this one shows them, and watches for the key it
 * reserved to take the keys back - Escape and ^C belong to the program.
 */
static void keys_in(struct app *a, const char *data, size_t len)
{
    struct tile *t = a->keys_on >= 0 ? tile_by_id(a, a->keys_on) : NULL;
    size_t i;

    if(!t) return;
    for(i = 0; i < len; i++){
        if(data[i] == 0x1c){           /* ^\ : this host's way out */
            fytim_surface_set_keys(t->sf, false);
            a->keys_on = -1;
            t->typed_len = 0;
            t->typed[0] = '\0';
            say(a, "tile %d gave the keys back", t->id);
            return;
        }
        if(t->typed_len + 1 < sizeof t->typed){
            unsigned char c = (unsigned char)data[i];
            t->typed[t->typed_len++] = (c >= ' ' && c < 0x7f) ? (char)c : '?';
            t->typed[t->typed_len] = '\0';
        }
    }
}

int main(int argc, char **argv)
{
    struct fytim_cfg cfg;
    struct app a;
    int mouse = 0, i, ticks = 0;

    for(i = 1; i < argc; i++)
        if(!strcmp(argv[i], "--mouse")) mouse = 1;

    memset(&a, 0, sizeof a);
    a.keys_on = -1;
    a.titles = 1;
    a.next_id = 1;

    fytim_cfg_default(&cfg);
    cfg.title = "agent_pane";
    cfg.mouse = mouse;
    a.ft = fytim_create(&cfg);
    if(!a.ft){ fprintf(stderr, "fytim_create failed\n"); return 1; }
    fytim_set_complete_fn(a.ft, complete_cb, &a);
    fytim_set_header(a.ft, "\x1b[1magent_pane\x1b[0m  one region, many screens");
    fytim_set_marker(a.ft, "pane> ");

    a.wp = fytim_workpane_create(a.ft);
    if(!a.wp){ fprintf(stderr, "fytim_workpane_create failed\n"); return 1; }
    /* Two programs to start with, so the tiling is on screen at once. */
    tile_add(&a, 5);
    tile_add(&a, 5);
    say(&a, "two programs running side by side; /help for the commands");

    while(!a.quit){
        struct fytim_event ev;
        struct pollfd pfd;
        int timeout = fytim_poll_timeout_ms(a.ft);

        /* The tiles are alive: give the pattern a step every so often. */
        if(timeout < 0 || timeout > 100) timeout = 100;
        pfd.fd = fytim_poll_fd(a.ft);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if(pfd.fd >= 0) poll(&pfd, 1, timeout);

        if(++ticks % 2 == 0)
            for(i = 0; i < a.n; i++){
                a.tiles[i].tick++;
                tile_paint(&a.tiles[i]);
            }
        for(i = 0; i < a.n; i++) tile_title(&a, &a.tiles[i]);
        status_update(&a);

        if(fytim_pump(a.ft) != FYTIM_OK) break;

        while(fytim_next_event(a.ft, &ev)){
            struct tile *t;
            switch(ev.type){
            case FYTIM_EVENT_LINE:
                do_command(&a, ev.text ? ev.text : "");
                fytim_history_add(a.ft, ev.text);
                fytim_set_input(a.ft, NULL);
                break;
            case FYTIM_EVENT_INTERRUPT:
            case FYTIM_EVENT_QUIT:
                a.quit = 1;
                break;
            case FYTIM_EVENT_SURFACE_KEYS:
                keys_in(&a, ev.text, ev.text_len);
                break;
            case FYTIM_EVENT_SURFACE_ZOOM:
                /* The library asked nothing of itself: the host decides. */
                t = tile_by_surface(&a, ev.surface);
                if(t)
                    fytim_workpane_set_zoom(a.wp,
                        fytim_workpane_zoomed(a.wp) == t->sf ? NULL : t->sf);
                break;
            case FYTIM_EVENT_SURFACE_CLOSE:
                t = tile_by_surface(&a, ev.surface);
                if(t) tile_drop(&a, t, 1);
                break;
            case FYTIM_EVENT_SURFACE_SCROLL:
                t = tile_by_surface(&a, ev.surface);
                if(t){
                    t->scroll_top += ev.delta;
                    if(t->scroll_top < 0) t->scroll_top = 0;
                    if(t->scroll_top > 500 - t->rows)
                        t->scroll_top = 500 - t->rows;
                    tile_paint(t);
                }
                break;
            default:
                break;
            }
        }
    }

    while(a.n) tile_drop(&a, &a.tiles[0], 0);
    fytim_workpane_destroy(a.wp);
    fytim_destroy(a.ft);
    return 0;
}
