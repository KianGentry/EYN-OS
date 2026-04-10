#include <fcntl.h>
#include <dirent.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

#include "../../install/index.h"
#include "../../install/package.h"
#include "../../install/resolve.h"

EYN_CMDMETA_V1("EYN-OS graphical installer.", "installer");

#define MAX_DRIVES 8
/*
 * LOW-MEM INVARIANT: Keep installer copy reads small.
 *
 * Why: RAM:/ media is read through VFS demand paths under tight memory.
 * Smaller reads reduce transient kernel buffer pressure and avoid wide
 * multi-block reads that are more failure-prone in installer mode.
 */
#define COPY_BUF_SZ 256
#define MAX_STATUS 128

#define INSTALL_CACHE_ROOT "/cache"
#define INSTALL_CACHE_INDEX "/cache/index.json"
#define INSTALL_CACHE_BASE_ARCHIVE "/cache/base.pkg"
#define INSTALL_CACHE_MANIFEST "/cache/base.manifest"
#define INSTALL_CACHE_PACKAGE_DIR "/cache/pkg"

#define INSTALLER_MEDIA_INSTALL "RAM:/binaries/install"
#define INSTALLER_MEDIA_EXTRACT "RAM:/binaries/extract"
#define INSTALLER_MEDIA_INSTALLER "RAM:/binaries/installer"
#define INSTALLER_MEDIA_INDEX "RAM:/installer/index.json"
#define INSTALLER_MEDIA_BASE_ARCHIVE "RAM:/installer/base.pkg"
#define INSTALLER_MEDIA_BASE_MANIFEST "RAM:/installer/base.manifest"
#define INSTALLER_MEDIA_ETC_RESOLV "RAM:/etc/resolv.conf"
#define INSTALLER_MEDIA_CONFIG_DIR "RAM:/config"
#define INSTALLER_MEDIA_ICONS_DIR "RAM:/icons"
#define INSTALLER_MEDIA_ICONS16_DIR "RAM:/icons16"
#define INSTALLER_MEDIA_VIEW_DIR "RAM:/.view"
#define INSTALLER_MEDIA_FONTS_DIR "RAM:/fonts"
#define INSTALLER_MEDIA_CHIBICC "RAM:/programs/chibicc"

#define INSTALLER_MAX_MANIFEST_PACKAGES 256
#define INSTALLER_MAX_PACKAGE_NAME 64
#define INSTALLER_MAX_FONT_FILES 64
#define INSTALLER_MAX_FONT_NAME 64
#define INSTALLER_OPTION_VISIBLE_ROWS 10

/*
 * ABI-INVARIANT: Raw on-disk LBA used for installer-written kernel payload.
 *
 * Why: GRUB core image embedded by installer boots via blocklist
 * `multiboot (hd0)+KERNEL_RAW_LBA,<sectors>` to avoid filesystem module
 * dependencies during early boot.
 * Invariant: Must stay below first partition start (currently 2048).
 */
#define KERNEL_RAW_LBA 1024u

typedef enum {
    STEP_WELCOME = 0,
    STEP_SELECT_DRIVE = 1,
    STEP_OPTIONS = 2,
    STEP_FORMAT = 3,
    STEP_COPY = 4,
    STEP_BOOTLOADER = 5,
    STEP_DONE = 6,
    STEP_ERROR = 7,
} installer_step_t;

typedef struct {
    int drive_count;
    int drives[MAX_DRIVES];
    int selected_idx;
    int selected_drive;

    int options_loaded;
    int option_cursor;
    int option_scroll;

    int package_count;
    char packages[INSTALLER_MAX_MANIFEST_PACKAGES][INSTALLER_MAX_PACKAGE_NAME];
    unsigned char package_selected[INSTALLER_MAX_MANIFEST_PACKAGES];

    int font_count;
    char fonts[INSTALLER_MAX_FONT_FILES][INSTALLER_MAX_FONT_NAME];
    unsigned char font_selected[INSTALLER_MAX_FONT_FILES];

    int include_icons;
    int include_fonts;
    int include_chibicc;

    installer_step_t step;
    int running;

    int copied_files;
    int copied_dirs;
    int progress_permille;
    int progress_total;
    int progress_done;
    char status[MAX_STATUS];
    char current_item[160];
    char error[MAX_STATUS];
} installer_t;

static int g_installer_gui_handle = -1;
static int g_gui_draw_failures = 0;
static void draw_ui(int h, installer_t* s);
static int path_is_directory(const char* path);
static int read_manifest_packages(const char* manifest_path,
                                  char out_pkgs[INSTALLER_MAX_MANIFEST_PACKAGES][INSTALLER_MAX_PACKAGE_NAME],
                                  int* out_count);

static int gui_call_ok(int rc, const char* what, installer_t* s) {
    if (rc >= 0) return 1;
    g_gui_draw_failures++;
    if (g_gui_draw_failures <= 16 || (g_gui_draw_failures % 64) == 0) {
        int step = s ? (int)s->step : -1;
        printf("[installer] gui call failed: %s rc=%d step=%d failures=%d\n",
               what ? what : "?", rc, step, g_gui_draw_failures);
    }
    return 0;
}

static void installer_ui_pulse(installer_t* s) {
    if (!s) return;
    if (g_installer_gui_handle >= 0) {
        draw_ui(g_installer_gui_handle, s);
    }
}

static int clamp_permille(int v) {
    if (v < 0) return 0;
    if (v > 1000) return 1000;
    return v;
}

static void progress_reset(installer_t* s, int total) {
    if (!s) return;
    s->progress_total = (total > 0) ? total : 0;
    s->progress_done = 0;
    s->progress_permille = 0;
    s->current_item[0] = '\0';
    installer_ui_pulse(s);
}

static void progress_step(installer_t* s, const char* item) {
    if (!s) return;
    if (item) {
        strncpy(s->current_item, item, sizeof(s->current_item) - 1);
        s->current_item[sizeof(s->current_item) - 1] = '\0';
    }
    if (s->progress_total > 0) {
        s->progress_done++;
        if (s->progress_done > s->progress_total) s->progress_done = s->progress_total;
        s->progress_permille = clamp_permille((s->progress_done * 1000) / s->progress_total);
    }
    installer_ui_pulse(s);
}

static void progress_note_item(installer_t* s, const char* item) {
    if (!s) return;
    if (!item) item = "";
    strncpy(s->current_item, item, sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
}

static void progress_set_fraction(installer_t* s, uint32_t entry_done, uint32_t entry_total) {
    if (!s) return;
    if (s->progress_total <= 0) return;
    if (entry_total == 0u) return;

    if (entry_done > entry_total) entry_done = entry_total;

    /* Keep in-flight entry progress below the next completed-entry boundary. */
    uint32_t entry_permille = (entry_done * 1000u) / entry_total;
    if (entry_permille >= 1000u) entry_permille = 999u;

    int combined = (int)(((uint32_t)s->progress_done * 1000u + entry_permille) /
                         (uint32_t)s->progress_total);
    combined = clamp_permille(combined);
    if (combined > s->progress_permille) {
        s->progress_permille = combined;
    }
    installer_ui_pulse(s);
}

static void status_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "";
    strncpy(s->status, msg, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = '\0';
    installer_ui_pulse(s);
}

static void error_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "Unknown error";
    strncpy(s->error, msg, sizeof(s->error) - 1);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static void error_set_path(installer_t* s, const char* prefix, const char* path) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s", prefix, path);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static void error_set_path_code(installer_t* s, const char* prefix, const char* path, int code) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s (rc=%d)", prefix, path, code);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static int copy_file_stream_with_ui(installer_t* s, const char* src_ram_path, const char* dst_abs_path) {
    int fd = open(src_ram_path, O_RDONLY, 0);
    if (fd < 0) return -10;

    uint32_t src_size = 0;
    {
        int sz = lseek(fd, 0, SEEK_END);
        if (sz > 0 && lseek(fd, 0, SEEK_SET) >= 0) {
            src_size = (uint32_t)sz;
        }
    }

    int sh = eynfs_stream_begin(dst_abs_path);
    if (sh < 0) {
        close(fd);
        return sh;
    }

    char buf[COPY_BUF_SZ];
    int chunks_since_pulse = 0;
    uint32_t copied_bytes = 0;
    for (;;) {
        int n = (int)read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            (void)eynfs_stream_end(sh);
            return -30;
        }
        if (n == 0) break;
        int w = (int)eynfs_stream_write(sh, buf, (size_t)n);
        if (w != n) {
            close(fd);
            (void)eynfs_stream_end(sh);
            return -40;
        }
        copied_bytes += (uint32_t)n;
        chunks_since_pulse++;
        if (s && chunks_since_pulse >= 64) {
            if (src_size > 0u) progress_set_fraction(s, copied_bytes, src_size);
            else installer_ui_pulse(s);
            chunks_since_pulse = 0;
        }
    }

    if (s && src_size > 0u) {
        progress_set_fraction(s, copied_bytes, src_size);
    }

    if (close(fd) != 0) return -50;

    {
        int end_rc = eynfs_stream_end(sh);
        if (end_rc != 0) return -60;
    }
    return 0;
}

static int installer_target_preflight(installer_t* s) {
    const char* probe = "/.__install_probe";
    const char marker = 'X';

    int wr = writefile(probe, &marker, 1);
    if (wr < 0) {
        error_set(s, "Target write probe failed");
        return -1;
    }
    (void)unlink(probe);
    return 0;
}

static int installer_ram_preflight(installer_t* s) {
    int fd = open("RAM:/", O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "Could not open RAM:/ root");
        return -1;
    }

    eyn_dirent_t entries[16];
    int bytes = getdents(fd, entries, sizeof(entries));
    close(fd);

    if (bytes < 0) {
        error_set(s, "Could not list RAM:/ root");
        return -1;
    }
    if (bytes == 0) {
        error_set(s, "RAM:/ is empty (no files)");
        return -1;
    }

    fd = open("RAM:/binaries/installer", O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/binaries/installer missing");
        return -1;
    }
    close(fd);

    fd = open(INSTALLER_MEDIA_INSTALL, O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/binaries/install missing");
        return -1;
    }
    close(fd);

    fd = open(INSTALLER_MEDIA_EXTRACT, O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/binaries/extract missing");
        return -1;
    }
    close(fd);

    fd = open(INSTALLER_MEDIA_BASE_MANIFEST, O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/installer/base.manifest missing");
        return -1;
    }
    close(fd);

    fd = open(INSTALLER_MEDIA_ETC_RESOLV, O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/etc/resolv.conf missing");
        return -1;
    }
    close(fd);

    if (!path_is_directory(INSTALLER_MEDIA_CONFIG_DIR)) {
        error_set(s, "RAM:/config missing");
        return -1;
    }

    if (!path_is_directory(INSTALLER_MEDIA_VIEW_DIR)) {
        error_set(s, "RAM:/.view missing");
        return -1;
    }

    return 0;
}

static int path_exists(const char* path) {
    if (!path || !path[0]) return 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int path_is_directory(const char* path) {
    if (!path || !path[0]) return 0;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;

    eyn_dirent_t entries[1];
    int rc = getdents(fd, entries, sizeof(entries));
    close(fd);
    return rc >= 0;
}

static int ensure_target_directory(const char* path) {
    if (!path || path[0] != '/') return -1;

    char work[256];
    int needed = snprintf(work, sizeof(work), "%s", path);
    if (needed <= 0 || needed >= (int)sizeof(work)) return -1;

    size_t len = strlen(work);
    while (len > 1 && work[len - 1] == '/') {
        work[len - 1] = '\0';
        len--;
    }

    for (size_t i = 1; work[i]; i++) {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0) != 0 && !path_is_directory(work)) {
            work[i] = '/';
            return -1;
        }
        work[i] = '/';
    }

    if (mkdir(work, 0) != 0 && !path_is_directory(work)) return -1;
    return 0;
}

static int join_path2(const char* base, const char* leaf, char* out, size_t out_cap) {
    if (!base || !leaf || !out || out_cap == 0) return -1;

    size_t base_len = strlen(base);
    int needs_sep = (base_len > 0 && base[base_len - 1] != '/');
    int needed = snprintf(out,
                          out_cap,
                          needs_sep ? "%s/%s" : "%s%s",
                          base,
                          leaf);
    if (needed <= 0 || needed >= (int)out_cap) return -1;
    return 0;
}

static int read_directory_file_names(const char* dir_path,
                                     char out_names[INSTALLER_MAX_FONT_FILES][INSTALLER_MAX_FONT_NAME],
                                     int max_count,
                                     int* out_count) {
    if (!dir_path || !out_names || !out_count || max_count <= 0) return -1;

    int fd = open(dir_path, O_RDONLY, 0);
    if (fd < 0) {
        *out_count = 0;
        return -2;
    }

    int count = 0;
    for (;;) {
        eyn_dirent_t entries[16];
        int bytes = getdents(fd, entries, sizeof(entries));
        if (bytes < 0) {
            close(fd);
            return -1;
        }
        if (bytes == 0) break;

        int entry_count = bytes / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < entry_count; i++) {
            const char* name = entries[i].name;
            if (!name || !name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            if (entries[i].is_dir) continue;

            if (count >= max_count) {
                close(fd);
                return -1;
            }

            size_t name_len = strlen(name);
            if (name_len == 0 || name_len >= INSTALLER_MAX_FONT_NAME) {
                close(fd);
                return -1;
            }

            strncpy(out_names[count], name, INSTALLER_MAX_FONT_NAME - 1);
            out_names[count][INSTALLER_MAX_FONT_NAME - 1] = '\0';
            count++;
        }
    }

    close(fd);

    for (int i = 1; i < count; i++) {
        char tmp[INSTALLER_MAX_FONT_NAME];
        strncpy(tmp, out_names[i], sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        int j = i - 1;
        while (j >= 0 && strcmp(out_names[j], tmp) > 0) {
            strncpy(out_names[j + 1], out_names[j], INSTALLER_MAX_FONT_NAME - 1);
            out_names[j + 1][INSTALLER_MAX_FONT_NAME - 1] = '\0';
            j--;
        }

        strncpy(out_names[j + 1], tmp, INSTALLER_MAX_FONT_NAME - 1);
        out_names[j + 1][INSTALLER_MAX_FONT_NAME - 1] = '\0';
    }

    *out_count = count;
    return 0;
}

static int option_row_icons(const installer_t* s) {
    if (!s) return 0;
    return s->package_count;
}

static int option_row_fonts_toggle(const installer_t* s) {
    if (!s) return 1;
    return s->package_count + 1;
}

static int option_row_fonts_first(const installer_t* s) {
    if (!s) return 2;
    return s->package_count + 2;
}

static int option_row_chibicc(const installer_t* s) {
    if (!s) return 2;
    return option_row_fonts_first(s) + (s->include_fonts ? s->font_count : 0);
}

static int option_row_continue(const installer_t* s) {
    return option_row_chibicc(s) + 1;
}

static int options_row_count(const installer_t* s) {
    return option_row_continue(s) + 1;
}

static int selected_package_count(const installer_t* s) {
    if (!s) return 0;

    int selected = 0;
    for (int i = 0; i < s->package_count; i++) {
        if (s->package_selected[i]) selected++;
    }
    return selected;
}

static int selected_font_count(const installer_t* s) {
    if (!s || !s->include_fonts) return 0;

    int selected = 0;
    for (int i = 0; i < s->font_count; i++) {
        if (s->font_selected[i]) selected++;
    }
    return selected;
}

static void options_ensure_cursor_visible(installer_t* s) {
    if (!s) return;

    int rows = options_row_count(s);
    if (rows <= 0) {
        s->option_cursor = 0;
        s->option_scroll = 0;
        return;
    }

    if (s->option_cursor < 0) s->option_cursor = 0;
    if (s->option_cursor >= rows) s->option_cursor = rows - 1;

    if (s->option_scroll < 0) s->option_scroll = 0;
    if (s->option_scroll > s->option_cursor) s->option_scroll = s->option_cursor;

    if (s->option_cursor >= s->option_scroll + INSTALLER_OPTION_VISIBLE_ROWS) {
        s->option_scroll = s->option_cursor - INSTALLER_OPTION_VISIBLE_ROWS + 1;
    }

    int max_scroll = rows - INSTALLER_OPTION_VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (s->option_scroll > max_scroll) s->option_scroll = max_scroll;
}

static void toggle_all_packages(installer_t* s) {
    if (!s || s->package_count <= 0) return;

    int any_unselected = 0;
    for (int i = 0; i < s->package_count; i++) {
        if (!s->package_selected[i]) {
            any_unselected = 1;
            break;
        }
    }

    unsigned char new_value = any_unselected ? 1u : 0u;
    for (int i = 0; i < s->package_count; i++) {
        s->package_selected[i] = new_value;
    }
}

static int toggle_option_row(installer_t* s, int row) {
    if (!s || row < 0) return -1;

    if (row < s->package_count) {
        s->package_selected[row] = s->package_selected[row] ? 0u : 1u;
        return 0;
    }

    if (row == option_row_icons(s)) {
        s->include_icons = s->include_icons ? 0 : 1;
        return 0;
    }

    if (row == option_row_fonts_toggle(s)) {
        if (s->font_count <= 0) {
            s->include_fonts = 0;
            return 0;
        }
        s->include_fonts = s->include_fonts ? 0 : 1;
        options_ensure_cursor_visible(s);
        return 0;
    }

    if (s->include_fonts) {
        int font_first = option_row_fonts_first(s);
        int font_last = font_first + s->font_count - 1;
        if (row >= font_first && row <= font_last) {
            int font_idx = row - font_first;
            s->font_selected[font_idx] = s->font_selected[font_idx] ? 0u : 1u;
            return 0;
        }
    }

    if (row == option_row_chibicc(s)) {
        s->include_chibicc = s->include_chibicc ? 0 : 1;
        return 0;
    }

    if (row == option_row_continue(s)) return 1;
    return -1;
}

static int installer_prepare_options(installer_t* s) {
    if (!s) return -1;
    if (s->options_loaded) return 0;

    if (read_manifest_packages(INSTALLER_MEDIA_BASE_MANIFEST,
                               s->packages,
                               &s->package_count) != 0) {
        return -1;
    }

    if (s->package_count <= 0) return -1;

    for (int i = 0; i < s->package_count; i++) {
        s->package_selected[i] = 1u;
    }

    s->include_icons = 1;
    s->include_fonts = 0;
    s->include_chibicc = 0;

    int font_scan_rc = read_directory_file_names(INSTALLER_MEDIA_FONTS_DIR,
                                                 s->fonts,
                                                 INSTALLER_MAX_FONT_FILES,
                                                 &s->font_count);
    if (font_scan_rc < 0 && font_scan_rc != -2) {
        return -1;
    }
    if (font_scan_rc == -2) {
        s->font_count = 0;
    }

    for (int i = 0; i < s->font_count; i++) {
        s->font_selected[i] = 1u;
    }

    s->option_cursor = 0;
    s->option_scroll = 0;
    s->options_loaded = 1;
    options_ensure_cursor_visible(s);
    return 0;
}

static int copy_media_file_to_target(installer_t* s,
                                     const char* src_path,
                                     const char* dst_path,
                                     const char* item_label) {
    if (!src_path || !dst_path) return -1;

    if (item_label && item_label[0]) {
        progress_note_item(s, item_label);
    }

    int cfr = copy_file_stream_with_ui(s, src_path, dst_path);
    if (cfr != 0) {
        if (cfr == -10) error_set_path(s, "Copy source open failed", src_path);
        else if (cfr == -30) error_set_path(s, "Copy source read failed", src_path);
        else if (cfr == -40) error_set_path(s, "Copy target write failed", dst_path);
        else if (cfr == -50) error_set_path(s, "Copy source close failed", src_path);
        else if (cfr == -60) error_set_path(s, "Copy finalize failed", dst_path);
        else if (cfr < 0) error_set_path_code(s, "Copy target create failed", dst_path, cfr);
        else error_set_path(s, "Copy failed", src_path);
        return -1;
    }

    if (s) s->copied_files++;

    return 0;
}

static int copy_media_tree_recursive(installer_t* s,
                                     const char* src_root,
                                     const char* dst_root) {
    if (!src_root || !dst_root) return -1;

    int fd = open(src_root, O_RDONLY, 0);
    if (fd < 0) return -1;

    if (ensure_target_directory(dst_root) != 0) {
        close(fd);
        return -1;
    }

    if (s) s->copied_dirs++;

    for (;;) {
        eyn_dirent_t entries[16];
        int bytes = getdents(fd, entries, sizeof(entries));
        if (bytes < 0) {
            close(fd);
            return -1;
        }
        if (bytes == 0) break;

        int entry_count = bytes / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < entry_count; i++) {
            const char* name = entries[i].name;
            if (!name || !name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            char src_child[256];
            char dst_child[256];
            if (join_path2(src_root, name, src_child, sizeof(src_child)) != 0
                || join_path2(dst_root, name, dst_child, sizeof(dst_child)) != 0) {
                close(fd);
                return -1;
            }

            if (entries[i].is_dir) {
                if (copy_media_tree_recursive(s, src_child, dst_child) != 0) {
                    close(fd);
                    return -1;
                }
                continue;
            }

            if (copy_media_file_to_target(s, src_child, dst_child, dst_child) != 0) {
                close(fd);
                return -1;
            }
        }
    }

    close(fd);
    return 0;
}

static int copy_media_tree_to_target(installer_t* s,
                                     const char* src_root,
                                     const char* dst_root,
                                     const char* missing_msg) {
    if (!src_root || !dst_root) return -1;

    if (!path_is_directory(src_root)) {
        if (missing_msg && missing_msg[0]) {
            error_set(s, missing_msg);
        }
        return -1;
    }

    if (copy_media_tree_recursive(s, src_root, dst_root) != 0) {
        error_set_path(s, "Failed to copy media tree", src_root);
        return -1;
    }

    return 0;
}

static int seed_system_content(installer_t* s) {
    if (!s) return -1;

    if (ensure_target_directory("/etc") != 0) {
        error_set_path(s, "Failed to create target directory", "/etc");
        return -1;
    }

    if (copy_media_file_to_target(s,
                                  INSTALLER_MEDIA_ETC_RESOLV,
                                  "/etc/resolv.conf",
                                  "Seeding /etc/resolv.conf") != 0) {
        return -1;
    }
    progress_step(s, "resolv.conf");

    if (copy_media_tree_to_target(s,
                                  INSTALLER_MEDIA_CONFIG_DIR,
                                  "/config",
                                  "RAM:/config missing") != 0) {
        return -1;
    }
    progress_step(s, "config");

    if (copy_media_tree_to_target(s,
                                  INSTALLER_MEDIA_VIEW_DIR,
                                  "/.view",
                                  "RAM:/.view missing") != 0) {
        return -1;
    }
    progress_step(s, ".view");

    if (s->include_icons) {
        if (copy_media_tree_to_target(s,
                                      INSTALLER_MEDIA_ICONS_DIR,
                                      "/icons",
                                      "RAM:/icons missing") != 0) {
            return -1;
        }
        progress_step(s, "icons");

        if (copy_media_tree_to_target(s,
                                      INSTALLER_MEDIA_ICONS16_DIR,
                                      "/icons16",
                                      "RAM:/icons16 missing") != 0) {
            return -1;
        }
        progress_step(s, "icons16");
    }

    if (s->include_fonts) {
        int fonts_selected = selected_font_count(s);
        if (fonts_selected <= 0) {
            error_set(s, "Fonts enabled but no fonts selected");
            return -1;
        }

        if (ensure_target_directory("/fonts") != 0) {
            error_set_path(s, "Failed to create target directory", "/fonts");
            return -1;
        }

        for (int i = 0; i < s->font_count; i++) {
            if (!s->font_selected[i]) continue;

            char src_font[256];
            char dst_font[256];
            if (join_path2(INSTALLER_MEDIA_FONTS_DIR, s->fonts[i], src_font, sizeof(src_font)) != 0
                || join_path2("/fonts", s->fonts[i], dst_font, sizeof(dst_font)) != 0) {
                error_set(s, "Font path too long");
                return -1;
            }

            char item[160];
            snprintf(item, sizeof(item), "Seeding /fonts/%s", s->fonts[i]);
            if (copy_media_file_to_target(s, src_font, dst_font, item) != 0) {
                return -1;
            }
            progress_step(s, s->fonts[i]);
        }
    }

    if (s->include_chibicc) {
        if (!path_exists(INSTALLER_MEDIA_CHIBICC)) {
            error_set(s, "RAM:/programs/chibicc missing");
            return -1;
        }

        if (ensure_target_directory("/binaries") != 0) {
            error_set_path(s, "Failed to create target directory", "/binaries");
            return -1;
        }

        if (copy_media_file_to_target(s,
                                      INSTALLER_MEDIA_CHIBICC,
                                      "/binaries/chibicc",
                                      "Seeding /binaries/chibicc") != 0) {
            return -1;
        }
        progress_step(s, "chibicc");
    }

    return 0;
}

static int parse_manifest_line(const char* line,
                               char out_name[INSTALLER_MAX_PACKAGE_NAME]) {
    if (!line || !out_name) return 1;

    size_t start = 0;
    size_t len = strlen(line);
    while (start < len && (line[start] == ' ' || line[start] == '\t')) start++;

    if (start >= len) return 1;
    if (line[start] == '#') return 1;

    size_t end = len;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    if (end <= start) return 1;

    size_t token_end = start;
    while (token_end < end && line[token_end] != ' ' && line[token_end] != '\t') token_end++;
    size_t token_len = token_end - start;

    if (token_len == 0 || token_len >= INSTALLER_MAX_PACKAGE_NAME) return -1;

    memcpy(out_name, line + start, token_len);
    out_name[token_len] = '\0';
    return 0;
}

static int read_manifest_packages(const char* manifest_path,
                                  char out_pkgs[INSTALLER_MAX_MANIFEST_PACKAGES][INSTALLER_MAX_PACKAGE_NAME],
                                  int* out_count) {
    if (!manifest_path || !out_pkgs || !out_count) return -1;

    int fd = open(manifest_path, O_RDONLY, 0);
    if (fd < 0) return -1;

    char line[160];
    int line_len = 0;
    int count = 0;

    char chunk[128];
    for (;;) {
        int n = (int)read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;

            if (c == '\n') {
                line[line_len] = '\0';
                char parsed[INSTALLER_MAX_PACKAGE_NAME];
                int pr = parse_manifest_line(line, parsed);
                if (pr < 0) {
                    close(fd);
                    return -1;
                }
                if (pr == 0) {
                    if (count >= INSTALLER_MAX_MANIFEST_PACKAGES) {
                        close(fd);
                        return -1;
                    }
                    strncpy(out_pkgs[count], parsed, INSTALLER_MAX_PACKAGE_NAME - 1);
                    out_pkgs[count][INSTALLER_MAX_PACKAGE_NAME - 1] = '\0';
                    count++;
                }
                line_len = 0;
                continue;
            }

            if (line_len + 1 < (int)sizeof(line)) {
                line[line_len++] = c;
            } else {
                close(fd);
                return -1;
            }
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        char parsed[INSTALLER_MAX_PACKAGE_NAME];
        int pr = parse_manifest_line(line, parsed);
        if (pr < 0) {
            close(fd);
            return -1;
        }
        if (pr == 0) {
            if (count >= INSTALLER_MAX_MANIFEST_PACKAGES) {
                close(fd);
                return -1;
            }
            strncpy(out_pkgs[count], parsed, INSTALLER_MAX_PACKAGE_NAME - 1);
            out_pkgs[count][INSTALLER_MAX_PACKAGE_NAME - 1] = '\0';
            count++;
        }
    }

    close(fd);
    *out_count = count;
    return 0;
}

static int install_mbr_boot_code(int logical_drive) {
    int fd_boot = open("RAM:/installer/grub/boot.img", O_RDONLY, 0);
    if (fd_boot < 0) return -1;

    unsigned char boot[512];
    int boot_bytes = (int)read(fd_boot, boot, sizeof(boot));
    close(fd_boot);
    if (boot_bytes != 512) return -2;

    int fd_core = open("RAM:/installer/grub/core.img", O_RDONLY, 0);
    if (fd_core < 0) return -3;

    int core_size = lseek(fd_core, 0, SEEK_END);
    if (core_size <= 0) {
        close(fd_core);
        return -4;
    }
    if (lseek(fd_core, 0, SEEK_SET) < 0) {
        close(fd_core);
        return -5;
    }

    unsigned char* core = (unsigned char*)malloc((size_t)core_size);
    if (!core) {
        close(fd_core);
        return -6;
    }

    int got = 0;
    while (got < core_size) {
        int n = (int)read(fd_core, core + got, (size_t)(core_size - got));
        if (n <= 0) {
            free(core);
            close(fd_core);
            return -7;
        }
        got += n;
    }
    close(fd_core);

    int core_sectors = (core_size + 511) / 512;
    if (core_sectors <= 0 || core_sectors >= 2048) {
        free(core);
        return -8;
    }

    int fd_kernel = open("/boot/kernel.bin", O_RDONLY, 0);
    if (fd_kernel < 0) {
        free(core);
        return -12;
    }
    int kernel_size = lseek(fd_kernel, 0, SEEK_END);
    if (kernel_size <= 0 || lseek(fd_kernel, 0, SEEK_SET) < 0) {
        close(fd_kernel);
        free(core);
        return -13;
    }
    int kernel_sectors = (kernel_size + 511) / 512;

    /* Rebuild MBR with current partition entries while replacing only boot code. */
    eyn_installer_partitions_t parts;
    memset(&parts, 0, sizeof(parts));
    if (eyn_sys_installer_get_partitions((uint32_t)logical_drive, &parts) != 0) {
        free(core);
        return -9;
    }

    unsigned char mbr[512];
    memset(mbr, 0, sizeof(mbr));
    memcpy(mbr, boot, 446);

    int has_bootable = 0;
    for (int i = 0; i < 4; ++i) {
        if (!parts.partitions[i].present) continue;
        if (parts.partitions[i].bootable) has_bootable = 1;
    }

    for (int i = 0; i < 4; ++i) {
        if (!parts.partitions[i].present) continue;

        unsigned char* e = mbr + 0x1BE + i * 16;
        int mark_boot = parts.partitions[i].bootable ? 1 : 0;
        if (!has_bootable && i == 0) mark_boot = 1;

        e[0] = (unsigned char)(mark_boot ? 0x80 : 0x00); /* status */
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;           /* CHS start (LBA) */
        e[4] = parts.partitions[i].type;                 /* type */
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;           /* CHS end (LBA) */

        uint32_t lba = parts.partitions[i].lba_start;
        uint32_t cnt = parts.partitions[i].sector_count;
        e[8]  = (unsigned char)(lba & 0xFFu);
        e[9]  = (unsigned char)((lba >> 8) & 0xFFu);
        e[10] = (unsigned char)((lba >> 16) & 0xFFu);
        e[11] = (unsigned char)((lba >> 24) & 0xFFu);
        e[12] = (unsigned char)(cnt & 0xFFu);
        e[13] = (unsigned char)((cnt >> 8) & 0xFFu);
        e[14] = (unsigned char)((cnt >> 16) & 0xFFu);
        e[15] = (unsigned char)((cnt >> 24) & 0xFFu);

        if (i == 0) {
            uint32_t raw_end = KERNEL_RAW_LBA + (uint32_t)kernel_sectors;
            if (raw_end >= parts.partitions[i].lba_start) {
                close(fd_kernel);
                free(core);
                return -14;
            }
        }
    }

    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (eyn_sys_installer_write_sector((uint32_t)logical_drive, 0, mbr) != 0) {
        free(core);
        return -10;
    }

    /* Embed GRUB core image directly after MBR (LBA 1..N). */
    for (int s = 0; s < core_sectors; ++s) {
        unsigned char sec[512];
        memset(sec, 0, sizeof(sec));

        int off = s * 512;
        int rem = core_size - off;
        int take = rem > 512 ? 512 : rem;
        if (take > 0) memcpy(sec, core + off, (size_t)take);

        if (eyn_sys_installer_write_sector((uint32_t)logical_drive, (uint32_t)(1 + s), sec) != 0) {
            close(fd_kernel);
            free(core);
            return -11;
        }
    }

    /* Write raw kernel blob at fixed LBA expected by embedded GRUB config. */
    for (int s = 0; s < kernel_sectors; ++s) {
        unsigned char sec[512];
        memset(sec, 0, sizeof(sec));
        int n = (int)read(fd_kernel, sec, sizeof(sec));
        if (n < 0) {
            close(fd_kernel);
            free(core);
            return -15;
        }

        if (eyn_sys_installer_write_sector((uint32_t)logical_drive,
                                           (uint32_t)(KERNEL_RAW_LBA + (uint32_t)s),
                                           sec) != 0) {
            close(fd_kernel);
            free(core);
            return -16;
        }

        if (n == 0) break;
    }

    close(fd_kernel);

    free(core);
    return 0;
}

static int write_grub_cfg(void) {
    const char* cfg =
        "set default=0\n"
        "set timeout=0\n"
        "menuentry \"EYN-OS\" {\n"
        "    multiboot /boot/kernel.bin\n"
        "    boot\n"
        "}\n";

    (void)mkdir("/boot", 0);
    (void)mkdir("/boot/grub", 0);

    /*
     * Use the EYNFS stream writer here as well, to match the installer's
     * copy path and avoid backend differences in writefile() behavior.
     */
    int sh = eynfs_stream_begin("/boot/grub/grub.cfg");
    if (sh < 0) return sh;

    int len = (int)strlen(cfg);
    int w = (int)eynfs_stream_write(sh, cfg, (size_t)len);
    if (w != len) {
        (void)eynfs_stream_end(sh);
        return -200;
    }

    int end_rc = eynfs_stream_end(sh);
    if (end_rc != 0) return -201;

    return 0;
}

static int run_install_for_package(installer_t* s,
                                   const PackageIndex* package_index,
                                   const char* pkg_name,
                                   int index,
                                   int total) {
    if (!s || !package_index || !pkg_name || !pkg_name[0]) return -1;

    char status[96];
    snprintf(status, sizeof(status), "Installing package %d/%d", index + 1, total);
    status_set(s, status);

    ResolvePlan plan;
    if (resolve_install_plan(package_index, pkg_name, &plan) != 0) {
        error_set_path(s, "Failed to resolve install plan", pkg_name);
        return -1;
    }

    if (plan.count <= 0) {
        error_set_path(s, "Empty install plan", pkg_name);
        return -1;
    }

    for (int plan_idx = 0; plan_idx < plan.count; plan_idx++) {
        const Package* pkg = plan.ordered[plan_idx];
        if (!pkg || !pkg->name[0]) {
            error_set_path(s, "Invalid package in install plan", pkg_name);
            return -1;
        }

        char item[160];
        snprintf(item, sizeof(item), "install %s", pkg->name);
        progress_note_item(s, item);

        if (install_package(package_index, pkg) != 0) {
            error_set_path(s, "Package install failed", pkg->name);
            return -1;
        }
    }

    progress_step(s, pkg_name);
    return 0;
}

static int install_from_package_manifest(installer_t* s) {
    if (!s) return -1;

    if (installer_prepare_options(s) != 0) {
        error_set(s, "Failed to load installer options");
        return -1;
    }

    int packages_selected = selected_package_count(s);
    if (packages_selected <= 0) {
        error_set(s, "Select at least one package before install");
        return -1;
    }

    int system_seed_steps = 3;
    if (s->include_icons) system_seed_steps += 2;
    if (s->include_fonts) system_seed_steps += selected_font_count(s);
    if (s->include_chibicc) system_seed_steps += 1;

    (void)mkdir("/binaries", 0);
    (void)mkdir("/boot", 0);
    (void)mkdir(INSTALL_CACHE_ROOT, 0);
    (void)mkdir(INSTALL_CACHE_PACKAGE_DIR, 0);

    status_set(s, "Seeding installer tools and cache ...");
    progress_reset(s, 7 + system_seed_steps);

    if (copy_media_file_to_target(s,
                                  INSTALLER_MEDIA_INSTALL,
                                  "/binaries/install",
                                  "Seeding /binaries/install") != 0) {
        return -1;
    }
    progress_step(s, "install");

    if (copy_media_file_to_target(s,
                                  INSTALLER_MEDIA_EXTRACT,
                                  "/binaries/extract",
                                  "Seeding /binaries/extract") != 0) {
        return -1;
    }
    progress_step(s, "extract");

    if (copy_media_file_to_target(s,
                                  INSTALLER_MEDIA_INSTALLER,
                                  "/binaries/installer",
                                  "Seeding /binaries/installer") != 0) {
        return -1;
    }
    progress_step(s, "installer");

    if (copy_media_file_to_target(s,
                                  "RAM:/boot/kernel.bin",
                                  "/boot/kernel.bin",
                                  "Copying /boot/kernel.bin") != 0) {
        return -1;
    }
    progress_step(s, "kernel");

    if (path_exists(INSTALLER_MEDIA_INDEX)) {
        if (copy_media_file_to_target(s,
                                      INSTALLER_MEDIA_INDEX,
                                      INSTALL_CACHE_INDEX,
                                      "Copying local index.json") != 0) {
            return -1;
        }
    }
    progress_step(s, "index");

    if (path_exists(INSTALLER_MEDIA_BASE_ARCHIVE)) {
        if (copy_media_file_to_target(s,
                                      INSTALLER_MEDIA_BASE_ARCHIVE,
                                      INSTALL_CACHE_BASE_ARCHIVE,
                                      "Copying base.pkg") != 0) {
            return -1;
        }
    }
    progress_step(s, "base.pkg");

    if (!path_exists(INSTALLER_MEDIA_BASE_MANIFEST)) {
        error_set(s, "RAM:/installer/base.manifest missing");
        return -1;
    }
    if (copy_media_file_to_target(s,
                                  INSTALLER_MEDIA_BASE_MANIFEST,
                                  INSTALL_CACHE_MANIFEST,
                                  "Copying base.manifest") != 0) {
        return -1;
    }
    progress_step(s, "manifest");

    if (seed_system_content(s) != 0) {
        return -1;
    }

    PackageIndex package_index;
    if (index_fetch_and_parse(&package_index) != 0) {
        error_set(s, "Failed to load package index");
        return -1;
    }

    s->step = STEP_COPY;
    status_set(s, "Installing base packages ...");
    progress_reset(s, packages_selected);

    int previous_confirm_mode = package_set_confirm_mode(PACKAGE_CONFIRM_AUTO_ACCEPT);

    int install_index = 0;
    for (int i = 0; i < s->package_count; i++) {
        if (!s->package_selected[i]) continue;

        if (run_install_for_package(s,
                                    &package_index,
                                    s->packages[i],
                                    install_index,
                                    packages_selected) != 0) {
            (void)package_set_confirm_mode(previous_confirm_mode);
            return -1;
        }

        install_index++;
    }

    (void)package_set_confirm_mode(previous_confirm_mode);

    return 0;
}

static int run_install(installer_t* s) {
    if (!s) return -1;

    status_set(s, "Checking RAM:/ installer media ...");
    progress_reset(s, 100);
    s->progress_permille = 30;
    installer_ui_pulse(s);
    if (installer_ram_preflight(s) != 0) return -101;

    if (eyn_sys_installer_prepare_drive((uint32_t)s->selected_drive) != 0) {
        error_set(s, "Drive partition/format failed");
        return -102;
    }
    s->progress_permille = 80;
    installer_ui_pulse(s);

    if (eyn_sys_drive_set_logical((uint32_t)s->selected_drive) < 0) {
        error_set(s, "Could not switch to target drive");
        return -103;
    }

    status_set(s, "Verifying target filesystem ...");
    if (installer_target_preflight(s) != 0) return -104;
    s->progress_permille = 120;
    installer_ui_pulse(s);

    s->step = STEP_COPY;
    if (install_from_package_manifest(s) != 0) {
        if (s->error[0] == '\0') {
            error_set(s, "Package-manifest install failed");
        }
        return -109;
    }

    status_set(s, "Writing GRUB config ...");
    s->step = STEP_BOOTLOADER;
    s->progress_total = 3;
    s->progress_done = 0;
    s->progress_permille = 0;
    strncpy(s->current_item, "Writing /boot/grub/grub.cfg", sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
    {
        int cfg_rc = write_grub_cfg();
        if (cfg_rc != 0) {
            error_set_path_code(s, "Failed writing /boot/grub/grub.cfg", "/boot/grub/grub.cfg", cfg_rc);
            return -106;
        }
        progress_step(s, "grub.cfg written");
    }

    status_set(s, "Installing GRUB (embedded core) ...");
    strncpy(s->current_item, "Embedding boot.img/core.img and kernel blob", sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
    {
        int mbr_rc = install_mbr_boot_code(s->selected_drive);
        if (mbr_rc < 0) {
            error_set_path_code(s, "Bootloader install failed", "/installer/grub/{boot.img,core.img}", mbr_rc);
            return -107;
        }
        progress_step(s, "Bootloader installed");
    }

    status_set(s, "Installation complete");
    progress_step(s, "Done");
    s->progress_permille = 1000;
    installer_ui_pulse(s);
    s->step = STEP_DONE;
    return 0;
}

static void draw_center_text(int h, int y, const char* text, unsigned char r, unsigned char g, unsigned char b) {
    gui_text_t t;
    t.x = 12;
    t.y = y;
    t.r = r;
    t.g = g;
    t.b = b;
    t._pad = 0;
    t.text = text;
    (void)gui_draw_text(h, &t);
}

static void draw_ui(int h, installer_t* s) {
    gui_size_t sz;
    sz.w = 640;
    sz.h = 360;
    if (!gui_call_ok(gui_get_content_size(h, &sz), "gui_get_content_size", s)) {
        sz.w = 640;
        sz.h = 360;
    }

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    if (!gui_call_ok(gui_begin(h), "gui_begin", s)) return;
    if (!gui_call_ok(gui_clear(h, &bg), "gui_clear", s)) return;

    gui_rect_t header = {0, 0, sz.w, 24, GUI_PAL_HEADER_R, GUI_PAL_HEADER_G, GUI_PAL_HEADER_B, 0};
    if (!gui_call_ok(gui_fill_rect(h, &header), "gui_fill_rect(header)", s)) return;

    draw_center_text(h, 6, "EYN-OS Installer", 245, 245, 245);

    if (s->step == STEP_WELCOME) {
        draw_center_text(h, 52, "This will install EYN-OS to a selected disk.", 220, 220, 220);
        draw_center_text(h, 72, "Press Enter to continue or Q to cancel.", 170, 170, 170);
    } else if (s->step == STEP_SELECT_DRIVE) {
        draw_center_text(h, 44, "Select target drive (Up/Down + Enter)", 220, 220, 220);
        int y = 72;
        for (int i = 0; i < s->drive_count; ++i) {
            char line[64];
            snprintf(line, sizeof(line), "Drive %d", s->drives[i]);
            if (i == s->selected_idx) {
                gui_rect_t sel = {8, y - 2, sz.w - 16, 18, GUI_PAL_SEL_R, GUI_PAL_SEL_G, GUI_PAL_SEL_B, 0};
                if (!gui_call_ok(gui_fill_rect(h, &sel), "gui_fill_rect(select)", s)) return;
            }
            draw_center_text(h, y, line, 240, 240, 240);
            y += 20;
        }
        if (s->drive_count == 0) {
            draw_center_text(h, 72, "No drives detected", 255, 140, 140);
        }
    } else if (s->step == STEP_OPTIONS) {
        int selected_pkgs = selected_package_count(s);
        int selected_fonts = selected_font_count(s);

        char summary_pkgs[96];
        char summary_assets[128];
        snprintf(summary_pkgs,
                 sizeof(summary_pkgs),
                 "Packages selected: %d / %d",
                 selected_pkgs,
                 s->package_count);
        snprintf(summary_assets,
                 sizeof(summary_assets),
                 "Icons: %s   Fonts: %s (%d)   Chibicc: %s",
                 s->include_icons ? "on" : "off",
                 s->include_fonts ? "on" : "off",
                 selected_fonts,
                 s->include_chibicc ? "on" : "off");

        draw_center_text(h, 42, "Configure install options (menuconfig-style)", 220, 220, 220);
        draw_center_text(h, 62, summary_pkgs, 180, 200, 220);
        draw_center_text(h, 80, summary_assets, 170, 190, 210);

        int rows = options_row_count(s);
        int first = s->option_scroll;
        int last = first + INSTALLER_OPTION_VISIBLE_ROWS;
        if (last > rows) last = rows;

        int y = 106;
        for (int row = first; row < last; row++) {
            int is_cursor = (row == s->option_cursor);
            if (is_cursor) {
                gui_rect_t sel = {8, y - 2, sz.w - 16, 16, GUI_PAL_SEL_R, GUI_PAL_SEL_G, GUI_PAL_SEL_B, 0};
                if (!gui_call_ok(gui_fill_rect(h, &sel), "gui_fill_rect(options)", s)) return;
            }

            char line[176];
            if (row < s->package_count) {
                snprintf(line,
                         sizeof(line),
                         "[%c] package: %s",
                         s->package_selected[row] ? 'x' : ' ',
                         s->packages[row]);
            } else if (row == option_row_icons(s)) {
                snprintf(line,
                         sizeof(line),
                         "[%c] seed /icons and /icons16",
                         s->include_icons ? 'x' : ' ');
            } else if (row == option_row_fonts_toggle(s)) {
                if (s->font_count > 0) {
                    snprintf(line,
                             sizeof(line),
                             "[%c] seed fonts",
                             s->include_fonts ? 'x' : ' ');
                } else {
                    snprintf(line, sizeof(line), "[ ] seed fonts (none available on media)");
                }
            } else if (s->include_fonts
                       && row >= option_row_fonts_first(s)
                       && row < option_row_fonts_first(s) + s->font_count) {
                int font_idx = row - option_row_fonts_first(s);
                snprintf(line,
                         sizeof(line),
                         "    [%c] font: %s",
                         s->font_selected[font_idx] ? 'x' : ' ',
                         s->fonts[font_idx]);
            } else if (row == option_row_chibicc(s)) {
                snprintf(line,
                         sizeof(line),
                         "[%c] seed /binaries/chibicc",
                         s->include_chibicc ? 'x' : ' ');
            } else if (row == option_row_continue(s)) {
                snprintf(line, sizeof(line), "[>] continue to disk format");
            } else {
                snprintf(line, sizeof(line), "");
            }

            draw_center_text(h, y, line, 230, 230, 230);
            y += 18;
        }

        draw_center_text(h, 306, "Up/Down: move   Enter/Space: toggle", 170, 170, 170);
        draw_center_text(h, 324, "A: toggle all packages   B: back   I: continue", 170, 170, 170);
    } else if (s->step == STEP_FORMAT) {
        char line[96];
        snprintf(line, sizeof(line), "Target drive: %d", s->selected_drive);
        draw_center_text(h, 52, line, 235, 235, 235);
        draw_center_text(h, 76, "Press Enter to FORMAT + INSTALL", 255, 200, 130);
        draw_center_text(h, 96, "Warning: this erases target disk contents.", 255, 120, 120);
    } else if (s->step == STEP_COPY || s->step == STEP_BOOTLOADER) {
        char c1[96];
        char ptxt[64];
        int bar_x = 14;
        int bar_y = 120;
        int bar_w = sz.w - 28;
        int bar_h = 12;
        int fill_w = (bar_w * clamp_permille(s->progress_permille)) / 1000;
        snprintf(c1, sizeof(c1), "Files copied: %d   Dirs: %d", s->copied_files, s->copied_dirs);
        snprintf(ptxt, sizeof(ptxt), "Progress: %d%%", clamp_permille(s->progress_permille) / 10);
        draw_center_text(h, 56, s->status[0] ? s->status : "Working...", 200, 230, 255);
        draw_center_text(h, 78, c1, 185, 185, 185);

        gui_rect_t bar_bg = {bar_x, bar_y, bar_w, bar_h, 58, 61, 72, 0};
        if (!gui_call_ok(gui_fill_rect(h, &bar_bg), "gui_fill_rect(bar_bg)", s)) return;

        if (fill_w > 0) {
            gui_rect_t bar_fg = {bar_x, bar_y, fill_w, bar_h, 86, 176, 240, 0};
            if (!gui_call_ok(gui_fill_rect(h, &bar_fg), "gui_fill_rect(bar_fg)", s)) return;
        }

        draw_center_text(h, 138, ptxt, 190, 210, 230);
        if (s->current_item[0]) {
            draw_center_text(h, 158, s->current_item, 170, 170, 170);
        } else {
            draw_center_text(h, 158, "Please wait...", 170, 170, 170);
        }
    } else if (s->step == STEP_DONE) {
        char c1[96];
        snprintf(c1, sizeof(c1), "Copied files: %d   directories: %d", s->copied_files, s->copied_dirs);
        draw_center_text(h, 56, "Install finished successfully.", 140, 255, 160);
        draw_center_text(h, 78, c1, 220, 220, 220);
        draw_center_text(h, 102, "Reboot and boot from installed disk.", 190, 190, 190);
        draw_center_text(h, 126, "Press Q to close installer.", 170, 170, 170);
    } else if (s->step == STEP_ERROR) {
        draw_center_text(h, 56, "Install failed", 255, 120, 120);
        draw_center_text(h, 78, s->error, 230, 190, 190);
        draw_center_text(h, 102, "Press Q to close installer.", 170, 170, 170);
    } else {
        draw_center_text(h, 56, "Installer state error", 255, 140, 140);
        draw_center_text(h, 78, "Unexpected installer step; operation aborted.", 210, 180, 180);
        draw_center_text(h, 102, "Press Q to close installer.", 170, 170, 170);
    }

    (void)gui_call_ok(gui_present(h), "gui_present", s);
}

static void refresh_drives(installer_t* s) {
    s->drive_count = 0;
    int n = eyn_sys_drive_get_count();
    if (n < 0) n = 0;
    for (int i = 0; i < n && s->drive_count < MAX_DRIVES; ++i) {
        if (eyn_sys_drive_is_present((uint32_t)i) > 0) {
            s->drives[s->drive_count++] = i;
        }
    }
    if (s->selected_idx >= s->drive_count) s->selected_idx = 0;
}

int main(void) {
    installer_t st;
    memset(&st, 0, sizeof(st));
    st.running = 1;
    st.step = STEP_WELCOME;
    status_set(&st, "Ready");

    int h = gui_attach("Installer", "EYN-OS setup");
    if (h < 0) {
        puts("installer: gui_attach failed");
        return 1;
    }
    /*
     * UI-INVARIANT: Installer text must remain visible while current drive
     * switches from RAM:/ to the target disk during install.
     *
     * Why: The default GUI font path may resolve against the current logical
     * drive. During target formatting/copy, that drive can be empty, causing
     * glyph lookups to fail and the window to appear blank.
     *
     * Contract: Use built-in kernel font (empty path) so rendering does not
     * depend on filesystem availability or current-drive context.
     */
    (void)gui_set_font(h, "");
    g_installer_gui_handle = h;

    refresh_drives(&st);
    draw_ui(h, &st);

    while (st.running) {
        gui_event_t ev;
        int rc = gui_wait_event(h, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        if (ev.type == GUI_EVENT_CLOSE) break;

        if (ev.type == GUI_EVENT_KEY) {
            unsigned ch = (unsigned)ev.a & 0xFFu;
            if (ch == 'q' || ch == 'Q') break;

            if (st.step == STEP_WELCOME) {
                if (ch == '\r' || ch == '\n') st.step = STEP_SELECT_DRIVE;
            } else if (st.step == STEP_SELECT_DRIVE) {
                if (ev.a == GUI_KEY_UP && st.selected_idx > 0) st.selected_idx--;
                if (ev.a == GUI_KEY_DOWN && st.selected_idx + 1 < st.drive_count) st.selected_idx++;
                if ((ch == '\r' || ch == '\n') && st.drive_count > 0) {
                    st.selected_drive = st.drives[st.selected_idx];
                    if (installer_prepare_options(&st) != 0) {
                        error_set(&st, "Failed to load installer options");
                        st.step = STEP_ERROR;
                    } else {
                        st.step = STEP_OPTIONS;
                    }
                }
            } else if (st.step == STEP_OPTIONS) {
                if (ev.a == GUI_KEY_UP && st.option_cursor > 0) {
                    st.option_cursor--;
                }
                if (ev.a == GUI_KEY_DOWN && st.option_cursor + 1 < options_row_count(&st)) {
                    st.option_cursor++;
                }

                if (ch == 'a' || ch == 'A') {
                    toggle_all_packages(&st);
                } else if (ch == 'b' || ch == 'B') {
                    st.step = STEP_SELECT_DRIVE;
                } else if (ch == 'i' || ch == 'I') {
                    if (selected_package_count(&st) <= 0) {
                        status_set(&st, "Select at least one package");
                    } else {
                        st.step = STEP_FORMAT;
                    }
                } else if (ch == '\r' || ch == '\n' || ch == ' ') {
                    int row_action = toggle_option_row(&st, st.option_cursor);
                    if (row_action == 1) {
                        if (selected_package_count(&st) <= 0) {
                            status_set(&st, "Select at least one package");
                        } else {
                            st.step = STEP_FORMAT;
                        }
                    }
                }

                options_ensure_cursor_visible(&st);
            } else if (st.step == STEP_FORMAT) {
                if (ch == '\r' || ch == '\n') {
                    st.step = STEP_COPY;
                    status_set(&st, "Preparing disk ...");
                    draw_ui(h, &st);
                    int install_rc = run_install(&st);
                    if (install_rc != 0) {
                        if (st.error[0] == '\0') {
                            snprintf(st.error, sizeof(st.error), "Installer failed (rc=%d)", install_rc);
                            st.error[sizeof(st.error) - 1] = '\0';
                        }
                        printf("[installer] failure rc=%d msg=%s\n", install_rc, st.error);
                        st.step = STEP_ERROR;
                    }
                }
            }
        }

        draw_ui(h, &st);
    }

    g_installer_gui_handle = -1;
    return 0;
}

/*
 * build_user_c.sh compiles one translation unit per command source.
 * Include install module implementations directly to preserve modular files
 * under /install without changing the userland build pipeline.
 */
#define PKG_EXTRACT_EMBEDDED_ONLY 1
#include "../../install/package.c"
#include "../../install/index.c"
#include "../../install/resolve.c"
#undef PKG_EXTRACT_EMBEDDED_ONLY

#define EXTRACT_EMBEDDED_MODE 1
#define main install_embedded_extract_main
#include "extract_uelf.c"
#undef main
#undef EXTRACT_EMBEDDED_MODE
