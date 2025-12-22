#include <tui.h>
#include <shell_command_info.h>
#include <help_tui.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <serial.h>
#include <tile_manager.h>

extern const shell_command_info_t __start_shellcmds[];
extern const shell_command_info_t __stop_shellcmds[];

#define HELP_TUI_WIDTH 80
#define CMD_LIST_WIDTH 22
#define DESC_WIDTH (HELP_TUI_WIDTH - CMD_LIST_WIDTH - 4) // -4 for separator and borders
#define MAX_VISIBLE 22

// Structure to hold sub-command information
typedef struct {
    const char* name;
    const char* description;
    const char* usage;
} subcommand_info_t;

// Sub-command definitions for each base command
static const subcommand_info_t search_subcommands[] = {
    {"search_size", "Find files by size using operators", "search_size <op> <size>"},
    {"search_type", "Find files by extension", "search_type <extension>"},
    {"search_empty", "Find empty files and directories", "search_empty"},
    {"search_depth", "Search with depth limitation", "search_depth <depth> <pattern>"},
    {NULL, NULL, NULL}
};

static const subcommand_info_t ls_subcommands[] = {
    {"ls_tree", "Tree view listing", "ls_tree [depth]"},
    {"ls_size", "Size-based listing", "ls_size [depth]"},
    {"ls_detail", "Detailed listing", "ls_detail [depth]"},
    {NULL, NULL, NULL}
};

static const subcommand_info_t fs_subcommands[] = {
    {"fsstat", "Filesystem status", "fsstat"},
    {"cache_stats", "Cache statistics", "cache_stats"},
    {"cache_clear", "Clear all caches", "cache_clear"},
    {"cache_reset", "Reset cache statistics", "cache_reset"},
    {"blockmap", "Visual block map", "blockmap"},
    {"debug_superblock", "Superblock debug info", "debug_superblock"},
    {"debug_directory", "Directory debug", "debug_directory <path>"},
    {NULL, NULL, NULL}
};

static const subcommand_info_t read_subcommands[] = {
    {"read_raw", "Display raw file contents", "read_raw <filename>"},
    {"read_md", "Display markdown files with formatting", "read_md <filename>"},
    {"read_image", "Display image files (.png, .jpg, .jpeg, .rei)", "read_image <filename>"},
    {NULL, NULL, NULL}
};

// Function to check if a command has sub-commands
static int has_subcommands(const char* cmd_name) {
    return (strcmp(cmd_name, "search") == 0 ||
            strcmp(cmd_name, "ls") == 0 ||
            strcmp(cmd_name, "fs") == 0 ||
            strcmp(cmd_name, "read") == 0);
}

// Function to get sub-commands for a base command
static const subcommand_info_t* get_subcommands(const char* cmd_name) {
    if (strcmp(cmd_name, "search") == 0) return search_subcommands;
    if (strcmp(cmd_name, "ls") == 0) return ls_subcommands;
    if (strcmp(cmd_name, "fs") == 0) return fs_subcommands;
    if (strcmp(cmd_name, "read") == 0) return read_subcommands;
    return NULL;
}

// Function to count sub-commands
static int count_subcommands(const subcommand_info_t* subcmds) {
    int count = 0;
    while (subcmds[count].name != NULL) count++;
    return count;
}

// --- GUI-backed help state (used when running inside a tile) ---
static const shell_command_info_t** g_sorted_cmds = NULL;
static int g_cmd_count = 0;
static int g_selected = 0;
static int g_selected_sub = 0;
static int g_scroll = 0;
static int g_max_visible = MAX_VISIBLE;
// dynamic expanded flags (allocated to g_cmd_count)
static int* g_expanded_commands = NULL;
// persistent copies of command metadata and strings. These are allocated by
// help_tui_init_state() and freed when the GUI exits. Keeping copies avoids
// relying on pointers into other sections that might be transient.
static shell_command_info_t* g_copied_cmds = NULL;
static char* g_cmd_string_pool = NULL;
static volatile int g_help_running = 0;

// Forward declarations for GUI callbacks
void help_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
void help_gui_key(int tile_idx, int key, void* userdata);
void help_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata);
// Forward declarations for init/show helpers
void help_tui_init_state(void);
void help_tui_show(void);

// Remember last content rect for mouse hit-testing
static int g_last_cx = 0, g_last_cy = 0, g_last_cw = 0, g_last_ch = 0;

static void help_dbg(const char* s) {
    if (!s) return;
    // Keep this extremely low-volume: serial output can stall the UI if spammed.
    const char* pfx = "[help] ";
    for (const char* p = pfx; *p; ++p) serial_write_char(SERIAL_COM1, *p);
    for (const char* p = s; *p; ++p) serial_write_char(SERIAL_COM1, *p);
    serial_write_char(SERIAL_COM1, '\n');
}

static void help_dbg_ch(char c) {
    serial_write_char(SERIAL_COM1, '[');
    serial_write_char(SERIAL_COM1, 'h');
    serial_write_char(SERIAL_COM1, ':');
    serial_write_char(SERIAL_COM1, c);
    serial_write_char(SERIAL_COM1, ']');
    serial_write_char(SERIAL_COM1, '\n');
}

static int help_is_kernel_ptr(const void* p) {
    extern uint32 __kernel_start;
    extern uint32 __kernel_end;
    uint32 v = (uint32)p;
    uint32 lo = (uint32)&__kernel_start;
    uint32 hi = (uint32)&__kernel_end;
    return (v >= lo && v < hi);
}

static size_t help_strnlen_kernel(const char* s, size_t limit) {
    if (!s) return 0;
    if (!help_is_kernel_ptr(s)) return 0;
    size_t n = 0;
    while (n < limit) {
        char c = s[n];
        if (c == '\0') return n;
        n++;
    }
    return limit;
}

void help_tui() {
    help_dbg_ch('E');
    // Defensive: ensure no shell redirection/capture is active before switching to GUI mode.
    // Some commands (like ls/read) run with redirect active in terminals; if a GUI opens mid-state,
    // guarantee a clean slate so GUI drawing is never captured or influenced by shell IO modes.
    extern int g_shell_capture_mode; // from vga.c
    extern int shell_redirect_active; // from vga.c
    if (shell_redirect_active) stop_shell_redirect();
    g_shell_capture_mode = 0;
    
    // Use or prepare the global, persistent help state (copies of command strings)
    help_dbg_ch('I');
    help_tui_init_state();
    help_dbg_ch('i');
    if (!g_sorted_cmds || g_cmd_count == 0) {
        printf("No commands available.\n");
        return;
    }

    // Use global values for selection and window geometry
    int selected = g_selected;
    int scroll = g_scroll;
    int selected_sub = g_selected_sub; // local selection for non-tiling
    int max_visible = MAX_VISIBLE;
    int win_height = max_visible + 3; // title + separator + list + bottom border

    tui_window_t left_win = {0, 0, CMD_LIST_WIDTH, win_height, "Commands", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};
    tui_window_t right_win = {CMD_LIST_WIDTH + 2, 0, DESC_WIDTH, win_height, "Description", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};

    // If tiling is active, run in GUI mode: register a GUI client for the focused tile
    if (tile_is_tiling_active()) {
        help_dbg_ch('G');
        // Publish shared arrays for GUI callbacks (they're prepared by init)
        g_selected = selected;
        g_selected_sub = 0;
        g_scroll = scroll;
        g_max_visible = max_visible;
        if (!g_expanded_commands) {
            g_expanded_commands = (int*)calloc(g_cmd_count, sizeof(int));
        }
        if (g_expanded_commands) {
            for (int i = 0; i < g_cmd_count; ++i) g_expanded_commands[i] = 0;
        }
        g_help_running = 1;

        int focused = tile_get_focused();
        tile_set_title_status(focused, "EYN-OS Help", NULL, NULL);
        tile_register_gui_client2(focused, help_gui_draw, help_gui_key, help_gui_mouse, NULL);
        help_dbg_ch('g');
        return;
    }

    // Fallback: non-tiling behavior (existing code path)
    char** cmd_names = NULL;
    while (1) {
        tui_clear();
        tui_draw_window(&left_win);
        tui_draw_window(&right_win);

        // Create command names with asterisks for those with sub-commands
        // Build transient view arrays for non-tiling TUI
        cmd_names = (char**)malloc(g_cmd_count * sizeof(char*));
        if (!cmd_names) {
            printf("Error: Memory allocation failed.\n");
            return;
        }
        for (int i = 0; i < g_cmd_count; ++i) {
            cmd_names[i] = (char*)g_sorted_cmds[i]->name;
        }

        tui_style_t norm_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
        tui_style_t sel_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
        tui_style_t sub_style = {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0};

        // Custom list drawing with collapsible sub-commands
        int max_visible_rows = left_win.height - 3;
        int display_y = 0;
        int items_above_scroll = 0;

        // First pass: count items above scroll position
        for (int i = 0; i < scroll; ++i) {
            items_above_scroll++;
            if (g_expanded_commands && g_expanded_commands[i] && has_subcommands(cmd_names[i])) {
                const subcommand_info_t* subcmd_list = get_subcommands(cmd_names[i]);
                if (subcmd_list) {
                    items_above_scroll += count_subcommands(subcmd_list);
                }
            }
        }

        // Second pass: draw visible items
        for (int i = scroll; i < g_cmd_count && display_y < max_visible_rows; ++i) {
            int y_pos = left_win.y + 2 + display_y;
            
            // Draw main command
            if (i == selected && selected_sub == 0) {
                tui_draw_text(left_win.x + 1, y_pos, "!", sel_style);
                tui_draw_text(left_win.x + 2, y_pos, cmd_names[i], norm_style);
                // Add asterisk if command has sub-commands
                if (has_subcommands(cmd_names[i])) {
                    tui_draw_text(left_win.x + 2 + strlen(cmd_names[i]), y_pos, " *", sel_style);
                }
            } else {
                tui_draw_text(left_win.x + 1, y_pos, cmd_names[i], norm_style);
                // Add asterisk if command has sub-commands
                if (has_subcommands(cmd_names[i])) {
                    tui_draw_text(left_win.x + 1 + strlen(cmd_names[i]), y_pos, " *", norm_style);
                }
            }
            display_y++;
            
            // Draw sub-commands if expanded
            if (g_expanded_commands && g_expanded_commands[i] && has_subcommands(cmd_names[i])) {
                const subcommand_info_t* subcmd_list = get_subcommands(cmd_names[i]);
                if (subcmd_list) {
                    int subcmd_count = count_subcommands(subcmd_list);
                    for (int j = 0; j < subcmd_count && display_y < max_visible_rows; ++j) {
                        int sub_y_pos = left_win.y + 2 + display_y;
                        char sub_line[64];
                        snprintf(sub_line, sizeof(sub_line), "  %s", subcmd_list[j].name);
                        
                        // Check if this sub-command is selected
                        if (i == selected && selected_sub == j + 1) {
                            tui_draw_text(left_win.x + 1, sub_y_pos, "!", sel_style);
                            tui_draw_text(left_win.x + 2, sub_y_pos, sub_line, sub_style);
                        } else {
                            tui_draw_text(left_win.x + 1, sub_y_pos, sub_line, sub_style);
                        }
                        display_y++;
                    }
                }
            }
        }

        char desc_buf[256] = "";
        
        // Check if we're selecting a sub-command
        if (selected_sub > 0 && g_expanded_commands && g_expanded_commands[selected] && has_subcommands(g_sorted_cmds[selected]->name)) {
            const subcommand_info_t* subcmd_list = get_subcommands(g_sorted_cmds[selected]->name);
            if (subcmd_list && selected_sub <= count_subcommands(subcmd_list)) {
                const subcommand_info_t* selected_subcmd = &subcmd_list[selected_sub - 1];
                strncat(desc_buf, selected_subcmd->description ? selected_subcmd->description : "No description available", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                if (selected_subcmd->usage) {
                    strncat(desc_buf, "Usage: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
                    strncat(desc_buf, selected_subcmd->usage, sizeof(desc_buf) - strlen(desc_buf) - 1);
                }
            }
        } else {
            // Show main command description
            if (g_sorted_cmds[selected]->description && g_sorted_cmds[selected]->description[0]) {
                strncat(desc_buf, g_sorted_cmds[selected]->description, sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
            if (g_sorted_cmds[selected]->example && g_sorted_cmds[selected]->example[0]) {
                strncat(desc_buf, "Example: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, g_sorted_cmds[selected]->example, sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
            
            // Add sub-command information if available
            if (has_subcommands(g_sorted_cmds[selected]->name)) {
                strncat(desc_buf, "\n\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                if (g_expanded_commands && g_expanded_commands[selected]) {
                    strncat(desc_buf, "Sub-commands are expanded.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                    strncat(desc_buf, "Press Enter to collapse.", sizeof(desc_buf) - strlen(desc_buf) - 1);
                } else {
                    strncat(desc_buf, "This command has sub-commands.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                    strncat(desc_buf, "Press Enter to expand.", sizeof(desc_buf) - strlen(desc_buf) - 1);
                }
            }
        }
        
        tui_draw_text_area(&right_win, desc_buf, 0, norm_style);

        tui_style_t status_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
        tui_draw_status_bar(NULL, "^/v: Move | Enter: Toggle | Ctrl+X: Exit", status_style);

        tui_refresh();
        int key = tui_read_key();
        if (key == 0x1001) { // Up
            if (selected_sub > 0) {
                // Move up within sub-commands
                selected_sub--;
            } else if (selected > 0) {
                // Move to previous main command
                selected--;
                selected_sub = 0;
                // Adjust scroll if needed
                if (selected < scroll) {
                    scroll = selected;
                }
            }
        } else if (key == 0x1002) { // Down
            // Check if we can move down within sub-commands
            if (g_expanded_commands && g_expanded_commands[selected] && has_subcommands(g_sorted_cmds[selected]->name)) {
                const subcommand_info_t* subcmd_list = get_subcommands(g_sorted_cmds[selected]->name);
                if (subcmd_list && selected_sub < count_subcommands(subcmd_list)) {
                    selected_sub++;
                } else if (selected < g_cmd_count - 1) {
                    // Move to next main command
                    selected++;
                    selected_sub = 0;
                    // Adjust scroll if needed
                    int total_items = 0;
                    for (int i = 0; i <= selected; ++i) {
                        total_items++;
                        if (g_expanded_commands && g_expanded_commands[i] && has_subcommands(g_sorted_cmds[i]->name)) {
                            const subcommand_info_t* subcmd_list2 = get_subcommands(g_sorted_cmds[i]->name);
                            if (subcmd_list2) {
                                total_items += count_subcommands(subcmd_list2);
                            }
                        }
                    }
                    if (total_items > scroll + max_visible) {
                        scroll = total_items - max_visible;
                    }
                }
            } else if (selected < g_cmd_count - 1) {
                // Move to next main command
                selected++;
                selected_sub = 0;
                // Adjust scroll if needed
                int total_items = 0;
                for (int i = 0; i <= selected; ++i) {
                    total_items++;
                    if (g_expanded_commands && g_expanded_commands[i] && has_subcommands(g_sorted_cmds[i]->name)) {
                        const subcommand_info_t* subcmd_list3 = get_subcommands(g_sorted_cmds[i]->name);
                        if (subcmd_list3) {
                            total_items += count_subcommands(subcmd_list3);
                        }
                    }
                }
                if (total_items > scroll + max_visible) {
                    scroll = total_items - max_visible;
                }
            }
        } else if (key == 0x2002) { // Ctrl+X
            break;
        } else if (key == '\n' || key == 13) {
            // Toggle sub-command expansion
            if (has_subcommands(g_sorted_cmds[selected]->name)) {
                if (!g_expanded_commands) g_expanded_commands = (int*)calloc(g_cmd_count, sizeof(int));
                g_expanded_commands[selected] = !g_expanded_commands[selected];
                selected_sub = 0; // Reset sub-command selection when toggling
            }
        }
    }

    // Clean up transient allocations used only in non-tiling mode
    free(cmd_names);
    printf("\n\n");
} 

// GUI draw callback: draw two panes inside the provided content rectangle (pixel coords)
void help_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    // Stash the rect for mouse hit-testing
    g_last_cx = content_x; g_last_cy = content_y; g_last_cw = content_w; g_last_ch = content_h;
    // Belt-and-suspenders: fully clear the content area first to avoid any residuals from prior
    // terminal rendering when opening Help after heavy output (e.g., ls). The tiler already clears
    // content, but double-clearing here is cheap and guarantees a clean slate inside the GUI draw.
    if (content_w > 0 && content_h > 0) {
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
    }
    // Convert pixels -> TUI grid (strictly within content rect)
    int cell_x = content_x / 8;
    int cell_y = content_y / 8;
    int cell_w = content_w / 8;
    int cell_h = content_h / 8;
    // Background: only clear the rows we'll redraw this pass.
    // Top subtitle row
    if (content_w > 0 && content_h > 0) {
        drawRect(content_x, content_y, content_w, 8, 0, 0, 0);
    }

    // Left pane width in chars
    int left_chars = CMD_LIST_WIDTH;
    if (left_chars + 4 >= cell_w) left_chars = (cell_w > 1) ? (cell_w / 2) : cell_w; // fallback & clamp

    // Use empty titles for underlying TUI windows (we draw our own subtitles inside content)
    tui_window_t left_win = {cell_x, cell_y + 1, left_chars, (cell_h > 1 ? cell_h - 1 : 0), "", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};
    int right_chars = cell_w - left_chars - 2; if (right_chars < 0) right_chars = 0;
    tui_window_t right_win = {cell_x + left_chars + 2, cell_y + 1, right_chars, (cell_h > 1 ? cell_h - 1 : 0), "", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};

    // Draw a single faint vertical separator between panes inside content
    int sep_x = content_x + left_chars * 8;
    int sep_h = content_h; if (sep_h < 0) sep_h = 0;
    if (sep_x >= content_x && sep_x < content_x + content_w) {
        int sep_w = 3; if (sep_x + sep_w > content_x + content_w) sep_w = (content_x + content_w) - sep_x;
        if (sep_w > 0) drawRect(sep_x, content_y, sep_w, sep_h, 96, 96, 96);
    }

    // Draw subtitle bars on the first 8px row inside the content
    int subtitle_y = content_y; // first row of content
    // Left subtitle
    int left_px_x = content_x;
    int left_px_w = left_chars * 8; if (left_px_w > content_w) left_px_w = content_w;
    if (left_px_w > 0) {
        drawRect(left_px_x, subtitle_y, left_px_w, 8, 64, 64, 64);
        const char* left_label = "Commands";
        for (int i = 0; left_label[i] && (8 + i * 8) < left_px_w - 1; ++i) {
            drawCharAt(left_px_x + 8 + i * 8, subtitle_y, (int)(unsigned char)left_label[i], 255, 255, 255);
        }
    }
    // Right subtitle
    int right_px_x = content_x + left_chars * 8 + 8; // a little padding after separator
    if (right_px_x < content_x) right_px_x = content_x;
    int right_px_w = content_w - (right_px_x - content_x);
    if (right_px_w > 0) {
        drawRect(right_px_x, subtitle_y, right_px_w, 8, 64, 64, 64);
        const char* right_label = "Description";
        for (int i = 0; right_label[i] && (8 + i * 8) < right_px_w - 1; ++i) {
            drawCharAt(right_px_x + 8 + i * 8, subtitle_y, (int)(unsigned char)right_label[i], 255, 255, 255);
        }
    }

    // Reuse non-GUI list/drawing code but based on global shared arrays
    tui_style_t norm_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
    tui_style_t sel_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
    tui_style_t sub_style = {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0};

    // Prepare names array (allocate once and grow as needed)
    static char** cmd_names = NULL;
    static int cmd_names_cap = 0;
    if (g_cmd_count > cmd_names_cap) {
        if (cmd_names) free(cmd_names);
        cmd_names = (char**)malloc(g_cmd_count * sizeof(char*));
        if (!cmd_names) {
            // Allocation failed; fall back to using pointers directly from g_sorted_cmds
            cmd_names_cap = 0;
        } else {
            cmd_names_cap = g_cmd_count;
        }
    }
    if (cmd_names) {
        for (int i = 0; i < g_cmd_count; ++i) cmd_names[i] = (char*)g_sorted_cmds[i]->name;
    }

    int max_visible = (left_win.height > 3) ? (left_win.height - 3) : 0;
    // Update the global max visible rows so key handler logic matches the drawn height
    g_max_visible = max_visible;
    int display_y = 0;
    // Draw list similar to earlier behavior
    // Before drawing text rows, clear just those text rows' pixel bands.
    // Clear the exact bands where list rows will be drawn. Our list starts at
    // left_win.y + 2 (to mimic tui window title+separator spacing), so base the
    // clear on left_win.y rather than raw cell_y to avoid off-by-one artifacts
    // that left stale characters behind when help opened after prior terminal output.
    for (int row_idx = 0; row_idx < max_visible; ++row_idx) {
        int y_band = (left_win.y + 2 + row_idx) * 8;
        if (y_band >= content_y && y_band + 8 <= content_y + content_h) {
            drawRect(content_x, y_band, content_w, 8, 0, 0, 0);
        }
    }

    for (int i = g_scroll; i < g_cmd_count && display_y < max_visible; ++i) {
        int y_pos = left_win.y + 2 + display_y;
        const char* cname = (cmd_names ? cmd_names[i] : g_sorted_cmds[i]->name);
        if (i == g_selected && g_selected_sub == 0) {
            tui_draw_text(left_win.x + 1, y_pos, "!", sel_style);
            tui_draw_text(left_win.x + 2, y_pos, cname, norm_style);
            if (has_subcommands(cname)) {
                tui_draw_text(left_win.x + 2 + strlen(cname), y_pos, " *", sel_style);
            }
        } else {
            tui_draw_text(left_win.x + 1, y_pos, cname, norm_style);
            if (has_subcommands(cname)) {
                tui_draw_text(left_win.x + 1 + strlen(cname), y_pos, " *", norm_style);
            }
        }
        display_y++;
        if (g_expanded_commands && i < g_cmd_count && g_expanded_commands[i] && has_subcommands(cname)) {
            const subcommand_info_t* subcmd_list = get_subcommands(cname);
            if (subcmd_list) {
                int subcmd_count = count_subcommands(subcmd_list);
                for (int j = 0; j < subcmd_count && display_y < max_visible; ++j) {
                    int sub_y_pos = left_win.y + 2 + display_y;
                    // Clear just this sub-row band before drawing
                    int py = sub_y_pos * 8;
                    if (py >= content_y && py + 8 <= content_y + content_h) drawRect(content_x, py, content_w, 8, 0, 0, 0);
                    char sub_line[64];
                    snprintf(sub_line, sizeof(sub_line), "  %s", subcmd_list[j].name);
                    if (i == g_selected && g_selected_sub == j + 1) {
                        tui_draw_text(left_win.x + 1, sub_y_pos, "!", sel_style);
                        tui_draw_text(left_win.x + 2, sub_y_pos, sub_line, sub_style);
                    } else {
                        tui_draw_text(left_win.x + 1, sub_y_pos, sub_line, sub_style);
                    }
                    display_y++;
                }
            }
        }
    }

    // Build description buffer based on selection
    char desc_buf[512] = "";
    if (g_selected_sub > 0 && g_expanded_commands && g_expanded_commands[g_selected] && has_subcommands(g_sorted_cmds[g_selected]->name)) {
        const subcommand_info_t* subcmd_list = get_subcommands(g_sorted_cmds[g_selected]->name);
        if (subcmd_list && g_selected_sub <= count_subcommands(subcmd_list)) {
            const subcommand_info_t* s = &subcmd_list[g_selected_sub - 1];
            strncat(desc_buf, s->description ? s->description : "No description available", sizeof(desc_buf) - strlen(desc_buf) - 1);
            strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
            if (s->usage) {
                strncat(desc_buf, "Usage: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, s->usage, sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
        }
    } else {
        if (g_sorted_cmds[g_selected]->description && g_sorted_cmds[g_selected]->description[0]) {
            strncat(desc_buf, g_sorted_cmds[g_selected]->description, sizeof(desc_buf) - strlen(desc_buf) - 1);
            strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
        }
        if (g_sorted_cmds[g_selected]->example && g_sorted_cmds[g_selected]->example[0]) {
            strncat(desc_buf, "Example: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
            strncat(desc_buf, g_sorted_cmds[g_selected]->example, sizeof(desc_buf) - strlen(desc_buf) - 1);
        }
        if (has_subcommands(g_sorted_cmds[g_selected]->name)) {
            strncat(desc_buf, "\n\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
            if (g_expanded_commands && g_expanded_commands[g_selected]) {
                strncat(desc_buf, "Sub-commands are expanded.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "Press Enter to collapse.", sizeof(desc_buf) - strlen(desc_buf) - 1);
            } else {
                strncat(desc_buf, "This command has sub-commands.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "Press Enter to expand.", sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
        }
    }

    // Before drawing the description text area, clear its visible bands using the
    // right window's geometry as the baseline, mirroring the left list clearing.
    {
        int max_desc = (right_win.height > 3) ? (right_win.height - 3) : 0;
        for (int row_idx = 0; row_idx < max_desc; ++row_idx) {
            int y_band = (right_win.y + 2 + row_idx) * 8;
            if (y_band >= content_y && y_band + 8 <= content_y + content_h) {
                // Clear only the right pane region
                int rx = right_win.x * 8;
                int rw = (right_win.width > 0 ? right_win.width * 8 : 0);
                if (rw > 0) drawRect(rx, y_band, rw, 8, 0, 0, 0);
            }
        }
    }
    tui_draw_text_area(&right_win, desc_buf, 0, norm_style);
    // Draw bottom status inside the tile content area
    const char* status_text = "^/v: Move | Enter: Toggle | Ctrl+X: Exit";
    int status_px_y = (cell_y + cell_h - 1) * 8; // bottom-most character row inside content
    int status_px_x = cell_x * 8;
    int status_px_w = cell_w * 8;
    // Clear and redraw the background bar to avoid leftovers when switching to help
    drawRect(status_px_x, status_px_y, status_px_w, 8, 0, 0, 0);
    // background bar
    drawRect(status_px_x, status_px_y, status_px_w, 8, 32, 32, 32);
    // draw status text clipped to content area
    int clip_min = status_px_x + 4;
    int clip_max = status_px_x + status_px_w - 4;
    for (int i = 0; status_text[i]; ++i) {
        int cx = status_px_x + i * 8 + 4;
        if (cx + 7 > clip_max) break;
        if (cx < clip_min) continue;
        drawCharAt(cx, status_px_y, (int)(unsigned char)status_text[i], 255, 255, 255);
    }
}

// Initialize help state (build sorted command pointers) without registering GUI.
// This makes persistent copies of command metadata and strings into a single
// string pool so help UI does not rely on pointers that could be relocated
// or otherwise invalidated by other subsystems (e.g., filesystem buffers).
void help_tui_init_state() {
    if (g_sorted_cmds) return; // already initialized
    help_dbg_ch('B');
    int cmd_count = (int)(__stop_shellcmds - __start_shellcmds);
    if (cmd_count <= 0) return;
    if (cmd_count > 256) {
        // Something is very wrong; refuse to allocate huge tables.
        help_dbg("init_state: insane cmd_count");
        return;
    }

    // Compute total string pool size (bounded, and only for sane kernel pointers)
    size_t pool_size = 0;
    for (const shell_command_info_t* cmd = __start_shellcmds; cmd < __stop_shellcmds; ++cmd) {
        size_t ln = help_strnlen_kernel(cmd->name, 64);
        size_t ld = help_strnlen_kernel(cmd->description, 256);
        size_t le = help_strnlen_kernel(cmd->example, 256);
        if (ln) pool_size += ln + 1;
        if (ld) pool_size += ld + 1;
        if (le) pool_size += le + 1;
    }

    help_dbg_ch('P');

    g_cmd_string_pool = (char*) malloc(pool_size ? pool_size : 1);
    if (!g_cmd_string_pool) return;
    help_dbg_ch('p');

    g_copied_cmds = (shell_command_info_t*) malloc(cmd_count * sizeof(shell_command_info_t));
    if (!g_copied_cmds) { free(g_cmd_string_pool); g_cmd_string_pool = NULL; return; }
    help_dbg_ch('c');

    // Fill copied structs and string pool
    char* cur = g_cmd_string_pool;
    char* end = g_cmd_string_pool + (pool_size ? pool_size : 1);
    int i = 0;
    for (const shell_command_info_t* cmd = __start_shellcmds; cmd < __stop_shellcmds; ++cmd) {
        // copy handler and type
        g_copied_cmds[i].handler = cmd->handler;
        g_copied_cmds[i].type = cmd->type;
        // name
        {
            size_t l = help_strnlen_kernel(cmd->name, 64);
            if (l && cur + l + 1 <= end) {
                memcpy(cur, cmd->name, l);
                cur[l] = '\0';
                g_copied_cmds[i].name = cur;
                cur += l + 1;
            } else {
                g_copied_cmds[i].name = "(invalid)";
            }
        }
        // description
        {
            size_t l = help_strnlen_kernel(cmd->description, 256);
            if (l && cur + l + 1 <= end) {
                memcpy(cur, cmd->description, l);
                cur[l] = '\0';
                g_copied_cmds[i].description = cur;
                cur += l + 1;
            } else {
                g_copied_cmds[i].description = "";
            }
        }
        // example
        {
            size_t l = help_strnlen_kernel(cmd->example, 256);
            if (l && cur + l + 1 <= end) {
                memcpy(cur, cmd->example, l);
                cur[l] = '\0';
                g_copied_cmds[i].example = cur;
                cur += l + 1;
            } else {
                g_copied_cmds[i].example = "";
            }
        }
        i++;
    }
    help_dbg_ch('S');

    // Create pointer array and sort it
    g_sorted_cmds = (const shell_command_info_t**) malloc(cmd_count * sizeof(const shell_command_info_t*));
    if (!g_sorted_cmds) {
        free(g_copied_cmds); g_copied_cmds = NULL; free(g_cmd_string_pool); g_cmd_string_pool = NULL; return;
    }
    help_dbg_ch('A');
    for (int k = 0; k < cmd_count; ++k) g_sorted_cmds[k] = &g_copied_cmds[k];

    if (cmd_count > 1) {
        for (int a = 0; a < cmd_count - 1; a++) {
            for (int j = 0; j < cmd_count - a - 1; j++) {
                const char* n0 = g_sorted_cmds[j]->name ? g_sorted_cmds[j]->name : "";
                const char* n1 = g_sorted_cmds[j + 1]->name ? g_sorted_cmds[j + 1]->name : "";
                if (strcmp(n0, n1) > 0) {
                    const shell_command_info_t* tmp = g_sorted_cmds[j];
                    g_sorted_cmds[j] = g_sorted_cmds[j + 1];
                    g_sorted_cmds[j + 1] = tmp;
                }
            }
        }
    }
    help_dbg_ch('s');

    g_cmd_count = cmd_count;
    if (g_expanded_commands) free(g_expanded_commands);
    g_expanded_commands = (int*) calloc(g_cmd_count, sizeof(int));
    help_dbg_ch('D');
}

// Show the pre-initialized help UI inside the currently focused tile (tiling must be active)
void help_tui_show() {
    if (!g_sorted_cmds || g_cmd_count == 0) return;
    if (!tile_is_tiling_active()) return;
    int focused = tile_get_focused();
    tile_set_title_status(focused, "EYN-OS Help", NULL, NULL);
    tile_register_gui_client2(focused, help_gui_draw, help_gui_key, help_gui_mouse, NULL);
    g_help_running = 1;
}

// GUI key handler: update selection state and stop on Ctrl+X
void help_gui_key(int tile_idx, int key, void* userdata) {
    if (key == 0x1001) { // Up
        if (g_selected_sub > 0) {
            g_selected_sub--;
        } else if (g_selected > 0) {
            g_selected--;
            g_selected_sub = 0;
            if (g_selected < g_scroll) g_scroll = g_selected;
        }
    } else if (key == 0x1002) { // Down
        if (g_expanded_commands && g_expanded_commands[g_selected] && has_subcommands(g_sorted_cmds[g_selected]->name)) {
            const subcommand_info_t* subcmd_list = get_subcommands(g_sorted_cmds[g_selected]->name);
            if (subcmd_list && g_selected_sub < count_subcommands(subcmd_list)) {
                g_selected_sub++;
            } else if (g_selected < g_cmd_count - 1) {
                g_selected++;
                g_selected_sub = 0;
                // adjust scroll simplistically
                if (g_selected > g_scroll + g_max_visible - 1) g_scroll = g_selected - g_max_visible + 1;
            }
        } else if (g_selected < g_cmd_count - 1) {
            g_selected++;
            g_selected_sub = 0;
            if (g_selected > g_scroll + g_max_visible - 1) g_scroll = g_selected - g_max_visible + 1;
        }
    } else if (key == 0x2002) { // Ctrl+X
        // Clean up GUI mode: unregister and free resources
        int focused = tile_get_focused();
        tile_unregister_gui_client(focused);
        if (g_sorted_cmds) { free((void*)g_sorted_cmds); g_sorted_cmds = NULL; }
        if (g_copied_cmds) { free(g_copied_cmds); g_copied_cmds = NULL; }
        if (g_cmd_string_pool) { free(g_cmd_string_pool); g_cmd_string_pool = NULL; }
        if (g_expanded_commands) { free(g_expanded_commands); g_expanded_commands = NULL; }
        g_cmd_count = 0;
        g_help_running = 0;
    } else if (key == '\n' || key == 13) {
        if (has_subcommands(g_sorted_cmds[g_selected]->name)) {
            /* Ensure g_selected is within valid bounds before toggling. If the
             * expanded array is missing, allocate it so toggles are safe. */
            if (g_selected >= 0 && g_selected < g_cmd_count && g_cmd_count > 0) {
                if (!g_expanded_commands) {
                    g_expanded_commands = (int*) calloc(g_cmd_count, sizeof(int));
                }
                if (g_expanded_commands) {
                    g_expanded_commands[g_selected] = !g_expanded_commands[g_selected];
                }
            } else {
                // Out-of-range selection or no commands: ignore toggle safely.
            }
            g_selected_sub = 0;
        }
    }
}

// Mouse: wheel scrolls the left list; left-click selects a row in the left pane
void help_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata) {
    (void)tile_idx; (void)userdata;
    if (!g_sorted_cmds || g_cmd_count <= 0) return;
    // Convert to cell coords
    int cell_x = g_last_cx / 8;
    int cell_y = g_last_cy / 8;
    int cell_w = g_last_cw / 8;
    int cell_h = g_last_ch / 8;
    if (cell_w <= 0 || cell_h <= 0) return;
    int left_chars = CMD_LIST_WIDTH;
    if (left_chars + 4 >= cell_w) left_chars = (cell_w > 1) ? (cell_w / 2) : cell_w;
    int click_cx = me->x / 8;
    int click_cy = me->y / 8;
    // Wheel: adjust scroll
    if (me->wheel_delta != 0) {
        int delta = me->wheel_delta;
        int ns = g_scroll + (delta < 0 ? -1 : 1); // wheel up = scroll up
        if (ns < 0) ns = 0;
        // compute a rough maximum scroll: limit so at least one item remains visible
        int max_vis = (cell_h > 3) ? (cell_h - 3) : cell_h;
        int max_scroll = (g_cmd_count > max_vis) ? (g_cmd_count - max_vis) : 0;
        if (ns > max_scroll) ns = max_scroll;
        if (ns != g_scroll) g_scroll = ns;
        return;
    }
    // Left-click selection in left pane list
    uint8 left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    if (!left_down) return;
    // Window geometry from draw: left window starts at (cell_x, cell_y+1), list rows start at y+2
    int list_x0 = cell_x + 1;
    int list_x1 = cell_x + left_chars - 2;
    int list_y0 = cell_y + 1 + 2;
    if (click_cx >= list_x0 && click_cx <= list_x1 && click_cy >= list_y0) {
        int row = click_cy - list_y0; // 0-based visible row index
        int idx = g_scroll + row;
        if (idx < 0) idx = 0;
        if (idx >= g_cmd_count) idx = g_cmd_count - 1;
        g_selected = idx;
        g_selected_sub = 0;
        // keep selection in view
        if (g_selected < g_scroll) g_scroll = g_selected;
        int max_visible = (cell_h > 3) ? (cell_h - 3) : 1;
        if (g_selected > g_scroll + max_visible - 1) g_scroll = g_selected - max_visible + 1;
    }
}