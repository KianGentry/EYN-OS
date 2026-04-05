#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>
#include <gui.h>

EYN_CMDMETA_V1("Open system settings (video + customization).", "settings");

#define ROW_H 18
#define HEADER_H 26
#define DBLCLICK_FRAMES 20

typedef struct {
    int width;
    int height;
    int bpp;
    const char* label;
} mode_preset_t;

typedef struct {
    int handle;
    int running;
    int section; // 0=Video, 1=Customization
    int row;

    int hover_section;
    int hover_row;

    int prev_left_down;
    int last_click_row;
    int dblclick_timer;

    int scale_idx;
    int aspect_idx;
    int mode_idx;

    eyn_display_profile_t profile;
    eyn_display_mode_t mode;
    char status[96];
} settings_state_t;

typedef struct {
    int w;
    int h;
    int tab_y;
    int tab_h;
    int tab_video_x;
    int tab_video_w;
    int tab_custom_x;
    int tab_custom_w;
    int list_x;
    int list_y;
    int list_w;
    int row_count;
} settings_layout_t;

static const mode_preset_t k_mode_presets[] = {
    {640, 480, 32, "640x480  (4:3)"},
    {800, 600, 32, "800x600  (4:3)"},
    {1024, 768, 32, "1024x768 (4:3)"},
    {1280, 720, 32, "1280x720 (16:9)"},
    {1280, 800, 32, "1280x800 (16:10)"},
};

static const int k_scale_values[] = {100, 90, 75, 60, 50};
static const char* k_aspect_names[] = {"Native", "4:3", "16:10", "16:9", "21:9", "1:1"};

static void text_copy(char* dst, int cap, const char* src) {
    int i = 0;
    if (!dst || cap <= 0) return;
    if (!src) src = "";
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < (rx + rw) && y < (ry + rh);
}

static void set_status(settings_state_t* st, const char* msg) {
    if (!st) return;
    text_copy(st->status, (int)sizeof(st->status), msg);
}

static int section_row_count(int section) {
    if (section == 0) return 6; // Video rows
    return 4; // Customization rows
}

static int mode_index_for_values(int width, int height, int bpp) {
    int best_i = 0;
    int best_score = 0x7FFFFFFF;
    int i;
    for (i = 0; i < (int)(sizeof(k_mode_presets) / sizeof(k_mode_presets[0])); ++i) {
        int dw = k_mode_presets[i].width - width;
        int dh = k_mode_presets[i].height - height;
        int db = (k_mode_presets[i].bpp == bpp) ? 0 : 1000;
        if (dw < 0) dw = -dw;
        if (dh < 0) dh = -dh;
        int score = dw + dh + db;
        if (score < best_score) {
            best_score = score;
            best_i = i;
        }
    }
    return best_i;
}

static int scale_index_for_value(int value) {
    int i;
    int best_i = 0;
    int best_delta = 10000;
    for (i = 0; i < (int)(sizeof(k_scale_values) / sizeof(k_scale_values[0])); ++i) {
        int d = k_scale_values[i] - value;
        if (d < 0) d = -d;
        if (d < best_delta) {
            best_delta = d;
            best_i = i;
        }
    }
    return best_i;
}

static int aspect_index_for_value(int value) {
    if (value < EYN_ASPECT_NATIVE || value > EYN_ASPECT_1_1) return EYN_ASPECT_NATIVE;
    return value;
}

static void refresh_profile(settings_state_t* st) {
    if (!st) return;
    if (eyn_sys_get_display_profile(&st->profile) != 0) {
        st->profile.fb_w = 640;
        st->profile.fb_h = 480;
        st->profile.workspace_w = 640;
        st->profile.workspace_h = 480;
        st->profile.scale_pct = 100;
        st->profile.aspect_mode = EYN_ASPECT_NATIVE;
        set_status(st, "failed to read workspace profile");
        return;
    }
    st->scale_idx = scale_index_for_value(st->profile.scale_pct);
    st->aspect_idx = aspect_index_for_value(st->profile.aspect_mode);
}

static void refresh_mode(settings_state_t* st) {
    if (!st) return;
    if (eyn_sys_get_display_mode(&st->mode) != 0) {
        st->mode.width = st->profile.fb_w;
        st->mode.height = st->profile.fb_h;
        st->mode.bpp = 32;
        st->mode.can_switch = 0;
    }
    st->mode_idx = mode_index_for_values(st->mode.width, st->mode.height, st->mode.bpp);
}

static void refresh_all(settings_state_t* st) {
    refresh_profile(st);
    refresh_mode(st);
}

static void apply_font(settings_state_t* st, const char* path) {
    if (eyn_sys_setfont_path(path) != 0) {
        set_status(st, "font apply failed");
        return;
    }
    set_status(st, "font applied for current session");
}

static void apply_video(settings_state_t* st, int persist) {
    int mode_ok = 0;
    int profile_ok = 0;

    eyn_display_mode_set_t mode_req;
    mode_req.width = k_mode_presets[st->mode_idx].width;
    mode_req.height = k_mode_presets[st->mode_idx].height;
    mode_req.bpp = k_mode_presets[st->mode_idx].bpp;
    mode_req.persist = persist ? 1 : 0;

    if (eyn_sys_set_display_mode(&mode_req) == 0) mode_ok = 1;
    if (eyn_sys_set_display_profile(k_scale_values[st->scale_idx], st->aspect_idx, persist ? 1 : 0) == 0) {
        profile_ok = 1;
    }

    refresh_all(st);

    if (mode_ok && profile_ok) {
        set_status(st, persist ? "saved and applied" : "applied");
    } else if (!mode_ok && profile_ok) {
        set_status(st, "workspace applied; hardware switch unavailable");
    } else if (mode_ok && !profile_ok) {
        set_status(st, "hardware mode applied; workspace apply failed");
    } else {
        set_status(st, "apply failed");
    }
}

static void reset_video_defaults(settings_state_t* st) {
    st->mode_idx = 0; // 640x480x32
    st->scale_idx = 0; // 100%
    st->aspect_idx = EYN_ASPECT_NATIVE;
    apply_video(st, 1);
}

static void activate_current_row(settings_state_t* st) {
    if (!st) return;

    if (st->section == 0) {
        if (st->row == 3) {
            apply_video(st, 0);
        } else if (st->row == 4) {
            apply_video(st, 1);
        } else if (st->row == 5) {
            reset_video_defaults(st);
        }
        return;
    }

    if (st->row == 0) {
        apply_font(st, "builtin");
    } else if (st->row == 1) {
        apply_font(st, "/fonts/unscii-16.hex");
    } else if (st->row == 2) {
        st->running = 0;
        (void)eyn_sys_run("theme");
    } else if (st->row == 3) {
        st->running = 0;
        (void)eyn_sys_run("fontpreview /fonts/unscii-16.otf");
    }
}

static int cycle_video_row(settings_state_t* st, int row, int dir) {
    int changed = 0;
    if (!st || dir == 0) return 0;

    if (row == 0) {
        int max_i = (int)(sizeof(k_mode_presets) / sizeof(k_mode_presets[0])) - 1;
        int next = st->mode_idx + (dir > 0 ? 1 : -1);
        if (next < 0) next = max_i;
        if (next > max_i) next = 0;
        if (next != st->mode_idx) {
            st->mode_idx = next;
            changed = 1;
        }
    } else if (row == 1) {
        int max_i = (int)(sizeof(k_scale_values) / sizeof(k_scale_values[0])) - 1;
        int next = clampi(st->scale_idx + (dir > 0 ? 1 : -1), 0, max_i);
        if (next != st->scale_idx) {
            st->scale_idx = next;
            changed = 1;
        }
    } else if (row == 2) {
        int next = clampi(st->aspect_idx + (dir > 0 ? 1 : -1), EYN_ASPECT_NATIVE, EYN_ASPECT_1_1);
        if (next != st->aspect_idx) {
            st->aspect_idx = next;
            changed = 1;
        }
    }
    return changed;
}

static void layout_compute(const settings_state_t* st, settings_layout_t* lo) {
    (void)st;
    if (!lo) return;
    lo->tab_y = HEADER_H + 4;
    lo->tab_h = 18;
    lo->tab_video_x = 8;
    lo->tab_video_w = 96;
    lo->tab_custom_x = 110;
    lo->tab_custom_w = 136;
    lo->list_x = 8;
    lo->list_y = lo->tab_y + lo->tab_h + 8;
    lo->list_w = lo->w - 16;
    lo->row_count = section_row_count(st->section);
}

static int row_from_mouse(const settings_layout_t* lo, int mx, int my) {
    if (!lo) return -1;
    if (!point_in_rect(mx, my, lo->list_x, lo->list_y, lo->list_w, lo->row_count * ROW_H)) return -1;
    int row = (my - lo->list_y) / ROW_H;
    if (row < 0 || row >= lo->row_count) return -1;
    return row;
}

static int handle_key(settings_state_t* st, int key) {
    int changed = 0;
    unsigned ch;
    int rows;

    if (!st) return 0;
    ch = (unsigned)key & 0xFFu;

    if (key == 27 || ch == 'q' || ch == 'Q') {
        st->running = 0;
        return 1;
    }

    if (ch == '\t') {
        st->section = (st->section == 0) ? 1 : 0;
        st->row = 0;
        return 1;
    }

    rows = section_row_count(st->section);

    if (key == GUI_KEY_UP) {
        st->row--;
        if (st->row < 0) st->row = rows - 1;
        return 1;
    }
    if (key == GUI_KEY_DOWN) {
        st->row++;
        if (st->row >= rows) st->row = 0;
        return 1;
    }

    if (st->section == 0) {
        if (key == GUI_KEY_LEFT) {
            if (cycle_video_row(st, st->row, -1)) return 1;
            return 0;
        }
        if (key == GUI_KEY_RIGHT) {
            if (cycle_video_row(st, st->row, 1)) return 1;
            return 0;
        }
        if (ch == 'a' || ch == 'A') {
            apply_video(st, 0);
            return 1;
        }
        if (ch == 's' || ch == 'S') {
            apply_video(st, 1);
            return 1;
        }
    }

    if (key == '\n' || key == 13) {
        activate_current_row(st);
        return 1;
    }

    return changed;
}

static int handle_mouse(settings_state_t* st, const gui_event_t* ev, const settings_layout_t* lo) {
    if (!st || !ev || !lo) return 0;

    int changed = 0;
    int mx = ev->a;
    int my = ev->b;

    st->hover_section = -1;
    st->hover_row = -1;

    if (point_in_rect(mx, my, lo->tab_video_x, lo->tab_y, lo->tab_video_w, lo->tab_h)) {
        st->hover_section = 0;
    } else if (point_in_rect(mx, my, lo->tab_custom_x, lo->tab_y, lo->tab_custom_w, lo->tab_h)) {
        st->hover_section = 1;
    } else {
        st->hover_row = row_from_mouse(lo, mx, my);
    }

    if (ev->d > 0) {
        if (st->row > 0) st->row--;
        changed = 1;
    } else if (ev->d < 0) {
        int max_row = section_row_count(st->section) - 1;
        if (st->row < max_row) st->row++;
        changed = 1;
    }

    int left_down = (ev->c & 0x1) != 0;
    int press_edge = left_down && !st->prev_left_down;
    st->prev_left_down = left_down;

    if (!press_edge) {
        if (st->dblclick_timer > 0) st->dblclick_timer--;
        return changed;
    }

    if (st->hover_section == 0 || st->hover_section == 1) {
        if (st->section != st->hover_section) {
            st->section = st->hover_section;
            st->row = 0;
            changed = 1;
        }
        return changed;
    }

    if (st->hover_row < 0) return changed;

    st->row = st->hover_row;
    changed = 1;

    if (st->hover_row == st->last_click_row && st->dblclick_timer > 0) {
        activate_current_row(st);
        st->dblclick_timer = 0;
        st->last_click_row = -1;
        return 1;
    }

    st->last_click_row = st->hover_row;
    st->dblclick_timer = DBLCLICK_FRAMES;

    if (st->section == 0) {
        if (st->row <= 2) {
            int dir = (mx >= (lo->list_x + lo->list_w / 2)) ? 1 : -1;
            if (cycle_video_row(st, st->row, dir)) changed = 1;
        } else {
            activate_current_row(st);
            changed = 1;
        }
    } else {
        activate_current_row(st);
        changed = 1;
    }

    return changed;
}

static void draw_text_line(int h, int x, int y, unsigned char r, unsigned char g, unsigned char b, const char* text) {
    gui_text_t t;
    t.x = x;
    t.y = y;
    t.r = r;
    t.g = g;
    t.b = b;
    t._pad = 0;
    t.text = text;
    (void)gui_draw_text(h, &t);
}

static void draw_section_rows(settings_state_t* st, const settings_layout_t* lo) {
    int i;
    char line[120];

    for (i = 0; i < lo->row_count; ++i) {
        int y = lo->list_y + i * ROW_H;

        if (i == st->row) {
            gui_rect_t hi = {
                .x = lo->list_x,
                .y = y,
                .w = lo->list_w,
                .h = ROW_H,
                .r = GUI_PAL_SEL_R,
                .g = GUI_PAL_SEL_G,
                .b = GUI_PAL_SEL_B,
                ._pad = 0,
            };
            (void)gui_fill_rect(st->handle, &hi);
        } else if (i == st->hover_row) {
            gui_rect_t hv = {
                .x = lo->list_x,
                .y = y,
                .w = lo->list_w,
                .h = ROW_H,
                .r = GUI_PAL_HOVER_R,
                .g = GUI_PAL_HOVER_G,
                .b = GUI_PAL_HOVER_B,
                ._pad = 0,
            };
            (void)gui_fill_rect(st->handle, &hv);
        }

        if (st->section == 0) {
            if (i == 0) {
                snprintf(line, sizeof(line), "Hardware mode: %s", k_mode_presets[st->mode_idx].label);
            } else if (i == 1) {
                snprintf(line, sizeof(line), "Workspace scale: %d%%", k_scale_values[st->scale_idx]);
            } else if (i == 2) {
                snprintf(line, sizeof(line), "Workspace aspect: %s", k_aspect_names[st->aspect_idx]);
            } else if (i == 3) {
                snprintf(line, sizeof(line), "Apply now");
            } else if (i == 4) {
                snprintf(line, sizeof(line), "Save + apply");
            } else {
                snprintf(line, sizeof(line), "Reset defaults (640x480 + native + 100%%)");
            }
        } else {
            if (i == 0) snprintf(line, sizeof(line), "Use built-in font");
            else if (i == 1) snprintf(line, sizeof(line), "Use /fonts/unscii-16.hex");
            else if (i == 2) snprintf(line, sizeof(line), "Open Theme editor");
            else snprintf(line, sizeof(line), "Open Font preview");
        }

        draw_text_line(st->handle, lo->list_x + 8, y + 4, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, line);
    }
}

static void render(settings_state_t* st) {
    gui_size_t sz = {0, 0};
    settings_layout_t lo;

    (void)gui_get_content_size(st->handle, &sz);
    if (sz.w <= 0) sz.w = 560;
    if (sz.h <= 0) sz.h = 380;

    lo.w = sz.w;
    lo.h = sz.h;
    layout_compute(st, &lo);

    (void)gui_begin(st->handle);

    gui_rect_t bg = {
        .x = 0,
        .y = 0,
        .w = sz.w,
        .h = sz.h,
        .r = GUI_PAL_BG_R,
        .g = GUI_PAL_BG_G,
        .b = GUI_PAL_BG_B,
        ._pad = 0,
    };
    (void)gui_fill_rect(st->handle, &bg);

    gui_rect_t head = {
        .x = 0,
        .y = 0,
        .w = sz.w,
        .h = HEADER_H,
        .r = GUI_PAL_HEADER_R,
        .g = GUI_PAL_HEADER_G,
        .b = GUI_PAL_HEADER_B,
        ._pad = 0,
    };
    (void)gui_fill_rect(st->handle, &head);
    draw_text_line(st->handle, 8, 8, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, "Settings");

    gui_rect_t tab_video = {
        .x = lo.tab_video_x,
        .y = lo.tab_y,
        .w = lo.tab_video_w,
        .h = lo.tab_h,
        .r = (st->section == 0) ? GUI_PAL_SEL_R : (st->hover_section == 0 ? GUI_PAL_HOVER_R : GUI_PAL_SURFACE_R),
        .g = (st->section == 0) ? GUI_PAL_SEL_G : (st->hover_section == 0 ? GUI_PAL_HOVER_G : GUI_PAL_SURFACE_G),
        .b = (st->section == 0) ? GUI_PAL_SEL_B : (st->hover_section == 0 ? GUI_PAL_HOVER_B : GUI_PAL_SURFACE_B),
        ._pad = 0,
    };
    (void)gui_fill_rect(st->handle, &tab_video);
    draw_text_line(st->handle, lo.tab_video_x + 28, lo.tab_y + 4, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, "Video");

    gui_rect_t tab_custom = {
        .x = lo.tab_custom_x,
        .y = lo.tab_y,
        .w = lo.tab_custom_w,
        .h = lo.tab_h,
        .r = (st->section == 1) ? GUI_PAL_SEL_R : (st->hover_section == 1 ? GUI_PAL_HOVER_R : GUI_PAL_SURFACE_R),
        .g = (st->section == 1) ? GUI_PAL_SEL_G : (st->hover_section == 1 ? GUI_PAL_HOVER_G : GUI_PAL_SURFACE_G),
        .b = (st->section == 1) ? GUI_PAL_SEL_B : (st->hover_section == 1 ? GUI_PAL_HOVER_B : GUI_PAL_SURFACE_B),
        ._pad = 0,
    };
    (void)gui_fill_rect(st->handle, &tab_custom);
    draw_text_line(st->handle, lo.tab_custom_x + 24, lo.tab_y + 4, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, "Customization");

    gui_line_t sep = {
        .x1 = lo.list_x,
        .y1 = lo.list_y - 2,
        .x2 = lo.list_x + lo.list_w - 1,
        .y2 = lo.list_y - 2,
        .r = GUI_PAL_BORDER_R,
        .g = GUI_PAL_BORDER_G,
        .b = GUI_PAL_BORDER_B,
        ._pad = 0,
    };
    (void)gui_draw_line(st->handle, &sep);

    draw_section_rows(st, &lo);

    char info[128];
    snprintf(info, sizeof(info), "Framebuffer: %dx%dx%d | Workspace: %dx%d",
             st->mode.width, st->mode.height, st->mode.bpp,
             st->profile.workspace_w, st->profile.workspace_h);
    draw_text_line(st->handle, 8, sz.h - 44, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, info);

    draw_text_line(st->handle, 8, sz.h - 30, GUI_PAL_ACCENT_R, GUI_PAL_ACCENT_G, GUI_PAL_ACCENT_B, st->status);

    draw_text_line(st->handle, 8, sz.h - 16, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B,
                   "Mouse: click rows/tabs | Left/Right: adjust | Enter/A/S: apply | Q/Esc: quit");

    if (!st->mode.can_switch) {
        draw_text_line(st->handle, sz.w - 240, sz.h - 30, 255, 180, 120,
                       "hardware mode switch unavailable");
    }

    (void)gui_present(st->handle);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    settings_state_t st;
    memset(&st, 0, sizeof(st));

    st.running = 1;
    st.section = 0;
    st.row = 0;
    st.hover_section = -1;
    st.hover_row = -1;
    st.last_click_row = -1;

    st.handle = gui_attach("Settings", "Video + customization");
    if (st.handle < 0) {
        puts("settings: gui_attach failed");
        return 1;
    }

    refresh_all(&st);
    set_status(&st, "ready");

    int need_redraw = 1;
    while (st.running) {
        gui_size_t sz;
        settings_layout_t lo;

        sz.w = 0;
        sz.h = 0;
        (void)gui_get_content_size(st.handle, &sz);
        if (sz.w <= 0) sz.w = 560;
        if (sz.h <= 0) sz.h = 380;
        lo.w = sz.w;
        lo.h = sz.h;
        layout_compute(&st, &lo);

        gui_event_t ev;
        while (gui_poll_event(st.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                st.running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEY) {
                if (handle_key(&st, ev.a)) need_redraw = 1;
            } else if (ev.type == GUI_EVENT_MOUSE) {
                if (handle_mouse(&st, &ev, &lo)) need_redraw = 1;
                else need_redraw = 1; // keep hover visuals responsive
            }
        }

        if (need_redraw) {
            render(&st);
            need_redraw = 0;
        } else {
            usleep(8000);
        }
    }

    return 0;
}