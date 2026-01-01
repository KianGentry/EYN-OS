#include <ui_prefs.h>

#include <fs/vfs.h>
#include <tile_manager.h>
#include <vga.h>
#include <string.h>

#define UI_PREFS_PATH_PRIMARY "/config/ui.cfg"
#define UI_PREFS_PATH_FALLBACK "/ui.cfg"

static char g_ui_font_path[96] = "/fonts/unscii-16.hex";

const char* ui_prefs_get_font_path(void) {
    return g_ui_font_path;
}

void ui_prefs_set_font_path(const char* path) {
    if (!path || !path[0]) return;
    strncpy(g_ui_font_path, path, sizeof(g_ui_font_path) - 1);
    g_ui_font_path[sizeof(g_ui_font_path) - 1] = '\0';
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
    char buf[2048];
    int ok = 0;
    if (try_read_prefs(drive, UI_PREFS_PATH_PRIMARY, buf, (int)sizeof(buf)) != 0) {
        if (try_read_prefs(drive, UI_PREFS_PATH_FALLBACK, buf, (int)sizeof(buf)) != 0) {
            return -1;
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
        "status_text=%u,%u,%u\n",
        ui_prefs_get_font_path(),
        (unsigned)theme.title_focused_r, (unsigned)theme.title_focused_g, (unsigned)theme.title_focused_b,
        (unsigned)theme.title_unfocused_r, (unsigned)theme.title_unfocused_g, (unsigned)theme.title_unfocused_b,
        (unsigned)theme.status_r, (unsigned)theme.status_g, (unsigned)theme.status_b,
        (unsigned)theme.status_text_r, (unsigned)theme.status_text_g, (unsigned)theme.status_text_b
    );
    if (n <= 0) return -1;
    if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;

    // Prefer /config/ui.cfg; fall back to /ui.cfg.
    ensure_config_dir(drive);
    if (write_prefs(drive, UI_PREFS_PATH_PRIMARY, out, (uint32)n) == 0) return 0;
    if (write_prefs(drive, UI_PREFS_PATH_FALLBACK, out, (uint32)n) == 0) return 0;
    return -1;
}
