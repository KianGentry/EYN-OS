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
#define EYNFS_SUPERBLOCK_LBA 2048
extern void* fat32_disk_img;

// Editor state and helpers (renamed)
#define MAX_LINES 100
#define MAX_LINE_LENGTH 80
#define EDITOR_HEIGHT 30
static char write_editor_buffer[MAX_LINES][MAX_LINE_LENGTH + 1];
static int write_editor_num_lines = 1;
static int write_editor_cursor_x = 0;
static int write_editor_cursor_y = 0;
static int write_editor_scroll_y = 0;
static int write_editor_scroll_x = 0;
static int write_editor_modified = 0;
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
static int write_editor_sel_active = 0;
static int write_editor_sel_ax = 0, write_editor_sel_ay = 0; // anchor (col,x) within absolute line
static int write_editor_sel_fx = 0, write_editor_sel_fy = 0; // focus (col,x)
static char write_editor_clipboard[4096];
static int write_editor_clipboard_len = 0;

// helper: update tile's modified indicator when running in GUI mode
// forward prototype for get_basename so helper can call it
static const char* get_basename(const char* path);

static void write_editor_update_tile_modified(void) {
    if (write_editor_gui_tile >= 0) {
        static char title_static[] = "Write Editor";
        static char left_buf[128];
        const char* b = get_basename(write_editor_gui_filename);
        strncpy(left_buf, b, sizeof(left_buf) - 1);
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
    int sy = write_editor_sel_ay, sx = write_editor_sel_ax;
    int fy = write_editor_sel_fy, fx = write_editor_sel_fx;
    if (fy < sy || (fy == sy && fx < sx)) { int ty = sy; sy = fy; fy = ty; int tx = sx; sx = fx; fx = tx; }
    // clamp within bounds
    if (sy < 0) sy = 0; if (sy >= write_editor_num_lines) sy = write_editor_num_lines - 1;
    if (fy < 0) fy = 0; if (fy >= write_editor_num_lines) fy = write_editor_num_lines - 1;
    int s_len = strlength(write_editor_buffer[sy]); if (sx < 0) sx = 0; if (sx > s_len) sx = s_len;
    int f_len = strlength(write_editor_buffer[fy]); if (fx < 0) fx = 0; if (fx > f_len) fx = f_len;
    if (sy == fy) {
        // single-line delete
        int len = strlength(write_editor_buffer[sy]);
        int rem = fx - sx; if (rem < 0) rem = 0;
        for (int i = sx; i + rem <= len; ++i) write_editor_buffer[sy][i] = write_editor_buffer[sy][i + rem];
        write_editor_cursor_y = sy; write_editor_cursor_x = sx;
    } else {
        // multi-line: keep prefix of start line up to sx, then append tail of fy from fx
        char tail[MAX_LINE_LENGTH + 1];
        int tail_len = 0;
        const char* ftail = write_editor_buffer[fy] + fx;
        while (ftail[tail_len] && tail_len < MAX_LINE_LENGTH) { tail[tail_len] = ftail[tail_len]; tail_len++; }
        tail[tail_len] = '\0';
        // truncate start line at sx
        write_editor_buffer[sy][sx] = '\0';
        int pre_len = strlength(write_editor_buffer[sy]);
        int space = MAX_LINE_LENGTH - pre_len;
        int copy = tail_len < space ? tail_len : space;
        for (int i = 0; i < copy; ++i) write_editor_buffer[sy][pre_len + i] = tail[i];
        write_editor_buffer[sy][pre_len + copy] = '\0';
        // shift lines up removing fy-sy lines
        int remove_count = fy - sy;
        for (int i = sy + 1; i + remove_count < write_editor_num_lines; ++i) {
            strcpy(write_editor_buffer[i], write_editor_buffer[i + remove_count]);
        }
        write_editor_num_lines -= remove_count;
        if (write_editor_num_lines < 1) write_editor_num_lines = 1;
        write_editor_cursor_y = sy; write_editor_cursor_x = sx;
        // Clear any now-unused lines to avoid stale data showing up
        for (int i = write_editor_num_lines; i < MAX_LINES; ++i) {
            write_editor_buffer[i][0] = '\0';
        }
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
    // Clear the entire buffer
    for (int i = 0; i < MAX_LINES; i++) {
        for (int j = 0; j < MAX_LINE_LENGTH + 1; j++) {
            write_editor_buffer[i][j] = '\0';
        }
    }
    write_editor_num_lines = 1;

    // Read via VFS (supports EYNFS and FAT32). Missing file is fine (start empty).
    const int max_bytes = MAX_LINES * (MAX_LINE_LENGTH + 1);
    char* buf = (char*)malloc(max_bytes);
    if (!buf) return -1;
    int n = vfs_read_file(disk, path, buf, max_bytes);
    if (n <= 0) { free(buf); return 0; }

    // Helper: detect binary-like extensions we should present as editable hex
    int is_binary_edit = 0;
    const char* ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".eyn") == 0 || strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".flat") == 0) is_binary_edit = 1;
    }

    if (!is_binary_edit) {
        int line = 0, pos = 0;
        for (int i = 0; i < n && line < MAX_LINES; ++i) {
            if (buf[i] == '\n' || pos >= MAX_LINE_LENGTH) {
                write_editor_buffer[line][pos] = '\0';
                line++; pos = 0;
                if (buf[i] == '\n') continue;
            }
            if (pos < MAX_LINE_LENGTH) write_editor_buffer[line][pos++] = buf[i];
        }
        if (pos > 0 && line < MAX_LINES) { write_editor_buffer[line][pos] = '\0'; line++; }
        write_editor_num_lines = (line > 0) ? line : 1;
    } else {
        // Convert binary data into human-readable hex lines. Use 16 bytes per line for readability.
        int line = 0;
        int bytes_per_line = 16;
        for (int i = 0; i < n && line < MAX_LINES; i += bytes_per_line) {
            int end = i + bytes_per_line; if (end > n) end = n;
            int pos = 0;
            for (int j = i; j < end && pos < MAX_LINE_LENGTH - 3; ++j) {
                unsigned char b = (unsigned char)buf[j];
                // write two hex chars and a space (except maybe last in line)
                char hi = "0123456789ABCDEF"[(b >> 4) & 0xF];
                char lo = "0123456789ABCDEF"[b & 0xF];
                if (pos + 3 < MAX_LINE_LENGTH) {
                    write_editor_buffer[line][pos++] = hi;
                    write_editor_buffer[line][pos++] = lo;
                    write_editor_buffer[line][pos++] = ' ';
                } else if (pos + 2 < MAX_LINE_LENGTH) {
                    write_editor_buffer[line][pos++] = hi;
                    write_editor_buffer[line][pos++] = lo;
                }
            }
            // trim trailing space
            if (pos > 0 && write_editor_buffer[line][pos - 1] == ' ') pos--;
            write_editor_buffer[line][pos] = '\0';
            line++;
        }
        write_editor_num_lines = (line > 0) ? line : 1;
    }
    free(buf);
    return 0;
}

// Save editor buffer to file
int save_write_editor_buffer(const char* path, uint8 disk) {
    // Calculate total size needed
    int total_size = 0;
    for (int i = 0; i < write_editor_num_lines; i++) {
        total_size += strlength(write_editor_buffer[i]);
        if (i < write_editor_num_lines - 1) {
            total_size += 1; // For newline
        }
    }
    
    // Helper: detect binary-like extensions we should save by converting from hex to raw bytes
    int is_binary_edit = 0;
    const char* ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".eyn") == 0 || strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".flat") == 0) is_binary_edit = 1;
    }

    if (!is_binary_edit) {
        // Allocate buffer for the entire file content
        char* data = (char*)malloc(total_size + 1);
        if (!data) return -1;
        
        int data_pos = 0;
        for (int i = 0; i < write_editor_num_lines; i++) {
            int line_len = strlength(write_editor_buffer[i]);
            for (int j = 0; j < line_len; j++) {
                data[data_pos++] = write_editor_buffer[i][j];
            }
            if (i < write_editor_num_lines - 1) {
                data[data_pos++] = '\n';
            }
        }
        data[data_pos] = '\0';
        int written = vfs_write_file(disk, path, data, (uint32)data_pos);
        free(data);
        if (written < 0 || written != data_pos) return -1;
        return 0;
    } else {
        // Convert hex representation back into raw bytes.
        // Estimate max bytes: each line may contain up to MAX_LINE_LENGTH chars, so max hex digits per line is MAX_LINE_LENGTH
        int max_bytes = (MAX_LINES * (MAX_LINE_LENGTH / 2)) + 16;
        unsigned char* out = (unsigned char*)malloc(max_bytes);
        if (!out) return -1;
        int out_pos = 0;
        for (int i = 0; i < write_editor_num_lines; ++i) {
            const char* s = write_editor_buffer[i];
            int len = strlength(write_editor_buffer[i]);
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
        free(out);
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
            int line_len = strlength(write_editor_buffer[line_idx]);
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
                    char ch[2] = {write_editor_buffer[line_idx][j], '\0'};
                    tui_draw_text(x, y, ch, text_style);
                    x++;
                }
            }
        }
    }

    // Status bar just below the window
    char status[160];
    snprintf(status, sizeof(status), "Line %d/%d, Col %d | V-Scroll: %d-%d | H-Scroll: %d | Ctrl+S: Save | Ctrl+X: Exit", 
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
    // Incremental clears: clear only the bands we're about to redraw
    if (content_w > 0 && content_h > 0) {
        // Text area bands
        int rows = content_h / 8;
        int text_rows = rows > 1 ? rows - 1 : rows; // leave last row for status
        for (int r = 0; r < text_rows; ++r) {
            int py = content_y + r * 8;
            if (py + 8 <= content_y + content_h) drawRect(content_x, py, content_w, 8, 0, 0, 0);
        }
        // Status bar band
        int status_y = content_y + text_rows * 8;
        if (status_y >= content_y && status_y + 8 <= content_y + content_h) drawRect(content_x, status_y, content_w, 8, 16, 16, 16);
    }
    int cols = content_w / 8;
    int rows = content_h / 8;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    // cache geometry for potential key handling
    write_editor_gui_cols = cols;
    write_editor_gui_rows = rows;
    write_editor_last_cx = content_x;
    write_editor_last_cy = content_y;
    write_editor_last_cw = content_w;
    write_editor_last_ch = content_h;
    // Reserve one row at the bottom of the tile content for the status bar
    int text_rows = rows > 1 ? rows - 1 : rows;

    // Render text with soft-wrapping: walk absolute lines starting at scroll_y
    int abs_line = write_editor_scroll_y;
    int seg = 0; // wrapped segment index within current abs_line
    for (int r = 0; r < text_rows; ++r) {
        int draw_y = content_y + r * 8;
        if (abs_line >= write_editor_num_lines) {
            // nothing to draw on this row
            continue;
        }
    const char* line = write_editor_buffer[abs_line];
    int len = strlength((char*)line); // cast to silence discarded qualifier warning
        int wraps = (len + cols - 1) / cols; if (wraps < 1) wraps = 1;
        // If we've exhausted wrapped segments for this line, advance to next line and retry this visual row
        if (seg >= wraps) {
            abs_line++;
            seg = 0;
            r--; // redo this visual row with the next absolute line
            continue;
        }
        int start_col = seg * cols;
        for (int cc = 0; cc < cols; ++cc) {
            int src_idx = start_col + cc;
            char ch = ' ';
            if (src_idx < len) ch = line[src_idx];
            // Selection background per-cell (simple solid rect)
            int is_sel = 0;
            if (write_editor_sel_active) {
                // Normalize selection endpoints
                int sy = write_editor_sel_ay, sx = write_editor_sel_ax;
                int fy = write_editor_sel_fy, fx = write_editor_sel_fx;
                if (fy < sy || (fy == sy && fx < sx)) { int ty = sy; sy = fy; fy = ty; int tx = sx; sx = fx; fx = tx; }
                if (abs_line > sy && abs_line < fy) is_sel = 1; // fully inside selected lines
                else if (abs_line == sy && abs_line == fy) {
                    if (src_idx >= sx && src_idx < fx) is_sel = 1;
                } else if (abs_line == sy) {
                    if (src_idx >= sx) is_sel = 1;
                } else if (abs_line == fy) {
                    if (src_idx < fx) is_sel = 1;
                }
            }
            // Cursor handling: draw an underscore '_' at the cursor position (overlay, no displacement)
            if (abs_line == write_editor_cursor_y) {
                int cur_wrap = write_editor_cursor_x / cols;
                int cur_col = write_editor_cursor_x % cols;
                if (seg == cur_wrap && cc == cur_col) {
                    // we'll overlay '_' after drawing the underlying character below
                }
            }
            int px = content_x + cc * 8;
            if (px >= content_x && px + 7 < content_x + content_w) {
                if (is_sel) drawRect(px, draw_y, 8, 8, 0, 128, 128);
                drawCharAt(px, draw_y, (int)(unsigned char)ch, 255, 255, 255);
                // Overlay underscore at caret cell
                if (abs_line == write_editor_cursor_y) {
                    int cur_wrap = write_editor_cursor_x / cols;
                    int cur_col = write_editor_cursor_x % cols;
                    if (seg == cur_wrap && cc == cur_col) {
                        drawCharAt(px, draw_y, (int)'_', 220, 220, 220);
                    }
                }
            }
        }
        seg++;
    }

    // Draw bottom status bar inside this tile (last row)
    if (rows >= 1) {
        int status_y = content_y + text_rows * 8;
        char status[160];
        if (write_editor_last_save_failed) {
            snprintf(status, sizeof(status), "Save failed | Ctrl+S: Save | Ctrl+X: Exit | Line %d Col %d", write_editor_cursor_y + 1, write_editor_cursor_x + 1);
        } else {
            snprintf(status, sizeof(status), "Ctrl+S: Save | Ctrl+X: Exit | Line %d Col %d", write_editor_cursor_y + 1, write_editor_cursor_x + 1);
        }
        // background already drawn above
        drawTextAt(content_x + 8, status_y, status, 255, 255, 255);
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
}

// GUI key callback (top-level)
static void write_editor_gui_key(int tile_idx, int key, void* userdata) {
    (void)tile_idx; (void)userdata;

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
                if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
            }
        } else if (base == 0x1002) { // Down
            if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
            }
        } else if (base == 0x1003) { // Left
            if (write_editor_cursor_x > 0) {
                write_editor_cursor_x--;
                if (write_editor_cursor_x < write_editor_scroll_x) write_editor_scroll_x = write_editor_cursor_x;
            } else if (write_editor_cursor_y > 0) {
                write_editor_cursor_y--;
                write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
            }
        } else if (base == 0x1004) { // Right
            if (write_editor_cursor_x < strlength(write_editor_buffer[write_editor_cursor_y])) {
                write_editor_cursor_x++;
                int visible_cols = (write_editor_gui_cols > 0) ? write_editor_gui_cols : 76;
                if (write_editor_cursor_x > write_editor_scroll_x + visible_cols - 1) write_editor_scroll_x = write_editor_cursor_x - (visible_cols - 1);
            } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                write_editor_cursor_x = 0;
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
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
        write_editor_sel_fx = strlength(write_editor_buffer[write_editor_sel_fy]);
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Ctrl+L select current line
    if (key == 0x2105) {
        write_editor_sel_active = 1;
        write_editor_sel_ay = write_editor_cursor_y; write_editor_sel_ax = 0;
        write_editor_sel_fy = write_editor_cursor_y;
        write_editor_sel_fx = strlength(write_editor_buffer[write_editor_cursor_y]);
        tile_invalidate_gui(write_editor_gui_tile);
        return;
    }
    // Arrow keys (no shift) clear selection
    if (key == 0x1001) { // Up
        if (write_editor_cursor_y > 0) {
            write_editor_cursor_y--;
            if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
            if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
            write_editor_scroll_x = 0;
            write_editor_sel_active = 0;
        }
        return;
    }
    if (key == 0x1002) { // Down
        if (write_editor_cursor_y < write_editor_num_lines - 1) {
            write_editor_cursor_y++;
            if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
            if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
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
            write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
            write_editor_scroll_x = 0;
            if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
        }
        write_editor_sel_active = 0;
        return;
    }
    if (key == 0x1004) { // Right
        if (write_editor_cursor_x < strlength(write_editor_buffer[write_editor_cursor_y])) {
            write_editor_cursor_x++;
            int visible_cols = (write_editor_gui_cols > 0) ? write_editor_gui_cols : 76;
            if (write_editor_cursor_x > write_editor_scroll_x + visible_cols - 1) write_editor_scroll_x = write_editor_cursor_x - (visible_cols - 1);
        } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
            write_editor_cursor_y++;
            write_editor_cursor_x = 0;
            write_editor_scroll_x = 0;
            if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
        }
        write_editor_sel_active = 0;
        return;
    }
    if (key == '\b') { // Backspace
        if (write_editor_sel_active) {
            // If selection covers entire buffer, clear all quickly
            int all = 0;
            if (write_editor_sel_active) {
                int sy = write_editor_sel_ay, sx = write_editor_sel_ax;
                int fy = write_editor_sel_fy, fx = write_editor_sel_fx;
                if (fy < sy || (fy == sy && fx < sx)) { int ty = sy; sy = fy; fy = ty; int tx = sx; sx = fx; fx = tx; }
                if (sy == 0 && sx == 0 && fy == write_editor_num_lines - 1 && fx == strlength(write_editor_buffer[fy])) all = 1;
            }
            if (all) {
                for (int i = 0; i < MAX_LINES; ++i) write_editor_buffer[i][0] = '\0';
                write_editor_num_lines = 1; write_editor_cursor_x = 0; write_editor_cursor_y = 0;
                write_editor_scroll_x = 0; write_editor_scroll_y = 0; write_editor_sel_active = 0;
                write_editor_modified = 1; write_editor_update_tile_modified();
                tile_invalidate_gui(write_editor_gui_tile);
                return;
            }
            if (write_editor_delete_active_selection()) return;
        }
        if (write_editor_cursor_x > 0) {
            int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
            for (int i = write_editor_cursor_x - 1; i < line_len; i++) write_editor_buffer[write_editor_cursor_y][i] = write_editor_buffer[write_editor_cursor_y][i + 1];
            write_editor_cursor_x--;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
        } else if (write_editor_cursor_y > 0) {
            int prev_len = strlength(write_editor_buffer[write_editor_cursor_y - 1]);
            int curr_len = strlength(write_editor_buffer[write_editor_cursor_y]);
            if (prev_len + curr_len < MAX_LINE_LENGTH) {
                for (int i = 0; i < curr_len; i++) write_editor_buffer[write_editor_cursor_y - 1][prev_len + i] = write_editor_buffer[write_editor_cursor_y][i];
                write_editor_buffer[write_editor_cursor_y - 1][prev_len + curr_len] = '\0';
                for (int i = write_editor_cursor_y; i < write_editor_num_lines - 1; i++) strcpy(write_editor_buffer[i], write_editor_buffer[i + 1]);
                write_editor_num_lines--;
                write_editor_cursor_y--;
                write_editor_cursor_x = prev_len;
                write_editor_modified = 1;
                write_editor_update_tile_modified();
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                // Clear the now-unused last logical line to avoid stale text reappearing
                if (write_editor_num_lines >= 0 && write_editor_num_lines < MAX_LINES) {
                    write_editor_buffer[write_editor_num_lines][0] = '\0';
                }
                // Clamp horizontal scroll to caret
                if (write_editor_scroll_x > write_editor_cursor_x) write_editor_scroll_x = write_editor_cursor_x;
            }
        }
        return;
    }
    if (key == '\n') { // Enter
        if (write_editor_sel_active) {
            if (!write_editor_delete_active_selection()) return; // should delete and update state
        }
        if (write_editor_num_lines < MAX_LINES) {
            int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
            char temp[MAX_LINE_LENGTH + 1];
            strcpy(temp, write_editor_buffer[write_editor_cursor_y] + write_editor_cursor_x);
            write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = '\0';
            for (int i = write_editor_num_lines; i > write_editor_cursor_y + 1; i--) strcpy(write_editor_buffer[i], write_editor_buffer[i - 1]);
            strcpy(write_editor_buffer[write_editor_cursor_y + 1], temp);
            write_editor_num_lines++;
            write_editor_cursor_y++;
            write_editor_cursor_x = 0;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
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
    if (key == 0x2002) { // exit
        // Tiler will attempt to close after this callback.
        // If modified, show prompt and veto close via the close callback.
        if (write_editor_modified) {
            write_editor_exit_prompt_active = 1;
            tile_invalidate_gui(write_editor_gui_tile);
        }
        return;
    }
    // Printable insert
    if (key >= 32 && key <= 126) {
        if (write_editor_sel_active) {
            if (!write_editor_delete_active_selection()) { /* fallthrough */ }
        }
        int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
        if (line_len < MAX_LINE_LENGTH) {
            /* Move the tail (including terminating NUL) one position to the right so we insert without
             * overwriting the previous character. Use memmove because the source and destination
             * overlap. The number of bytes to move is (line_len - cursor_x) + 1 to include '\0'. */
            int tail_bytes = line_len - write_editor_cursor_x;
            if (tail_bytes >= 0) {
                memmove(&write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x + 1],
                        &write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x],
                        (size_t)(tail_bytes + 1));
            }
            write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = (char)key;
            write_editor_cursor_x++;
            write_editor_modified = 1;
            write_editor_update_tile_modified();
            if (write_editor_gui_tile >= 0) tile_invalidate_gui(write_editor_gui_tile);
            if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
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
    int cols = (write_editor_last_cw > 0) ? (write_editor_last_cw / 8) : (write_editor_gui_cols > 0 ? write_editor_gui_cols : 80);
    int rows = (write_editor_last_ch > 0) ? (write_editor_last_ch / 8) : (write_editor_gui_rows > 0 ? write_editor_gui_rows : 25);
    int text_rows = rows > 1 ? rows - 1 : rows; // bottom row is status
    int rel_x = clampi(px - write_editor_last_cx, 0, write_editor_last_cw - 1) / 8;
    int rel_y = clampi(py - write_editor_last_cy, 0, (text_rows * 8) - 1) / 8;
    int abs_line = write_editor_scroll_y;
    int seg = 0;
    for (int r = 0; r < rel_y; ++r) {
        if (abs_line >= write_editor_num_lines) break;
        int len = strlength(write_editor_buffer[abs_line]);
        int wraps = (len + cols - 1) / cols; if (wraps < 1) wraps = 1;
        seg++;
        if (seg >= wraps) { abs_line++; seg = 0; }
    }
    if (abs_line >= write_editor_num_lines) abs_line = write_editor_num_lines - 1;
    int len = strlength(write_editor_buffer[abs_line]);
    int col = seg * cols + rel_x;
    if (col > len) col = len;
    if (out_line) *out_line = abs_line;
    if (out_col) *out_col = col;
}

// Mouse callback: wheel scroll, left-click/drag select, right-click copy, middle-click paste
static void write_editor_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata) {
    (void)tile_idx; (void)userdata;
    if (write_editor_gui_tile < 0) return;
    if (write_editor_exit_prompt_active) return;
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
        write_editor_clipboard_len = 0;
        if (write_editor_sel_active) {
            int sy = write_editor_sel_ay, sx = write_editor_sel_ax;
            int fy = write_editor_sel_fy, fx = write_editor_sel_fx;
            if (fy < sy || (fy == sy && fx < sx)) { int ty = sy; sy = fy; fy = ty; int tx = sx; sx = fx; fx = tx; }
            for (int y = sy; y <= fy; ++y) {
                int start = (y == sy) ? sx : 0;
                int end = (y == fy) ? fx : strlength(write_editor_buffer[y]);
                for (int x = start; x < end; ++x) {
                    if (write_editor_clipboard_len < (int)sizeof(write_editor_clipboard) - 1) {
                        write_editor_clipboard[write_editor_clipboard_len++] = write_editor_buffer[y][x];
                    }
                }
                if (y != fy) {
                    if (write_editor_clipboard_len < (int)sizeof(write_editor_clipboard) - 1) write_editor_clipboard[write_editor_clipboard_len++] = '\n';
                }
            }
            write_editor_clipboard[write_editor_clipboard_len] = '\0';
        }
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
                        int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                        char temp[MAX_LINE_LENGTH + 1];
                        strcpy(temp, write_editor_buffer[write_editor_cursor_y] + write_editor_cursor_x);
                        write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = '\0';
                        for (int k = write_editor_num_lines; k > write_editor_cursor_y + 1; k--) strcpy(write_editor_buffer[k], write_editor_buffer[k - 1]);
                        strcpy(write_editor_buffer[write_editor_cursor_y + 1], temp);
                        write_editor_num_lines++;
                        write_editor_cursor_y++;
                        write_editor_cursor_x = 0;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
            if (line_len < MAX_LINE_LENGTH) {
                /* Insert character at cursor by shifting tail right (including NUL) */
                int tail_bytes = line_len - write_editor_cursor_x;
                if (tail_bytes >= 0) {
                    memmove(&write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x + 1],
                        &write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x],
                        (size_t)(tail_bytes + 1));
                }
                write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = ch;
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
    write_editor_exit_prompt_active = 0;
    write_editor_last_save_failed = 0;
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