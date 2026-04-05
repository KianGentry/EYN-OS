#include <ui_prefs.h>

#include <fs/vfs.h>
#include <tile_manager.h>
#include <vga.h>
#include <string.h>
#include <crashlog.h>

#define UI_PREFS_PATH_PRIMARY "/config/ui.cfg"
#define UI_PREFS_PATH_FALLBACK "/ui.cfg"

static char g_ui_font_path[96] = "/fonts/unscii-16.hex";
static int g_ui_status_bar_mode = 0; // 0=hold Alt, 1=pinned
static int g_ui_workspace_scale_pct = 100;
static int g_ui_workspace_aspect_mode = 0;
static int g_ui_display_width = 640;
static int g_ui_display_height = 480;
static int g_ui_display_bpp = 32;
static uint32 g_ui_prefs_epoch = 1;

typedef struct ui_prefs_snapshot_t {
    char font_path[96];
    uint8 status_bar_mode;
    uint8 _pad[3];
} ui_prefs_snapshot_t;

const char* ui_prefs_get_font_path(void) {
    return g_ui_font_path;
}

void ui_prefs_set_font_path(const char* path) {
    if (!path || !path[0]) return;
    strncpy(g_ui_font_path, path, sizeof(g_ui_font_path) - 1);
    g_ui_font_path[sizeof(g_ui_font_path) - 1] = '\0';
}

int ui_prefs_get_status_bar_mode(void) {
    return g_ui_status_bar_mode ? 1 : 0;
}

void ui_prefs_set_status_bar_mode(int mode) {
    g_ui_status_bar_mode = mode ? 1 : 0;
}

int ui_prefs_get_workspace_scale_pct(void) {
    return g_ui_workspace_scale_pct;
}

void ui_prefs_set_workspace_scale_pct(int pct) {
    if (pct < 50) pct = 50;
    if (pct > 100) pct = 100;
    g_ui_workspace_scale_pct = pct;
}

int ui_prefs_get_workspace_aspect_mode(void) {
    return g_ui_workspace_aspect_mode;
}

void ui_prefs_set_workspace_aspect_mode(int mode) {
    if (mode < 0 || mode > 5) mode = 0;
    g_ui_workspace_aspect_mode = mode;
}

int ui_prefs_get_display_width(void) {
    return g_ui_display_width;
}

void ui_prefs_set_display_width(int mode_w) {
    if (mode_w < 320) mode_w = 320;
    if (mode_w > 3840) mode_w = 3840;
    g_ui_display_width = mode_w;
}

int ui_prefs_get_display_height(void) {
    return g_ui_display_height;
}

void ui_prefs_set_display_height(int mode_h) {
    if (mode_h < 200) mode_h = 200;
    if (mode_h > 2160) mode_h = 2160;
    g_ui_display_height = mode_h;
}

int ui_prefs_get_display_bpp(void) {
    return g_ui_display_bpp;
}

void ui_prefs_set_display_bpp(int bpp) {
    if (bpp != 16 && bpp != 24 && bpp != 32) bpp = 32;
    g_ui_display_bpp = bpp;
}

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    return s;
}

static int starts_with(const char* s, const char* pfx) {
    while (*pfx) {
        if (*s++ != *pfx++) return 0;
    }
    return 1;
}

static int parse_u8(const char* s, int* out_v, const char** out_end) {
    int v = 0;
    int any = 0;
    s = skip_ws(s);
    while (*s >= '0' && *s <= '9') {
        any = 1;
        v = v * 10 + (*s - '0');
        if (v > 255) v = 255;
        ++s;
    }
    if (!any) return -1;
    if (out_v) *out_v = v;
    if (out_end) *out_end = s;
    return 0;
}

static int parse_u32(const char* s, uint32* out_v, const char** out_end) {
    uint32 v = 0;
    int any = 0;
    s = skip_ws(s);
    while (*s >= '0' && *s <= '9') {
        any = 1;
        uint32 d = (uint32)(*s - '0');
        if (v > 429496729u) v = 429496729u;
        v = v * 10u + d;
        ++s;
    }
    if (!any) return -1;
    if (out_v) *out_v = v;
    if (out_end) *out_end = s;
    return 0;
}

static int parse_int01(const char* s, int* out_v) {
    int v = 0;
    const char* end = NULL;
    if (parse_u8(s, &v, &end) != 0) return -1;
    if (v != 0) v = 1;
    if (out_v) *out_v = v;
    return 0;
}

static int parse_rgb_triplet(const char* s, uint8* out_r, uint8* out_g, uint8* out_b) {
    int r = 0, g = 0, b = 0;
    const char* p = s;
    if (parse_u8(p, &r, &p) != 0) return -1;
    p = skip_ws(p);
    if (*p != ',') return -1;
    p++;
    if (parse_u8(p, &g, &p) != 0) return -1;
    p = skip_ws(p);
    if (*p != ',') return -1;
    p++;
    if (parse_u8(p, &b, &p) != 0) return -1;
    if (out_r) *out_r = (uint8)r;
    if (out_g) *out_g = (uint8)g;
    if (out_b) *out_b = (uint8)b;
    return 0;
}

static void trim_newline(char* s) {
    if (!s) return;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '\n' || s[i] == '\r') { s[i] = '\0'; return; }
    }
}

static int try_read_prefs(uint8 drive, const char* path, char* buf, int buf_sz) {
    if (!buf || buf_sz <= 0) return -1;
    int n = vfs_read_file(drive, path, buf, buf_sz - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

int ui_prefs_load_apply(uint8 drive) {
    ui_prefs_snapshot_t snap;
    if (crashlog_recover_latest(CRASHLOG_OBJ_UI_PREFS, 1, &snap, sizeof(snap), &g_ui_prefs_epoch) > 0) {
        if (snap.font_path[0]) {
            if (vga_system_font_set(drive, snap.font_path) == 0) {
                ui_prefs_set_font_path(snap.font_path);
            }
        }
        ui_prefs_set_status_bar_mode((int)snap.status_bar_mode);
    }

    char buf[2048];
    int ok = 0;
    if (try_read_prefs(drive, UI_PREFS_PATH_PRIMARY, buf, (int)sizeof(buf)) != 0) {
        if (try_read_prefs(drive, UI_PREFS_PATH_FALLBACK, buf, (int)sizeof(buf)) != 0) {
            return (snap.font_path[0] || snap.status_bar_mode) ? 0 : -1;
        }
    }

    wm_theme_t theme;
    wm_theme_get(&theme);

    // Parse line-by-line: key=value
    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) { *next = '\0'; next++; }
        trim_newline(line);
        const char* s = skip_ws(line);
        if (*s == '\0' || *s == '#') { line = next; continue; }

        // Split key/value
        char* eq = strchr((char*)s, '=');
        if (!eq) { line = next; continue; }
        *eq = '\0';
        const char* key = s;
        const char* val = skip_ws(eq + 1);

        if (starts_with(key, "font")) {
            if (val[0]) {
                // Best-effort apply; if it fails, keep defaults.
                if (vga_system_font_set(drive, val) == 0) {
                    ui_prefs_set_font_path(val);
                    ok = 1;
                }
            }
        } else if (starts_with(key, "title_focused")) {
            parse_rgb_triplet(val, &theme.title_focused_r, &theme.title_focused_g, &theme.title_focused_b);
            ok = 1;
        } else if (starts_with(key, "title_unfocused")) {
            parse_rgb_triplet(val, &theme.title_unfocused_r, &theme.title_unfocused_g, &theme.title_unfocused_b);
            ok = 1;
        } else if (starts_with(key, "status")) {
            // Disambiguate status_text vs status
            if (starts_with(key, "status_text")) {
                parse_rgb_triplet(val, &theme.status_text_r, &theme.status_text_g, &theme.status_text_b);
            } else {
                parse_rgb_triplet(val, &theme.status_r, &theme.status_g, &theme.status_b);
            }
            ok = 1;
        } else if (starts_with(key, "status_bar_mode")) {
            int v = 0;
            if (parse_int01(val, &v) == 0) {
                ui_prefs_set_status_bar_mode(v);
                ok = 1;
            }
        } else if (starts_with(key, "workspace_scale")) {
            int v = 0;
            if (parse_u8(val, &v, NULL) == 0) {
                ui_prefs_set_workspace_scale_pct(v);
                ok = 1;
            }
        } else if (starts_with(key, "workspace_aspect")) {
            int v = 0;
            if (parse_u8(val, &v, NULL) == 0) {
                ui_prefs_set_workspace_aspect_mode(v);
                ok = 1;
            }
        } else if (starts_with(key, "display_width")) {
            uint32 v = 0;
            if (parse_u32(val, &v, NULL) == 0) {
                ui_prefs_set_display_width((int)v);
                ok = 1;
            }
        } else if (starts_with(key, "display_height")) {
            uint32 v = 0;
            if (parse_u32(val, &v, NULL) == 0) {
                ui_prefs_set_display_height((int)v);
                ok = 1;
            }
        } else if (starts_with(key, "display_bpp")) {
            int v = 0;
            if (parse_u8(val, &v, NULL) == 0) {
                ui_prefs_set_display_bpp(v);
                ok = 1;
            }
        }

        line = next;
    }

    wm_theme_set(&theme);
    return ok ? 0 : -1;
}

static int ensure_config_dir(uint8 drive) {
    vfs_stat_t st;
    if (vfs_stat(drive, "/config", &st) == 0 && st.type == VFS_NODE_DIR) return 0;
    // Best-effort create; ignore errors (might already exist).
    return vfs_mkdir(drive, "/config");
}

static int write_prefs(uint8 drive, const char* path, const char* text, uint32 len) {
    int w = vfs_write_file(drive, path, text, len);
    if (w < 0) return -1;
    return 0;
}

int ui_prefs_save(uint8 drive) {
    wm_theme_t theme;
    wm_theme_get(&theme);

    char out[512];
    int n = snprintf(out, sizeof(out),
        "# EYN-OS UI Preferences\n"
        "font=%s\n"
        "title_focused=%u,%u,%u\n"
        "title_unfocused=%u,%u,%u\n"
        "status=%u,%u,%u\n"
        "status_text=%u,%u,%u\n"
        "status_bar_mode=%u\n"
        "workspace_scale=%u\n"
        "workspace_aspect=%u\n"
        "display_width=%u\n"
        "display_height=%u\n"
        "display_bpp=%u\n",
        ui_prefs_get_font_path(),
        (unsigned)theme.title_focused_r, (unsigned)theme.title_focused_g, (unsigned)theme.title_focused_b,
        (unsigned)theme.title_unfocused_r, (unsigned)theme.title_unfocused_g, (unsigned)theme.title_unfocused_b,
        (unsigned)theme.status_r, (unsigned)theme.status_g, (unsigned)theme.status_b,
        (unsigned)theme.status_text_r, (unsigned)theme.status_text_g, (unsigned)theme.status_text_b,
        (unsigned)ui_prefs_get_status_bar_mode(),
        (unsigned)ui_prefs_get_workspace_scale_pct(),
        (unsigned)ui_prefs_get_workspace_aspect_mode(),
        (unsigned)ui_prefs_get_display_width(),
        (unsigned)ui_prefs_get_display_height(),
        (unsigned)ui_prefs_get_display_bpp()
    );
    if (n <= 0) return -1;
    if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;

    // Prefer /config/ui.cfg; fall back to /ui.cfg.
    ensure_config_dir(drive);
    ui_prefs_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.font_path, ui_prefs_get_font_path(), sizeof(snap.font_path) - 1);
    snap.font_path[sizeof(snap.font_path) - 1] = '\0';
    snap.status_bar_mode = (uint8)ui_prefs_get_status_bar_mode();
    g_ui_prefs_epoch++;
    (void)crashlog_checkpoint(CRASHLOG_OBJ_UI_PREFS, 1, g_ui_prefs_epoch, &snap, sizeof(snap));
    if (write_prefs(drive, UI_PREFS_PATH_PRIMARY, out, (uint32)n) == 0) return 0;
    if (write_prefs(drive, UI_PREFS_PATH_FALLBACK, out, (uint32)n) == 0) return 0;
    return -1;
}
