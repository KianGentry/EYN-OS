#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Graphical file explorer.", "files [/path]");


// Constants
#define MAX_ENTRIES   64
#define MAX_PATH      256

/* Layout (pixels) */
#define ROW_H         18
#define HEADER_H      26
#define ICON_SIZE     16       /* max expected icon dimension (cache)  */
#define ICON_X         2       /* x offset for icon                    */
#define NAME_COL_X    22       /* text starts after icon column        */
#define FONT_H        10

/*
    Colour palette -- uses the OS default from <gui.h>.
    App-local aliases for brevity.
*/

#define BG_R    GUI_PAL_BG_R
#define BG_G    GUI_PAL_BG_G
#define BG_B    GUI_PAL_BG_B

#define HDR_R   GUI_PAL_HEADER_R
#define HDR_G   GUI_PAL_HEADER_G
#define HDR_B   GUI_PAL_HEADER_B

#define SEL_R   GUI_PAL_SEL_R
#define SEL_G   GUI_PAL_SEL_G
#define SEL_B   GUI_PAL_SEL_B

#define HOV_R   GUI_PAL_HOVER_R
#define HOV_G   GUI_PAL_HOVER_G
#define HOV_B   GUI_PAL_HOVER_B

#define DIR_R   GUI_PAL_ACCENT_R
#define DIR_G   GUI_PAL_ACCENT_G
#define DIR_B   GUI_PAL_ACCENT_B

#define FILE_R  GUI_PAL_TEXT_R
#define FILE_G  GUI_PAL_TEXT_G
#define FILE_B  GUI_PAL_TEXT_B

#define PATH_R  GUI_PAL_TEXT_R
#define PATH_G  GUI_PAL_TEXT_G
#define PATH_B  GUI_PAL_TEXT_B

#define DIM_R  GUI_PAL_DIM_R
#define DIM_G  GUI_PAL_DIM_G
#define DIM_B  GUI_PAL_DIM_B

#define BORDER_R  GUI_PAL_BORDER_R
#define BORDER_G  GUI_PAL_BORDER_G
#define BORDER_B  GUI_PAL_BORDER_B

/* Double-click detection threshold (frames at ~16 ms each) */
#define DBLCLICK_FRAMES 20


// Application state

typedef struct {
    char     name[56];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  is_drive;
    uint8_t  drive_index;
} entry_t;

typedef struct {
    int      handle;
    int      running;

    char     path[MAX_PATH];
    int      drive;
    int      at_drive_select;

    entry_t  entries[MAX_ENTRIES];
    int      entry_count;

    int      selected;
    int      scroll;

    /* mouse */
    int      prev_left_down;
    int      last_click_idx;
    int      dblclick_timer;
} app_t;

// Helpers

static void safe_strcpy(char* dst, int dst_sz, const char* src) {
    int i = 0;
    for (; i < dst_sz - 1 && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}

static void build_path_label(const app_t* app, char* out, int cap) {
    if (!out || cap <= 0) return;
    if (app->at_drive_select) {
        safe_strcpy(out, cap, "Drives");
        return;
    }

    const char* path = app->path[0] ? app->path : "/";
    snprintf(out, (size_t)cap, "%d:%s", app->drive, path);
}

static void format_size(char* buf, int buf_sz, uint32_t sz) {
    (void)buf_sz;
    if (sz >= 1048576) {
        unsigned whole = sz / 1048576;
        unsigned frac  = ((sz % 1048576) * 10) / 1048576;
        int i = 0;
        if (whole >= 1000) buf[i++] = (char)('0' + (whole / 1000) % 10);
        if (whole >= 100)  buf[i++] = (char)('0' + (whole / 100) % 10);
        if (whole >= 10)   buf[i++] = (char)('0' + (whole / 10) % 10);
        buf[i++] = (char)('0' + whole % 10);
        buf[i++] = '.';
        buf[i++] = (char)('0' + frac % 10);
        buf[i++] = ' ';
        buf[i++] = 'M';
        buf[i]   = '\0';
    } else if (sz >= 1024) {
        unsigned whole = sz / 1024;
        int i = 0;
        if (whole >= 100) buf[i++] = (char)('0' + (whole / 100) % 10);
        if (whole >= 10)  buf[i++] = (char)('0' + (whole / 10) % 10);
        buf[i++] = (char)('0' + whole % 10);
        buf[i++] = ' ';
        buf[i++] = 'K';
        buf[i]   = '\0';
    } else {
        unsigned v = sz;
        int i = 0;
        if (v >= 10000) buf[i++] = (char)('0' + (v / 10000) % 10);
        if (v >= 1000)  buf[i++] = (char)('0' + (v / 1000) % 10);
        if (v >= 100)   buf[i++] = (char)('0' + (v / 100) % 10);
        if (v >= 10)    buf[i++] = (char)('0' + (v / 10) % 10);
        buf[i++] = (char)('0' + v % 10);
        buf[i++] = ' ';
        buf[i++] = 'B';
        buf[i]   = '\0';
    }
}


/* File-type icon names (kernel-side REI icon cache keys)             */


/*
 * Returns the icon cache key for a file entry, e.g. "file_c",
 * "dir_empty", "dir_full".  The keys correspond to REI filenames
 * in /icons16/ (or /icons/ as fallback).
 */
static const char* icon_name_for_entry(const entry_t* e) {
    if (e->is_dir) {
        /* Directories: dir_full for non-empty, dir_empty otherwise.
         * We don't track child count, so default to dir_full. */
        return "dir_full";
    }

    const char* dot = NULL;
    for (int i = 0; e->name[i]; ++i)
        if (e->name[i] == '.') dot = &e->name[i];

    if (dot) {
        if (strcmp(dot, ".txt") == 0)  return "file_txt";
        if (strcmp(dot, ".md") == 0)   return "file_md";
        if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)
            return "file_c";
        if (strcmp(dot, ".asm") == 0 || strcmp(dot, ".s") == 0)
            return "file_asm";
        if (strcmp(dot, ".rei") == 0 || strcmp(dot, ".reiv") == 0)
            return "file_rei";
        if (strcmp(dot, ".shell") == 0)
            return "file_shell";
        if (strcmp(dot, ".uelf") == 0)
            return "file_bin";
        if (strcmp(dot, ".eyn") == 0)
            return "file_eyn";
    }

    return "file_none";
}

// File-type extension helpers

static const char* find_ext(const char* name) {
    const char* dot = NULL;
    for (int i = 0; name[i]; ++i)
        if (name[i] == '.') dot = &name[i];
    return dot;
}

static int load_drive_list(app_t* app) {
    app->entry_count = 0;
    app->selected    = 0;
    app->scroll      = 0;

    int count = eyn_sys_drive_get_count();
    if (count < 0) count = 0;

    for (int i = 0; i < count && app->entry_count < MAX_ENTRIES; ++i) {
        int present = eyn_sys_drive_is_present((uint32_t)i);
        if (present <= 0) continue;

        entry_t* e = &app->entries[app->entry_count++];
        snprintf(e->name, sizeof(e->name), "Drive %d", i);
        e->size = 0;
        e->is_dir = 1;
        e->is_drive = 1;
        e->drive_index = (uint8_t)i;
    }

    return app->entry_count;
}


// Directory loading

static int load_directory(app_t* app) {
    if (app->at_drive_select) {
        return load_drive_list(app);
    }

    app->entry_count = 0;
    app->selected    = 0;
    app->scroll      = 0;

    int fd = open(app->path, O_RDONLY, 0);
    if (fd < 0) return -1;

    eyn_dirent_t raw[16];
    for (;;) {
        int rc = getdents(fd, raw, sizeof(raw));
        if (rc <= 0) break;
        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count && app->entry_count < MAX_ENTRIES; ++i) {
            if (raw[i].name[0] == '\0') continue;
            entry_t* e = &app->entries[app->entry_count];
            safe_strcpy(e->name, (int)sizeof(e->name), raw[i].name);
            e->size   = raw[i].size;
            e->is_dir = raw[i].is_dir;
            e->is_drive = 0;
            e->drive_index = 0;
            app->entry_count++;
        }
    }

    for (int i = 1; i < app->entry_count; ++i) {
        entry_t key = app->entries[i];
        int j = i - 1;
        while (j >= 0 && strcmp(app->entries[j].name, key.name) > 0) {
            app->entries[j + 1] = app->entries[j];
            --j;
        }
        app->entries[j + 1] = key;
    }

    (void)close(fd);
    return app->entry_count;
}

// Navigation

static void navigate_into(app_t* app, const char* child) {
    int plen = str_len(app->path);

    if (plen > 0 && app->path[plen - 1] != '/') {
        if (plen + 1 < MAX_PATH) {
            app->path[plen++] = '/';
            app->path[plen]   = '\0';
        }
    }

    int clen = str_len(child);
    if (plen + clen < MAX_PATH) {
        for (int i = 0; i < clen; ++i)
            app->path[plen + i] = child[i];
        app->path[plen + clen] = '\0';
    }

    load_directory(app);
}

static void navigate_up(app_t* app) {
    if (app->at_drive_select) return;

    int plen = str_len(app->path);

    if (plen > 1 && app->path[plen - 1] == '/') {
        app->path[--plen] = '\0';
    }

    if (plen <= 1) {
        app->at_drive_select = 1;
        safe_strcpy(app->path, MAX_PATH, "/");
        load_directory(app);
        return;
    }

    int last = -1;
    for (int i = plen - 1; i >= 0; --i) {
        if (app->path[i] == '/') { last = i; break; }
    }

    if (last <= 0)
        safe_strcpy(app->path, MAX_PATH, "/");
    else
        app->path[last] = '\0';

    load_directory(app);
}

static int is_elf_file(const char* path) {
    int fd = open(path, 0, 0);
    if (fd < 0) return 0;
    unsigned char hdr[4] = {0};
    int n = read(fd, hdr, 4);
    close(fd);
    if (n < 4) return 0;
    return (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F');
}

static void activate_entry(app_t* app) {
    if (app->selected < 0 || app->selected >= app->entry_count) return;
    entry_t* e = &app->entries[app->selected];

    if (e->is_drive) {
        (void)eyn_sys_drive_set_logical((uint32_t)e->drive_index);
        app->drive = (int)e->drive_index;
        app->at_drive_select = 0;
        safe_strcpy(app->path, MAX_PATH, "/");
        load_directory(app);
        return;
    }

    if (e->is_dir) {
        navigate_into(app, e->name);
        return;
    }

    /* Build full path to the selected file */
    char full[MAX_PATH];
    int plen = str_len(app->path);
    safe_strcpy(full, MAX_PATH, app->path);
    if (plen > 0 && full[plen - 1] != '/' && plen + 1 < MAX_PATH) {
        full[plen++] = '/';
        full[plen]   = '\0';
    }
    int nlen = str_len(e->name);
    if (plen + nlen < MAX_PATH) {
        for (int i = 0; i < nlen; ++i)
            full[plen + i] = e->name[i];
        full[plen + nlen] = '\0';
    }

    const char* ext = find_ext(e->name);

    /*
     * Dispatch based on file type:
     *   .uelf / ELF magic  → run (replaces this process)
     *   .rei / .reiv        → open with view
     *   .shell              → run as shell script
     *   extensionless ELF   → run (detected via magic bytes)
     *   anything else       → do nothing (write editor unavailable)
     *
     * eyn_sys_run() replaces the current process with the target
     * program.  Close the GUI before calling so the tile is free
     * for the new program's output.
     */
    char cmd[MAX_PATH + 32];

    if (ext && strcmp(ext, ".uelf") == 0) {
        /* Executable UELF -- run directly */
        (void)gui_set_continuous_redraw(app->handle, 0);
        app->running = 0;
        eyn_sys_run(full);
        return;
    }

    if (ext && (strcmp(ext, ".rei") == 0 || strcmp(ext, ".reiv") == 0)) {
        /* Image -- open with view */
        safe_strcpy(cmd, (int)sizeof(cmd), "/binaries/view ");
        int clen = str_len(cmd);
        int flen = str_len(full);
        if (clen + flen < (int)sizeof(cmd)) {
            for (int i = 0; i < flen; ++i)
                cmd[clen + i] = full[i];
            cmd[clen + flen] = '\0';
        }
        (void)gui_set_continuous_redraw(app->handle, 0);
        app->running = 0;
        eyn_sys_run(cmd);
        return;
    }

    if (ext && strcmp(ext, ".shell") == 0) {
        /* Shell script -- run directly */
        (void)gui_set_continuous_redraw(app->handle, 0);
        app->running = 0;
        eyn_sys_run(full);
        return;
    }

    /* No recognized extension -- check if it's an ELF binary (magic bytes) */
    if (is_elf_file(full)) {
        (void)gui_set_continuous_redraw(app->handle, 0);
        app->running = 0;
        /*
         * Most user commands are extensionless ELFs in /binaries and are
         * intended to be launched by command name. Use the short name in that
         * directory to match shell behavior; otherwise use absolute path.
         */
        if (strcmp(app->path, "/binaries") == 0) {
            eyn_sys_run(e->name);
        } else {
            eyn_sys_run(full);
        }
        return;
    }

    /* Unrecognised file type -- do nothing (write editor unavailable) */
}


// Clamp helpers


static void clamp_selection(app_t* app) {
    if (app->entry_count == 0) { app->selected = 0; return; }
    if (app->selected < 0) app->selected = 0;
    if (app->selected >= app->entry_count) app->selected = app->entry_count - 1;
}

static void ensure_visible(app_t* app, int max_rows) {
    if (max_rows <= 0) max_rows = 1;
    if (app->selected < app->scroll) app->scroll = app->selected;
    if (app->selected >= app->scroll + max_rows)
        app->scroll = app->selected - max_rows + 1;
    if (app->scroll < 0) app->scroll = 0;
}

// Drawing

static void draw_ui(app_t* app) {
    gui_size_t sz;
    sz.w = 0;
    sz.h = 0;
    (void)gui_get_content_size(app->handle, &sz);
    if (sz.w <= 0) sz.w = 480;
    if (sz.h <= 0) sz.h = 360;

    /* Begin a new frame -- resets the kernel draw-command buffer.
     * Without this, commands accumulate across frames and exceed the
     * 48-slot limit, causing subsequent draw calls to silently fail. */
    (void)gui_begin(app->handle);

    /* Background */
    gui_rgb_t bg = { .r = BG_R, .g = BG_G, .b = BG_B, ._pad = 0 };
    (void)gui_clear(app->handle, &bg);

    /* Header bar */
    gui_rect_t hdr = {
        .x = 0, .y = 0, .w = sz.w, .h = HEADER_H,
        .r = HDR_R, .g = HDR_G, .b = HDR_B, ._pad = 0
    };
    (void)gui_fill_rect(app->handle, &hdr);

    /* Path breadcrumb */
    char path_buf[MAX_PATH + 16];
    build_path_label(app, path_buf, (int)sizeof(path_buf));
    gui_text_t path_txt = {
        .x = 8, .y = (HEADER_H - FONT_H) / 2,
        .r = PATH_R, .g = PATH_G, .b = PATH_B, ._pad = 0,
        .text = path_buf
    };
    (void)gui_draw_text(app->handle, &path_txt);

    /* Separator below header */
    gui_line_t hdr_sep = {
        .x1 = 0, .y1 = HEADER_H, .x2 = sz.w - 1, .y2 = HEADER_H,
        .r = BORDER_R, .g = BORDER_G, .b = BORDER_B, ._pad = 0
    };
    (void)gui_draw_line(app->handle, &hdr_sep);

    /* Column headers */
    int col_y = HEADER_H + 4;
    gui_text_t col_name = {
        .x = NAME_COL_X, .y = col_y,
        .r = DIM_R, .g = DIM_G, .b = DIM_B, ._pad = 0,
        .text = "Name"
    };
    gui_text_t col_size = {
        .x = sz.w - 70, .y = col_y,
        .r = DIM_R, .g = DIM_G, .b = DIM_B, ._pad = 0,
        .text = "Size"
    };
    (void)gui_draw_text(app->handle, &col_name);
    (void)gui_draw_text(app->handle, &col_size);

    /* Separator below column headers */
    int sep_y = col_y + FONT_H + 3;
    gui_line_t col_sep = {
        .x1 = 2, .y1 = sep_y, .x2 = sz.w - 3, .y2 = sep_y,
        .r = BORDER_R, .g = BORDER_G, .b = BORDER_B, ._pad = 0
    };
    (void)gui_draw_line(app->handle, &col_sep);

    /* Entry list */
    int list_y = sep_y + 2;
    int max_rows = (sz.h - list_y - 2) / ROW_H;
    if (max_rows < 1) max_rows = 1;

    clamp_selection(app);
    ensure_visible(app, max_rows);

    if (app->entry_count == 0) {
        gui_text_t empty = {
            .x = NAME_COL_X, .y = list_y + 6,
            .r = DIM_R, .g = DIM_G, .b = DIM_B, ._pad = 0,
            .text = app->at_drive_select ? "(no drives present)" : "(empty directory)"
        };
        (void)gui_draw_text(app->handle, &empty);
    }

    for (int i = 0; i < max_rows; ++i) {
        int idx = app->scroll + i;
        if (idx >= app->entry_count) break;

        entry_t* e = &app->entries[idx];
        int ey = list_y + i * ROW_H;

        /* Selected row highlight */
        if (idx == app->selected) {
            gui_rect_t hi = {
                .x = 2, .y = ey, .w = sz.w - 4, .h = ROW_H,
                .r = SEL_R, .g = SEL_G, .b = SEL_B, ._pad = 0
            };
            (void)gui_fill_rect(app->handle, &hi);
        }

        /* File-type icon (REI image from kernel icon cache) */
        if (!e->is_drive) {
            const char* iname = icon_name_for_entry(e);
            gui_icon_t ic = {
                .x = ICON_X, .y = ey + (ROW_H - ICON_SIZE) / 2,
                .icon_name = iname
            };
            (void)gui_draw_icon(app->handle, &ic);
        }

        /* Name */
        if (e->is_drive) {
            gui_text_t t = {
                .x = NAME_COL_X, .y = ey + 2,
                .r = DIR_R, .g = DIR_G, .b = DIR_B, ._pad = 0,
                .text = e->name
            };
            (void)gui_draw_text(app->handle, &t);
        } else if (e->is_dir) {
            char dir_name[60];
            int ni = 0;
            for (; ni < 55 && e->name[ni]; ++ni)
                dir_name[ni] = e->name[ni];
            dir_name[ni++] = '/';
            dir_name[ni]   = '\0';

            gui_text_t t = {
                .x = NAME_COL_X, .y = ey + 2,
                .r = DIR_R, .g = DIR_G, .b = DIR_B, ._pad = 0,
                .text = dir_name
            };
            (void)gui_draw_text(app->handle, &t);
        } else {
            gui_text_t t = {
                .x = NAME_COL_X, .y = ey + 2,
                .r = FILE_R, .g = FILE_G, .b = FILE_B, ._pad = 0,
                .text = e->name
            };
            (void)gui_draw_text(app->handle, &t);

            /* Size */
            char size_buf[16];
            format_size(size_buf, (int)sizeof(size_buf), e->size);
            gui_text_t st = {
                .x = sz.w - 70, .y = ey + 2,
                .r = DIM_R, .g = DIM_G, .b = DIM_B, ._pad = 0,
                .text = size_buf
            };
            (void)gui_draw_text(app->handle, &st);
        }
    }

    /* Scrollbar */
    if (app->entry_count > max_rows) {
        int bar_total_h = sz.h - list_y - 2;
        int bar_h = (max_rows * bar_total_h) / app->entry_count;
        if (bar_h < 8) bar_h = 8;
        int bar_y = list_y + (app->scroll * bar_total_h) / app->entry_count;
        gui_rect_t sb = {
            .x = sz.w - 4, .y = bar_y, .w = 3, .h = bar_h,
            .r = BORDER_R, .g = BORDER_G, .b = BORDER_B, ._pad = 0
        };
        (void)gui_fill_rect(app->handle, &sb);
    }

    (void)gui_present(app->handle);
}

// Event handling

static void handle_key(app_t* app, int key) {
    int base = key & 0x0FFF;
    unsigned ch = (unsigned)key & 0xFFu;

    if (ch == (unsigned)'q' || ch == (unsigned)'Q' || ch == 27u) {
        app->running = 0;
        return;
    }

    /* Up */
    if (key == 0x1001 || base == 0x1001 || key == 0x4800 || key == 72) {
        if (app->selected > 0) app->selected--;
        return;
    }

    /* Down */
    if (key == 0x1002 || base == 0x1002 || key == 0x5000 || key == 80) {
        if (app->selected + 1 < app->entry_count) app->selected++;
        return;
    }

    /* Enter */
    if (ch == '\n' || ch == '\r') {
        activate_entry(app);
        return;
    }

    /* Backspace */
    if (ch == 8 || ch == 127) {
        navigate_up(app);
        return;
    }
}

static void handle_mouse(app_t* app, gui_event_t* ev) {
    gui_size_t sz;
    sz.w = 0;
    sz.h = 0;
    (void)gui_get_content_size(app->handle, &sz);
    if (sz.w <= 0) sz.w = 480;
    if (sz.h <= 0) sz.h = 360;

    /* Scroll wheel */
    if (ev->d > 0 && app->scroll > 0) {
        app->scroll -= 2;
        if (app->scroll < 0) app->scroll = 0;
    } else if (ev->d < 0) {
        app->scroll += 2;
        int max_rows = (sz.h - (HEADER_H + 4 + FONT_H + 3 + 2) - 2) / ROW_H;
        if (max_rows < 1) max_rows = 1;
        int max_scroll = app->entry_count - max_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (app->scroll > max_scroll) app->scroll = max_scroll;
    }

    /* Click edge */
    int left_down  = (ev->c & 0x1) != 0;
    int press_edge = left_down && !app->prev_left_down;
    app->prev_left_down = left_down;

    if (!press_edge) {
        if (app->dblclick_timer > 0) app->dblclick_timer--;
        return;
    }

    int col_y  = HEADER_H + 4;
    int sep_y  = col_y + FONT_H + 3;
    int list_y = sep_y + 2;
    int max_rows = (sz.h - list_y - 2) / ROW_H;
    if (max_rows < 1) max_rows = 1;

    /* Click in header → navigate up */
    if (ev->b < list_y) {
        navigate_up(app);
        return;
    }

    int row = (ev->b - list_y) / ROW_H;
    int idx = app->scroll + row;
    if (idx < 0 || idx >= app->entry_count) return;

    /* Double click → activate */
    if (idx == app->last_click_idx && app->dblclick_timer > 0) {
        app->selected = idx;
        activate_entry(app);
        app->dblclick_timer = 0;
        app->last_click_idx = -1;
        return;
    }

    /* Single click → select */
    app->selected       = idx;
    app->last_click_idx = idx;
    app->dblclick_timer = DBLCLICK_FRAMES;
}


/* Main */


int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: files [path]");
        return 0;
    }

    app_t app;
    memset(&app, 0, sizeof(app));
    app.running        = 1;
    app.last_click_idx = -1;
    app.drive          = 0;
    app.at_drive_select = 0;

    int drive_count = eyn_sys_drive_get_count();
    if (drive_count <= 0) {
        app.at_drive_select = 1;
    } else {
        int cur = eyn_sys_drive_get_logical();
        if (cur < 0 || cur >= drive_count) {
            cur = 0;
            (void)eyn_sys_drive_set_logical(0);
        }
        app.drive = cur;
    }

    if (argc >= 2 && argv[1] && argv[1][0]) {
        safe_strcpy(app.path, MAX_PATH, argv[1]);
    } else {
        safe_strcpy(app.path, MAX_PATH, "/");
    }

    if (load_directory(&app) < 0 && !app.at_drive_select) {
        printf("files: cannot open %s\n", app.path);
        return 1;
    }

    app.handle = gui_attach("File Explorer",
                             "Arrows/click browse | Enter/dblclick open | Backspace up | q quit");
    if (app.handle < 0) {
        puts("files: gui_attach failed");
        return 1;
    }

    int need_redraw = 1;

    while (app.running) {
        gui_event_t ev;
        while (gui_poll_event(app.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                app.running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEY) {
                int old_selected = app.selected;
                int old_scroll = app.scroll;
                int old_entry_count = app.entry_count;
                int old_at_drive_select = app.at_drive_select;
                int old_running = app.running;
                char old_path[MAX_PATH];
                safe_strcpy(old_path, MAX_PATH, app.path);
                handle_key(&app, ev.a);

                if (app.selected != old_selected ||
                    app.scroll != old_scroll ||
                    app.entry_count != old_entry_count ||
                    app.at_drive_select != old_at_drive_select ||
                    app.running != old_running ||
                    strcmp(app.path, old_path) != 0) {
                    need_redraw = 1;
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                int old_selected = app.selected;
                int old_scroll = app.scroll;
                int old_entry_count = app.entry_count;
                int old_at_drive_select = app.at_drive_select;
                int old_running = app.running;
                char old_path[MAX_PATH];
                safe_strcpy(old_path, MAX_PATH, app.path);
                handle_mouse(&app, &ev);

                if (app.selected != old_selected ||
                    app.scroll != old_scroll ||
                    app.entry_count != old_entry_count ||
                    app.at_drive_select != old_at_drive_select ||
                    app.running != old_running ||
                    strcmp(app.path, old_path) != 0) {
                    need_redraw = 1;
                }
            }
        }

        if (need_redraw) {
            draw_ui(&app);
            need_redraw = 0;
        } else {
            usleep(8000);
        }
    }

    return 0;
}
