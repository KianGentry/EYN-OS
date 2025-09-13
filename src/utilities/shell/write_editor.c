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
                            for (int k = 0; k < file_size && line < MAX_LINES; k++) {
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
    // Draw filename left-aligned in white (bold)
    tui_style_t file_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 1};
    tui_draw_text(editor_win.x + 2, editor_win.y, filename, file_style);
    // Draw title centered
    tui_draw_text(editor_win.x + center_start, editor_win.y, title, editor_win.title_style);
    // Draw [Modified] right-aligned in red if needed
    if (write_editor_modified) {
        tui_style_t mod_style = {TUI_COLOR_RED, TUI_COLOR_BLACK, 1};
        tui_draw_text(editor_win.x + mod_start, editor_win.y, mod_str, mod_style);
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
    printf("%cStarting write editor for %s...\n", 255, 255, 255, filename);
    int running = 1;
    while (running) {
        write_editor_draw(filename);
        int key = tui_read_key();
        switch (key) {
            case 0x1001:
                if (write_editor_cursor_y > 0) {
                    write_editor_cursor_y--;
                    if (write_editor_cursor_y < write_editor_scroll_y) {
                        write_editor_scroll_y = write_editor_cursor_y;
                    }
                    int target_line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    if (write_editor_cursor_x > target_line_len) {
                        write_editor_cursor_x = target_line_len;
                    }
                    // reset horizontal scroll when changing lines
                    write_editor_scroll_x = 0;
                }
                break;
            case 0x1002:
                if (write_editor_cursor_y < write_editor_num_lines - 1) {
                    write_editor_cursor_y++;
                    if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) {
                        write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                    }
                    int target_line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    if (write_editor_cursor_x > target_line_len) {
                        write_editor_cursor_x = target_line_len;
                    }
                    // reset horizontal scroll when changing lines
                    write_editor_scroll_x = 0;
                }
                break;
            case 0x1003:
                if (write_editor_cursor_x > 0) {
                    write_editor_cursor_x--;
                    // adjust horizontal scroll if cursor moves out of view
                    if (write_editor_cursor_x < write_editor_scroll_x) {
                        write_editor_scroll_x = write_editor_cursor_x;
                    }
                } else if (write_editor_cursor_y > 0) {
                    // if at beginning of line and not on first line, move to end of previous line
                    write_editor_cursor_y--;
                    write_editor_cursor_x = strlength(write_editor_buffer[write_editor_cursor_y]);
                    write_editor_scroll_x = 0; // reset horizontal scroll
                    // adjust vertical scroll if needed
                    if (write_editor_cursor_y < write_editor_scroll_y) {
                        write_editor_scroll_y = write_editor_cursor_y;
                    }
                }
                break;
            case 0x1004:
                if (write_editor_cursor_x < strlength(write_editor_buffer[write_editor_cursor_y])) {
                    write_editor_cursor_x++;
                    // adjust horizontal scroll if cursor moves out of view
                    int max_visible_x = write_editor_scroll_x + (78 - 2); // window width - borders
                    if (write_editor_cursor_x > max_visible_x) {
                        write_editor_scroll_x = write_editor_cursor_x - (78 - 2);
                    }
                } else if (write_editor_cursor_y < write_editor_num_lines - 1) {
                    // if at end of line and not on last line, move to start of next line
                    write_editor_cursor_y++;
                    write_editor_cursor_x = 0;
                    write_editor_scroll_x = 0; // reset horizontal scroll
                    // adjust vertical scroll if needed
                    if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) {
                        write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                    }
                }
                break;
            case 0x1008:
                if (write_editor_scroll_y > 0) {
                    write_editor_scroll_y -= EDITOR_HEIGHT / 2;
                    if (write_editor_scroll_y < 0) write_editor_scroll_y = 0;
                    write_editor_cursor_y -= EDITOR_HEIGHT / 2;
                    if (write_editor_cursor_y < write_editor_scroll_y) {
                        write_editor_cursor_y = write_editor_scroll_y;
                    }
                }
                break;
            case 0x1009:
                if (write_editor_scroll_y + EDITOR_HEIGHT < write_editor_num_lines) {
                    write_editor_scroll_y += EDITOR_HEIGHT / 2;
                    if (write_editor_scroll_y + EDITOR_HEIGHT > write_editor_num_lines) {
                        write_editor_scroll_y = write_editor_num_lines - EDITOR_HEIGHT;
                        if (write_editor_scroll_y < 0) write_editor_scroll_y = 0;
                    }
                    write_editor_cursor_y += EDITOR_HEIGHT / 2;
                    if (write_editor_cursor_y >= write_editor_num_lines) {
                        write_editor_cursor_y = write_editor_num_lines - 1;
                    }
                    if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) {
                        write_editor_cursor_y = write_editor_scroll_y + EDITOR_HEIGHT - 1;
                    }
                }
                break;
            case '\b':
                if (write_editor_cursor_x > 0) {
                    int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    for (int i = write_editor_cursor_x - 1; i < line_len; i++) {
                        write_editor_buffer[write_editor_cursor_y][i] = write_editor_buffer[write_editor_cursor_y][i + 1];
                    }
                    write_editor_cursor_x--;
                    write_editor_modified = 1;
                } else if (write_editor_cursor_y > 0) {
                    int prev_len = strlength(write_editor_buffer[write_editor_cursor_y - 1]);
                    int curr_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    if (prev_len + curr_len < MAX_LINE_LENGTH) {
                        for (int i = 0; i < curr_len; i++) {
                            write_editor_buffer[write_editor_cursor_y - 1][prev_len + i] = write_editor_buffer[write_editor_cursor_y][i];
                        }
                        write_editor_buffer[write_editor_cursor_y - 1][prev_len + curr_len] = '\0';
                        for (int i = write_editor_cursor_y; i < write_editor_num_lines - 1; i++) {
                            strcpy(write_editor_buffer[i], write_editor_buffer[i + 1]);
                        }
                        write_editor_num_lines--;
                        write_editor_cursor_y--;
                        write_editor_cursor_x = prev_len;
                        write_editor_modified = 1;
                        // Scroll up if needed
                        if (write_editor_cursor_y < write_editor_scroll_y) {
                            write_editor_scroll_y = write_editor_cursor_y;
                        } else if (write_editor_cursor_y == write_editor_scroll_y) {
                            if (write_editor_scroll_y > 0) write_editor_scroll_y--;
                        }
                    }
                }
                break;
            case '\n':
                if (write_editor_num_lines < MAX_LINES) {
                    int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    char temp[MAX_LINE_LENGTH + 1];
                    strcpy(temp, write_editor_buffer[write_editor_cursor_y] + write_editor_cursor_x);
                    write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = '\0';
                    for (int i = write_editor_num_lines; i > write_editor_cursor_y + 1; i--) {
                        strcpy(write_editor_buffer[i], write_editor_buffer[i - 1]);
                    }
                    strcpy(write_editor_buffer[write_editor_cursor_y + 1], temp);
                    write_editor_num_lines++;
                    write_editor_cursor_y++;
                    write_editor_cursor_x = 0;
                    write_editor_modified = 1;
                    if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) {
                        write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                    }
                }
                break;
            case 0x2001:
                if (save_write_editor_buffer(filename, disk) == 0) {
                    printf("%cFile saved successfully!\n", 0, 255, 0);
                    write_editor_modified = 0;
                    sleep(10);
                } else {
                    printf("%cFailed to save file!\n", 255, 0, 0);
                    sleep(100);
                }
                break;
            case 0x2002:
                running = 0;
                break;
            case 27:
                running = 0;
                break;
            default:
                if (key >= 32 && key <= 126) {
                    int line_len = strlength(write_editor_buffer[write_editor_cursor_y]);
                    if (line_len < MAX_LINE_LENGTH) {
                        for (int i = line_len; i > write_editor_cursor_x; i--) {
                            write_editor_buffer[write_editor_cursor_y][i] = write_editor_buffer[write_editor_cursor_y][i - 1];
                        }
                        write_editor_buffer[write_editor_cursor_y][write_editor_cursor_x] = (char)key;
                        write_editor_cursor_x++;
                        write_editor_modified = 1;
                        if (write_editor_cursor_y >= write_editor_scroll_y + EDITOR_HEIGHT) {
                            write_editor_scroll_y = write_editor_cursor_y - EDITOR_HEIGHT + 1;
                        }
                    }
                }
                break;
        }
    }
    printf("\n\n");
}