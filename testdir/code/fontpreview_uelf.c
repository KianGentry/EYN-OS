#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>
#include <gui.h>

#include <stdio.h>
#include <string.h>

EYN_CMDMETA_V1("Preview an OTF/TTF font in multiple sizes.", "fontpreview /fonts/unscii-16.otf");

#define FONT_SIZE_MIN 6
#define FONT_SIZE_MAX 64
#define FONT_SIZE_DEFAULT 16

static void str_copy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    if (!src) src = "";
    int i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int str_len(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int str_ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    int sl = str_len(s);
    int su = str_len(suffix);
    if (su <= 0 || sl < su) return 0;
    return strcmp(s + sl - su, suffix) == 0;
}

static int is_scalable_font_path(const char* path) {
    if (!path) return 0;
    return str_ends_with(path, ".otf") || str_ends_with(path, ".ttf") ||
           str_ends_with(path, ".OTF") || str_ends_with(path, ".TTF");
}

static void build_font_spec(const char* base_path, int size_px, char* out, int out_cap) {
    if (!out || out_cap <= 0) return;
    if (!base_path) base_path = "";
    if (!is_scalable_font_path(base_path)) {
        str_copy(out, out_cap, base_path);
        return;
    }

    if (size_px < FONT_SIZE_MIN) size_px = FONT_SIZE_MIN;
    if (size_px > FONT_SIZE_MAX) size_px = FONT_SIZE_MAX;
    snprintf(out, (size_t)out_cap, "%s@%d", base_path, size_px);
}

static int parse_int(const char* s, int* out) {
    if (!s || !s[0] || !out) return -1;
    int i = 0;
    int v = 0;
    for (; s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
        if (v > 1000) return -1;
    }
    *out = v;
    return 0;
}

typedef struct {
    int handle;
    char base_path[128];
    char active_spec[128];
    char status[128];
    char install_status[128];
    int size_px;
    int running;
} app_state_t;

static void set_status(app_state_t* st, const char* msg) {
    str_copy(st->status, (int)sizeof(st->status), msg ? msg : "");
}

static void set_install_status(app_state_t* st, const char* msg) {
    str_copy(st->install_status, (int)sizeof(st->install_status), msg ? msg : "");
}

static int apply_preview_font(app_state_t* st) {
    if (!st) return -1;
    build_font_spec(st->base_path, st->size_px, st->active_spec, (int)sizeof(st->active_spec));
    if (!st->active_spec[0]) {
        set_status(st, "no font path");
        return -1;
    }
    if (gui_set_font(st->handle, st->active_spec) != 0) {
        set_status(st, "preview load failed");
        return -1;
    }
    set_status(st, "preview loaded");
    return 0;
}

static void draw_line(int h, int x, int y, int r, int g, int b, const char* text) {
    gui_text_t t;
    t.x = x;
    t.y = y;
    t.r = (unsigned char)r;
    t.g = (unsigned char)g;
    t.b = (unsigned char)b;
    t._pad = 0;
    t.text = text;
    (void)gui_draw_text(h, &t);
}

static void render(app_state_t* st) {
    if (!st) return;

    (void)gui_begin(st->handle);

    gui_rgb_t bg = { GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0 };
    (void)gui_clear(st->handle, &bg);

    gui_size_t sz;
    sz.w = 0;
    sz.h = 0;
    (void)gui_get_content_size(st->handle, &sz);
    if (sz.w <= 0) sz.w = 320;
    if (sz.h <= 0) sz.h = 200;

    gui_font_metrics_t m;
    m.char_w = 8;
    m.char_h = 8;
    (void)gui_get_font_metrics(st->handle, &m);
    if (m.char_h < 8) m.char_h = 8;
    int row_step = m.char_h + 2;
    int top_h = row_step + 6;
    if (top_h < 16) top_h = 16;

    gui_rect_t top;
    top.x = 0;
    top.y = 0;
    top.w = sz.w;
    top.h = top_h;
    top.r = GUI_PAL_STATUS_R;
    top.g = GUI_PAL_STATUS_G;
    top.b = GUI_PAL_STATUS_B;
    top._pad = 0;
    (void)gui_fill_rect(st->handle, &top);

    gui_line_t border;
    border.x1 = 0;
    border.y1 = top_h;
    border.x2 = sz.w - 1;
    border.y2 = top_h;
    border.r = GUI_PAL_BORDER_R;
    border.g = GUI_PAL_BORDER_G;
    border.b = GUI_PAL_BORDER_B;
    border._pad = 0;
    (void)gui_draw_line(st->handle, &border);

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "Font Preview  |  size=%d px", st->size_px);
    draw_line(st->handle, 6, 4, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, hdr);

    int y = top_h + 6;

    draw_line(st->handle, 6, y, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B,
              "keys: 1=8px  2=16px  +/- size +/-1  I=install size  Q/Esc=quit");
    y += row_step;

    char path_ln[160];
    snprintf(path_ln, sizeof(path_ln), "source: %s", st->base_path);
    draw_line(st->handle, 6, y, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, path_ln);
    y += row_step;

    char spec_ln[160];
    snprintf(spec_ln, sizeof(spec_ln), "preview spec: %s", st->active_spec);
    draw_line(st->handle, 6, y, GUI_PAL_ACCENT_R, GUI_PAL_ACCENT_G, GUI_PAL_ACCENT_B, spec_ln);
    y += row_step;

    draw_line(st->handle, 6, y, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, st->status);
    y += row_step;
    draw_line(st->handle, 6, y, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, st->install_status);
    y += row_step;

    char met[80];
    snprintf(met, sizeof(met), "metrics: char=%dx%d", m.char_w, m.char_h);
    draw_line(st->handle, 6, y, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, met);
    y += row_step + 2;

    int y0 = y;
    draw_line(st->handle, 6, y0 + 0 * m.char_h, 220, 220, 220, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    draw_line(st->handle, 6, y0 + 1 * m.char_h, 220, 220, 220, "abcdefghijklmnopqrstuvwxyz");
    draw_line(st->handle, 6, y0 + 2 * m.char_h, 220, 220, 220, "0123456789 !@#$%^&*()[]{}<>?/\\|");
    draw_line(st->handle, 6, y0 + 3 * m.char_h, 160, 210, 255, "The quick brown fox jumps over the lazy dog.");
    draw_line(st->handle, 6, y0 + 4 * m.char_h, 160, 210, 255, "Sphinx of black quartz, judge my vow.");

    (void)gui_present(st->handle);
}

static void install_selected_font(app_state_t* st) {
    if (!st) return;
    if (!st->active_spec[0]) {
        set_install_status(st, "install skipped: no active font");
        return;
    }
    if (eyn_sys_setfont_path(st->active_spec) != 0) {
        set_install_status(st, "install failed");
        return;
    }
    set_install_status(st, "installed as system font");
}

static void change_size(app_state_t* st, int size_px) {
    if (!st) return;
    if (size_px < FONT_SIZE_MIN) size_px = FONT_SIZE_MIN;
    if (size_px > FONT_SIZE_MAX) size_px = FONT_SIZE_MAX;
    st->size_px = size_px;
    (void)apply_preview_font(st);
}

static void usage(void) {
    puts("Usage: fontpreview <font.otf|font.ttf> [initial-size]");
    puts("Example: fontpreview /fonts/unscii-16.otf 14");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    app_state_t st;
    memset(&st, 0, sizeof(st));
    st.running = 1;
    st.size_px = FONT_SIZE_DEFAULT;
    str_copy(st.base_path, (int)sizeof(st.base_path), argv[1]);
    if (argc >= 3 && argv[2] && argv[2][0]) {
        int parsed = 0;
        if (parse_int(argv[2], &parsed) == 0) {
            if (parsed < FONT_SIZE_MIN) parsed = FONT_SIZE_MIN;
            if (parsed > FONT_SIZE_MAX) parsed = FONT_SIZE_MAX;
            st.size_px = parsed;
        }
    }
    str_copy(st.status, (int)sizeof(st.status), "ready");
    str_copy(st.install_status, (int)sizeof(st.install_status), "press I to install selected size");

    st.handle = gui_attach("Font Preview", "1/2 switch size | I install | Q quit");
    if (st.handle < 0) {
        puts("fontpreview: gui_attach failed");
        return 1;
    }

    if (apply_preview_font(&st) != 0) {
        printf("fontpreview: failed to load %s\n", st.base_path);
    }

    render(&st);

    while (st.running) {
        gui_event_t ev;
        int rc = gui_wait_event(st.handle, &ev);
        if (rc <= 0) continue;

        if (ev.type == GUI_EVENT_CLOSE) {
            break;
        }

        if (ev.type == GUI_EVENT_KEY) {
            unsigned ch = (unsigned)ev.a & 0xFFu;
            if (ev.a == 27 || ch == 'q' || ch == 'Q') {
                break;
            } else if (ch == '1') {
                change_size(&st, 8);
            } else if (ch == '2') {
                change_size(&st, 16);
            } else if (ch == '+' || ch == '=') {
                change_size(&st, st.size_px + 1);
            } else if (ch == '-' || ch == '_') {
                change_size(&st, st.size_px - 1);
            } else if (ch == 'i' || ch == 'I') {
                install_selected_font(&st);
            }
        }

        render(&st);
    }

    return 0;
}
