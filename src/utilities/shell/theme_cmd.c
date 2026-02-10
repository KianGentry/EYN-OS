#include <shell.h>
#include <types.h>
#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <fs/vfs.h>
#include <ui_prefs.h>
#include <context.h>
#include <misc/sched.h>

#include <shell_command_info.h>
#include <string.h>


extern uint8_t g_current_drive;

#define THEME_GUI_MAX_FONTS 64
#define THEME_GUI_MAX_FONT_PATH 96

typedef enum {
    THEME_FIELD_FONT = 0,
    THEME_FIELD_TITLE_FOCUSED,
    THEME_FIELD_TITLE_UNFOCUSED,
    THEME_FIELD_STATUS,
    THEME_FIELD_STATUS_TEXT,
    THEME_FIELD_SAVE,
    THEME_FIELD_RESET,
    THEME_FIELD_CLOSE,
    THEME_FIELD_COUNT
} theme_field_t;

typedef struct {
    int active;
    int win_id;

    int selected_field;
    int channel; // 0=R,1=G,2=B

    int font_dropdown;
    int font_scroll;
    int font_selected;
    int font_count;
    char fonts[THEME_GUI_MAX_FONTS][THEME_GUI_MAX_FONT_PATH]; // full paths

    wm_theme_t theme;
    char status_right[64];
} theme_gui_t;

static theme_gui_t g_theme_gui;

static int theme_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static int ends_with_hex(const char* name) {
    if (!name) return 0;
    int n = (int)strlen(name);
    if (n < 4) return 0;
    return (name[n-4] == '.' && name[n-3] == 'h' && name[n-2] == 'e' && name[n-1] == 'x');
}

static int font_list_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    theme_gui_t* st = (theme_gui_t*)user;
    if (!st || is_dir) return 0;
    if (!ends_with_hex(name)) return 0;
    if (st->font_count >= THEME_GUI_MAX_FONTS) return 1; // stop
    snprintf(st->fonts[st->font_count], THEME_GUI_MAX_FONT_PATH, "/fonts/%s", name);
    st->fonts[st->font_count][THEME_GUI_MAX_FONT_PATH - 1] = '\0';
    st->font_count++;
    return 0;
}

static void theme_gui_refresh_fonts(theme_gui_t* st) {
    if (!st) return;
    st->font_count = 0;
    memset(st->fonts, 0, sizeof(st->fonts));
    if (!theme_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    vfs_listdir(g_current_drive, "/fonts", font_list_cb, st);

    // Choose current font if present
    const char* cur = ui_prefs_get_font_path();
    st->font_selected = 0;
    for (int i = 0; i < st->font_count; ++i) {
        if (strcmp(st->fonts[i], cur) == 0) { st->font_selected = i; break; }
    }
    st->font_scroll = 0;
}

static void theme_gui_apply_theme(theme_gui_t* st) {
    if (!st) return;
    wm_theme_set(&st->theme);
    wm_invalidate_window(st->win_id);
}

static void theme_gui_apply_font(theme_gui_t* st) {
    if (!st) return;
    if (st->font_count <= 0) return;
    const char* path = st->fonts[st->font_selected];
    if (!path || !path[0]) return;
    if (!theme_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    if (vga_system_font_set(g_current_drive, path) == 0) {
        ui_prefs_set_font_path(path);
        snprintf(st->status_right, sizeof(st->status_right), "Font: %s", path);
    } else {
        snprintf(st->status_right, sizeof(st->status_right), "Font load failed");
    }
    st->status_right[sizeof(st->status_right) - 1] = '\0';
    wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
    wm_invalidate_window(st->win_id);
}

static void theme_gui_draw(int tile_idx, int cx, int cy, int cw, int ch, void* ud) {
    (void)tile_idx;
    (void)ud;

    theme_gui_t* st = &g_theme_gui;
    if (!st->active) return;

    vga_mark_dirty_rect(cx, cy, cw, ch);
    drawRect(cx, cy, cw, ch, 0, 0, 0);

    int cell_w = vga_text_cell_w();
    int cell_h = vga_text_cell_h();
    int row_h = cell_h + 2;

    int x = cx + 8;
    int y = cy + 8;

    const char* labels[THEME_FIELD_COUNT] = {
        "Font",
        "Title (focused)",
        "Title (unfocused)",
        "Status bar",
        "Status text",
        "Save",
        "Reset defaults",
        "Close",
    };

    // Helper: draw a single line with selection
    for (int i = 0; i < THEME_FIELD_COUNT; ++i) {
        int sel = (i == st->selected_field);
        int line_y = y + i * row_h;
        if (sel) {
            drawRect(cx + 2, line_y - 1, cw - 4, row_h, 24, 24, 24);
        }

        char buf[192];
        buf[0] = '\0';

        if (i == THEME_FIELD_FONT) {
            const char* cur = ui_prefs_get_font_path();
            snprintf(buf, sizeof(buf), "%s: %s%s", labels[i], cur ? cur : "(none)", st->font_dropdown ? "  [v]" : "  [>]" );
        } else if (i == THEME_FIELD_TITLE_FOCUSED) {
            snprintf(buf, sizeof(buf), "%s: R=%u G=%u B=%u", labels[i], (unsigned)st->theme.title_focused_r, (unsigned)st->theme.title_focused_g, (unsigned)st->theme.title_focused_b);
        } else if (i == THEME_FIELD_TITLE_UNFOCUSED) {
            snprintf(buf, sizeof(buf), "%s: R=%u G=%u B=%u", labels[i], (unsigned)st->theme.title_unfocused_r, (unsigned)st->theme.title_unfocused_g, (unsigned)st->theme.title_unfocused_b);
        } else if (i == THEME_FIELD_STATUS) {
            snprintf(buf, sizeof(buf), "%s: R=%u G=%u B=%u", labels[i], (unsigned)st->theme.status_r, (unsigned)st->theme.status_g, (unsigned)st->theme.status_b);
        } else if (i == THEME_FIELD_STATUS_TEXT) {
            snprintf(buf, sizeof(buf), "%s: R=%u G=%u B=%u", labels[i], (unsigned)st->theme.status_text_r, (unsigned)st->theme.status_text_g, (unsigned)st->theme.status_text_b);
        } else {
            snprintf(buf, sizeof(buf), "%s", labels[i]);
        }

        // Mark current channel for color fields
        if (sel && (i >= THEME_FIELD_TITLE_FOCUSED && i <= THEME_FIELD_STATUS_TEXT)) {
            const char* chn = (st->channel == 0) ? "R" : (st->channel == 1 ? "G" : "B");
            int len = (int)strlen(buf);
            if (len + 6 < (int)sizeof(buf)) {
                snprintf(buf + len, sizeof(buf) - (size_t)len, "  [ch:%s]", chn);
            }
        }

        // Draw prefix marker
        if (sel) {
            drawCharAt(x - 2 * cell_w, line_y, '>', 200, 200, 200);
        }
        drawTextAt(x, line_y, buf, 200, 200, 200);
    }

    // Font dropdown list
    if (st->font_dropdown) {
        int list_x = x + 2 * cell_w;
        int list_y = y + THEME_FIELD_FONT * row_h + row_h;
        int max_rows = (ch - (list_y - cy) - 8) / row_h;
        if (max_rows > 10) max_rows = 10;
        if (max_rows < 3) max_rows = 3;

        int box_h = max_rows * row_h + 4;
        int box_w = cw - (list_x - cx) - 12;
        if (box_w < 80) box_w = 80;
        drawRect(list_x - 4, list_y - 2, box_w, box_h, 12, 12, 12);

        int start = st->font_scroll;
        if (start < 0) start = 0;
        if (start > st->font_count - 1) start = st->font_count - 1;
        for (int r = 0; r < max_rows; ++r) {
            int idx = start + r;
            if (idx >= st->font_count) break;
            int yy = list_y + r * row_h;
            int sel = (idx == st->font_selected);
            if (sel) drawRect(list_x - 2, yy - 1, box_w - 4, row_h, 24, 24, 24);
            // display just filename
            const char* full = st->fonts[idx];
            const char* base = full;
            for (const char* p = full; *p; ++p) { if (*p == '/') base = p + 1; }
            drawTextAt(list_x, yy, base, 200, 200, 200);
        }
    }
}

static void adjust_u8(uint8* v, int delta) {
    int x = (int)(*v) + delta;
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    *v = (uint8)x;
}

static void theme_gui_adjust_selected(theme_gui_t* st, int delta) {
    if (!st) return;

    uint8* r = NULL; uint8* g = NULL; uint8* b = NULL;
    if (st->selected_field == THEME_FIELD_TITLE_FOCUSED) {
        r = &st->theme.title_focused_r; g = &st->theme.title_focused_g; b = &st->theme.title_focused_b;
    } else if (st->selected_field == THEME_FIELD_TITLE_UNFOCUSED) {
        r = &st->theme.title_unfocused_r; g = &st->theme.title_unfocused_g; b = &st->theme.title_unfocused_b;
    } else if (st->selected_field == THEME_FIELD_STATUS) {
        r = &st->theme.status_r; g = &st->theme.status_g; b = &st->theme.status_b;
    } else if (st->selected_field == THEME_FIELD_STATUS_TEXT) {
        r = &st->theme.status_text_r; g = &st->theme.status_text_g; b = &st->theme.status_text_b;
    } else {
        return;
    }

    if (st->channel == 0) adjust_u8(r, delta);
    else if (st->channel == 1) adjust_u8(g, delta);
    else adjust_u8(b, delta);

    theme_gui_apply_theme(st);
}

static void theme_gui_key(int tile_idx, int key, void* ud) {
    (void)tile_idx;
    (void)ud;

    theme_gui_t* st = &g_theme_gui;
    if (!st->active) return;

    // Close shortcuts
    if (key == 0x2002 || key == 0x2101) { // Ctrl+X or Ctrl+Q
        wm_close_window(st->win_id);
        st->active = 0;
        return;
    }

    if (key == '\t') {
        st->channel = (st->channel + 1) % 3;
        wm_invalidate_window(st->win_id);
        return;
    }

    if (key == 0x1001) { // Up
        if (st->font_dropdown && st->selected_field == THEME_FIELD_FONT) {
            if (st->font_selected > 0) st->font_selected--;
            if (st->font_selected < st->font_scroll) st->font_scroll = st->font_selected;
            wm_invalidate_window(st->win_id);
            return;
        }
        if (st->selected_field > 0) st->selected_field--;
        wm_invalidate_window(st->win_id);
        return;
    }

    if (key == 0x1002) { // Down
        if (st->font_dropdown && st->selected_field == THEME_FIELD_FONT) {
            if (st->font_selected + 1 < st->font_count) st->font_selected++;
            int view_rows = 8;
            if (st->font_selected >= st->font_scroll + view_rows) st->font_scroll = st->font_selected - view_rows + 1;
            wm_invalidate_window(st->win_id);
            return;
        }
        if (st->selected_field + 1 < THEME_FIELD_COUNT) st->selected_field++;
        wm_invalidate_window(st->win_id);
        return;
    }

    if (key == 0x1003) { // Left
        if (st->selected_field == THEME_FIELD_FONT) {
            if (!st->font_dropdown && st->font_count > 0) {
                if (st->font_selected > 0) { st->font_selected--; theme_gui_apply_font(st); }
                wm_invalidate_window(st->win_id);
            }
            return;
        }
        theme_gui_adjust_selected(st, -5);
        return;
    }

    if (key == 0x1004) { // Right
        if (st->selected_field == THEME_FIELD_FONT) {
            if (!st->font_dropdown && st->font_count > 0) {
                if (st->font_selected + 1 < st->font_count) { st->font_selected++; theme_gui_apply_font(st); }
                wm_invalidate_window(st->win_id);
            }
            return;
        }
        theme_gui_adjust_selected(st, +5);
        return;
    }

    if (key == 's' || key == 'S') {
        if (!theme_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) {
            snprintf(st->status_right, sizeof(st->status_right), "Save blocked");
            st->status_right[sizeof(st->status_right) - 1] = '\0';
            wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
            wm_invalidate_window(st->win_id);
            return;
        }
        int r = ui_prefs_save(g_current_drive);
        snprintf(st->status_right, sizeof(st->status_right), (r == 0) ? "Saved" : "Save failed");
        st->status_right[sizeof(st->status_right) - 1] = '\0';
        wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
        wm_invalidate_window(st->win_id);
        return;
    }

    if (key == 'r' || key == 'R') {
        wm_theme_reset_defaults();
        wm_theme_get(&st->theme);
        snprintf(st->status_right, sizeof(st->status_right), "Defaults");
        wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
        wm_invalidate_window(st->win_id);
        return;
    }

    if (key == '\n' || key == 13) {
        if (st->selected_field == THEME_FIELD_FONT) {
            st->font_dropdown = !st->font_dropdown;
            if (!st->font_dropdown) {
                // closing dropdown applies selected font
                theme_gui_apply_font(st);
            }
            wm_invalidate_window(st->win_id);
            return;
        }
        if (st->selected_field == THEME_FIELD_SAVE) {
            if (!theme_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) {
                snprintf(st->status_right, sizeof(st->status_right), "Save blocked");
                st->status_right[sizeof(st->status_right) - 1] = '\0';
                wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
                wm_invalidate_window(st->win_id);
                return;
            }
            int r = ui_prefs_save(g_current_drive);
            snprintf(st->status_right, sizeof(st->status_right), (r == 0) ? "Saved" : "Save failed");
            wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
            wm_invalidate_window(st->win_id);
            return;
        }
        if (st->selected_field == THEME_FIELD_RESET) {
            wm_theme_reset_defaults();
            wm_theme_get(&st->theme);
            snprintf(st->status_right, sizeof(st->status_right), "Defaults");
            wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", st->status_right);
            wm_invalidate_window(st->win_id);
            return;
        }
        if (st->selected_field == THEME_FIELD_CLOSE) {
            wm_close_window(st->win_id);
            st->active = 0;
            return;
        }
    }
}

static void theme_gui_mouse(int tile_idx, const mouse_event_t* me, void* ud) {
    (void)tile_idx;
    (void)ud;
    theme_gui_t* st = &g_theme_gui;
    if (!st->active || !me) return;
    int left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    if (!left_down) return;

    // Convert click to selection based on current font metrics.
    // We only use relative coordinates within the window content.
    (void)me;
}

static void theme_cmd(string arg) {
    (void)arg;
    if (!theme_ctx_allow(CAP_WRITE_CONSOLE | CAP_READ_FS, SCHED_COST_CONSOLE)) return;

    theme_gui_t* st = &g_theme_gui;
    if (st->active && st->win_id >= 0) {
        wm_invalidate_window(st->win_id);
        return;
    }

    memset(st, 0, sizeof(*st));
    st->active = 1;
    st->selected_field = 0;
    st->channel = 0;
    st->font_dropdown = 0;

    wm_theme_get(&st->theme);
    theme_gui_refresh_fonts(st);

    int w = 520;
    int h = 260;
    int x = 60;
    int y = 50;
    st->win_id = wm_create_window("Theme", x, y, w, h, "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close");
    if (st->win_id < 0) {
        st->active = 0;
        return;
    }

    st->status_right[0] = '\0';
    wm_set_title_status(st->win_id, "Theme", "^/v: Select | </>: Adjust | Tab: Channel | Enter: Toggle Font | S: Save | R: Reset | Ctrl+X: Close", NULL);
    wm_register_gui_client2(st->win_id, theme_gui_draw, theme_gui_key, theme_gui_mouse, NULL);
    wm_invalidate_window(st->win_id);
}

REGISTER_SHELL_COMMAND(theme_cmd_info, "theme", theme_cmd, CMD_STREAMING, "Open a GUI theme editor (colors + font).", "theme");
