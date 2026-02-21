#include <write_editor.h>
#include <types.h>
#include <eynfs.h>
#include <fat32.h>
#include <vga.h>
#include <mouse.h>
#include <system.h>
#include <string.h>
#include <util.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <string.h>
#include <tui.h>
#include <tile_manager.h>
#include <vga.h>
#include <fs/vfs.h>
#include <context.h>
#include <misc/sched.h>
#define EYNFS_SUPERBLOCK_LBA 2048
extern void* fat32_disk_img;

// Editor state and helpers (renamed)
// Variable-length lines stored as per-line heap buffers.
// We cap both line count and total bytes to keep memory use predictable.
#define MAX_LINES 10000
#define MAX_LINE_LENGTH 4096
// Max bytes we will load/edit/save as a single in-memory buffer.
#define WRITE_EDITOR_MAX_FILE_BYTES (256 * 1024)
#define EDITOR_HEIGHT 30
static char* write_editor_buffer[MAX_LINES];
static uint16_t write_editor_line_caps[MAX_LINES];
static int write_editor_num_lines = 1;
static int write_editor_cursor_x = 0;
static int write_editor_cursor_y = 0;
static int write_editor_scroll_y = 0;
static int write_editor_scroll_x = 0;
static int write_editor_modified = 0;
static int write_editor_show_whitespace = 0;

static int write_editor_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Selection and clipboard state
static int write_editor_sel_active = 0;
static int write_editor_sel_ax = 0, write_editor_sel_ay = 0; // anchor (col,x) within absolute line
static int write_editor_sel_fx = 0, write_editor_sel_fy = 0; // focus (col,x)
static char write_editor_clipboard[4096];
static int write_editor_clipboard_len = 0;

// Forward declarations for helpers used before their definitions
static void write_editor_update_tile_modified(void);
static int write_editor_ensure_line_cap(int idx, int need_chars_including_nul);
static int write_editor_line_len(int idx);
static int write_editor_copy_range_text(int start_y, int start_x, int finish_y, int finish_x, char* out, int out_cap, int* out_len);
static void write_editor_set_line_from_span(int idx, const char* s, int len, int* out_truncated);
static void write_editor_shift_lines_down(int at_idx);
static void write_editor_delete_line_at(int idx);

// Lightweight status message (shown in GUI status bar when non-empty)
static char write_editor_status_msg[96];

// Find / Go-to overlays (GUI mode)
typedef enum {
    WRITE_EDITOR_MODAL_NONE = 0,
    WRITE_EDITOR_MODAL_FIND,
    WRITE_EDITOR_MODAL_GOTO,
} write_editor_modal_t;
static write_editor_modal_t write_editor_modal = WRITE_EDITOR_MODAL_NONE;
static char write_editor_find_query[64];
static int write_editor_find_len = 0;
static char write_editor_goto_buf[16];
static int write_editor_goto_len = 0;

// Pop-out scrollbar geometry (pixels) for mouse hit-testing
static int write_editor_scrollbar_active = 0;
static int write_editor_scrollbar_x = 0;
static int write_editor_scrollbar_y = 0;
static int write_editor_scrollbar_w = 0;
static int write_editor_scrollbar_h = 0;
static int write_editor_scrollbar_thumb_y = 0;
static int write_editor_scrollbar_thumb_h = 0;

// Undo/Redo (bounded, no heap)
#define WRITE_EDITOR_UNDO_DEPTH 48
#define WRITE_EDITOR_UNDO_TEXT_MAX 256
typedef struct {
    uint16 y;
    uint16 x;
    uint16 before_len;
    uint16 after_len;
    char before[WRITE_EDITOR_UNDO_TEXT_MAX];
    char after[WRITE_EDITOR_UNDO_TEXT_MAX];
} write_editor_undo_t;
static write_editor_undo_t write_editor_undo_stack[WRITE_EDITOR_UNDO_DEPTH];
static int write_editor_undo_len = 0;
static write_editor_undo_t write_editor_redo_stack[WRITE_EDITOR_UNDO_DEPTH];
static int write_editor_redo_len = 0;

static void write_editor_set_status(const char* msg) {
    if (!msg) msg = "";
    snprintf(write_editor_status_msg, sizeof(write_editor_status_msg), "%s", msg);
}

static void write_editor_copy_selection_to_clipboard(void) {
    write_editor_clipboard_len = 0;
    if (!write_editor_sel_active) {
        write_editor_clipboard[0] = '\0';
        return;
    }
    int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
    int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
    if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
    (void)write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, write_editor_clipboard, (int)sizeof(write_editor_clipboard) - 1, &write_editor_clipboard_len);
    if (write_editor_clipboard_len < 0) write_editor_clipboard_len = 0;
    write_editor_clipboard[write_editor_clipboard_len] = '\0';
}

static void write_editor_clear_status(void) {
    write_editor_status_msg[0] = '\0';
}

static void write_editor_undo_clear(void) {
    write_editor_undo_len = 0;
    write_editor_redo_len = 0;
}

static void write_editor_undo_clear_redo(void) {
    write_editor_redo_len = 0;
}

static int write_editor_undo_push(write_editor_undo_t rec) {
    if (rec.before_len > WRITE_EDITOR_UNDO_TEXT_MAX || rec.after_len > WRITE_EDITOR_UNDO_TEXT_MAX) return -1;
    if (write_editor_undo_len >= WRITE_EDITOR_UNDO_DEPTH) {
        // Drop oldest
        memmove(&write_editor_undo_stack[0], &write_editor_undo_stack[1], sizeof(write_editor_undo_stack[0]) * (WRITE_EDITOR_UNDO_DEPTH - 1));
        write_editor_undo_len = WRITE_EDITOR_UNDO_DEPTH - 1;
    }
    write_editor_undo_stack[write_editor_undo_len++] = rec;
    write_editor_undo_clear_redo();
    return 0;
}

static int write_editor_redo_push(write_editor_undo_t rec) {
    if (rec.before_len > WRITE_EDITOR_UNDO_TEXT_MAX || rec.after_len > WRITE_EDITOR_UNDO_TEXT_MAX) return -1;
    if (write_editor_redo_len >= WRITE_EDITOR_UNDO_DEPTH) {
        memmove(&write_editor_redo_stack[0], &write_editor_redo_stack[1], sizeof(write_editor_redo_stack[0]) * (WRITE_EDITOR_UNDO_DEPTH - 1));
        write_editor_redo_len = WRITE_EDITOR_UNDO_DEPTH - 1;
    }
    write_editor_redo_stack[write_editor_redo_len++] = rec;
    return 0;
}

static int write_editor_copy_range_text(int start_y, int start_x, int finish_y, int finish_x, char* out, int out_cap, int* out_len) {
    if (out_len) *out_len = 0;
    if (!out || out_cap <= 0) return -1;
    out[0] = '\0';
    if (start_y < 0) start_y = 0;
    if (finish_y < 0) finish_y = 0;
    if (start_y >= write_editor_num_lines) start_y = write_editor_num_lines - 1;
    if (finish_y >= write_editor_num_lines) finish_y = write_editor_num_lines - 1;
    int s_len = write_editor_line_len(start_y);
    int f_len = write_editor_line_len(finish_y);
    if (start_x < 0) start_x = 0;
    if (finish_x < 0) finish_x = 0;
    if (start_x > s_len) start_x = s_len;
    if (finish_x > f_len) finish_x = f_len;
    int w = 0;
    for (int y = start_y; y <= finish_y; ++y) {
        int start = (y == start_y) ? start_x : 0;
        int end = (y == finish_y) ? finish_x : write_editor_line_len(y);
        const char* s = write_editor_buffer[y] ? write_editor_buffer[y] : "";
        for (int x = start; x < end; ++x) {
            if (w >= out_cap) { if (out_len) *out_len = w; return -1; }
            out[w++] = s[x];
        }
        if (y != finish_y) {
            if (w >= out_cap) { if (out_len) *out_len = w; return -1; }
            out[w++] = '\n';
        }
    }
    if (w >= out_cap) { if (out_len) *out_len = w; return -1; }
    out[w] = '\0';
    if (out_len) *out_len = w;
    return 0;
}

static int write_editor_insert_text_at(int y, int x, const char* text, int len, int* out_end_y, int* out_end_x) {
    if (out_end_y) *out_end_y = y;
    if (out_end_x) *out_end_x = x;
    if (!text || len <= 0) return 0;
    if (y < 0) y = 0;
    if (y >= write_editor_num_lines) y = write_editor_num_lines - 1;
    int cx = x;
    int cy = y;
    for (int i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == '\0') break;
        if (ch == '\n') {
            if (write_editor_num_lines >= MAX_LINES) return -1;
            int line_len = write_editor_line_len(cy);
            if (cx > line_len) cx = line_len;
            char* line = write_editor_buffer[cy];
            if (!line) {
                write_editor_ensure_line_cap(cy, 1);
                line = write_editor_buffer[cy];
                line_len = write_editor_line_len(cy);
                if (cx > line_len) cx = line_len;
            }
            const char* tail = line ? &line[cx] : "";
            int tail_len = line_len - cx;
            if (tail_len < 0) tail_len = 0;
            if (line) line[cx] = '\0';
            write_editor_shift_lines_down(cy + 1);
            write_editor_set_line_from_span(cy + 1, tail, tail_len, NULL);
            cy++;
            cx = 0;
            continue;
        }
        if (ch < 32 || ch > 126) continue;
        int line_len = write_editor_line_len(cy);
        if (line_len >= MAX_LINE_LENGTH) continue;
        int new_len = line_len + 1;
        if (write_editor_ensure_line_cap(cy, new_len + 1) != 0) return -1;
        char* line = write_editor_buffer[cy];
        if (!line) return -1;
        if (cx > line_len) cx = line_len;
        int tail_bytes = line_len - cx;
        if (tail_bytes >= 0) {
            memmove(&line[cx + 1], &line[cx], (size_t)(tail_bytes + 1));
        }
        line[cx] = ch;
        cx++;
    }
    if (out_end_y) *out_end_y = cy;
    if (out_end_x) *out_end_x = cx;
    return 0;
}

static int write_editor_delete_text_at(int y, int x, const char* text, int len) {
    if (!text || len <= 0) return 0;
    int cy = y;
    int cx = x;
    if (cy < 0) cy = 0;
    if (cy >= write_editor_num_lines) cy = write_editor_num_lines - 1;
    for (int i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == '\0') break;
        if (ch == '\n') {
            // Delete a line break: merge current line with next line
            if (cy >= write_editor_num_lines - 1) continue;
            int left_len = write_editor_line_len(cy);
            if (cx > left_len) cx = left_len;
            int right_len = write_editor_line_len(cy + 1);
            int can_copy = MAX_LINE_LENGTH - left_len;
            int copy = (right_len < can_copy) ? right_len : can_copy;
            if (write_editor_ensure_line_cap(cy, left_len + copy + 1) != 0) return -1;
            char* left = write_editor_buffer[cy];
            char* right = write_editor_buffer[cy + 1];
            if (left && right && copy > 0) memcpy(&left[left_len], right, (size_t)copy);
            if (left) left[left_len + copy] = '\0';
            write_editor_delete_line_at(cy + 1);
            continue;
        }
        if (cy < 0 || cy >= write_editor_num_lines) break;
        int line_len = write_editor_line_len(cy);
        if (cx > line_len) cx = line_len;
        if (cx >= line_len) {
            // nothing to delete on this line; if next char would have been newline, it will be handled by '\n'
            continue;
        }
        char* line = write_editor_buffer[cy];
        if (!line) {
            write_editor_ensure_line_cap(cy, 1);
            line = write_editor_buffer[cy];
        }
        if (!line) return -1;
        memmove(&line[cx], &line[cx + 1], (size_t)((line_len - cx) + 1));
    }
    return 0;
}

static void write_editor_apply_undo_record(const write_editor_undo_t* rec, int is_redo) {
    if (!rec) return;
    int y = (int)rec->y;
    int x = (int)rec->x;
    if (!is_redo) {
        // Undo: remove after, restore before
        (void)write_editor_delete_text_at(y, x, rec->after, (int)rec->after_len);
        int end_y = y, end_x = x;
        (void)write_editor_insert_text_at(y, x, rec->before, (int)rec->before_len, &end_y, &end_x);
        write_editor_cursor_y = end_y;
        write_editor_cursor_x = end_x;
    } else {
        // Redo: remove before, restore after
        (void)write_editor_delete_text_at(y, x, rec->before, (int)rec->before_len);
        int end_y = y, end_x = x;
        (void)write_editor_insert_text_at(y, x, rec->after, (int)rec->after_len, &end_y, &end_x);
        write_editor_cursor_y = end_y;
        write_editor_cursor_x = end_x;
    }
    write_editor_sel_active = 0;
    write_editor_modified = 1;
    write_editor_update_tile_modified();
}

// Large-file warning (GUI mode)
static int write_editor_open_warn_active = 0;
static char write_editor_open_warn_line1[96];
static char write_editor_open_warn_line2[96];

// Editor-local storage to avoid heap fragmentation/exhaustion.
// NOTE: Keep this modest; QEMU default RAM is tiny.
static char write_editor_read_buf[WRITE_EDITOR_MAX_FILE_BYTES + 1];
static char write_editor_save_buf[WRITE_EDITOR_MAX_FILE_BYTES + 1];
static uint8 write_editor_arena[WRITE_EDITOR_MAX_FILE_BYTES + 16384];
static uint32 write_editor_arena_pos = 0;

static void write_editor_warn(const char* l1, const char* l2) {
    write_editor_open_warn_active = 1;
    snprintf(write_editor_open_warn_line1, sizeof(write_editor_open_warn_line1), "%s", l1 ? l1 : "");
    snprintf(write_editor_open_warn_line2, sizeof(write_editor_open_warn_line2), "%s", l2 ? l2 : "");
}

static void write_editor_arena_reset(void) {
    write_editor_arena_pos = 0;
}

static void* write_editor_arena_alloc(uint32 bytes, uint32 align) {
    if (align < 1) align = 1;
    uint32 p = write_editor_arena_pos;
    uint32 mask = align - 1;
    if ((align & mask) == 0) {
        p = (p + mask) & ~mask;
    }
    if (p + bytes > (uint32)sizeof(write_editor_arena)) return NULL;
    void* out = &write_editor_arena[p];
    write_editor_arena_pos = p + bytes;
    return out;
}

static void write_editor_free_line(int idx) {
    if (idx < 0 || idx >= MAX_LINES) return;
    // No heap free: editor storage comes from a simple arena.
    write_editor_buffer[idx] = NULL;
    write_editor_line_caps[idx] = 0;
}

static void write_editor_free_all_lines(void) {
    for (int i = 0; i < MAX_LINES; ++i) {
        write_editor_buffer[i] = NULL;
        write_editor_line_caps[i] = 0;
    }
    write_editor_arena_reset();
}

static void write_editor_reset_ephemeral_state(void) {
    write_editor_modal = WRITE_EDITOR_MODAL_NONE;
    write_editor_set_status("");
    write_editor_undo_clear();
}

static int write_editor_ensure_line_cap(int idx, int need_chars_including_nul) {
    if (idx < 0 || idx >= MAX_LINES) return -1;
    if (need_chars_including_nul < 1) need_chars_including_nul = 1;
    if (need_chars_including_nul > (MAX_LINE_LENGTH + 1)) need_chars_including_nul = MAX_LINE_LENGTH + 1;
    uint16_t have = write_editor_line_caps[idx];
    if (write_editor_buffer[idx] && have >= (uint16_t)need_chars_including_nul) return 0;

    int new_cap = have ? have : 64;
    while (new_cap < need_chars_including_nul) {
        int next = new_cap * 2;
        if (next <= new_cap) break;
        new_cap = next;
        if (new_cap > (MAX_LINE_LENGTH + 1)) { new_cap = MAX_LINE_LENGTH + 1; break; }
    }
    if (new_cap < need_chars_including_nul) new_cap = need_chars_including_nul;

    // Arena-backed grow: allocate a new buffer and copy old contents.
    char* p = (char*)write_editor_arena_alloc((uint32)new_cap, 16);
    if (!p) return -1;
    if (write_editor_buffer[idx]) {
        int old_len = strlength(write_editor_buffer[idx]);
        if (old_len >= new_cap) old_len = new_cap - 1;
        memcpy(p, write_editor_buffer[idx], (size_t)old_len);
        p[old_len] = '\0';
    } else {
        p[0] = '\0';
    }
    write_editor_buffer[idx] = p;
    write_editor_line_caps[idx] = (uint16_t)new_cap;
    return 0;
}

static int write_editor_line_len(int idx) {
    if (idx < 0 || idx >= write_editor_num_lines) return 0;
    if (!write_editor_buffer[idx]) return 0;
    return strlength(write_editor_buffer[idx]);
}

static void write_editor_set_line_from_span(int idx, const char* s, int len, int* out_truncated) {
    if (out_truncated) *out_truncated = 0;
    if (!s) { s = ""; len = 0; }
    if (len < 0) len = 0;
    if (len > MAX_LINE_LENGTH) {
        len = MAX_LINE_LENGTH;
        if (out_truncated) *out_truncated = 1;
    }
    if (write_editor_ensure_line_cap(idx, len + 1) != 0) return;
    memcpy(write_editor_buffer[idx], s, (size_t)len);
    write_editor_buffer[idx][len] = '\0';
}

static void write_editor_shift_lines_down(int at_idx) {
    if (at_idx < 0) at_idx = 0;
    if (at_idx > write_editor_num_lines) at_idx = write_editor_num_lines;
    if (write_editor_num_lines >= MAX_LINES) return;
    // Make room for a new line at at_idx by shifting pointers down
    for (int i = write_editor_num_lines; i > at_idx; --i) {
        write_editor_buffer[i] = write_editor_buffer[i - 1];
        write_editor_line_caps[i] = write_editor_line_caps[i - 1];
    }
    write_editor_buffer[at_idx] = NULL;
    write_editor_line_caps[at_idx] = 0;
    write_editor_num_lines++;
}

static void write_editor_delete_line_at(int idx) {
    if (idx < 0 || idx >= write_editor_num_lines) return;
    write_editor_free_line(idx);
        for (int i = idx; i < write_editor_num_lines - 1; ++i) {
        write_editor_buffer[i] = write_editor_buffer[i + 1];
        write_editor_line_caps[i] = write_editor_line_caps[i + 1];
    }
    write_editor_buffer[write_editor_num_lines - 1] = NULL;
    write_editor_line_caps[write_editor_num_lines - 1] = 0;
    write_editor_num_lines--;
    if (write_editor_num_lines < 1) {
        write_editor_num_lines = 1;
        write_editor_buffer[0] = NULL;
        write_editor_line_caps[0] = 0;
        write_editor_ensure_line_cap(0, 1);
        if (write_editor_buffer[0]) write_editor_buffer[0][0] = '\0';
    }
}
// GUI integration state (for tiling mode)
static int write_editor_gui_tile = -1;
static char write_editor_gui_filename[128];
static uint8 write_editor_gui_disk = 0;

// Unsaved-changes close prompt state (GUI mode)
static int write_editor_exit_prompt_active = 0;
static int write_editor_last_save_failed = 0;

// GUI geometry cached for scroll logic
static int write_editor_gui_cols = 0;
static int write_editor_gui_rows = 0;
// Last content rect (pixels) for hit-testing in mouse handler
static int write_editor_last_cx = 0;
static int write_editor_last_cy = 0;
static int write_editor_last_cw = 0;
static int write_editor_last_ch = 0;

// Selection and clipboard state
// (declared above)

// helper: update tile's modified indicator when running in GUI mode
// forward prototype for get_basename so helper can call it
static const char* get_basename(const char* path);

static void write_editor_update_tile_modified(void) {
    if (write_editor_gui_tile >= 0) {
        static char title_static[] = "Write Editor";
        static char left_buf[256];
        const char* b = get_basename(write_editor_gui_filename);

        // Move the editor's help/status line into the WM overlay instead of drawing a permanent bottom bar.
        const char* msg = NULL;
        if (write_editor_last_save_failed) msg = "Save failed";
        else if (write_editor_status_msg[0]) msg = write_editor_status_msg;

        if (msg) {
            snprintf(left_buf, sizeof(left_buf),
                     "%s | %s | Ctrl+S: Save | Ctrl+X: Cut | Ctrl+W: Whitespace | Ctrl+F: Find | Ctrl+G: Goto | Ctrl+Z/Y: Undo/Redo | Line %d Col %d",
                     b, msg, write_editor_cursor_y + 1, write_editor_cursor_x + 1);
        } else {
            snprintf(left_buf, sizeof(left_buf),
                     "%s | Ctrl+S: Save | Ctrl+X: Cut | Ctrl+W: Whitespace | Ctrl+F: Find | Ctrl+G: Goto | Ctrl+Z/Y: Undo/Redo | Line %d Col %d",
                     b, write_editor_cursor_y + 1, write_editor_cursor_x + 1);
        }
        left_buf[sizeof(left_buf) - 1] = '\0';

        if (write_editor_modified) tile_set_title_status(write_editor_gui_tile, title_static, left_buf, "[Modified] ");
        else tile_set_title_status(write_editor_gui_tile, title_static, left_buf, NULL);
    }
}

// Forward declarations for GUI callbacks
static void write_editor_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
static void write_editor_gui_key(int tile_idx, int key, void* userdata);
static void write_editor_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata);
static int write_editor_gui_close_request(int tile_idx, void* userdata);

static void write_editor_draw(const char* filename);

static int write_editor_digits10(int v) {
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

// Number of character columns reserved for the line-number gutter in GUI mode.
// Includes one trailing spacer column.
static int write_editor_gui_gutter_cols(void) {
    int digits = write_editor_digits10(write_editor_num_lines > 0 ? write_editor_num_lines : 1);
    // Example for 123 lines: " 123 " -> digits + 2
    return digits + 2;
}

static int write_editor_gui_visible_text_rows(void) {
    int cell_h = vga_text_cell_h();
    if (cell_h <= 0) cell_h = 8;
    int rows = write_editor_gui_rows;
    if (rows <= 0 && write_editor_last_ch > 0) rows = write_editor_last_ch / cell_h;
    if (rows < 1) rows = 1;
    // GUI mode no longer reserves a permanent bottom status bar row.
    return rows;
}

// Helper: get last path component (filename) from a path
static const char* get_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

// Delete current selection (if any). Returns 1 if something was deleted and state updated; 0 if no active selection.
static int write_editor_delete_active_selection(void) {
    if (!write_editor_sel_active) return 0;
    int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
    int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
    if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
    // clamp within bounds
    if (sel_sy < 0) sel_sy = 0;
    if (sel_sy >= write_editor_num_lines) sel_sy = write_editor_num_lines - 1;
    if (sel_fy < 0) sel_fy = 0;
    if (sel_fy >= write_editor_num_lines) sel_fy = write_editor_num_lines - 1;
    int s_len = write_editor_line_len(sel_sy); if (sel_sx < 0) sel_sx = 0; if (sel_sx > s_len) sel_sx = s_len;
    int f_len = write_editor_line_len(sel_fy); if (sel_fx < 0) sel_fx = 0; if (sel_fx > f_len) sel_fx = f_len;
    if (sel_sy == sel_fy) {
        // single-line delete
        char* line = write_editor_buffer[sel_sy];
        if (!line) {
            write_editor_ensure_line_cap(sel_sy, 1);
            line = write_editor_buffer[sel_sy];
        }
        int len = write_editor_line_len(sel_sy);
        if (line) memmove(&line[sel_sx], &line[sel_fx], (size_t)((len - sel_fx) + 1));
        write_editor_cursor_y = sel_sy; write_editor_cursor_x = sel_sx;
    } else {
        // multi-line: keep prefix of start line up to sx, then append tail of fy from fx
        char* sline = write_editor_buffer[sel_sy];
        if (!sline) {
            write_editor_ensure_line_cap(sel_sy, 1);
            sline = write_editor_buffer[sel_sy];
        }
        char* fline = write_editor_buffer[sel_fy];
        int pre_len = sel_sx;
        int tail_len = f_len - sel_fx;
        if (pre_len < 0) pre_len = 0;
        if (tail_len < 0) tail_len = 0;
        int new_len = pre_len + tail_len;
        if (new_len > MAX_LINE_LENGTH) new_len = MAX_LINE_LENGTH;
        if (write_editor_ensure_line_cap(sel_sy, new_len + 1) == 0 && sline) {
            if (pre_len > write_editor_line_len(sel_sy)) pre_len = write_editor_line_len(sel_sy);
            sline[pre_len] = '\0';
            int can_copy = MAX_LINE_LENGTH - pre_len;
            int copy = (tail_len < can_copy) ? tail_len : can_copy;
            if (copy > 0 && fline) memcpy(&sline[pre_len], &fline[sel_fx], (size_t)copy);
            sline[pre_len + copy] = '\0';
        }
        // delete lines sy+1 .. fy (inclusive)
        int remove = sel_fy - sel_sy;
        while (remove-- > 0) write_editor_delete_line_at(sel_sy + 1);
        write_editor_cursor_y = sel_sy; write_editor_cursor_x = sel_sx;
        // We only ever draw up to write_editor_num_lines, so clearing beyond that is unnecessary.
    }
    write_editor_sel_active = 0;
    write_editor_modified = 1;
    write_editor_update_tile_modified();
    // After large deletions (e.g., Ctrl+A), ensure scroll positions are in range
    if (write_editor_scroll_y > write_editor_cursor_y) write_editor_scroll_y = write_editor_cursor_y;
    if (write_editor_scroll_x > write_editor_cursor_x) write_editor_scroll_x = write_editor_cursor_x;
    tile_invalidate_gui(write_editor_gui_tile);
    return 1;
}

// Load file content into editor buffer
int load_file_to_write_editor(const char* path, uint8 disk) {
    if (!write_editor_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    write_editor_open_warn_active = 0;
    write_editor_open_warn_line1[0] = '\0';
    write_editor_open_warn_line2[0] = '\0';

    write_editor_free_all_lines();
    write_editor_num_lines = 1;
    write_editor_ensure_line_cap(0, 1);
    if (write_editor_buffer[0]) write_editor_buffer[0][0] = '\0';

    // Determine file size up-front (for warnings)
    vfs_stat_t st;
    uint32 file_size = 0;
    if (vfs_stat(disk, path, &st) == 0 && st.type == VFS_NODE_FILE) file_size = st.size;

    // Read via VFS (supports EYNFS and FAT32). Missing file is fine (start empty).
    // Use a fixed buffer to avoid heap pressure.
    uint32 max_bytes = WRITE_EDITOR_MAX_FILE_BYTES;
    uint32 want_bytes = file_size;
    if (want_bytes == 0) want_bytes = 4096; // fallback when stat missing/zero
    if (want_bytes > max_bytes) want_bytes = max_bytes;
    int n = vfs_read_file(disk, path, write_editor_read_buf, want_bytes);
    if (n <= 0) return 0;
    if ((uint32)n > want_bytes) n = (int)want_bytes;
    write_editor_read_buf[n] = '\0';

    // Warn if the file cannot fit in our read buffer (likely truncated)
    int any_line_truncated = 0;
    int truncated_lines = 0;
    if (file_size > (uint32)max_bytes) {
        write_editor_open_warn_active = 1;
        snprintf(write_editor_open_warn_line1, sizeof(write_editor_open_warn_line1), "Warning: file is large (%u bytes)", (unsigned)file_size);
        snprintf(write_editor_open_warn_line2, sizeof(write_editor_open_warn_line2), "Loaded first %u bytes", (unsigned)max_bytes);
    }
    // If we didn't trust stat (size==0) and filled our fallback buffer, it's probably truncated.
    if (!write_editor_open_warn_active && file_size == 0 && want_bytes < max_bytes && (uint32)n == want_bytes) {
        write_editor_warn("Warning: file may be large", "Loaded partial content");
    }

    // Helper: detect binary-like extensions we should present as editable hex
    int is_binary_edit = 0;
    const char* ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".eyn") == 0 || strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".flat") == 0) is_binary_edit = 1;
    }

    if (!is_binary_edit) {
        write_editor_free_all_lines();
        write_editor_num_lines = 0;
        int line = 0;
        int start = 0;
        for (int i = 0; i <= n; ++i) {
            if (i == n || write_editor_read_buf[i] == '\n') {
                int len = i - start;
                if (len > 0 && write_editor_read_buf[i - 1] == '\r') len--;
                if (line >= MAX_LINES) { truncated_lines = 1; break; }
                int trunc = 0;
                write_editor_set_line_from_span(line, write_editor_read_buf + start, len, &trunc);
                if (trunc) any_line_truncated = 1;
                line++;
                start = i + 1;
            }
        }
        write_editor_num_lines = (line > 0) ? line : 1;
        if (write_editor_num_lines < 1) write_editor_num_lines = 1;
        if (!write_editor_buffer[0]) {
            write_editor_ensure_line_cap(0, 1);
            if (write_editor_buffer[0]) write_editor_buffer[0][0] = '\0';
        }

        if (!write_editor_open_warn_active) {
            if (truncated_lines) {
                write_editor_open_warn_active = 1;
                snprintf(write_editor_open_warn_line1, sizeof(write_editor_open_warn_line1), "Warning: too many lines (>%d)", MAX_LINES);
                snprintf(write_editor_open_warn_line2, sizeof(write_editor_open_warn_line2), "Loaded first %d lines", MAX_LINES);
            } else if (any_line_truncated) {
                write_editor_open_warn_active = 1;
                snprintf(write_editor_open_warn_line1, sizeof(write_editor_open_warn_line1), "Warning: long lines truncated");
                snprintf(write_editor_open_warn_line2, sizeof(write_editor_open_warn_line2), "Max %d chars per line", MAX_LINE_LENGTH);
            }
        }
    } else {
        // Convert binary data into human-readable hex lines. Use 16 bytes per line for readability.
        write_editor_free_all_lines();
        write_editor_num_lines = 0;
        int line = 0;
        int bytes_per_line = 16;
        for (int i = 0; i < n && line < MAX_LINES; i += bytes_per_line) {
            int end = i + bytes_per_line; if (end > n) end = n;
            int pos = 0;
            char linebuf[MAX_LINE_LENGTH + 1];
            for (int j = i; j < end && pos < MAX_LINE_LENGTH - 3; ++j) {
                unsigned char b = (unsigned char)write_editor_read_buf[j];
                // write two hex chars and a space (except maybe last in line)
                char hi = "0123456789ABCDEF"[(b >> 4) & 0xF];
                char lo = "0123456789ABCDEF"[b & 0xF];
                if (pos + 3 < MAX_LINE_LENGTH) {
                    linebuf[pos++] = hi;
                    linebuf[pos++] = lo;
                    linebuf[pos++] = ' ';
                } else if (pos + 2 < MAX_LINE_LENGTH) {
                    linebuf[pos++] = hi;
                    linebuf[pos++] = lo;
                }
            }
            // trim trailing space
            if (pos > 0 && linebuf[pos - 1] == ' ') pos--;
            linebuf[pos] = '\0';
            write_editor_set_line_from_span(line, linebuf, pos, NULL);
            line++;
        }
        write_editor_num_lines = (line > 0) ? line : 1;
        if (!write_editor_buffer[0]) {
            write_editor_ensure_line_cap(0, 1);
            if (write_editor_buffer[0]) write_editor_buffer[0][0] = '\0';
        }
    }
    return 0;
}

// Save editor buffer to file
int save_write_editor_buffer(const char* path, uint8 disk) {
    if (!write_editor_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    // Calculate total size needed
    int total_size = 0;
    for (int i = 0; i < write_editor_num_lines; i++) {
        total_size += write_editor_line_len(i);
        if (i < write_editor_num_lines - 1) {
            total_size += 1; // For newline
        }
    }
    if (total_size > (int)WRITE_EDITOR_MAX_FILE_BYTES) return -1;
    
    // Helper: detect binary-like extensions we should save by converting from hex to raw bytes
    int is_binary_edit = 0;
    const char* ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".eyn") == 0 || strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".flat") == 0) is_binary_edit = 1;
    }

    if (!is_binary_edit) {
        char* data = write_editor_save_buf;
        int data_pos = 0;
        for (int i = 0; i < write_editor_num_lines; i++) {
            const char* s = write_editor_buffer[i] ? write_editor_buffer[i] : "";
            int line_len = write_editor_line_len(i);
            memcpy(&data[data_pos], s, (size_t)line_len);
            data_pos += line_len;
            if (i < write_editor_num_lines - 1) {
                data[data_pos++] = '\n';
            }
        }
        data[data_pos] = '\0';
        int written = vfs_write_file(disk, path, data, (uint32)data_pos);
        if (written < 0 || written != data_pos) return -1;
        return 0;
    } else {
        // Convert hex representation back into raw bytes.
        // Upper bound: at most half of the total character count will be hex digits.
        int max_bytes = (total_size / 2) + 16;
        if (max_bytes > (int)WRITE_EDITOR_MAX_FILE_BYTES) return -1;
        unsigned char* out = (unsigned char*)write_editor_save_buf;
        int out_pos = 0;
        for (int i = 0; i < write_editor_num_lines; ++i) {
            const char* s = write_editor_buffer[i];
            if (!s) s = "";
            int len = write_editor_line_len(i);
            int hi_nibble = -1;
            for (int j = 0; j < len; ++j) {
                char c = s[j];
                // simple ASCII whitespace check (space, tab, CR, LF, VT, FF)
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') continue;
                int v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
                else continue; // ignore non-hex chars
                if (hi_nibble < 0) { hi_nibble = v; }
                else {
                    int byte = (hi_nibble << 4) | v;
                    if (out_pos < max_bytes) out[out_pos++] = (unsigned char)byte;
                    hi_nibble = -1;
                }
            }
            // If there's a dangling hi nibble (odd count), ignore it.
        }
        int written = vfs_write_file(disk, path, (char*)out, (uint32)out_pos);
        if (written < 0 || written != out_pos) return -1;
        return 0;
    }
}

// Draw the editor screen
void write_editor_draw(const char* filename) {
    tui_clear();
    // Editor window
    int win_width = 78;
    tui_window_t editor_win = {0, 0, win_width, EDITOR_HEIGHT + 3, "", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};
    tui_draw_window(&editor_win);

    // Titlebar layout
    const char* title = "EYN-OS Write Editor";
    const char* mod_str = write_editor_modified ? "[Modified]" : "";
    int mod_len = write_editor_modified ? 10 : 0;
    int title_len = strlen(title);
    int file_len = strlen(filename);
    int left_space = win_width / 3;
    int right_space = win_width / 3;
    int center_start = (win_width - title_len) / 2;
    int mod_start = win_width - mod_len - 2;
    char filebuf[64];
    // Truncate filename if needed
    if (file_len > left_space - 2) {
        const char* ext = strchr(filename, '.');
        if (ext && ext - filename < file_len - 6) {
            int keep = left_space - 2 - strlen(ext) - 2; // room for '-.'
            if (keep > 0) {
                strncpy(filebuf, filename, keep);
                strcpy(filebuf + keep, "-." );
                strcpy(filebuf + keep + 2, ext + 1);
            } else {
                strncpy(filebuf, filename, left_space - 2);
                filebuf[left_space - 2] = '\0';
            }
        } else {
            strncpy(filebuf, filename, left_space - 2);
            filebuf[left_space - 2] = '\0';
        }
        filename = filebuf;
        file_len = strlen(filename);
    }
    // Draw title centered on title line
    tui_draw_text(editor_win.x + center_start, editor_win.y, title, editor_win.title_style);

    // Small status row directly under the title: filename left, [MODIFIED] right (red)
    tui_style_t file_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 1};
    tui_style_t mod_style = {TUI_COLOR_RED, TUI_COLOR_BLACK, 1};
    int status_y = editor_win.y + 1; // row under title
    tui_draw_text(editor_win.x + 2, status_y, filename, file_style);
    if (write_editor_modified) {
        int dyn_mod_len = strlen(mod_str);
        int dyn_mod_start = editor_win.x + editor_win.width - dyn_mod_len - 2;
        tui_draw_text(dyn_mod_start, status_y, mod_str, mod_style);
    }

    // Draw text area (lines)
    tui_style_t text_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
    tui_style_t cursor_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
    tui_style_t more_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
    int max_visible = EDITOR_HEIGHT;
    for (int i = 0; i < max_visible; i++) {
        int line_idx = i + write_editor_scroll_y;
        int y = editor_win.y + 2 + i;
        if (i == 0 && write_editor_scroll_y > 0) {
            tui_draw_text(editor_win.x + 1, y, "--- More above ---", more_style);
            continue;
        }
        if (i == max_visible - 1 && write_editor_scroll_y + max_visible < write_editor_num_lines) {
            tui_draw_text(editor_win.x + 1, y, "--- More below ---", text_style);
            continue;
        }
        if (line_idx < write_editor_num_lines) {
            int line_len = write_editor_line_len(line_idx);
            const char* s = write_editor_buffer[line_idx] ? write_editor_buffer[line_idx] : "";
            int x = editor_win.x + 1;
            // only draw characters that fit within the window width, accounting for horizontal scroll
            int max_chars = editor_win.width - 2; // account for borders
            int start_char = write_editor_scroll_x;
            int end_char = start_char + max_chars;
            
            for (int j = start_char; j <= line_len && (x - editor_win.x - 1) < max_chars; j++) {
                if (line_idx == write_editor_cursor_y && j == write_editor_cursor_x) {
                    tui_draw_text(x, y, "!", cursor_style);
                    x++;
                }
                if (j < line_len && (x - editor_win.x - 1) < max_chars) {
                    char ch[2] = {s[j], '\0'};
                    tui_draw_text(x, y, ch, text_style);
                    x++;
                }
            }
        }
    }

    // Status bar just below the window
    char status[160];
    snprintf(status, sizeof(status), "Line %d/%d, Col %d | V-Scroll: %d-%d | H-Scroll: %d | Ctrl+S: Save | Ctrl+Q: Exit | Ctrl+X: Cut", 
        write_editor_cursor_y + 1, write_editor_num_lines, write_editor_cursor_x + 1, 
        write_editor_scroll_y + 1, write_editor_scroll_y + EDITOR_HEIGHT, write_editor_scroll_x);
    tui_style_t status_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
    tui_draw_status_bar(&editor_win, status, status_style);

    // Colored cursor info on the next line
    char cursor_info[64];
    snprintf(cursor_info, sizeof(cursor_info), "Cursor at line %d, col %d", write_editor_cursor_y + 1, write_editor_cursor_x + 1);
    tui_style_t cursor_info_style = {TUI_COLOR_MAGENTA, TUI_COLOR_BLACK, 0};
    tui_draw_text(editor_win.x, editor_win.y + editor_win.height + 1, cursor_info, cursor_info_style);
}

// GUI draw callback (top-level)
static void write_editor_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    // Keep status text in sync with cursor/message state.
    (void)tile_idx;
    (void)userdata;
    write_editor_update_tile_modified();

    // Incremental clears: clear only the bands we're about to redraw
    int cell_w = vga_text_cell_w();
    int cell_h = vga_text_cell_h();
    if (cell_w <= 0) cell_w = 8;
    if (cell_h <= 0) cell_h = 8;
    if (content_w > 0 && content_h > 0) {
        // Text area bands
        int rows = content_h / cell_h;
        int text_rows = rows;
        for (int r = 0; r < text_rows; ++r) {
            int py = content_y + r * cell_h;
            if (py + cell_h <= content_y + content_h) drawRect(content_x, py, content_w, cell_h, 0, 0, 0);
        }
    }
    int cols = content_w / cell_w;
    int rows = content_h / cell_h;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    // cache geometry for potential key handling
    write_editor_gui_cols = cols;
    write_editor_gui_rows = rows;
    write_editor_last_cx = content_x;
    write_editor_last_cy = content_y;
    write_editor_last_cw = content_w;
    write_editor_last_ch = content_h;
    // Use the full tile content area for text (no permanent bottom status bar).
    int text_rows = rows;

    // Pop-out scrollbar: only active if content exceeds the visible rows.
    // We draw it as a thin overlay on the right edge.
    write_editor_scrollbar_active = 0;
    write_editor_scrollbar_x = write_editor_scrollbar_y = write_editor_scrollbar_w = write_editor_scrollbar_h = 0;
    write_editor_scrollbar_thumb_y = write_editor_scrollbar_thumb_h = 0;
    if (write_editor_num_lines > text_rows) {
        int track_w = 3;
        int track_h = text_rows * cell_h;
        int track_x = content_x + content_w - track_w;
        int track_y = content_y;
        if (track_w > 0 && track_h > 0 && track_x >= content_x) {
            write_editor_scrollbar_active = 1;
            write_editor_scrollbar_x = track_x;
            write_editor_scrollbar_y = track_y;
            write_editor_scrollbar_w = track_w;
            write_editor_scrollbar_h = track_h;

            // Track background
            drawRect(track_x, track_y, track_w, track_h, 24, 24, 24);

            // Thumb size and position (based on absolute line scroll)
            int min_thumb = cell_h; if (min_thumb < 6) min_thumb = 6;
            int thumb_h = (int)((uint32)track_h * (uint32)text_rows / (uint32)write_editor_num_lines);
            if (thumb_h < min_thumb) thumb_h = min_thumb;
            if (thumb_h > track_h) thumb_h = track_h;
            int denom = write_editor_num_lines - text_rows;
            if (denom < 1) denom = 1;
            int thumb_y = track_y;
            if (track_h > thumb_h) {
                thumb_y = track_y + (int)((uint32)(track_h - thumb_h) * (uint32)write_editor_scroll_y / (uint32)denom);
            }
            write_editor_scrollbar_thumb_y = thumb_y;
            write_editor_scrollbar_thumb_h = thumb_h;
            drawRect(track_x, thumb_y, track_w, thumb_h, 200, 200, 200);
        }
    }

    // Line-number gutter (character columns)
    int gutter_cols = write_editor_gui_gutter_cols();
    if (gutter_cols < 0) gutter_cols = 0;
    if (gutter_cols > cols - 1) gutter_cols = cols - 1;
    int text_cols = cols - gutter_cols;
    if (text_cols < 1) text_cols = 1;

    // Gutter background + separator
    if (gutter_cols > 0) {
        int gw = gutter_cols * cell_w;
        if (gw > 0) {
            drawRect(content_x, content_y, gw, text_rows * cell_h, 16, 16, 16);
            int sep_x = content_x + gw - 1;
            if (sep_x >= content_x && sep_x < content_x + content_w) {
                drawRect(sep_x, content_y, 1, content_h, 48, 48, 48);
            }
        }
    }

    // Render text with soft-wrapping: walk absolute lines starting at scroll_y
    int abs_line = write_editor_scroll_y;
    int seg = 0; // wrapped segment index within current abs_line
    for (int r = 0; r < text_rows; ++r) {
        int draw_y = content_y + r * cell_h;
        if (abs_line >= write_editor_num_lines) {
            // nothing to draw on this row
            continue;
        }
    const char* line = write_editor_buffer[abs_line];
    int len = strlength((char*)line); // cast to silence discarded qualifier warning
        int last_ns = len;
        while (last_ns > 0) {
            char t = line[last_ns - 1];
            if (t == ' ' || t == '\t') last_ns--;
            else break;
        }
        int wraps = (len + text_cols - 1) / text_cols; if (wraps < 1) wraps = 1;
        // If we've exhausted wrapped segments for this line, advance to next line and retry this visual row
        if (seg >= wraps) {
            abs_line++;
            seg = 0;
            r--; // redo this visual row with the next absolute line
            continue;
        }
        int start_col = seg * text_cols;

        // Draw gutter (only show the line number on the first wrapped segment)
        if (gutter_cols > 0) {
            int gx = content_x;
            // Build a right-aligned line number into a fixed-width field.
            // We draw per-char to avoid relying on drawTextAt's fixed 8px advance.
            char gbuf[16];
            for (int i = 0; i < gutter_cols && i < (int)sizeof(gbuf) - 1; ++i) gbuf[i] = ' ';
            int gmax = gutter_cols < (int)sizeof(gbuf) - 1 ? gutter_cols : (int)sizeof(gbuf) - 1;
            gbuf[gmax] = '\0';
            if (seg == 0) {
                int num = abs_line + 1;
                char nb[12];
                snprintf(nb, sizeof(nb), "%d", num);
                int nlen = (int)strlen(nb);
                // place digits ending at gutter_cols-2; leave gutter_cols-1 as spacer
                int start = (gmax - 1) - nlen;
                if (start < 0) start = 0;
                for (int i = 0; i < nlen && (start + i) < (gmax - 1); ++i) gbuf[start + i] = nb[i];
            }
            int gr = (abs_line == write_editor_cursor_y) ? 220 : 160;
            int gg = (abs_line == write_editor_cursor_y) ? 220 : 160;
            int gb = (abs_line == write_editor_cursor_y) ? 220 : 160;
            for (int i = 0; i < gutter_cols; ++i) {
                drawCharAt(gx + i * cell_w, draw_y, (int)(unsigned char)gbuf[i], gr, gg, gb);
            }
        }

        for (int cc = 0; cc < text_cols; ++cc) {
            int src_idx = start_col + cc;
            char ch = ' ';
            if (src_idx < len) ch = line[src_idx];
            char draw_ch = ch;
            int rr = 255, gg = 255, bb = 255;
            if (write_editor_show_whitespace) {
                if (ch == '\t') {
                    draw_ch = '>';
                    rr = 200; gg = 200; bb = 200;
                } else if (ch == ' ' && src_idx >= last_ns) {
                    draw_ch = '.';
                    rr = 200; gg = 200; bb = 200;
                }
            }
            // Selection background per-cell (simple solid rect)
            int is_sel = 0;
            if (write_editor_sel_active) {
                // Normalize selection endpoints
                int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
                int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
                if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
                if (abs_line > sel_sy && abs_line < sel_fy) is_sel = 1; // fully inside selected lines
                else if (abs_line == sel_sy && abs_line == sel_fy) {
                    if (src_idx >= sel_sx && src_idx < sel_fx) is_sel = 1;
                } else if (abs_line == sel_sy) {
                    if (src_idx >= sel_sx) is_sel = 1;
                } else if (abs_line == sel_fy) {
                    if (src_idx < sel_fx) is_sel = 1;
                }
            }
            // Cursor handling: draw an underscore '_' at the cursor position (overlay, no displacement)
            if (abs_line == write_editor_cursor_y) {
                int cur_wrap = write_editor_cursor_x / text_cols;
                int cur_col = write_editor_cursor_x % text_cols;
                if (seg == cur_wrap && cc == cur_col) {
                    // we'll overlay '_' after drawing the underlying character below
                }
            }
            int px = content_x + (gutter_cols + cc) * cell_w;
            if (px >= content_x && px + (cell_w - 1) < content_x + content_w) {
                if (is_sel) drawRect(px, draw_y, cell_w, cell_h, 0, 128, 128);
                drawCharAt(px, draw_y, (int)(unsigned char)draw_ch, rr, gg, bb);
                // Overlay underscore at caret cell
                if (abs_line == write_editor_cursor_y) {
                    int cur_wrap = write_editor_cursor_x / text_cols;
                    int cur_col = write_editor_cursor_x % text_cols;
                    if (seg == cur_wrap && cc == cur_col) {
                        drawCharAt(px, draw_y, (int)'_', 220, 220, 220);
                    }
                }
            }
        }
        seg++;
    }

    // Unsaved-changes prompt overlay (draw last so it appears on top)
    if (write_editor_exit_prompt_active) {
        int box_w = 320;
        int box_h = 64;
        if (box_w > content_w - 16) box_w = content_w - 16;
        if (box_h > content_h - 16) box_h = content_h - 16;
        if (box_w < 160) box_w = 160;
        if (box_h < 48) box_h = 48;
        int bx = content_x + (content_w - box_w) / 2;
        int by = content_y + (content_h - box_h) / 2;

        drawRect(bx, by, box_w, box_h, 0, 0, 0);
        // Border
        drawRect(bx, by, box_w, 1, 255, 255, 255);
        drawRect(bx, by + box_h - 1, box_w, 1, 255, 255, 255);
        drawRect(bx, by, 1, box_h, 255, 255, 255);
        drawRect(bx + box_w - 1, by, 1, box_h, 255, 255, 255);

        drawTextAt(bx + 8, by + 10, "Unsaved changes", 255, 255, 255);
        drawTextAt(bx + 8, by + 26, "Save (S) / Exit (X)", 200, 200, 200);
        drawTextAt(bx + 8, by + 40, "Cancel (Esc)", 200, 200, 200);
    }

    // Large-file warning overlay (draw on top of everything)
    if (write_editor_open_warn_active) {
        int box_w = 360;
        int box_h = 72;
        if (box_w > content_w - 16) box_w = content_w - 16;
        if (box_h > content_h - 16) box_h = content_h - 16;
        if (box_w < 200) box_w = 200;
        if (box_h < 56) box_h = 56;
        int bx = content_x + (content_w - box_w) / 2;
        int by = content_y + (content_h - box_h) / 2;

        drawRect(bx, by, box_w, box_h, 0, 0, 0);
        drawRect(bx, by, box_w, 1, 255, 255, 255);
        drawRect(bx, by + box_h - 1, box_w, 1, 255, 255, 255);
        drawRect(bx, by, 1, box_h, 255, 255, 255);
        drawRect(bx + box_w - 1, by, 1, box_h, 255, 255, 255);

        drawTextAt(bx + 8, by + 10, "Large file", 255, 255, 255);
        if (write_editor_open_warn_line1[0]) drawTextAt(bx + 8, by + 26, write_editor_open_warn_line1, 200, 200, 200);
        if (write_editor_open_warn_line2[0]) drawTextAt(bx + 8, by + 40, write_editor_open_warn_line2, 200, 200, 200);
        drawTextAt(bx + 8, by + 56, "Enter: continue", 200, 200, 200);
    }

    // Find / Goto overlays (draw last so they're always visible)
    if (write_editor_modal != WRITE_EDITOR_MODAL_NONE) {
        int box_w = 360;
        int box_h = 64;
        if (box_w > content_w - 16) box_w = content_w - 16;
        if (box_h > content_h - 16) box_h = content_h - 16;
        if (box_w < 200) box_w = 200;
        if (box_h < 48) box_h = 48;
        int bx = content_x + (content_w - box_w) / 2;
        int by = content_y + (content_h - box_h) / 2;

        drawRect(bx, by, box_w, box_h, 0, 0, 0);
        drawRect(bx, by, box_w, 1, 255, 255, 255);
        drawRect(bx, by + box_h - 1, box_w, 1, 255, 255, 255);
        drawRect(bx, by, 1, box_h, 255, 255, 255);
        drawRect(bx + box_w - 1, by, 1, box_h, 255, 255, 255);

        if (write_editor_modal == WRITE_EDITOR_MODAL_FIND) {
            drawTextAt(bx + 8, by + 10, "Find", 255, 255, 255);
            drawTextAt(bx + 8, by + 26, write_editor_find_query, 200, 200, 200);
            drawTextAt(bx + 8, by + 40, "Enter: search  Esc: cancel", 200, 200, 200);
        } else if (write_editor_modal == WRITE_EDITOR_MODAL_GOTO) {
            drawTextAt(bx + 8, by + 10, "Go to line", 255, 255, 255);
            drawTextAt(bx + 8, by + 26, write_editor_goto_buf, 200, 200, 200);
            drawTextAt(bx + 8, by + 40, "Enter: go  Esc: cancel", 200, 200, 200);
        }
    }
}

// GUI key callback (top-level)
static void write_editor_gui_key(int tile_idx, int key, void* userdata) {
    (void)tile_idx; (void)userdata;

    int visible_rows = write_editor_gui_visible_text_rows();

    // Modal overlays: find / goto consume input until dismissed
    if (write_editor_modal != WRITE_EDITOR_MODAL_NONE) {
        if (key == 27) {
            write_editor_modal = WRITE_EDITOR_MODAL_NONE;
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        if (write_editor_modal == WRITE_EDITOR_MODAL_FIND) {
            if (key == '\b') {
                if (write_editor_find_len > 0) {
                    write_editor_find_query[--write_editor_find_len] = '\0';
                    tile_invalidate_gui(write_editor_gui_tile);
                }
                return;
            }
            if (key == '\n') {
                if (write_editor_find_len <= 0) {
                    write_editor_set_status("Find: empty");
                    write_editor_modal = WRITE_EDITOR_MODAL_NONE;
                    tile_invalidate_gui(write_editor_gui_tile);
                    return;
                }
                // Search forward from the current cursor
                int start_y = write_editor_cursor_y;
                int start_x = write_editor_cursor_x;
                int found = 0;
                for (int y = start_y; y < write_editor_num_lines && !found; ++y) {
                    const char* s = write_editor_buffer[y] ? write_editor_buffer[y] : "";
                    int len = write_editor_line_len(y);
                    int from = (y == start_y) ? start_x : 0;
                    if (from < 0) from = 0;
                    if (from > len) from = len;
                    for (int x = from; x + write_editor_find_len <= len; ++x) {
                        int match = 1;
                        for (int k = 0; k < write_editor_find_len; ++k) {
                            if (s[x + k] != write_editor_find_query[k]) { match = 0; break; }
                        }
                        if (match) {
                            write_editor_cursor_y = y;
                            write_editor_cursor_x = x;
                            write_editor_sel_active = 1;
                            write_editor_sel_ay = y; write_editor_sel_ax = x;
                            write_editor_sel_fy = y; write_editor_sel_fx = x + write_editor_find_len;
                            if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                            if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
                            write_editor_set_status("Found");
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) write_editor_set_status("Not found");
                write_editor_modal = WRITE_EDITOR_MODAL_NONE;
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
            if (key >= 32 && key <= 126) {
                if (write_editor_find_len < (int)sizeof(write_editor_find_query) - 1) {
                    write_editor_find_query[write_editor_find_len++] = (char)key;
                    write_editor_find_query[write_editor_find_len] = '\0';
                    tile_invalidate_gui(write_editor_gui_tile);
                }
                return;
            }
            return;
        }
        if (write_editor_modal == WRITE_EDITOR_MODAL_GOTO) {
            if (key == '\b') {
                if (write_editor_goto_len > 0) {
                    write_editor_goto_buf[--write_editor_goto_len] = '\0';
                    tile_invalidate_gui(write_editor_gui_tile);
                }
                return;
            }
            if (key == '\n') {
                int v = 0;
                for (int i = 0; i < write_editor_goto_len; ++i) {
                    char c = write_editor_goto_buf[i];
                    if (c < '0' || c > '9') { v = -1; break; }
                    v = v * 10 + (c - '0');
                    if (v > 1000000) { v = -1; break; }
                }
                if (v < 1) {
                    write_editor_set_status("Goto: invalid");
                } else {
                    if (v > write_editor_num_lines) v = write_editor_num_lines;
                    write_editor_cursor_y = v - 1;
                    write_editor_cursor_x = 0;
                    write_editor_sel_active = 0;
                    if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                    if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
                    write_editor_set_status("Goto ok");
                }
                write_editor_modal = WRITE_EDITOR_MODAL_NONE;
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
            if (key >= '0' && key <= '9') {
                if (write_editor_goto_len < (int)sizeof(write_editor_goto_buf) - 1) {
                    write_editor_goto_buf[write_editor_goto_len++] = (char)key;
                    write_editor_goto_buf[write_editor_goto_len] = '\0';
                    tile_invalidate_gui(write_editor_gui_tile);
                }
                return;
            }
            return;
        }
    }

    // Large-file warning: block input until acknowledged.
    if (write_editor_open_warn_active) {
        if (key == '\n' || key == 27) { // Enter or Esc
            write_editor_open_warn_active = 0;
            tile_invalidate_gui(write_editor_gui_tile);
        }
        return;
    }

    // If the unsaved-changes prompt is active, consume keys to avoid editing behind it.
    if (write_editor_exit_prompt_active) {
        if (key == 27) { // Esc cancels
            write_editor_exit_prompt_active = 0;
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        if (key == 0x2001 || key == 's' || key == 'S' || key == '\n') {
            write_editor_last_save_failed = 0;
            if (save_write_editor_buffer(write_editor_gui_filename, write_editor_gui_disk) == 0) {
                write_editor_modified = 0;
                write_editor_update_tile_modified();
                write_editor_exit_prompt_active = 0;
                if (write_editor_gui_tile >= 0) tile_unregister_gui_client(write_editor_gui_tile);
            } else {
                write_editor_last_save_failed = 1;
            }
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        if (key == 'x' || key == 'X') {
            write_editor_exit_prompt_active = 0;
            if (write_editor_gui_tile >= 0) tile_unregister_gui_client(write_editor_gui_tile);
            return;
        }
        return;
    }
    // Use global helper to delete active selection when needed
    // Shift+Arrows selection extension
    if ((key & 0x3000) && ((key & 0x0FFF) >= 0x1001 && (key & 0x0FFF) <= 0x1004)) {
        int base = key & 0x0FFF;
    if (!write_editor_sel_active) { write_editor_sel_active = 1; write_editor_sel_ay = write_editor_cursor_y; write_editor_sel_ax = write_editor_cursor_x; }
        if (base == 0x1001) { // Up
            if (write_editor_cursor_y > 0) {
                write_editor_cursor_y--;
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                int line_len = write_editor_line_len(write_editor_cursor_y);
                if (write_editor_cursor_x > line_len) write_editor_cursor_x = line_len;
                write_editor_scroll_x = 0;
            }
        } else if (base == 0x1002) { // Down
            if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
                int line_len = write_editor_line_len(write_editor_cursor_y);
                if (write_editor_cursor_x > line_len) write_editor_cursor_x = line_len;
                write_editor_scroll_x = 0;
            }
        } else if (base == 0x1003) { // Left
            if (write_editor_cursor_x > 0) {
                write_editor_cursor_x--;
                if (write_editor_cursor_x < write_editor_scroll_x) write_editor_scroll_x = write_editor_cursor_x;
            } else if (write_editor_cursor_y > 0) {
                write_editor_cursor_y--;
                write_editor_cursor_x = write_editor_line_len(write_editor_cursor_y);
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
            }
        } else if (base == 0x1004) { // Right
            if (write_editor_cursor_x < write_editor_line_len(write_editor_cursor_y)) {
                write_editor_cursor_x++;
                int visible_cols = (write_editor_gui_cols > 0) ? write_editor_gui_cols : 76;
                if (write_editor_cursor_x > write_editor_scroll_x + visible_cols - 1) write_editor_scroll_x = write_editor_cursor_x - (visible_cols - 1);
            } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                write_editor_cursor_x = 0;
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
            }
        }
        write_editor_sel_fy = write_editor_cursor_y; write_editor_sel_fx = write_editor_cursor_x;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Ctrl+A select all
    if (key == 0x2104) {
        write_editor_sel_active = 1;
        write_editor_sel_ay = 0; write_editor_sel_ax = 0;
        write_editor_sel_fy = write_editor_num_lines - 1;
        write_editor_sel_fx = write_editor_line_len(write_editor_sel_fy);
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Ctrl+L select current line
    if (key == 0x2105) {
        write_editor_sel_active = 1;
        write_editor_sel_ay = write_editor_cursor_y; write_editor_sel_ax = 0;
        write_editor_sel_fy = write_editor_cursor_y;
        write_editor_sel_fx = write_editor_line_len(write_editor_cursor_y);
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+F find
    if (key == 0x2107) {
        write_editor_modal = WRITE_EDITOR_MODAL_FIND;
        // keep existing query; caret at end
        write_editor_find_len = (int)strlen(write_editor_find_query);
        if (write_editor_find_len >= (int)sizeof(write_editor_find_query)) write_editor_find_len = (int)sizeof(write_editor_find_query) - 1;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+G goto line
    if (key == 0x2108) {
        write_editor_modal = WRITE_EDITOR_MODAL_GOTO;
        write_editor_goto_len = 0;
        write_editor_goto_buf[0] = '\0';
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+X cut (selection only)
    if (key == 0x210B) {
        write_editor_clear_status();
        if (!write_editor_sel_active) {
            write_editor_set_status("Cut: no selection");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }

        // Copy selection to clipboard
        write_editor_copy_selection_to_clipboard();

        // Record selection deletion for undo when bounded
        char before[WRITE_EDITOR_UNDO_TEXT_MAX];
        int before_len = 0;
        int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
        int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
        if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
        int ok = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
        if (write_editor_delete_active_selection()) {
            if (ok) {
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)sel_sy;
                rec.x = (uint16)sel_sx;
                rec.before_len = (uint16)before_len;
                memcpy(rec.before, before, (size_t)before_len);
                rec.after_len = 0;
                (void)write_editor_undo_push(rec);
            } else {
                write_editor_set_status("Undo disabled (selection too big)");
                write_editor_undo_clear();
            }
        }
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+C copy (selection only)
    if (key == 0x2206) {
        write_editor_clear_status();
        if (!write_editor_sel_active) {
            write_editor_set_status("Copy: no selection");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        write_editor_copy_selection_to_clipboard();
        write_editor_set_status("Copied");
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+V paste
    if (key == 0x2207) {
        write_editor_clear_status();
        if (write_editor_clipboard_len <= 0) {
            write_editor_set_status("Paste: empty");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }

        int ins_y = write_editor_cursor_y;
        int ins_x = write_editor_cursor_x;

        int replaced_selection = 0;

        char before[WRITE_EDITOR_UNDO_TEXT_MAX];
        int before_len = 0;
        int ok_before = 0;

        if (write_editor_sel_active) {
            replaced_selection = 1;
            int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
            int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
            if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
            ins_y = sel_sy;
            ins_x = sel_sx;
            ok_before = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
            if (!write_editor_delete_active_selection()) {
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
        }

        int end_y = ins_y;
        int end_x = ins_x;
        if (write_editor_insert_text_at(ins_y, ins_x, write_editor_clipboard, write_editor_clipboard_len, &end_y, &end_x) != 0) {
            write_editor_set_status("Paste failed");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }

        // Cursor moves to end of pasted text
        write_editor_cursor_y = end_y;
        write_editor_cursor_x = end_x;
        write_editor_sel_active = 0;

        // Record undo when bounded
        if ((write_editor_clipboard_len <= WRITE_EDITOR_UNDO_TEXT_MAX) && (!replaced_selection || ok_before)) {
            write_editor_undo_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.y = (uint16)ins_y;
            rec.x = (uint16)ins_x;
            rec.before_len = (uint16)(ok_before ? before_len : 0);
            if (ok_before && before_len > 0) memcpy(rec.before, before, (size_t)before_len);
            rec.after_len = (uint16)write_editor_clipboard_len;
            memcpy(rec.after, write_editor_clipboard, (size_t)write_editor_clipboard_len);
            (void)write_editor_undo_push(rec);
        } else {
            write_editor_set_status("Undo disabled (paste too big)");
            write_editor_undo_clear();
        }

        write_editor_modified = 1;
        write_editor_update_tile_modified();
        if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
        if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+Z undo
    if (key == 0x2109) {
        if (write_editor_undo_len <= 0) {
            write_editor_set_status("Undo: empty");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        write_editor_undo_t rec = write_editor_undo_stack[--write_editor_undo_len];
        write_editor_apply_undo_record(&rec, 0);
        (void)write_editor_redo_push(rec);
        // keep cursor visible
        if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
        if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Ctrl+Y redo
    if (key == 0x210A) {
        if (write_editor_redo_len <= 0) {
            write_editor_set_status("Redo: empty");
            tile_invalidate_gui(write_editor_gui_tile);
            return;
        }
        write_editor_undo_t rec = write_editor_redo_stack[--write_editor_redo_len];
        write_editor_apply_undo_record(&rec, 1);
        (void)write_editor_undo_push(rec);
        if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
        if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Arrow keys (no shift) clear selection
    if (key == 0x1001) { // Up
        if (write_editor_cursor_y > 0) {
            write_editor_cursor_y--;
            if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
            int line_len = write_editor_line_len(write_editor_cursor_y);
            if (write_editor_cursor_x > line_len) write_editor_cursor_x = line_len;
            write_editor_scroll_x = 0;
            write_editor_sel_active = 0;
        }
        return;
    }
    if (key == 0x1002) { // Down
        if (write_editor_cursor_y < write_editor_num_lines - 1) {
            write_editor_cursor_y++;
            if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
            int line_len = write_editor_line_len(write_editor_cursor_y);
            if (write_editor_cursor_x > line_len) write_editor_cursor_x = line_len;
            write_editor_scroll_x = 0;
            write_editor_sel_active = 0;
        }
        return;
    }
    if (key == 0x1003) { // Left
        if (write_editor_cursor_x > 0) {
            write_editor_cursor_x--;
            if (write_editor_cursor_x < write_editor_scroll_x) write_editor_scroll_x = write_editor_cursor_x;
        } else if (write_editor_cursor_y > 0) {
            write_editor_cursor_y--;
            write_editor_cursor_x = write_editor_line_len(write_editor_cursor_y);
            write_editor_scroll_x = 0;
            if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
        }
        write_editor_sel_active = 0;
        return;
    }
    if (key == 0x1004) { // Right
        if (write_editor_cursor_x < write_editor_line_len(write_editor_cursor_y)) {
            write_editor_cursor_x++;
            int visible_cols = (write_editor_gui_cols > 0) ? write_editor_gui_cols : 76;
            if (write_editor_cursor_x > write_editor_scroll_x + visible_cols - 1) write_editor_scroll_x = write_editor_cursor_x - (visible_cols - 1);
        } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
            write_editor_cursor_y++;
            write_editor_cursor_x = 0;
            write_editor_scroll_x = 0;
            if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
        }
        write_editor_sel_active = 0;
        return;
    }
    if (key == '\b') { // Backspace
        write_editor_clear_status();
        if (write_editor_sel_active) {
            // If selection covers entire buffer, clear all quickly
            int all = 0;
            if (write_editor_sel_active) {
                int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
                int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
                if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
                if (sel_sy == 0 && sel_sx == 0 && sel_fy == write_editor_num_lines - 1 && sel_fx == write_editor_line_len(sel_fy)) all = 1;
            }
            if (all) {
                write_editor_free_all_lines();
                write_editor_num_lines = 1;
                write_editor_ensure_line_cap(0, 1);
                if (write_editor_buffer[0]) write_editor_buffer[0][0] = '\0';
                write_editor_cursor_x = 0; write_editor_cursor_y = 0;
                write_editor_scroll_x = 0; write_editor_scroll_y = 0; write_editor_sel_active = 0;
                write_editor_modified = 1; write_editor_update_tile_modified();
                write_editor_undo_clear();
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
            // Record selection deletion for undo (bounded)
            char before[WRITE_EDITOR_UNDO_TEXT_MAX];
            int before_len = 0;
            int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
            int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
            if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
            int ok = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
            if (write_editor_delete_active_selection()) {
                if (ok) {
                    write_editor_undo_t rec;
                    memset(&rec, 0, sizeof(rec));
                    rec.y = (uint16)sel_sy;
                    rec.x = (uint16)sel_sx;
                    rec.before_len = (uint16)before_len;
                    memcpy(rec.before, before, (size_t)before_len);
                    rec.after_len = 0;
                    (void)write_editor_undo_push(rec);
                } else {
                    write_editor_set_status("Undo disabled (selection too big)");
                    write_editor_undo_clear();
                }
                return;
            }
        }
        if (write_editor_cursor_x > 0) {
            // Record single-char delete
            int y0 = write_editor_cursor_y;
            int x0 = write_editor_cursor_x - 1;
            char before[2] = {0, 0};
            const char* s = write_editor_buffer[y0] ? write_editor_buffer[y0] : "";
            if (x0 >= 0 && x0 < write_editor_line_len(y0)) before[0] = s[x0];
            char* line = write_editor_buffer[write_editor_cursor_y];
            if (!line) {
                write_editor_ensure_line_cap(write_editor_cursor_y, 1);
                line = write_editor_buffer[write_editor_cursor_y];
            }
            int line_len = write_editor_line_len(write_editor_cursor_y);
            if (line) memmove(&line[write_editor_cursor_x - 1], &line[write_editor_cursor_x], (size_t)((line_len - write_editor_cursor_x) + 1));
            write_editor_cursor_x--;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            if (before[0]) {
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)y0;
                rec.x = (uint16)x0;
                rec.before_len = 1;
                rec.before[0] = before[0];
                rec.after_len = 0;
                (void)write_editor_undo_push(rec);
            }
        } else if (write_editor_cursor_y > 0) {
            // Record newline delete (line join)
            int y0 = write_editor_cursor_y - 1;
            int x0 = write_editor_line_len(y0);
            int prev_len = write_editor_line_len(write_editor_cursor_y - 1);
            int curr_len = write_editor_line_len(write_editor_cursor_y);
            int can_copy = MAX_LINE_LENGTH - prev_len;
            int copy = (curr_len < can_copy) ? curr_len : can_copy;
            if (copy >= 0) {
                if (write_editor_ensure_line_cap(write_editor_cursor_y - 1, prev_len + copy + 1) == 0) {
                    char* prev = write_editor_buffer[write_editor_cursor_y - 1];
                    char* curr = write_editor_buffer[write_editor_cursor_y];
                    if (!prev) prev_len = 0;
                    if (prev && curr && copy > 0) memcpy(&prev[prev_len], curr, (size_t)copy);
                    if (prev) prev[prev_len + copy] = '\0';
                    write_editor_delete_line_at(write_editor_cursor_y);
                    write_editor_cursor_y--;
                    write_editor_cursor_x = prev_len;
                    write_editor_modified = 1;
                    write_editor_update_tile_modified();
                    if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                    if (write_editor_scroll_x > write_editor_cursor_x) write_editor_scroll_x = write_editor_cursor_x;

                    write_editor_undo_t rec;
                    memset(&rec, 0, sizeof(rec));
                    rec.y = (uint16)y0;
                    rec.x = (uint16)x0;
                    rec.before_len = 1;
                    rec.before[0] = '\n';
                    rec.after_len = 0;
                    (void)write_editor_undo_push(rec);
                }
            }
        }
        return;
    }
    if (key == '\n') { // Enter
        write_editor_clear_status();
        if (write_editor_sel_active) {
            // Record selection replace (selection -> "\n") when bounded
            char before[WRITE_EDITOR_UNDO_TEXT_MAX];
            int before_len = 0;
            int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
            int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
            if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
            int ok = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
            if (!write_editor_delete_active_selection()) return;
            // After deletion, cursor is at (sy,sx)
            if (ok) {
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)sel_sy;
                rec.x = (uint16)sel_sx;
                rec.before_len = (uint16)before_len;
                memcpy(rec.before, before, (size_t)before_len);
                rec.after_len = 1;
                rec.after[0] = '\n';
                (void)write_editor_undo_push(rec);
            } else {
                write_editor_set_status("Undo disabled (selection too big)");
                write_editor_undo_clear();
            }
        }
        if (write_editor_num_lines < MAX_LINES) {
            // Record newline insert (no selection)
            if (!write_editor_sel_active) {
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)write_editor_cursor_y;
                rec.x = (uint16)write_editor_cursor_x;
                rec.before_len = 0;
                rec.after_len = 1;
                rec.after[0] = '\n';
                (void)write_editor_undo_push(rec);
            }
            int y = write_editor_cursor_y;
            int len = write_editor_line_len(y);
            if (write_editor_cursor_x > len) write_editor_cursor_x = len;
            char* line = write_editor_buffer[y];
            if (!line) {
                write_editor_ensure_line_cap(y, 1);
                line = write_editor_buffer[y];
                len = write_editor_line_len(y);
                if (write_editor_cursor_x > len) write_editor_cursor_x = len;
            }
            const char* tail = line ? &line[write_editor_cursor_x] : "";
            int tail_len = len - write_editor_cursor_x;
            if (tail_len < 0) tail_len = 0;
            if (line) line[write_editor_cursor_x] = '\0';

            write_editor_shift_lines_down(y + 1);
            write_editor_set_line_from_span(y + 1, tail, tail_len, NULL);
            write_editor_cursor_y++;
            write_editor_cursor_x = 0;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
        }
        return;
    }
    if (key == 0x2001) { // save
        write_editor_last_save_failed = 0;
        if (save_write_editor_buffer(write_editor_gui_filename, write_editor_gui_disk) == 0) {
            write_editor_modified = 0;
            write_editor_last_save_failed = 0;
            write_editor_update_tile_modified();
            if (write_editor_gui_tile >= 0) {
                static char title_static[] = "Write Editor";
                static char left_buf[128];
                const char* b = get_basename(write_editor_gui_filename);
                strncpy(left_buf, b, sizeof(left_buf) - 1);
                left_buf[sizeof(left_buf) - 1] = '\0';
                tile_set_title_status(write_editor_gui_tile, title_static, left_buf, NULL);
            }
        } else {
            write_editor_last_save_failed = 1;
        }
        return;
    }
    if (key == 0x2101) { // exit (Ctrl+Q)
        // Tiler will attempt to close after this callback.
        // If modified, show prompt and veto close via the close callback.
        if (write_editor_modified) {
            write_editor_exit_prompt_active = 1;
            tile_invalidate_gui(write_editor_gui_tile);
        }
        return;
    }

    // Ctrl+W: toggle visible whitespace
    if (key == 0x2106) {
        write_editor_show_whitespace = !write_editor_show_whitespace;
        if (write_editor_gui_tile >= 0) tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Tab: insert spaces to next 4-column boundary
    if (key == '\t') {
        write_editor_clear_status();
        if (write_editor_sel_active) {
            // Record selection replace (selection -> spaces) if bounded
            char before[WRITE_EDITOR_UNDO_TEXT_MAX];
            int before_len = 0;
            int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
            int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
            if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
            int ok = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
            if (!write_editor_delete_active_selection()) { /* continue */ }
            // We'll push a combined record after we compute the space count below.
            // Store selection info in locals.
            if (!ok) {
                write_editor_set_status("Undo disabled (selection too big)");
                write_editor_undo_clear();
            }
        }
        int line_len = write_editor_line_len(write_editor_cursor_y);
        if (line_len < MAX_LINE_LENGTH) {
            int tab_stop = 4;
            int want = tab_stop - (write_editor_cursor_x % tab_stop);
            if (want < 1) want = tab_stop;
            int can = MAX_LINE_LENGTH - line_len;
            if (want > can) want = can;
            if (want > 0) {
                // Undo record: insertion of N spaces
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)write_editor_cursor_y;
                rec.x = (uint16)write_editor_cursor_x;
                rec.before_len = 0;
                rec.after_len = (uint16)want;
                if (want > WRITE_EDITOR_UNDO_TEXT_MAX) {
                    write_editor_set_status("Undo disabled (tab too big)");
                    write_editor_undo_clear();
                } else {
                    for (int i = 0; i < want; ++i) rec.after[i] = ' ';
                    (void)write_editor_undo_push(rec);
                }
                int new_len = line_len + want;
                if (write_editor_ensure_line_cap(write_editor_cursor_y, new_len + 1) != 0) return;
                char* line = write_editor_buffer[write_editor_cursor_y];
                if (!line) return;
                int tail_bytes = line_len - write_editor_cursor_x;
                if (tail_bytes >= 0) {
                    memmove(&line[write_editor_cursor_x + want],
                            &line[write_editor_cursor_x],
                            (size_t)(tail_bytes + 1));
                }
                for (int i = 0; i < want; ++i) line[write_editor_cursor_x + i] = ' ';
                write_editor_cursor_x += want;
                write_editor_modified = 1;
                write_editor_update_tile_modified();
                if (write_editor_gui_tile >= 0) tile_invalidate_gui(write_editor_gui_tile);
                write_editor_sel_active = 0;
            }
        }
        return;
    }
    // Printable insert
    if (key >= 32 && key <= 126) {
        write_editor_clear_status();
        if (write_editor_sel_active) {
            // Record selection replace (selection -> single char) if bounded
            char before[WRITE_EDITOR_UNDO_TEXT_MAX];
            int before_len = 0;
            int sel_sy = write_editor_sel_ay, sel_sx = write_editor_sel_ax;
            int sel_fy = write_editor_sel_fy, sel_fx = write_editor_sel_fx;
            if (sel_fy < sel_sy || (sel_fy == sel_sy && sel_fx < sel_sx)) { int ty = sel_sy; sel_sy = sel_fy; sel_fy = ty; int tx = sel_sx; sel_sx = sel_fx; sel_fx = tx; }
            int ok = (write_editor_copy_range_text(sel_sy, sel_sx, sel_fy, sel_fx, before, (int)sizeof(before) - 1, &before_len) == 0);
            if (!write_editor_delete_active_selection()) { /* fallthrough */ }
            if (ok) {
                write_editor_undo_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.y = (uint16)sel_sy;
                rec.x = (uint16)sel_sx;
                rec.before_len = (uint16)before_len;
                memcpy(rec.before, before, (size_t)before_len);
                rec.after_len = 1;
                rec.after[0] = (char)key;
                (void)write_editor_undo_push(rec);
            } else {
                write_editor_set_status("Undo disabled (selection too big)");
                write_editor_undo_clear();
            }
        } else {
            // Record single char insertion
            write_editor_undo_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.y = (uint16)write_editor_cursor_y;
            rec.x = (uint16)write_editor_cursor_x;
            rec.before_len = 0;
            rec.after_len = 1;
            rec.after[0] = (char)key;
            (void)write_editor_undo_push(rec);
        }
        int line_len = write_editor_line_len(write_editor_cursor_y);
        if (line_len < MAX_LINE_LENGTH) {
            /* Move the tail (including terminating NUL) one position to the right so we insert without
             * overwriting the previous character. Use memmove because the source and destination
             * overlap. The number of bytes to move is (line_len - cursor_x) + 1 to include '\0'. */
            int new_len = line_len + 1;
            if (write_editor_ensure_line_cap(write_editor_cursor_y, new_len + 1) != 0) return;
            char* line = write_editor_buffer[write_editor_cursor_y];
            if (!line) return;
            int tail_bytes = line_len - write_editor_cursor_x;
            if (tail_bytes >= 0) {
                memmove(&line[write_editor_cursor_x + 1],
                        &line[write_editor_cursor_x],
                        (size_t)(tail_bytes + 1));
            }
            line[write_editor_cursor_x] = (char)key;
            write_editor_cursor_x++;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            if (write_editor_gui_tile >= 0) tile_invalidate_gui(write_editor_gui_tile);
            if (write_editor_cursor_y >= write_editor_scroll_y + visible_rows) write_editor_scroll_y = write_editor_cursor_y - visible_rows + 1;
            write_editor_sel_active = 0;
        }
        return;
    }
    // Ignore other keys
}

// Helper: clamp int to range
static int clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

// Convert pixel position to absolute buffer position (line,col) accounting for scroll and wrapping
static void write_editor_hit_test(int px, int py, int* out_line, int* out_col) {
    int cell_w = vga_text_cell_w();
    int cell_h = vga_text_cell_h();
    if (cell_w <= 0) cell_w = 8;
    if (cell_h <= 0) cell_h = 8;
    int cols = (write_editor_last_cw > 0) ? (write_editor_last_cw / cell_w) : (write_editor_gui_cols > 0 ? write_editor_gui_cols : 80);
    int rows = (write_editor_last_ch > 0) ? (write_editor_last_ch / cell_h) : (write_editor_gui_rows > 0 ? write_editor_gui_rows : 25);
    int text_rows = rows > 1 ? rows - 1 : rows; // bottom row is status

    int gutter_cols = write_editor_gui_gutter_cols();
    if (gutter_cols > cols - 1) gutter_cols = cols - 1;
    int text_cols = cols - gutter_cols;
    if (text_cols < 1) text_cols = 1;

    int rel_x_cells = clampi(px - write_editor_last_cx, 0, write_editor_last_cw - 1) / cell_w;
    int rel_x = rel_x_cells - gutter_cols;
    if (rel_x < 0) rel_x = 0;
    int rel_y = clampi(py - write_editor_last_cy, 0, (text_rows * cell_h) - 1) / cell_h;
    int abs_line = write_editor_scroll_y;
    int seg = 0;
    for (int r = 0; r < rel_y; ++r) {
        if (abs_line >= write_editor_num_lines) break;
        int len = write_editor_line_len(abs_line);
        int wraps = (len + text_cols - 1) / text_cols; if (wraps < 1) wraps = 1;
        seg++;
        if (seg >= wraps) { abs_line++; seg = 0; }
    }
    if (abs_line >= write_editor_num_lines) abs_line = write_editor_num_lines - 1;
    int len = write_editor_line_len(abs_line);
    int col = seg * text_cols + rel_x;
    if (col > len) col = len;
    if (out_line) *out_line = abs_line;
    if (out_col) *out_col = col;
}

// Mouse callback: wheel scroll, left-click/drag select, right-click copy, middle-click paste
static void write_editor_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata) {
    (void)tile_idx; (void)userdata;
    if (write_editor_gui_tile < 0) return;
    if (write_editor_open_warn_active) return;
    if (write_editor_exit_prompt_active) return;

    // Scrollbar click-to-jump (left button press in scrollbar track)
    if (write_editor_scrollbar_active && write_editor_scrollbar_w > 0 && write_editor_scrollbar_h > 0) {
        uint8 lb = me->buttons & MOUSE_BUTTON_LEFT;
        uint8 changed = me->button_changes;
        if ((changed & MOUSE_BUTTON_LEFT) && lb) {
            if (me->x >= write_editor_scrollbar_x && me->x < write_editor_scrollbar_x + write_editor_scrollbar_w &&
                me->y >= write_editor_scrollbar_y && me->y < write_editor_scrollbar_y + write_editor_scrollbar_h) {
                int rows = write_editor_gui_visible_text_rows();
                int denom = write_editor_num_lines - rows;
                if (denom < 1) denom = 1;
                int rel = me->y - write_editor_scrollbar_y;
                if (rel < 0) rel = 0;
                if (rel > write_editor_scrollbar_h - 1) rel = write_editor_scrollbar_h - 1;
                int max_move = write_editor_scrollbar_h - write_editor_scrollbar_thumb_h;
                int thumb_y = rel - (write_editor_scrollbar_thumb_h / 2);
                if (thumb_y < 0) thumb_y = 0;
                if (thumb_y > max_move) thumb_y = max_move;
                int new_scroll = 0;
                if (max_move > 0) {
                    new_scroll = (int)((uint32)thumb_y * (uint32)denom / (uint32)max_move);
                }
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > denom) new_scroll = denom;
                write_editor_scroll_y = new_scroll;
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
        }
    }
    // Wheel scroll vertical
    if (me->wheel_delta != 0) {
        int delta = me->wheel_delta; // up positive
        int new_scroll = write_editor_scroll_y + (delta < 0 ? -1 : 1);
        if (new_scroll < 0) new_scroll = 0;
        int max_scroll = write_editor_num_lines - 1;
        if (max_scroll < 0) max_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        if (new_scroll != write_editor_scroll_y) write_editor_scroll_y = new_scroll;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Hit-test to buffer coords
    int line = 0, col = 0;
    write_editor_hit_test(me->x, me->y, &line, &col);
    // Buttons
    uint8 lb = me->buttons & MOUSE_BUTTON_LEFT;
    uint8 rb = me->buttons & MOUSE_BUTTON_RIGHT;
    uint8 mb = me->buttons & MOUSE_BUTTON_MIDDLE;
    uint8 changed = me->button_changes;

    // Left button press: start selection and move cursor
    if ((changed & MOUSE_BUTTON_LEFT) && lb) {
        write_editor_cursor_y = line;
        write_editor_cursor_x = col;
        write_editor_sel_active = 1;
        write_editor_sel_ay = line; write_editor_sel_ax = col;
        write_editor_sel_fy = line; write_editor_sel_fx = col;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Left button release: keep selection but stop extending
    if ((changed & MOUSE_BUTTON_LEFT) && !lb) {
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Drag with left held: extend selection and update cursor
    if (lb && (me->delta_x != 0 || me->delta_y != 0)) {
        write_editor_cursor_y = line;
        write_editor_cursor_x = col;
        if (!write_editor_sel_active) { write_editor_sel_active = 1; write_editor_sel_ay = line; write_editor_sel_ax = col; }
        write_editor_sel_fy = line; write_editor_sel_fx = col;
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }

    // Right-click: copy selection to clipboard
    if ((changed & MOUSE_BUTTON_RIGHT) && rb) {
        write_editor_copy_selection_to_clipboard();
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Middle-click: paste clipboard at cursor
    if ((changed & MOUSE_BUTTON_MIDDLE) && mb) {
        if (write_editor_clipboard_len > 0) {
            for (int i = 0; i < write_editor_clipboard_len; ++i) {
                char ch = write_editor_clipboard[i];
                if (ch == '\n') {
                    // simulate Enter
                    if (write_editor_num_lines < MAX_LINES) {
                        int y = write_editor_cursor_y;
                        int len = write_editor_line_len(y);
                        if (write_editor_cursor_x > len) write_editor_cursor_x = len;
                        char* line_ptr = write_editor_buffer[y];
                        if (!line_ptr) {
                            write_editor_ensure_line_cap(y, 1);
                            line_ptr = write_editor_buffer[y];
                            len = write_editor_line_len(y);
                            if (write_editor_cursor_x > len) write_editor_cursor_x = len;
                        }
                        const char* tail = line_ptr ? &line_ptr[write_editor_cursor_x] : "";
                        int tail_len = len - write_editor_cursor_x;
                        if (tail_len < 0) tail_len = 0;
                        if (line_ptr) line_ptr[write_editor_cursor_x] = '\0';
                        write_editor_shift_lines_down(y + 1);
                        write_editor_set_line_from_span(y + 1, tail, tail_len, NULL);
                        write_editor_cursor_y++;
                        write_editor_cursor_x = 0;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    int line_len = write_editor_line_len(write_editor_cursor_y);
                    if (line_len < MAX_LINE_LENGTH) {
                        int new_len = line_len + 1;
                        if (write_editor_ensure_line_cap(write_editor_cursor_y, new_len + 1) != 0) continue;
                        char* line_ptr = write_editor_buffer[write_editor_cursor_y];
                        if (!line_ptr) continue;
                        int tail_bytes = line_len - write_editor_cursor_x;
                        if (tail_bytes >= 0) {
                            memmove(&line_ptr[write_editor_cursor_x + 1],
                                &line_ptr[write_editor_cursor_x],
                                (size_t)(tail_bytes + 1));
                        }
                        line_ptr[write_editor_cursor_x] = ch;
                        write_editor_cursor_x++;
                    }
                }
            }
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            tile_invalidate_gui(write_editor_gui_tile);
        }
        return;
    }
}

// Called by the tiler when a close is requested (close button, Super+Q, default Ctrl+X close).
// Return 1 to allow close, 0 to veto.
static int write_editor_gui_close_request(int tile_idx, void* userdata) {
    (void)tile_idx;
    (void)userdata;
    if (write_editor_exit_prompt_active) return 0;
    if (!write_editor_modified) return 1;
    write_editor_exit_prompt_active = 1;
    if (write_editor_gui_tile >= 0) tile_invalidate_gui(write_editor_gui_tile);
    return 0;
}

// Main editor loop
void write_editor(const char* filename, uint8 disk) {
    // Reset editor state for new file
    write_editor_cursor_x = 0;
    write_editor_cursor_y = 0;
    write_editor_scroll_y = 0;
    write_editor_scroll_x = 0;
    write_editor_modified = 0;
    write_editor_show_whitespace = 0;
    write_editor_reset_ephemeral_state();
    if (load_file_to_write_editor(filename, disk) < 0) {
        printf("%cFailed to load file.\n", 255, 0, 0);
        return;
    }
    // If tiling manager is active, register as a GUI client for focused tile
    if (tile_is_tiling_active()) {
        int t = tile_get_focused();
        // Build title and status strings
        static char title_buf[128];
        static char status_buf[128];
        snprintf(title_buf, sizeof(title_buf), "%s - Write Editor", get_basename(filename));
    if (write_editor_modified) snprintf(status_buf, sizeof(status_buf), "[Modified]"); else status_buf[0] = '\0';
    // place filename on the left status and [Modified] on the right when needed
    static char left_buf[128];
    const char* base = get_basename(filename);
    strncpy(left_buf, base, sizeof(left_buf) - 1);
    left_buf[sizeof(left_buf) - 1] = '\0';
    tile_set_title_status(t, title_buf, left_buf, status_buf[0] ? status_buf : NULL);

        // register top-level GUI callbacks and set GUI state
        write_editor_gui_tile = t;
        strncpy(write_editor_gui_filename, filename, sizeof(write_editor_gui_filename) - 1);
        write_editor_gui_filename[sizeof(write_editor_gui_filename) - 1] = '\0';
        write_editor_gui_disk = disk;
        tile_register_gui_client2(t, write_editor_gui_draw, write_editor_gui_key, write_editor_gui_mouse, NULL);
        tile_register_gui_close_cb(t, write_editor_gui_close_request);
        // return, GUI will be active until unregistered or tile closed
        return;
    }
    // Non-tiling TUI path (unchanged)
    printf("%cStarting write editor for %s...\n", 255, 255, 255, filename);
    int running = 1;
    while (running) {
        write_editor_draw(filename);
        int key = tui_read_key();
        // ...existing key handling (kept above) ...
    }
    printf("\n\n");
}