/* ---- text-area widget (v0.2) ------------------------------------------ *
 * A multi-line text editor. Click to focus, type to insert, Backspace/Delete
 * remove whole grapheme clusters. Lines are split on '\n'. Plain Enter can
 * submit when requested; Shift-Enter and paste insert line breaks. */
static int text_area_insert_span_(TimuiTextAreaState *st, const char *src, int nbytes){
    int j = 0;
    int changed = 0;
    while(j < nbytes){
        int n = utf8_lead_len((unsigned char)src[j]);
        size_t m = (size_t)(n > 0 ? n : 1);     /* defensive: stray byte as 1 */
        if(j + (int)m > nbytes) m = (size_t)(nbytes - j);
        if(!text_insert_(st->text, st->cap, st->cursor, src + j, m)) break;
        st->cursor += m;
        j += (int)m;
        changed = 1;
    }
    return changed;
}
static int text_area_insert_newline_(TimuiTextAreaState *st){
    if(!text_insert_(st->text, st->cap, st->cursor, "\n", 1)) return 0;
    st->cursor++;
    return 1;
}
static int text_area_enter_submits_(uint32_t mods, uint32_t flags){
    if(!(flags & TIMUI_TEXT_AREA_ENTER_SUBMITS)) return 0;
    return !(mods & TIMUI_MOD_SHIFT);
}
/* Word boundaries for the readline chords: a word is a run of non-blank
 * characters; newlines count as blanks so the chords cross lines the way
 * they cross spaces. */
static int text_area_blank_(char c){ return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
static size_t text_area_word_left_(const char *s, size_t c){
    while(c > 0 && text_area_blank_(s[c-1])) c--;
    while(c > 0 && !text_area_blank_(s[c-1])) c--;
    return c;
}
static size_t text_area_word_right_(const char *s, size_t c){
    size_t len = strlen(s);
    while(c < len && text_area_blank_(s[c])) c++;
    while(c < len && !text_area_blank_(s[c])) c++;
    return c;
}
static void text_area_apply_key_(TimuiTextAreaState *st, unsigned key, TimuiTextAreaResult *res){
    if(key == TIMUI_KEYIN_LEFT)  st->cursor = utf8_drop_last(st->text, st->cursor);
    if(key == TIMUI_KEYIN_RIGHT) st->cursor = utf8_next_(st->text, st->cursor, strlen(st->text));
    if(key == TIMUI_KEYIN_HOME)  st->cursor = line_start_(st->text, st->cursor);
    if(key == TIMUI_KEYIN_END)   st->cursor = line_end_(st->text, st->cursor);
    if(key == TIMUI_KEYIN_BACKSPACE && st->cursor > 0){
        size_t prev = utf8_drop_last(st->text, st->cursor);
        st->cursor = text_erase_(st->text, prev, st->cursor);
        res->changed = 1;
    }
    if(key == TIMUI_KEYIN_DELETE){
        size_t nxt = utf8_next_(st->text, st->cursor, strlen(st->text));
        if(nxt > st->cursor){
            (void)text_erase_(st->text, st->cursor, nxt);
            res->changed = 1;
        }
    }
    if(key == TIMUI_KEYIN_KILL_EOL){
        size_t end = line_end_(st->text, st->cursor);
        if(end == st->cursor && st->text[end] == '\n') end++;  /* at EOL: join */
        if(end > st->cursor){
            (void)text_erase_(st->text, st->cursor, end);
            res->changed = 1;
        }
    }
    if(key == TIMUI_KEYIN_KILL_BOL){
        size_t start = line_start_(st->text, st->cursor);
        if(start < st->cursor){
            st->cursor = text_erase_(st->text, start, st->cursor);
            res->changed = 1;
        }
    }
    if(key == TIMUI_KEYIN_KILL_WORD){
        size_t w = text_area_word_left_(st->text, st->cursor);
        if(w < st->cursor){
            st->cursor = text_erase_(st->text, w, st->cursor);
            res->changed = 1;
        }
    }
    if(key == TIMUI_KEYIN_KILL_WORD_FWD){
        size_t w = text_area_word_right_(st->text, st->cursor);
        if(w > st->cursor){
            (void)text_erase_(st->text, st->cursor, w);
            res->changed = 1;
        }
    }
    if(key == TIMUI_KEYIN_WORD_LEFT)  st->cursor = text_area_word_left_(st->text, st->cursor);
    if(key == TIMUI_KEYIN_WORD_RIGHT) st->cursor = text_area_word_right_(st->text, st->cursor);
    if(key == TIMUI_KEYIN_TRANSPOSE){
        /* readline: swap the clusters around the cursor and advance; at the
         * end of the text swap the last two and stay. */
        size_t len = strlen(st->text);
        size_t b = st->cursor, a, n;
        char tmp[8];
        if(len >= 2 && b > 0){
            if(b >= len) b = utf8_drop_last(st->text, len);   /* at end: back up */
            a = utf8_drop_last(st->text, b);
            n = utf8_next_(st->text, b, len);
            if(n - b <= sizeof tmp && b - a > 0 && n > b){
                memcpy(tmp, st->text + b, n - b);              /* second cluster */
                memmove(st->text + a + (n - b), st->text + a, b - a);
                memcpy(st->text + a, tmp, n - b);
                st->cursor = n;
                res->changed = 1;
            }
        }
    }
}
static void text_area_process_edit_ops_(Timui *ui, TimuiTextAreaState *st,
                                        uint32_t flags, TimuiTextAreaResult *res){
    int oi;
    for(oi = 0; oi < ui->edit_count; oi++){
        TimuiEditOp *op = &ui->edit_ops[oi];
        if(op->kind == TIMUI_EDIT_TEXT){
            if(text_area_insert_span_(st, ui->text_in + op->start, op->len)) res->changed = 1;
        } else if(op->kind == TIMUI_EDIT_KEY && op->key == TIMUI_EDIT_KEY_ENTER_){
            if(text_area_enter_submits_(op->mods, flags)){
                res->submitted = 1;
                timui_defer_edit_ops_after_(ui, oi + 1);
                return;
            }
            if(text_area_insert_newline_(st)) res->changed = 1;
        } else if(op->kind == TIMUI_EDIT_KEY){
            text_area_apply_key_(st, op->key, res);
        }
    }
}
static void text_area_pos_(const char *text, size_t limit, int width,
                           int *rowp, int *colp){
    size_t len = strlen(text), i = 0, next;
    int row = 0, col = 0, gw;
    if(limit > len) limit = len;
    if(width < 1) width = 1;
    while(i < limit){
        if(text[i] == '\r' || text[i] == '\n'){
            if(text[i] == '\r' && i + 1 < limit && text[i + 1] == '\n')
                i++;
            i++;
            row++;
            col = 0;
            continue;
        }
        next = timui_grapheme_next(text, limit, i);
        if(next <= i) next = i + 1;
        gw = timui_grapheme_width(text + i, next - i);
        if(gw < 1) gw = 1;
        if(col > 0 && col + gw > width){
            row++;
            col = 0;
        }
        col += gw;
        i = next;
        if(col >= width){
            row++;
            col = 0;
        }
    }
    if(rowp) *rowp = row;
    if(colp) *colp = col;
}
static TimuiTextAreaResult text_area_ex_(TimuiFrame *f, TimuiId id, TimuiRect r,
                                        TimuiTextAreaState st, uint32_t flags,
                                        const TimuiStyle *style){
    Timui *ui;
    TimuiInteractResult ir;
    TimuiRect content;
    TimuiTextAreaResult res;
    size_t i, text_len;
    int x = 0, y = 0;
    res.state = st;
    res.changed = 0;
    res.submitted = 0;
    res.focused = 0;
    if(!f || !f->ui || !st.text || st.cap == 0) return res;
    text_len = text_len_bounded_(st.text, st.cap);
    if(text_len >= st.cap){
        text_len = st.cap - 1;
        st.text[text_len] = '\0';
    }
    if(st.cursor > text_len) st.cursor = text_len;
    ui = f->ui;
    ir = timui_interact_button(&ui->ia, id, r);
    res.focused = ir.focused;
    if(ir.focused){
        text_area_process_edit_ops_(ui, &st, flags, &res);
        ui->text_in_len = 0;
        ui->enter_count = 0;
        ui->edit_count = 0;
        ui->key_in = 0;
    }
    text_len = text_len_bounded_(st.text, st.cap);
    { TimuiStyle sst = style ? *style :
          timui_widget_style_(ui, TIMUI_WIDGET_TEXT_AREA,
              ir.focused ? TIMUI_SLOT_INPUT_FOCUSED : TIMUI_SLOT_INPUT,
              ir.focused ? TIMUI_STYLE_STATE_FOCUSED : 0);
      {  int cursor_row, total_rows;
         text_area_pos_(st.text, st.cursor, r.w, &cursor_row, NULL);
         /* scroll back when the viewport grew: the view may not waste rows
          * below the last line while earlier lines sit hidden above
          * (regression/textarea-scroll-on-grow) */
         text_area_pos_(st.text, text_len, r.w, &total_rows, NULL);
         total_rows++;
         if(r.h > 0 && st.scroll_y > total_rows - r.h)
             st.scroll_y = total_rows - r.h;
         if(st.scroll_y < 0) st.scroll_y = 0;
         if(cursor_row < st.scroll_y) st.scroll_y = cursor_row;
         if(cursor_row >= st.scroll_y + r.h) st.scroll_y = cursor_row - r.h + 1;
         if(st.scroll_y < 0) st.scroll_y = 0;
      }
      /* paint the whole rect first: an input box reads as a box because its
       * empty cells share the background, not only the glyph cells */
      timui_draw_fill(&ui->curr, r, sst);
      content = timui_scroll_begin(f, r, st.scroll_y);
      i = 0;
      while(i < text_len){
          size_t next;
          int gw;
          if(st.text[i] == '\r' || st.text[i] == '\n'){
              if(st.text[i] == '\r' && i + 1 < text_len &&
                 st.text[i + 1] == '\n')
                  i++;
              i++;
              y++;
              x = 0;
              continue;
          }
          next = timui_grapheme_next(st.text, text_len, i);
          if(next <= i) next = i + 1;
          gw = timui_grapheme_width(st.text + i, next - i);
          if(gw < 1) gw = 1;
          if(x > 0 && x + gw > r.w){
              y++;
              x = 0;
          }
          timui_draw_text(&ui->curr, content.x + x, content.y + y,
                          (TimuiStr){ st.text + i, next - i }, sst);
          x += gw;
          i = next;
          if(x >= r.w){
              y++;
              x = 0;
          }
      }
      timui_scroll_end(f);
      if(ir.focused){                                 /* F1.4: request the hardware cursor */
          int crow, ccol;
          text_area_pos_(st.text, st.cursor, r.w, &crow, &ccol);
          if(crow >= st.scroll_y && crow < st.scroll_y + r.h && ccol < r.w){
              ui->cursor_x = r.x + ccol;                /* content.x == r.x (vertical scroll only) */
              ui->cursor_y = r.y + (crow - st.scroll_y);
              ui->cursor_visible = 1;
          }
      }
    }
    res.state = st;
    return res;
}
TIMUI_API TimuiTextAreaResult timui_text_area_ex(TimuiFrame *f, TimuiId id, TimuiRect r,
                                                 TimuiTextAreaState st, uint32_t flags){
    return text_area_ex_(f, id, r, st, flags, NULL);
}
TIMUI_API TimuiTextAreaResult timui_text_area_mut(TimuiFrame *f, TimuiId id, TimuiRect r,
                                                  TimuiTextAreaState *state, uint32_t flags){
    TimuiTextAreaResult res;
    if(!state){
        TimuiTextAreaState empty = {0};
        return timui_text_area_ex(f, id, r, empty, flags);
    }
    res = timui_text_area_ex(f, id, r, *state, flags);
    *state = res.state;
    return res;
}
TIMUI_API TimuiTextAreaResult timui_text_area_mut_styled(
    TimuiFrame *f, TimuiId id, TimuiRect r, TimuiTextAreaState *state,
    uint32_t flags, TimuiStyle style){
    TimuiTextAreaResult res;
    if(!state){
        TimuiTextAreaState empty = {0};
        return text_area_ex_(f, id, r, empty, flags, &style);
    }
    res = text_area_ex_(f, id, r, *state, flags, &style);
    *state = res.state;
    return res;
}
TIMUI_API void timui_text_area(TimuiFrame *f, TimuiId id, TimuiRect r, TimuiTextAreaState *state){
    (void)timui_text_area_mut(f, id, r, state, TIMUI_TEXT_AREA_DEFAULT);
}
