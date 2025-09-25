#include <write_editor.h>
#include <types.h>
#include <eynfs.h>
#include <fat32.h>
#include <vga.h>
#include <system.h>
#include <string.h>
#include <util.h>
#include <stdint.h>
#include <string.h>
#include <tui.h>
#include <tile_manager.h>
#include <vga.h>
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

// GUI geometry cached for scroll logic
static int write_editor_gui_cols = 0;
static int write_editor_gui_rows = 0;

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

// Helper: get last path component (filename) from a path
static const char* get_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, path, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            // Use ultra-small buffer for low-memory systems
            const int chunk_size = 128; // Much smaller than EYNFS_BLOCK_SIZE
            uint32_t bytes_left = entry.size;
            uint32_t offset = 0;
            char buf[chunk_size + 1];
            int line = 0;
            int pos = 0;
            while (bytes_left > 0 && line < MAX_LINES) {
                int to_read = (bytes_left < chunk_size) ? bytes_left : chunk_size;
                int n = eynfs_read_file(disk, &sb, &entry, buf, to_read, offset);
                if (n < 0) break;
                buf[n] = '\0';
                for (int i = 0; i < n && line < MAX_LINES; i++) {
                    if (buf[i] == '\n' || pos >= MAX_LINE_LENGTH) {
                        write_editor_buffer[line][pos] = '\0';
                        line++;
                        pos = 0;
                        if (buf[i] == '\n') continue;
                    }
                    if (pos < MAX_LINE_LENGTH) {
                        write_editor_buffer[line][pos++] = buf[i];
                    }
                }
                offset += n;
                bytes_left -= n;
                if (n == 0) break;
            }
            if (pos > 0 && line < MAX_LINES) {
                write_editor_buffer[line][pos] = '\0';
                line++;
            }
            write_editor_num_lines = line > 0 ? line : 1;
            return 0;
        }
        return 0;
    }
    uint32 partition_lba_start = fat32_get_partition_lba_start(disk);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(disk, partition_lba_start, &bpb) == 0) {
        char fatname[12];
        to_fat32_83(path, fatname);
        uint32 byts_per_sec = bpb.BytsPerSec;
        uint32 sec_per_clus = bpb.SecPerClus;
        uint32 rsvd_sec_cnt = bpb.RsvdSecCnt;
        uint32 num_fats = bpb.NumFATs;
        uint32 fatsz = bpb.FATSz32;
        uint32 root_clus = bpb.RootClus;
        uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
        uint8 sector[512];
        uint8 fat[512];
        uint32 cluster = root_clus;
        while (cluster < 0x0FFFFFF8) {
            uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
            for (uint32 sec = 0; sec < sec_per_clus; sec++) {
                if (ata_read_sector(disk, partition_lba_start + cluster_first_sec + sec, sector) != 0) break;
                struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].Name[0] == 0x00) break;
                    if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                    if (entries[i].Name[0] == 0xE5) continue;
                    int match = 1;
                    for (int j = 0; j < 11; j++) {
                        if (entries[i].Name[j] != fatname[j]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        uint32 file_cluster = entries[i].FstClusLO | (entries[i].FstClusHI << 16);
                        uint32 file_size = entries[i].FileSize;
                        if (file_size > 0) {
                            char* data_ptr = (char*)fat32_disk_img + (first_data_sec + ((file_cluster - 2) * sec_per_clus)) * byts_per_sec;
                            int line = 0;
                            int pos = 0;
                            for (int k = 0; k < (int)file_size && line < MAX_LINES; k++) {
                                if (data_ptr[k] == '\n' || pos >= MAX_LINE_LENGTH) {
                                    write_editor_buffer[line][pos] = '\0';
                                    line++;
                                    pos = 0;
                                    if (data_ptr[k] == '\n') continue;
                                }
                                if (pos < MAX_LINE_LENGTH) {
                                    write_editor_buffer[line][pos++] = data_ptr[k];
                                }
                            }
                            if (pos > 0 && line < MAX_LINES) {
                                write_editor_buffer[line][pos] = '\0';
                                line++;
                            }
                            write_editor_num_lines = line > 0 ? line : 1;
                            return 0;
                        }
                        return 0;
                    }
                }
            }
            cluster = fat32_next_cluster(fat32_disk_img, &bpb, cluster);
        }
    }
    // If we reach here, file was not found or an error occurred
    return -1;
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0) {
        return -1;
    }
    if (sb.magic != EYNFS_MAGIC) {
        return -1;
    }
        eynfs_dir_entry_t entry;
        uint32_t parent_block, entry_idx;
    int found = eynfs_traverse_path(disk, &sb, path, &entry, &parent_block, &entry_idx);
    if (found != 0) {
        // Try to create the file
            char parent_path[128];
            if (path) {
                strcpy(parent_path, path);
            } else {
                strcpy(parent_path, "/");
            }
            char* last_slash = strrchr(parent_path, '/');
            if (!last_slash || last_slash == parent_path) {
                strcpy(parent_path, "/");
            } else {
                *last_slash = '\0';
            }
            eynfs_dir_entry_t parent_entry;
            if (eynfs_traverse_path(disk, &sb, parent_path, &parent_entry, NULL, NULL) != 0 || parent_entry.type != EYNFS_TYPE_DIR) {
                return -1;
            }
            const char* filename = get_basename(path);
            if (eynfs_create_entry(disk, &sb, parent_entry.first_block, filename, EYNFS_TYPE_FILE) != 0) {
                return -1;
            }
            if (eynfs_traverse_path(disk, &sb, path, &entry, &parent_block, &entry_idx) != 0) {
                return -1;
            }
        }
    int written = eynfs_write_file(disk, &sb, &entry, data, data_pos, parent_block, entry_idx);
    free(data); // Clean up allocated memory
    if (written != data_pos) {
        return -1;
    }
    return 0;
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
    snprintf(status, sizeof(status), "Line %d/%d, Col %d | V-Scroll: %d-%d | H-Scroll: %d | Ctrl+O: Save | Ctrl+X: Exit", 
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
    // Clear only the content area so we don't overdraw WM decorations
    if (content_w > 0 && content_h > 0) {
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
    }
    int cols = content_w / 8;
    int rows = content_h / 8;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    // cache geometry for potential key handling
    write_editor_gui_cols = cols;
    write_editor_gui_rows = rows;
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
            // Cursor handling: place a '!' at the cursor position and shift the underlying character right by one cell if it fits.
            if (abs_line == write_editor_cursor_y) {
                int cur_wrap = write_editor_cursor_x / cols;
                int cur_col = write_editor_cursor_x % cols;
                if (seg == cur_wrap && cc == cur_col) {
                    // draw cursor glyph
                    int px = content_x + cc * 8;
                    if (px >= content_x && px + 7 < content_x + content_w)
                        drawCharAt(px, draw_y, (int)'!', 255, 255, 0);
                    // draw underlying character shifted right if room remains
                    if (cc + 1 < cols) {
                        int px2 = content_x + (cc + 1) * 8;
                        if (px2 >= content_x && px2 + 7 < content_x + content_w)
                            drawCharAt(px2, draw_y, (int)(unsigned char)ch, 255, 255, 255);
                        cc++; // consume the next cell
                    }
                    continue;
                }
            }
            int px = content_x + cc * 8;
            if (px >= content_x && px + 7 < content_x + content_w)
                drawCharAt(px, draw_y, (int)(unsigned char)ch, 255, 255, 255);
        }
        seg++;
    }

    // Draw bottom status bar inside this tile (last row)
    if (rows >= 1) {
        int status_y = content_y + text_rows * 8;
        char status[160];
        snprintf(status, sizeof(status), "Ctrl+O: Save | Ctrl+X: Exit | Line %d Col %d", write_editor_cursor_y + 1, write_editor_cursor_x + 1);
        // simple background: draw a dark rectangle then text
        drawRect(content_x, status_y, content_w, 8, 16, 16, 16);
        // small left padding
        drawTextAt(content_x + 8, status_y, status, 255, 255, 255);
    }
}

// GUI key callback (top-level)
static void write_editor_gui_key(int tile_idx, int key, void* userdata) {
    switch (key) {
        case 0x1001: // up
            if (write_editor_cursor_y > 0) {
                write_editor_cursor_y--;
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
                if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
            }
            break;
        case 0x1002: // down
            if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                if (write_editor_cursor_x > strlength(write_editor_buffer[write_editor_cursor_y])) write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
            }
            break;
        case 0x1003: // left
            if (write_editor_cursor_x > 0) {
                write_editor_cursor_x--;
                if (write_editor_cursor_x < write_editor_scroll_x) write_editor_scroll_x = write_editor_cursor_x;
            } else if (write_editor_cursor_y > 0) {
                write_editor_cursor_y--;
                write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y < write_editor_scroll_y) write_editor_scroll_y = write_editor_cursor_y;
            }
            break;
        case 0x1004: // right
            if (write_editor_cursor_x < strlength(write_editor_buffer[write_editor_cursor_y])) {
                write_editor_cursor_x++;
                // advance horizontal scroll if cursor leaves visible area; use cached cols if available
                int visible_cols = (write_editor_gui_cols > 0) ? write_editor_gui_cols : 76;
                if (write_editor_cursor_x > write_editor_scroll_x + visible_cols - 1) write_editor_scroll_x = write_editor_cursor_x - (visible_cols - 1);
            } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
                write_editor_cursor_y++;
                write_editor_cursor_x = 0;
                write_editor_scroll_x = 0;
                if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
            }
            break;
        case '\b':
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
                }
            }
            break;
        case '\n':
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
            break;
        case 0x2001: // save
            if (save_write_editor_buffer(write_editor_gui_filename, write_editor_gui_disk) == 0) {
                write_editor_modified = 0;
                write_editor_update_tile_modified();
                // update tile status to remove [MODIFIED]
                if (write_editor_gui_tile >= 0) {
                    static char title_static[] = "Write Editor";
                    static char left_buf[128];
                    const char* b = get_basename(write_editor_gui_filename);
                    strncpy(left_buf, b, sizeof(left_buf) - 1);
                    left_buf[sizeof(left_buf) - 1] = '\0';
                    tile_set_title_status(write_editor_gui_tile, title_static, left_buf, NULL);
                }
            }
            break;
        case 0x2002: // exit
            if (write_editor_gui_tile >= 0) tile_unregister_gui_client(write_editor_gui_tile);
            break;
        default:
            if (key >= 32 && key <= 126) {
                int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                if (line_len < MAX_LINE_LENGTH) {
                    for (int i = line_len; i > write_editor_cursor_x; i--) write_editor_buffer[write_editor_cursor_y][i] = write_editor_buffer[write_editor_cursor_y][i - 1];
                    write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = (char)key;
                    write_editor_cursor_x++;
                    write_editor_modified = 1;
                    write_editor_update_tile_modified();
                    if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                }
            }
            break;
    }
}

// Main editor loop
void write_editor(const char* filename, uint8 disk) {
    // Reset editor state for new file
    write_editor_cursor_x = 0;
    write_editor_cursor_y = 0;
    write_editor_scroll_y = 0;
    write_editor_scroll_x = 0;
    write_editor_modified = 0;
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
        tile_register_gui_client(t, write_editor_gui_draw, write_editor_gui_key, NULL);
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