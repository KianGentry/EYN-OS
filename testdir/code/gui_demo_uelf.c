#include <gui.h>
#include <stdio.h>
#include <string.h>

static void buf_append_char(char* out, int cap, int* io_len, char ch) {
    if (!out || !io_len || cap <= 0) return;
    int len = *io_len;
    if (len < 0) len = 0;
    if (len + 1 >= cap) return;
    out[len++] = ch;
    out[len] = '\0';
    *io_len = len;
}

static void buf_append_str(char* out, int cap, int* io_len, const char* s) {
    if (!s) s = "";
    for (int i = 0; s[i]; ++i) buf_append_char(out, cap, io_len, s[i]);
}

static void buf_append_uhex(char* out, int cap, int* io_len, unsigned v) {
    static const char* hex = "0123456789ABCDEF";
    buf_append_str(out, cap, io_len, "0x");
    for (int i = 28; i >= 0; i -= 4) {
        buf_append_char(out, cap, io_len, hex[(v >> (unsigned)i) & 0xFu]);
    }
}

static void buf_append_int(char* out, int cap, int* io_len, int v) {
    if (v == 0) {
        buf_append_char(out, cap, io_len, '0');
        return;
    }
    if (v < 0) {
        buf_append_char(out, cap, io_len, '-');
        // avoid overflow on INT_MIN: print as unsigned magnitude
        unsigned uv = (unsigned)(-(v + 1)) + 1u;
        char tmp[16];
        int n = 0;
        while (uv && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (uv % 10u)); uv /= 10u; }
        for (int i = n - 1; i >= 0; --i) buf_append_char(out, cap, io_len, tmp[i]);
        return;
    }
    unsigned uv = (unsigned)v;
    char tmp[16];
    int n = 0;
    while (uv && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (uv % 10u)); uv /= 10u; }
    for (int i = n - 1; i >= 0; --i) buf_append_char(out, cap, io_len, tmp[i]);
}

static void redraw(int h, int mx, int my, int buttons, int last_key) {
    (void)gui_begin(h);

    gui_size_t sz;
    sz.w = 0;
    sz.h = 0;
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 320;
    if (sz.h <= 0) sz.h = 200;

    gui_rgb_t bg = { .r = GUI_PAL_BG_R, .g = GUI_PAL_BG_G, .b = GUI_PAL_BG_B, ._pad = 0 };
    (void)gui_clear(h, &bg);

    gui_rect_t bar = { .x = 0, .y = 0, .w = sz.w, .h = 14, .r = GUI_PAL_STATUS_R, .g = GUI_PAL_STATUS_G, .b = GUI_PAL_STATUS_B, ._pad = 0 };
    (void)gui_fill_rect(h, &bar);

    // Border
    gui_line_t top = { .x1 = 0, .y1 = 0, .x2 = sz.w - 1, .y2 = 0, .r = GUI_PAL_BORDER_R, .g = GUI_PAL_BORDER_G, .b = GUI_PAL_BORDER_B, ._pad = 0 };
    gui_line_t bot = { .x1 = 0, .y1 = sz.h - 1, .x2 = sz.w - 1, .y2 = sz.h - 1, .r = GUI_PAL_BORDER_R, .g = GUI_PAL_BORDER_G, .b = GUI_PAL_BORDER_B, ._pad = 0 };
    gui_line_t lef = { .x1 = 0, .y1 = 0, .x2 = 0, .y2 = sz.h - 1, .r = GUI_PAL_BORDER_R, .g = GUI_PAL_BORDER_G, .b = GUI_PAL_BORDER_B, ._pad = 0 };
    gui_line_t rig = { .x1 = sz.w - 1, .y1 = 0, .x2 = sz.w - 1, .y2 = sz.h - 1, .r = GUI_PAL_BORDER_R, .g = GUI_PAL_BORDER_G, .b = GUI_PAL_BORDER_B, ._pad = 0 };
    (void)gui_draw_line(h, &top);
    (void)gui_draw_line(h, &bot);
    (void)gui_draw_line(h, &lef);
    (void)gui_draw_line(h, &rig);

    // Crosshair at mouse (clamped)
    int cx = mx;
    int cy = my;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= sz.w) cx = sz.w - 1;
    if (cy >= sz.h) cy = sz.h - 1;
    gui_line_t hline = { .x1 = 0, .y1 = cy, .x2 = sz.w - 1, .y2 = cy, .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0 };
    gui_line_t vline = { .x1 = cx, .y1 = 0, .x2 = cx, .y2 = sz.h - 1, .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0 };
    (void)gui_draw_line(h, &hline);
    (void)gui_draw_line(h, &vline);

    char line1[64];
    int l1 = 0;
    line1[0] = '\0';
    buf_append_str(line1, (int)sizeof(line1), &l1, "mouse ");
    buf_append_int(line1, (int)sizeof(line1), &l1, mx);
    buf_append_char(line1, (int)sizeof(line1), &l1, ',');
    buf_append_int(line1, (int)sizeof(line1), &l1, my);
    buf_append_str(line1, (int)sizeof(line1), &l1, " btn=");
    buf_append_uhex(line1, (int)sizeof(line1), &l1, (unsigned)buttons);

    char line2[64];
    int l2 = 0;
    line2[0] = '\0';
    if (last_key != 0) {
        buf_append_str(line2, (int)sizeof(line2), &l2, "last key=");
        buf_append_uhex(line2, (int)sizeof(line2), &l2, (unsigned)last_key);
    } else {
        buf_append_str(line2, (int)sizeof(line2), &l2, "press q to quit");
    }

    gui_text_t t1 = { .x = 8, .y = 3, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = line1 };
    gui_text_t t2 = { .x = 8, .y = 20, .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0, .text = line2 };
    gui_text_t t3 = { .x = 8, .y = 32, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = "demo: clear/rect/text/line + key/mouse events" };
    (void)gui_draw_text(h, &t1);
    (void)gui_draw_text(h, &t2);
    (void)gui_draw_text(h, &t3);

    (void)gui_present(h);
}

int main(void) {
    int h = gui_attach("GUI Demo", "q quits");
    if (h < 0) {
        puts("gui_attach failed");
        return 1;
    }

    int mx = 0, my = 0;
    int buttons = 0;
    int last_key = 0;

    redraw(h, mx, my, buttons, last_key);

    gui_event_t ev;
    for (;;) {
        int rc = gui_wait_event(h, &ev);
        if (rc < 0) {
            puts("gui_wait_event interrupted or failed");
            break;
        }
        if (rc == 0) {
            continue;
        }

        if (ev.type == GUI_EVENT_CLOSE) {
            break;
        }

        if (ev.type == GUI_EVENT_KEY) {
            last_key = ev.a;
            unsigned ch = (unsigned)ev.a & 0xFFu;
            if (ch == (unsigned)'q' || ch == (unsigned)'Q') {
                break;
            }
            redraw(h, mx, my, buttons, last_key);
            continue;
        }

        if (ev.type == GUI_EVENT_MOUSE) {
            mx = ev.a;
            my = ev.b;
            buttons = ev.c;
            redraw(h, mx, my, buttons, last_key);
            continue;
        }
    }

    return 0;
}
