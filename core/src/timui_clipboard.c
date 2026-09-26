/* ---- clipboard (OSC 52, v0.2) ----------------------------------------- *
 * Set the terminal clipboard via OSC 52: ESC]52;c;<base64>ESC\. The base64
 * encoding is done by hand (no external dependency). */
static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const unsigned char *src, size_t len, char *dst, size_t cap){
    size_t i, j = 0;
    for(i = 0; i < len; i += 3){
        unsigned v = (unsigned)src[i] << 16;
        if(i + 1 < len) v |= (unsigned)src[i + 1] << 8;
        if(i + 2 < len) v |= (unsigned)src[i + 2];
        if(j + 4 > cap) return (size_t)-1;        /* cap too small */
        dst[j++] = b64_tab[(v >> 18) & 0x3F];
        dst[j++] = b64_tab[(v >> 12) & 0x3F];
        dst[j++] = (i + 1 < len) ? b64_tab[(v >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < len) ? b64_tab[v & 0x3F] : '=';
    }
    return j;
}
/* An OSC 52 whose payload is not base64 clears the clipboard. */
#define CLIP_CLEAR "\x1b]52;c;!\x1b\\"
#define CLIP_CLEAR_LEN (sizeof CLIP_CLEAR - 1)

TIMUI_API void timui_clipboard_set(TimuiTransport *t, TimuiStr text){
    size_t b64cap, total, b64len;
    char *buf, *full;
    if(!t || !t->write || !text.ptr || text.len == 0) return;
    if(text.len > (SIZE_MAX - 1) / 4) return;      /* base64 size would overflow size_t */
    b64cap = ((text.len + 2) / 3) * 4 + 1;
    { TimuiAllocator al = timui_default_allocator();
      buf = (char *)al.alloc(al.userdata, b64cap);
      if(!buf) return;
      b64len = b64_encode((const unsigned char *)text.ptr, text.len, buf, b64cap - 1);
      if(b64len > 0 && b64len != (size_t)-1){
          /* Build the full OSC 52 in one buffer and write in a single call.
           * A clear goes first: a terminal that joins consecutive OSC 52
           * writes (kitty) would add this text to the last copy, and one
           * that does not takes the clear as an empty selection. */
          total = CLIP_CLEAR_LEN + 7 + b64len + 2;
          full = (char *)al.alloc(al.userdata, total);
          if(full){
              memcpy(full, CLIP_CLEAR, CLIP_CLEAR_LEN);
              memcpy(full + CLIP_CLEAR_LEN, "\x1b]52;c;", 7);
              memcpy(full + CLIP_CLEAR_LEN + 7, buf, b64len);
              memcpy(full + CLIP_CLEAR_LEN + 7 + b64len, "\x1b\\", 2);
              (void)t->write(t, full, total);
              al.free(al.userdata, full, total);
          } else {
              /* fallback: four writes (better than nothing) */
              (void)t->write(t, CLIP_CLEAR, CLIP_CLEAR_LEN);
              (void)t->write(t, "\x1b]52;c;", 7);
              (void)t->write(t, buf, b64len);
              (void)t->write(t, "\x1b\\", 2);
          }
      }
      al.free(al.userdata, buf, b64cap);
    }
}
