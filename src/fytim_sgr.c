/*
 * fytim_sgr.c - SGR escape stream -> styled text runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fytim_sgr.h"

#include <string.h>

#define MAX_PARAMS 32

void fytim_sgr_init(struct fytim_sgr_parser *p)
{
    if(!p) return;
    memset(p, 0, sizeof *p);
    p->style.fg = FYTIM_COLOR_DEFAULT;
    p->style.bg = FYTIM_COLOR_DEFAULT;
    p->style.attrs = 0;
}

/* Apply one SGR sequence's parameters. Colour parameters consume following
 * parameters, so this walks the list rather than switching per element. */
static void apply_sgr(struct fytim_sgr_style *st, const unsigned int *par, size_t n)
{
    size_t i;

    /* "ESC [ m" with no parameters means reset, as does "0". */
    if(n == 0){
        st->fg = st->bg = FYTIM_COLOR_DEFAULT;
        st->attrs = 0;
        return;
    }

    for(i = 0; i < n; ++i){
        unsigned int v = par[i];
        switch(v){
            case 0:  st->fg = st->bg = FYTIM_COLOR_DEFAULT; st->attrs = 0; break;
            case 1:  st->attrs |= FYTIM_ATTR_BOLD; break;
            case 2:  st->attrs |= FYTIM_ATTR_DIM; break;
            case 3:  st->attrs |= FYTIM_ATTR_ITALIC; break;
            case 4:  st->attrs |= FYTIM_ATTR_UNDERLINE; break;
            case 7:  st->attrs |= FYTIM_ATTR_REVERSE; break;
            case 9:  st->attrs |= FYTIM_ATTR_STRIKE; break;
            case 21:
            case 22: st->attrs &= ~(unsigned)(FYTIM_ATTR_BOLD | FYTIM_ATTR_DIM); break;
            case 23: st->attrs &= ~(unsigned)FYTIM_ATTR_ITALIC; break;
            case 24: st->attrs &= ~(unsigned)FYTIM_ATTR_UNDERLINE; break;
            case 27: st->attrs &= ~(unsigned)FYTIM_ATTR_REVERSE; break;
            case 29: st->attrs &= ~(unsigned)FYTIM_ATTR_STRIKE; break;
            case 39: st->fg = FYTIM_COLOR_DEFAULT; break;
            case 49: st->bg = FYTIM_COLOR_DEFAULT; break;
            case 38:
            case 48: {
                /* 38/48 ; 5 ; idx   or   38/48 ; 2 ; r ; g ; b
                 * A truncated form is ignored rather than guessed at. */
                uint32_t *slot = (v == 38) ? &st->fg : &st->bg;
                if(i + 2 < n && par[i + 1] == 5){
                    *slot = FYTIM_COLOR_INDEXED | (par[i + 2] & 0xFFu);
                    i += 2;
                }else if(i + 4 < n && par[i + 1] == 2){
                    *slot = ((par[i + 2] & 0xFFu) << 16) |
                            ((par[i + 3] & 0xFFu) << 8) |
                             (par[i + 4] & 0xFFu);
                    i += 4;
                }else{
                    i = n;   /* malformed: stop, do not misread later params */
                }
                break;
            }
            default:
                if(v >= 30 && v <= 37)       st->fg = FYTIM_COLOR_INDEXED | (v - 30);
                else if(v >= 40 && v <= 47)  st->bg = FYTIM_COLOR_INDEXED | (v - 40);
                else if(v >= 90 && v <= 97)  st->fg = FYTIM_COLOR_INDEXED | (v - 90 + 8);
                else if(v >= 100 && v <= 107) st->bg = FYTIM_COLOR_INDEXED | (v - 100 + 8);
                /* anything else is ignored rather than treated as an error */
                break;
        }
    }
}

/* Parse a complete CSI sequence body (between "ESC [" and the final byte).
 * Only 'm' is styling; every other final byte is a control we must not honour
 * and must not render. */
static void handle_csi(struct fytim_sgr_parser *p, const char *body, size_t len,
                       char final_byte)
{
    unsigned int par[MAX_PARAMS];
    size_t n = 0;
    size_t i;
    unsigned int cur = 0;
    bool have_digit = false;

    if(final_byte != 'm'){
        p->disallowed_seen = true;
        return;
    }

    /* Private-parameter sequences (ESC [ ? ... m) are not plain styling. */
    if(len > 0 && (body[0] == '?' || body[0] == '<' || body[0] == '>' || body[0] == '=')){
        p->disallowed_seen = true;
        return;
    }

    for(i = 0; i <= len; ++i){
        char ch = (i < len) ? body[i] : ';';   /* virtual trailing separator */
        if(ch >= '0' && ch <= '9'){
            if(cur < 100000000u) cur = cur * 10 + (unsigned)(ch - '0');
            have_digit = true;
        }else if(ch == ';' || ch == ':'){
            /* An empty parameter defaults to 0, per ECMA-48. */
            if(n < MAX_PARAMS) par[n++] = have_digit ? cur : 0;
            cur = 0;
            have_digit = false;
        }else{
            /* Unexpected byte inside the parameter area. */
            p->disallowed_seen = true;
            return;
        }
    }

    /* The virtual separator above always pushes a final parameter; an entirely
     * empty body therefore yields one zero, which is the reset we want. */
    apply_sgr(&p->style, par, n);
}

/* Scan buf for a complete escape sequence starting at offset 0 (buf[0] is
 * ESC). Returns the number of bytes consumed, or 0 if the sequence is
 * incomplete and should be carried to the next feed. */
static size_t consume_escape(struct fytim_sgr_parser *p, const char *buf, size_t len)
{
    size_t i;

    if(len < 2) return 0;

    if(buf[1] != '['){
        /* A two-byte escape (or the start of OSC/other). Not styling. */
        if(buf[1] == ']'){
            /* OSC: terminated by BEL or ST (ESC \). OSC 8 (hyperlink) is
             * the one non-SGR escape that passes -- it has no cursor/erase
             * semantics and keeps committed links clickable in scrollback.
             * Every other OSC (title, clipboard, palette) stays rejected. */
            bool is_link;
            if(len < 4) return 0;                        /* can't classify yet */
            is_link = buf[2] == '8' && buf[3] == ';';
            for(i = 2; i < len; ++i){
                if(buf[i] == '\a'){
                    if(!is_link) p->disallowed_seen = true;
                    return i + 1;
                }
                if(buf[i] == '\x1b' && i + 1 < len && buf[i + 1] == '\\'){
                    if(!is_link) p->disallowed_seen = true;
                    return i + 2;
                }
            }
            return 0;   /* incomplete */
        }
        p->disallowed_seen = true;
        return 2;
    }

    /* CSI: parameter/intermediate bytes then a final byte in 0x40..0x7E. */
    for(i = 2; i < len; ++i){
        unsigned char ch = (unsigned char)buf[i];
        if(ch >= 0x40 && ch <= 0x7E){
            handle_csi(p, buf + 2, i - 2, (char)ch);
            return i + 1;
        }
        /* Parameter (0x30-0x3F) and intermediate (0x20-0x2F) bytes continue. */
        if(!((ch >= 0x20 && ch <= 0x3F))){
            /* Illegal byte inside a CSI: abandon the sequence here rather than
             * scanning forever, and do not render the bytes. */
            p->disallowed_seen = true;
            return i;
        }
    }
    return 0;   /* incomplete */
}

void fytim_sgr_feed(struct fytim_sgr_parser *p, const char *buf, size_t len,
                    fytim_sgr_run_fn cb, void *user)
{
    /* Joined view of any carried partial escape plus the new bytes. Using a
     * bounded stack buffer for the carry keeps the parser allocation-free. */
    const char *cur;
    size_t curlen;
    size_t run_start;
    size_t i;
    char joined[sizeof p->pending + 256];
    size_t chunk;

    if(!p || !cb) return;
    if(!buf || len == 0) return;

    while(len > 0){
        /* Process at most 256 new bytes at a time so `joined` stays bounded. */
        chunk = (len > 256) ? 256 : len;

        if(p->pending_len > 0){
            memcpy(joined, p->pending, p->pending_len);
            memcpy(joined + p->pending_len, buf, chunk);
            cur = joined;
            curlen = p->pending_len + chunk;
            p->pending_len = 0;
        }else{
            cur = buf;
            curlen = chunk;
        }

        run_start = 0;
        i = 0;
        while(i < curlen){
            if(cur[i] != '\x1b'){ ++i; continue; }

            /* Flush the plain-text run before the escape. */
            if(i > run_start)
                if(!cb(user, cur + run_start, i - run_start, &p->style)) return;

            {
                size_t used = consume_escape(p, cur + i, curlen - i);
                if(used == 0){
                    /* Incomplete: carry the tail to the next feed. If it is too
                     * long to be a real sequence, drop it rather than overflow. */
                    size_t tail = curlen - i;
                    if(tail <= sizeof p->pending){
                        memcpy(p->pending, cur + i, tail);
                        p->pending_len = tail;
                    }else{
                        p->disallowed_seen = true;
                    }
                    run_start = curlen;
                    i = curlen;
                    break;
                }
                i += used;
                run_start = i;
            }
        }

        if(i > run_start)
            if(!cb(user, cur + run_start, i - run_start, &p->style)) return;

        buf += chunk;
        len -= chunk;
    }
}
