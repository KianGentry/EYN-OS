#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Open GUI text editor.", "write <filename>");

#define EDITOR_MAX_LINES 4096
#define EDITOR_MAX_LINE_LEN 2048
#define EDITOR_MAX_FILE_BYTES (256 * 1024)
#define EDITOR_LINE_H 12
#define EDITOR_CHAR_W 8

typedef struct {
    char* line[EDITOR_MAX_LINES];
    int line_count;
    int cursor_x;
    int cursor_y;
    int scroll_x;
    int scroll_y;
    int preferred_x;
    int modified;

    int sel_active;
    int sel_anchor_x;
    int sel_anchor_y;
    int sel_focus_x;
    int sel_focus_y;

    int mouse_dragging;
    int prev_left_down;

    char* clipboard;
    int clipboard_len;
    int clipboard_cap;

    char filename[192];
    char status[128];
} editor_t;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int digits10(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void str_copy(char* dst, int dst_cap, const char* src) {
    if (!dst || dst_cap <= 0) return;
    if (!src) src = "";
    int i = 0;
    for (; i + 1 < dst_cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static int str_len(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void str_append(char* dst, int dst_cap, const char* src) {
    if (!dst || dst_cap <= 0) return;
    int n = str_len(dst);
    if (n >= dst_cap - 1) return;
    if (!src) src = "";
    int i = 0;
    while (n + i + 1 < dst_cap && src[i]) {
        dst[n + i] = src[i];
        i++;
    }
    dst[n + i] = '\0';
}

static void str_append_int(char* dst, int dst_cap, int value, int min_width) {
    char tmp[24];
    int n = 0;
    unsigned v;
    int neg = 0;
    if (value < 0) {
        neg = 1;
        v = (unsigned)(-value);
    } else {
        v = (unsigned)value;
    }
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0 && n < (int)sizeof(tmp));
    while (n < min_width && n < (int)sizeof(tmp)) tmp[n++] = ' ';
    if (neg && n < (int)sizeof(tmp)) tmp[n++] = '-';

    char out[24];
    int w = 0;
    for (int i = n - 1; i >= 0; --i) out[w++] = tmp[i];
    out[w] = '\0';
    str_append(dst, dst_cap, out);
}

static void editor_set_status(editor_t* e, const char* s) {
    if (!e) return;
    str_copy(e->status, (int)sizeof(e->status), s ? s : "");
}

static int line_len(editor_t* e, int y) {
    if (!e || y < 0 || y >= e->line_count || !e->line[y]) return 0;
    return (int)strlen(e->line[y]);
}

static int ensure_line_alloc(editor_t* e, int y, int need_len) {
    if (!e || y < 0 || y >= e->line_count) return -1;
    if (need_len < 0) need_len = 0;
    if (need_len > EDITOR_MAX_LINE_LEN) return -1;
    char* oldp = e->line[y] ? e->line[y] : "";
    int old_len = (int)strlen(oldp);
    if (old_len >= need_len && e->line[y]) return 0;
    int cap = old_len + 1;
    if (cap < 16) cap = 16;
    while (cap <= need_len) cap <<= 1;
    if (cap > EDITOR_MAX_LINE_LEN + 1) cap = EDITOR_MAX_LINE_LEN + 1;
    char* np = (char*)malloc((size_t)cap);
    if (!np) return -1;
    memcpy(np, oldp, (size_t)old_len + 1);
    if (e->line[y]) free(e->line[y]);
    e->line[y] = np;
    return 0;
}

static int insert_empty_line(editor_t* e, int at) {
    if (!e) return -1;
    if (e->line_count >= EDITOR_MAX_LINES) return -1;
    at = clampi(at, 0, e->line_count);
    for (int i = e->line_count; i > at; --i) e->line[i] = e->line[i - 1];
    e->line[at] = (char*)malloc(1);
    if (!e->line[at]) {
        for (int i = at; i < e->line_count; ++i) e->line[i] = e->line[i + 1];
        return -1;
    }
    e->line[at][0] = '\0';
    e->line_count++;
    return 0;
}

static void delete_line(editor_t* e, int at) {
    if (!e || at < 0 || at >= e->line_count) return;
    if (e->line_count == 1) {
        if (e->line[0]) e->line[0][0] = '\0';
        return;
    }
    free(e->line[at]);
    for (int i = at; i < e->line_count - 1; ++i) e->line[i] = e->line[i + 1];
    e->line[e->line_count - 1] = NULL;
    e->line_count--;
}

static int normalize_pos(editor_t* e, int* x, int* y) {
    if (!e || !x || !y) return -1;
    *y = clampi(*y, 0, e->line_count - 1);
    *x = clampi(*x, 0, line_len(e, *y));
    return 0;
}

static void selection_order(editor_t* e, int* sx, int* sy, int* fx, int* fy) {
    *sx = e->sel_anchor_x;
    *sy = e->sel_anchor_y;
    *fx = e->sel_focus_x;
    *fy = e->sel_focus_y;
    normalize_pos(e, sx, sy);
    normalize_pos(e, fx, fy);
    if (*fy < *sy || (*fy == *sy && *fx < *sx)) {
        int tx = *sx, ty = *sy;
        *sx = *fx; *sy = *fy;
        *fx = tx;  *fy = ty;
    }
}

static int has_selection(editor_t* e) {
    if (!e || !e->sel_active) return 0;
    int sx, sy, fx, fy;
    selection_order(e, &sx, &sy, &fx, &fy);
    return !(sx == fx && sy == fy);
}

static void clear_selection(editor_t* e) {
    if (!e) return;
    e->sel_active = 0;
}

static int clipboard_set(editor_t* e, const char* data, int len) {
    if (!e || !data || len < 0) return -1;
    if (len + 1 > e->clipboard_cap) {
        int cap = 256;
        while (cap < len + 1) cap <<= 1;
        char* np = (char*)malloc((size_t)cap);
        if (!np) return -1;
        if (e->clipboard) free(e->clipboard);
        e->clipboard = np;
        e->clipboard_cap = cap;
    }
    memcpy(e->clipboard, data, (size_t)len);
    e->clipboard[len] = '\0';
    e->clipboard_len = len;
    return 0;
}

static int copy_selection(editor_t* e) {
    if (!has_selection(e)) return -1;
    int sx, sy, fx, fy;
    selection_order(e, &sx, &sy, &fx, &fy);
    int need = 0;
    for (int y = sy; y <= fy; ++y) {
        int ll = line_len(e, y);
        int from = (y == sy) ? sx : 0;
        int to = (y == fy) ? fx : ll;
        if (to > from) need += (to - from);
        if (y != fy) need += 1;
    }
    if (need > EDITOR_MAX_FILE_BYTES) return -1;
    char* tmp = (char*)malloc((size_t)need + 1);
    if (!tmp) return -1;
    int w = 0;
    for (int y = sy; y <= fy; ++y) {
        int ll = line_len(e, y);
        int from = (y == sy) ? sx : 0;
        int to = (y == fy) ? fx : ll;
        if (to > from) {
            memcpy(&tmp[w], &e->line[y][from], (size_t)(to - from));
            w += (to - from);
        }
        if (y != fy) tmp[w++] = '\n';
    }
    tmp[w] = '\0';
    int rc = clipboard_set(e, tmp, w);
    free(tmp);
    if (rc == 0) editor_set_status(e, "copied");
    return rc;
}

static int delete_selection(editor_t* e) {
    if (!has_selection(e)) return 0;
    int sx, sy, fx, fy;
    selection_order(e, &sx, &sy, &fx, &fy);

    if (sy == fy) {
        int ll = line_len(e, sy);
        if (sx < ll && fx <= ll && fx > sx) {
            memmove(&e->line[sy][sx], &e->line[sy][fx], (size_t)(ll - fx + 1));
        }
    } else {
        int first_len = line_len(e, sy);
        int last_len = line_len(e, fy);
        int left_len = sx;
        int right_len = last_len - fx;
        if (left_len < 0) left_len = 0;
        if (right_len < 0) right_len = 0;
        int new_len = left_len + right_len;
        if (new_len > EDITOR_MAX_LINE_LEN) return -1;
        if (ensure_line_alloc(e, sy, new_len) != 0) return -1;
        if (left_len > first_len) left_len = first_len;
        if (fx > last_len) fx = last_len;
        if (right_len > 0) {
            memcpy(&e->line[sy][left_len], &e->line[fy][fx], (size_t)right_len);
        }
        e->line[sy][new_len] = '\0';
        for (int y = fy; y > sy; --y) delete_line(e, y);
    }

    e->cursor_x = sx;
    e->cursor_y = sy;
    e->preferred_x = e->cursor_x;
    e->modified = 1;
    clear_selection(e);
    return 0;
}

static int insert_text(editor_t* e, const char* s, int len) {
    if (!e || !s || len <= 0) return 0;
    for (int i = 0; i < len; ++i) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            int y = e->cursor_y;
            int x = clampi(e->cursor_x, 0, line_len(e, y));
            int ll = line_len(e, y);
            char* tail = &e->line[y][x];
            int tail_len = ll - x;
            if (insert_empty_line(e, y + 1) != 0) return -1;
            if (ensure_line_alloc(e, y + 1, tail_len) != 0) return -1;
            memcpy(e->line[y + 1], tail, (size_t)tail_len);
            e->line[y + 1][tail_len] = '\0';
            e->line[y][x] = '\0';
            e->cursor_y++;
            e->cursor_x = 0;
            continue;
        }
        if ((unsigned char)ch < 32 || (unsigned char)ch > 126) continue;
        int y = e->cursor_y;
        int x = clampi(e->cursor_x, 0, line_len(e, y));
        int ll = line_len(e, y);
        if (ll >= EDITOR_MAX_LINE_LEN) continue;
        if (ensure_line_alloc(e, y, ll + 1) != 0) return -1;
        memmove(&e->line[y][x + 1], &e->line[y][x], (size_t)(ll - x + 1));
        e->line[y][x] = ch;
        e->cursor_x++;
    }
    e->preferred_x = e->cursor_x;
    e->modified = 1;
    return 0;
}

static int save_file(editor_t* e) {
    if (!e) return -1;
    int bytes = 0;
    for (int i = 0; i < e->line_count; ++i) {
        bytes += line_len(e, i);
        if (i + 1 < e->line_count) bytes += 1;
        if (bytes > EDITOR_MAX_FILE_BYTES) {
            editor_set_status(e, "save failed: file too large");
            return -1;
        }
    }
    char* out = (char*)malloc((size_t)bytes + 1);
    if (!out) {
        editor_set_status(e, "save failed: out of memory");
        return -1;
    }
    int w = 0;
    for (int i = 0; i < e->line_count; ++i) {
        int ll = line_len(e, i);
        if (ll > 0) {
            memcpy(&out[w], e->line[i], (size_t)ll);
            w += ll;
        }
        if (i + 1 < e->line_count) out[w++] = '\n';
    }
    out[w] = '\0';
    int rc = writefile(e->filename, out, (size_t)w);
    free(out);
    if (rc == 0) {
        e->modified = 0;
        editor_set_status(e, "saved");
        return 0;
    }
    editor_set_status(e, "save failed");
    return -1;
}

static int load_file(editor_t* e, const char* path) {
    if (!e || !path || !path[0]) return -1;
    str_copy(e->filename, (int)sizeof(e->filename), path);

    for (int i = 0; i < EDITOR_MAX_LINES; ++i) {
        if (e->line[i]) free(e->line[i]);
        e->line[i] = NULL;
    }
    e->line_count = 0;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        if (insert_empty_line(e, 0) != 0) return -1;
        e->modified = 0;
        editor_set_status(e, "new file");
        return 0;
    }

    char* buf = (char*)malloc(EDITOR_MAX_FILE_BYTES + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    int total = 0;
    for (;;) {
        int want = EDITOR_MAX_FILE_BYTES - total;
        if (want <= 0) break;
        ssize_t n = read(fd, &buf[total], (size_t)want);
        if (n <= 0) break;
        total += (int)n;
    }
    close(fd);
    buf[total] = '\0';

    int p = 0;
    while (p <= total) {
        if (e->line_count >= EDITOR_MAX_LINES) break;
        int start = p;
        while (p < total && buf[p] != '\n') p++;
        int len = p - start;
        if (len > 0 && buf[start + len - 1] == '\r') len--;
        if (len > EDITOR_MAX_LINE_LEN) len = EDITOR_MAX_LINE_LEN;
        if (insert_empty_line(e, e->line_count) != 0) {
            free(buf);
            return -1;
        }
        if (ensure_line_alloc(e, e->line_count - 1, len) != 0) {
            free(buf);
            return -1;
        }
        if (len > 0) memcpy(e->line[e->line_count - 1], &buf[start], (size_t)len);
        e->line[e->line_count - 1][len] = '\0';
        if (p >= total) break;
        p++;
    }
    free(buf);

    if (e->line_count <= 0) {
        if (insert_empty_line(e, 0) != 0) return -1;
    }
    e->cursor_x = 0;
    e->cursor_y = 0;
    e->scroll_x = 0;
    e->scroll_y = 0;
    e->preferred_x = 0;
    e->modified = 0;
    clear_selection(e);
    editor_set_status(e, "loaded");
    return 0;
}

static void ensure_cursor_visible(editor_t* e, int visible_rows, int visible_cols) {
    if (!e) return;
    if (e->cursor_y < e->scroll_y) e->scroll_y = e->cursor_y;
    if (e->cursor_y >= e->scroll_y + visible_rows) e->scroll_y = e->cursor_y - visible_rows + 1;
    if (e->cursor_x < e->scroll_x) e->scroll_x = e->cursor_x;
    if (e->cursor_x >= e->scroll_x + visible_cols) e->scroll_x = e->cursor_x - visible_cols + 1;
    if (e->scroll_y < 0) e->scroll_y = 0;
    if (e->scroll_x < 0) e->scroll_x = 0;
}

static int pos_in_selection(editor_t* e, int y, int x) {
    if (!has_selection(e)) return 0;
    int sx, sy, fx, fy;
    selection_order(e, &sx, &sy, &fx, &fy);
    if (y < sy || y > fy) return 0;
    if (sy == fy) return (x >= sx && x < fx);
    if (y == sy) return x >= sx;
    if (y == fy) return x < fx;
    return 1;
}

static void render(editor_t* e, int h) {
    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 900;
    if (sz.h <= 0) sz.h = 520;

    int gutter_digits = digits10(e->line_count + 1);
    int gutter_w = 10 + gutter_digits * EDITOR_CHAR_W + 8;
    int text_x = gutter_w + 6;
    int top_y = 18;
    int bottom_h = 16;
    int content_h = sz.h - top_y - bottom_h;
    if (content_h < EDITOR_LINE_H) content_h = EDITOR_LINE_H;
    int visible_rows = content_h / EDITOR_LINE_H;
    int visible_cols = (sz.w - text_x - 4) / EDITOR_CHAR_W;
    if (visible_cols < 4) visible_cols = 4;

    ensure_cursor_visible(e, visible_rows, visible_cols);

    (void)gui_begin(h);
    gui_rgb_t bg = { .r = 10, .g = 12, .b = 16, ._pad = 0 };
    (void)gui_clear(h, &bg);

    gui_rect_t head = { .x = 0, .y = 0, .w = sz.w, .h = top_y, .r = 28, .g = 34, .b = 56, ._pad = 0 };
    (void)gui_fill_rect(h, &head);

    char title[220];
    title[0] = '\0';
    str_append(title, (int)sizeof(title), "write: ");
    str_append(title, (int)sizeof(title), e->filename);
    if (e->modified) str_append(title, (int)sizeof(title), " *");
    gui_text_t ht = { .x = 6, .y = 4, .r = 235, .g = 235, .b = 235, ._pad = 0, .text = title };
    (void)gui_draw_text(h, &ht);

    gui_rect_t gutter = { .x = 0, .y = top_y, .w = gutter_w, .h = content_h, .r = 18, .g = 20, .b = 28, ._pad = 0 };
    (void)gui_fill_rect(h, &gutter);

    gui_line_t sep = { .x1 = gutter_w, .y1 = top_y, .x2 = gutter_w, .y2 = top_y + content_h - 1, .r = 65, .g = 72, .b = 92, ._pad = 0 };
    (void)gui_draw_line(h, &sep);

    for (int row = 0; row < visible_rows; ++row) {
        int y = e->scroll_y + row;
        if (y < 0 || y >= e->line_count) break;
        int py = top_y + row * EDITOR_LINE_H;

        char ln[16];
        ln[0] = '\0';
        str_append_int(ln, (int)sizeof(ln), y + 1, gutter_digits);
        gui_text_t lnt = { .x = 6, .y = py, .r = 130, .g = 145, .b = 170, ._pad = 0, .text = ln };
        (void)gui_draw_text(h, &lnt);

        int ll = line_len(e, y);
        int from = e->scroll_x;
        int to = ll;
        if (to > from + visible_cols) to = from + visible_cols;
        if (from < ll) {
            int hs = -1, he = -1;
            for (int col = from; col < to; ++col) {
                int in = pos_in_selection(e, y, col);
                if (in && hs < 0) hs = col;
                if (!in && hs >= 0) { he = col; break; }
            }
            if (hs >= 0 && he < 0) he = to;
            if (hs >= 0 && he > hs) {
                gui_rect_t hi = {
                    .x = text_x + (hs - from) * EDITOR_CHAR_W,
                    .y = py,
                    .w = (he - hs) * EDITOR_CHAR_W,
                    .h = EDITOR_LINE_H,
                    .r = 52, .g = 72, .b = 120, ._pad = 0
                };
                (void)gui_fill_rect(h, &hi);
            }
            static char rowbuf[EDITOR_MAX_LINE_LEN + 1];
            int n = to - from;
            if (n > 0) {
                memcpy(rowbuf, &e->line[y][from], (size_t)n);
                rowbuf[n] = '\0';
                gui_text_t txt = { .x = text_x, .y = py, .r = 226, .g = 232, .b = 238, ._pad = 0, .text = rowbuf };
                (void)gui_draw_text(h, &txt);
            }
        }
    }

    if (e->cursor_y >= e->scroll_y && e->cursor_y < e->scroll_y + visible_rows) {
        int cx = e->cursor_x - e->scroll_x;
        if (cx >= 0 && cx <= visible_cols) {
            int px = text_x + cx * EDITOR_CHAR_W;
            int py = top_y + (e->cursor_y - e->scroll_y) * EDITOR_LINE_H;
            gui_rect_t caret = { .x = px, .y = py, .w = 2, .h = EDITOR_LINE_H, .r = 255, .g = 220, .b = 120, ._pad = 0 };
            (void)gui_fill_rect(h, &caret);
        }
    }

    gui_rect_t foot = { .x = 0, .y = sz.h - bottom_h, .w = sz.w, .h = bottom_h, .r = 24, .g = 30, .b = 44, ._pad = 0 };
    (void)gui_fill_rect(h, &foot);
    char status[220];
    status[0] = '\0';
    str_append(status, (int)sizeof(status), "Ln ");
    str_append_int(status, (int)sizeof(status), e->cursor_y + 1, 1);
    str_append(status, (int)sizeof(status), ", Col ");
    str_append_int(status, (int)sizeof(status), e->cursor_x + 1, 1);
    str_append(status, (int)sizeof(status), " | ");
    str_append(status, (int)sizeof(status), e->status[0] ? e->status : "ready");
    str_append(status, (int)sizeof(status), " | Ctrl+S save  Ctrl+Q quit  Ctrl+C/X/V copy/cut/paste");
    gui_text_t st = { .x = 6, .y = sz.h - 12, .r = 180, .g = 200, .b = 228, ._pad = 0, .text = status };
    (void)gui_draw_text(h, &st);

    (void)gui_present(h);
}

static void move_left(editor_t* e) {
    if (e->cursor_x > 0) e->cursor_x--;
    else if (e->cursor_y > 0) {
        e->cursor_y--;
        e->cursor_x = line_len(e, e->cursor_y);
    }
    e->preferred_x = e->cursor_x;
}

static void move_right(editor_t* e) {
    int ll = line_len(e, e->cursor_y);
    if (e->cursor_x < ll) e->cursor_x++;
    else if (e->cursor_y + 1 < e->line_count) {
        e->cursor_y++;
        e->cursor_x = 0;
    }
    e->preferred_x = e->cursor_x;
}

static void move_up(editor_t* e) {
    if (e->cursor_y > 0) e->cursor_y--;
    e->cursor_x = clampi(e->preferred_x, 0, line_len(e, e->cursor_y));
}

static void move_down(editor_t* e) {
    if (e->cursor_y + 1 < e->line_count) e->cursor_y++;
    e->cursor_x = clampi(e->preferred_x, 0, line_len(e, e->cursor_y));
}

static void backspace(editor_t* e) {
    if (has_selection(e)) {
        (void)delete_selection(e);
        return;
    }
    if (e->cursor_x > 0) {
        int y = e->cursor_y;
        int ll = line_len(e, y);
        memmove(&e->line[y][e->cursor_x - 1], &e->line[y][e->cursor_x], (size_t)(ll - e->cursor_x + 1));
        e->cursor_x--;
        e->preferred_x = e->cursor_x;
        e->modified = 1;
        return;
    }
    if (e->cursor_y > 0) {
        int y = e->cursor_y;
        int prev = y - 1;
        int left = line_len(e, prev);
        int right = line_len(e, y);
        if (left + right > EDITOR_MAX_LINE_LEN) return;
        if (ensure_line_alloc(e, prev, left + right) != 0) return;
        memcpy(&e->line[prev][left], e->line[y], (size_t)right + 1);
        delete_line(e, y);
        e->cursor_y = prev;
        e->cursor_x = left;
        e->preferred_x = e->cursor_x;
        e->modified = 1;
    }
}

static void del_key(editor_t* e) {
    if (has_selection(e)) {
        (void)delete_selection(e);
        return;
    }
    int y = e->cursor_y;
    int ll = line_len(e, y);
    if (e->cursor_x < ll) {
        memmove(&e->line[y][e->cursor_x], &e->line[y][e->cursor_x + 1], (size_t)(ll - e->cursor_x));
        e->modified = 1;
        return;
    }
    if (y + 1 < e->line_count) {
        int next_len = line_len(e, y + 1);
        if (ll + next_len > EDITOR_MAX_LINE_LEN) return;
        if (ensure_line_alloc(e, y, ll + next_len) != 0) return;
        memcpy(&e->line[y][ll], e->line[y + 1], (size_t)next_len + 1);
        delete_line(e, y + 1);
        e->modified = 1;
    }
}

static void insert_newline(editor_t* e) {
    if (has_selection(e)) (void)delete_selection(e);
    (void)insert_text(e, "\n", 1);
}

static void insert_tab(editor_t* e) {
    if (has_selection(e)) (void)delete_selection(e);
    (void)insert_text(e, "    ", 4);
}

static void paste_clipboard(editor_t* e) {
    if (!e || !e->clipboard || e->clipboard_len <= 0) return;
    if (has_selection(e)) (void)delete_selection(e);
    if (insert_text(e, e->clipboard, e->clipboard_len) == 0) editor_set_status(e, "pasted");
}

static void move_cursor_from_mouse(editor_t* e, int mx, int my, int text_x, int top_y, int visible_rows, int visible_cols) {
    int row = (my - top_y) / EDITOR_LINE_H;
    int col = (mx - text_x) / EDITOR_CHAR_W;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= visible_rows) row = visible_rows - 1;
    if (col > visible_cols) col = visible_cols;
    int y = e->scroll_y + row;
    if (y >= e->line_count) y = e->line_count - 1;
    int x = e->scroll_x + col;
    x = clampi(x, 0, line_len(e, y));
    e->cursor_y = y;
    e->cursor_x = x;
    e->preferred_x = x;
}

static int handle_key(editor_t* e, int key) {
    int base = key & 0x0FFF;
    unsigned ch = (unsigned)key & 0xFFu;

    if (ch == 17u) {
        if (e->modified) editor_set_status(e, "unsaved changes");
        return 1;
    }
    if (ch == 19u) {
        (void)save_file(e);
        return 0;
    }
    if (ch == 3u) {
        if (copy_selection(e) != 0) editor_set_status(e, "nothing selected");
        return 0;
    }
    if (ch == 24u) {
        if (copy_selection(e) == 0) {
            (void)delete_selection(e);
            editor_set_status(e, "cut");
        } else {
            editor_set_status(e, "nothing selected");
        }
        return 0;
    }
    if (ch == 22u) {
        paste_clipboard(e);
        return 0;
    }
    if (ch == 1u) {
        e->sel_active = 1;
        e->sel_anchor_x = 0;
        e->sel_anchor_y = 0;
        e->sel_focus_y = e->line_count - 1;
        e->sel_focus_x = line_len(e, e->line_count - 1);
        editor_set_status(e, "selected all");
        return 0;
    }

    if (key == 27 || ch == 27u) {
        clear_selection(e);
        editor_set_status(e, "selection cleared");
        return 0;
    }

    if (key == 8 || key == 127 || ch == 8u || ch == 127u) {
        backspace(e);
        return 0;
    }
    if (key == 10 || key == 13 || ch == 10u || ch == 13u) {
        insert_newline(e);
        return 0;
    }
    if (key == 9 || ch == 9u) {
        insert_tab(e);
        return 0;
    }

    if (key == 0x1001 || base == 0x1001 || key == 0x4800 || key == 72) {
        clear_selection(e);
        move_up(e);
        return 0;
    }
    if (key == 0x1002 || base == 0x1002 || key == 0x5000 || key == 80) {
        clear_selection(e);
        move_down(e);
        return 0;
    }
    if (key == 0x1003 || base == 0x1003 || key == 0x4B00 || key == 75) {
        clear_selection(e);
        move_left(e);
        return 0;
    }
    if (key == 0x1004 || base == 0x1004 || key == 0x4D00 || key == 77) {
        clear_selection(e);
        move_right(e);
        return 0;
    }

    if (key == 0x1005 || base == 0x1005) {
        clear_selection(e);
        e->cursor_x = 0;
        e->preferred_x = 0;
        return 0;
    }
    if (key == 0x1006 || base == 0x1006) {
        clear_selection(e);
        e->cursor_x = line_len(e, e->cursor_y);
        e->preferred_x = e->cursor_x;
        return 0;
    }

    if (key == 0x1007 || base == 0x1007) {
        del_key(e);
        return 0;
    }

    if (ch >= 32u && ch <= 126u) {
        if (has_selection(e)) (void)delete_selection(e);
        char c = (char)ch;
        (void)insert_text(e, &c, 1);
    }

    return 0;
}

static void editor_free(editor_t* e) {
    if (!e) return;
    for (int i = 0; i < EDITOR_MAX_LINES; ++i) {
        if (e->line[i]) free(e->line[i]);
        e->line[i] = NULL;
    }
    if (e->clipboard) {
        free(e->clipboard);
        e->clipboard = NULL;
    }
}

static void usage(void) {
    puts("Usage: write <filename>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    editor_t e;
    memset(&e, 0, sizeof(e));
    if (load_file(&e, argv[1]) != 0) {
        puts("write: failed to initialize editor");
        editor_free(&e);
        return 1;
    }

    int h = gui_attach("Write Editor", "Ctrl+S save | Ctrl+Q quit | Ctrl+C/X/V clipboard");
    if (h < 0) {
        puts("write: gui_attach failed");
        editor_free(&e);
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    int running = 1;
    while (running) {
        gui_size_t sz = {0, 0};
        (void)gui_get_content_size(h, &sz);
        if (sz.w <= 0) sz.w = 900;
        if (sz.h <= 0) sz.h = 520;
        int gutter_digits = digits10(e.line_count + 1);
        int gutter_w = 10 + gutter_digits * EDITOR_CHAR_W + 8;
        int text_x = gutter_w + 6;
        int top_y = 18;
        int bottom_h = 16;
        int content_h = sz.h - top_y - bottom_h;
        if (content_h < EDITOR_LINE_H) content_h = EDITOR_LINE_H;
        int visible_rows = content_h / EDITOR_LINE_H;
        int visible_cols = (sz.w - text_x - 4) / EDITOR_CHAR_W;
        if (visible_cols < 4) visible_cols = 4;

        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                if (handle_key(&e, ev.a) == 1) {
                    if (e.modified) {
                        editor_set_status(&e, "use Ctrl+S before quit");
                    } else {
                        running = 0;
                    }
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                int left_down = (ev.c & 0x1) != 0;
                int press_edge = left_down && !e.prev_left_down;
                int release_edge = (!left_down) && e.prev_left_down;
                e.prev_left_down = left_down;

                if (ev.d > 0) {
                    e.scroll_y -= 3;
                    if (e.scroll_y < 0) e.scroll_y = 0;
                } else if (ev.d < 0) {
                    e.scroll_y += 3;
                    if (e.scroll_y > e.line_count - 1) e.scroll_y = e.line_count - 1;
                }

                if (press_edge && ev.b >= top_y && ev.b < top_y + content_h && ev.a >= text_x) {
                    move_cursor_from_mouse(&e, ev.a, ev.b, text_x, top_y, visible_rows, visible_cols);
                    e.sel_active = 1;
                    e.sel_anchor_x = e.cursor_x;
                    e.sel_anchor_y = e.cursor_y;
                    e.sel_focus_x = e.cursor_x;
                    e.sel_focus_y = e.cursor_y;
                    e.mouse_dragging = 1;
                } else if (left_down && e.mouse_dragging) {
                    move_cursor_from_mouse(&e, ev.a, ev.b, text_x, top_y, visible_rows, visible_cols);
                    e.sel_focus_x = e.cursor_x;
                    e.sel_focus_y = e.cursor_y;
                } else if (release_edge && e.mouse_dragging) {
                    e.mouse_dragging = 0;
                    if (!has_selection(&e)) clear_selection(&e);
                }
            }
        }

        render(&e, h);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(h, 0);
    editor_free(&e);
    return 0;
}
