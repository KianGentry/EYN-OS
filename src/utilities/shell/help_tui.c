#include <tui.h>
#include <shell_command_info.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <string.h>
#include <tile_manager.h>

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
static int g_expanded_commands[128] = {0};
static volatile int g_help_running = 0;

// Forward declarations for GUI callbacks
static void help_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata);
static void help_gui_key(int tile_idx, int key, void* userdata);

void help_tui() {
    extern const shell_command_info_t __start_shellcmds[];
    extern const shell_command_info_t __stop_shellcmds[];
    
    // Count commands
    int cmd_count = 0;
    for (const shell_command_info_t* cmd = __start_shellcmds; cmd < __stop_shellcmds; ++cmd) {
        cmd_count++;
    }
    
    if (cmd_count == 0) {
        printf("No commands available.\n");
        return;
    }
    
    // Create sorted array of command pointers
    const shell_command_info_t** sorted_cmds = (const shell_command_info_t**) malloc(cmd_count * sizeof(const shell_command_info_t*));
    if (!sorted_cmds) {
        printf("Error: Memory allocation failed.\n");
        return;
    }
    
    // Fill array with command pointers
    int idx = 0;
    for (const shell_command_info_t* cmd = __start_shellcmds; cmd < __stop_shellcmds; ++cmd) {
        sorted_cmds[idx++] = cmd;
    }
    
    // Sort commands alphabetically by name
    if (cmd_count > 1) {
        for (int i = 0; i < cmd_count - 1; i++) {
            for (int j = 0; j < cmd_count - i - 1; j++) {
                if (strcmp(sorted_cmds[j]->name, sorted_cmds[j + 1]->name) > 0) {
                    const shell_command_info_t* temp = sorted_cmds[j];
                    sorted_cmds[j] = sorted_cmds[j + 1];
                    sorted_cmds[j + 1] = temp;
                }
            }
        }
    }
    
    int selected = 0;
    int scroll = 0;
    int max_visible = MAX_VISIBLE;
    int win_height = max_visible + 3; // title + separator + list + bottom border

    tui_window_t left_win = {0, 0, CMD_LIST_WIDTH, win_height, "Commands", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};
    tui_window_t right_win = {CMD_LIST_WIDTH + 2, 0, DESC_WIDTH, win_height, "Description", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};

    // Track which commands are expanded
    static int expanded_commands[128] = {0};
    
    // Track selection state (0 = main command, 1+ = sub-command index)
    int selected_sub = 0;

    // If tiling is active, run in GUI mode: register a GUI client for the focused tile
    if (tile_is_tiling_active()) {
        // Publish shared arrays for GUI callbacks
        g_sorted_cmds = sorted_cmds;
        g_cmd_count = cmd_count;
        g_selected = selected;
        g_selected_sub = 0;
        g_scroll = scroll;
        g_max_visible = max_visible;
        for (int i = 0; i < 128; ++i) g_expanded_commands[i] = 0;
        g_help_running = 1;

        int focused = tile_get_focused();
        // Set the tile title so the tile shows it's the Help UI. Do NOT set a tile-level status
        // because the GUI will draw its own bottom status bar inside the content area.
        tile_set_title_status(focused, "EYN-OS Help", NULL, NULL);
        tile_register_gui_client(focused, help_gui_draw, help_gui_key, NULL);
        // Return immediately; the tiling manager will drive drawing and input via our callbacks.
        return;
    }

    // Fallback: non-tiling behavior (existing code path)
    while (1) {
        tui_clear();
        tui_draw_window(&left_win);
        tui_draw_window(&right_win);

        // Create command names with asterisks for those with sub-commands
        static char* cmd_names[128];
        for (int i = 0; i < cmd_count; ++i) {
            cmd_names[i] = (char*)sorted_cmds[i]->name;
        }

        tui_style_t norm_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
        tui_style_t sel_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
        tui_style_t sub_style = {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0};

        // Custom list drawing with collapsible sub-commands
        int max_visible = left_win.height - 3;
        int display_y = 0;
        int items_above_scroll = 0;

        // First pass: count items above scroll position
        for (int i = 0; i < scroll; ++i) {
            items_above_scroll++;
            if (expanded_commands[i] && has_subcommands(cmd_names[i])) {
                const subcommand_info_t* subcmds = get_subcommands(cmd_names[i]);
                if (subcmds) {
                    items_above_scroll += count_subcommands(subcmds);
                }
            }
        }

        // Second pass: draw visible items
        for (int i = scroll; i < cmd_count && display_y < max_visible; ++i) {
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
            if (expanded_commands[i] && has_subcommands(cmd_names[i])) {
                const subcommand_info_t* subcmds = get_subcommands(cmd_names[i]);
                if (subcmds) {
                    int subcmd_count = count_subcommands(subcmds);
                    for (int j = 0; j < subcmd_count && display_y < max_visible; ++j) {
                        int sub_y_pos = left_win.y + 2 + display_y;
                        char sub_line[64];
                        snprintf(sub_line, sizeof(sub_line), "  %s", subcmds[j].name);
                        
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
        if (selected_sub > 0 && expanded_commands[selected] && has_subcommands(sorted_cmds[selected]->name)) {
            const subcommand_info_t* subcmds = get_subcommands(sorted_cmds[selected]->name);
            if (subcmds && selected_sub <= count_subcommands(subcmds)) {
                const subcommand_info_t* selected_subcmd = &subcmds[selected_sub - 1];
                strncat(desc_buf, selected_subcmd->description ? selected_subcmd->description : "No description available", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                if (selected_subcmd->usage) {
                    strncat(desc_buf, "Usage: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
                    strncat(desc_buf, selected_subcmd->usage, sizeof(desc_buf) - strlen(desc_buf) - 1);
                }
            }
        } else {
            // Show main command description
            if (sorted_cmds[selected]->description && sorted_cmds[selected]->description[0]) {
                strncat(desc_buf, sorted_cmds[selected]->description, sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
            if (sorted_cmds[selected]->example && sorted_cmds[selected]->example[0]) {
                strncat(desc_buf, "Example: ", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, sorted_cmds[selected]->example, sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
            
            // Add sub-command information if available
            if (has_subcommands(sorted_cmds[selected]->name)) {
                strncat(desc_buf, "\n\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                if (expanded_commands[selected]) {
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
            if (expanded_commands[selected] && has_subcommands(sorted_cmds[selected]->name)) {
                const subcommand_info_t* subcmds = get_subcommands(sorted_cmds[selected]->name);
                if (subcmds && selected_sub < count_subcommands(subcmds)) {
                    selected_sub++;
                } else if (selected < cmd_count - 1) {
                    // Move to next main command
                    selected++;
                    selected_sub = 0;
                    // Adjust scroll if needed
                    int total_items = 0;
                    for (int i = 0; i <= selected; ++i) {
                        total_items++;
                        if (expanded_commands[i] && has_subcommands(sorted_cmds[i]->name)) {
                            const subcommand_info_t* subcmds = get_subcommands(sorted_cmds[i]->name);
                            if (subcmds) {
                                total_items += count_subcommands(subcmds);
                            }
                        }
                    }
                    if (total_items > scroll + max_visible) {
                        scroll = total_items - max_visible;
                    }
                }
            } else if (selected < cmd_count - 1) {
                // Move to next main command
                selected++;
                selected_sub = 0;
                // Adjust scroll if needed
                int total_items = 0;
                for (int i = 0; i <= selected; ++i) {
                    total_items++;
                    if (expanded_commands[i] && has_subcommands(sorted_cmds[i]->name)) {
                        const subcommand_info_t* subcmds = get_subcommands(sorted_cmds[i]->name);
                        if (subcmds) {
                            total_items += count_subcommands(subcmds);
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
            if (has_subcommands(sorted_cmds[selected]->name)) {
                expanded_commands[selected] = !expanded_commands[selected];
                selected_sub = 0; // Reset sub-command selection when toggling
            }
        }
    }

    // Clean up
    free(sorted_cmds);
    printf("\n\n");
} 

// GUI draw callback: draw two panes inside the provided content rectangle (pixel coords)
static void help_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    // Convert pixels -> TUI grid
    int cell_x = content_x / 8;
    int cell_y = content_y / 8;
    int cell_w = content_w / 8;
    int cell_h = content_h / 8;

    // Left pane width in chars
    int left_chars = CMD_LIST_WIDTH;
    if (left_chars + 4 >= cell_w) left_chars = cell_w / 2; // fallback

    // Use empty titles for the underlying TUI windows so we can render our own subtitles
    tui_window_t left_win = {cell_x, cell_y, left_chars, cell_h, "", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};
    tui_window_t right_win = {cell_x + left_chars + 2, cell_y, cell_w - left_chars - 2, cell_h, "", {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1}, {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0}, {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}};

    // Draw a cleared pixel area including the subtitle row so underlying graphics don't show through.
    // Position the subtitle so its top touches the titlebar: subtitle_top = content_y - 2
    int subtitle_top = content_y - 2;
    if (subtitle_top < content_y - 8) subtitle_top = content_y - 8; // safety clamp
    drawRect(content_x, subtitle_top, content_w, content_h + 2 + (content_y - subtitle_top), 0, 0, 0);

    // Draw only a single faint vertical separator between the panes (no other window borders)
    int sep_x = content_x + left_chars * 8; // separator between left and right panes
    // make separator slightly wider to be more visible
    drawRect(sep_x, subtitle_top, 3, content_h + 2 + (content_y - subtitle_top), 96, 96, 96);

    // Draw subtitle bars that sit directly under the titlebar
    int left_px_x = content_x;
    int left_px_y = subtitle_top;
    int left_px_w = left_chars * 8;
    drawRect(left_px_x, left_px_y, left_px_w, 8, 64, 64, 64);
    const char* left_label = "Commands";
    for (int i = 0; left_label[i]; ++i) {
        drawCharAt(left_px_x + 8 + i * 8, left_px_y, (int)(unsigned char)left_label[i], 255, 255, 255);
    }

    int right_px_x = content_x + left_chars * 8 + 8; // a little padding after separator
    int right_px_y = subtitle_top;
    int right_px_w = content_w - left_chars * 8 - 8;
    drawRect(right_px_x, right_px_y, right_px_w, 8, 64, 64, 64);
    const char* right_label = "Description";
    for (int i = 0; right_label[i]; ++i) {
        drawCharAt(right_px_x + 8 + i * 8, right_px_y, (int)(unsigned char)right_label[i], 255, 255, 255);
    }

    // (single separator already drawn above spanning subtitle+content)

    // Reuse non-GUI list/drawing code but based on global shared arrays
    tui_style_t norm_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
    tui_style_t sel_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
    tui_style_t sub_style = {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0};

    // Prepare names array
    static char* cmd_names[128];
    for (int i = 0; i < g_cmd_count; ++i) cmd_names[i] = (char*)g_sorted_cmds[i]->name;

    int max_visible = left_win.height - 3;
    // Update the global max visible rows so key handler logic matches the drawn height
    g_max_visible = max_visible;
    int display_y = 0;
    // Draw list similar to earlier behavior
    for (int i = g_scroll; i < g_cmd_count && display_y < max_visible; ++i) {
        int y_pos = left_win.y + 2 + display_y;
        if (i == g_selected && g_selected_sub == 0) {
            tui_draw_text(left_win.x + 1, y_pos, "!", sel_style);
            tui_draw_text(left_win.x + 2, y_pos, cmd_names[i], norm_style);
            if (has_subcommands(cmd_names[i])) {
                tui_draw_text(left_win.x + 2 + strlen(cmd_names[i]), y_pos, " *", sel_style);
            }
        } else {
            tui_draw_text(left_win.x + 1, y_pos, cmd_names[i], norm_style);
            if (has_subcommands(cmd_names[i])) {
                tui_draw_text(left_win.x + 1 + strlen(cmd_names[i]), y_pos, " *", norm_style);
            }
        }
        display_y++;
        if (g_expanded_commands[i] && has_subcommands(cmd_names[i])) {
            const subcommand_info_t* subcmds = get_subcommands(cmd_names[i]);
            if (subcmds) {
                int subcmd_count = count_subcommands(subcmds);
                for (int j = 0; j < subcmd_count && display_y < max_visible; ++j) {
                    int sub_y_pos = left_win.y + 2 + display_y;
                    char sub_line[64];
                    snprintf(sub_line, sizeof(sub_line), "  %s", subcmds[j].name);
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
    if (g_selected_sub > 0 && g_expanded_commands[g_selected] && has_subcommands(g_sorted_cmds[g_selected]->name)) {
        const subcommand_info_t* subcmds = get_subcommands(g_sorted_cmds[g_selected]->name);
        if (subcmds && g_selected_sub <= count_subcommands(subcmds)) {
            const subcommand_info_t* s = &subcmds[g_selected_sub - 1];
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
            if (g_expanded_commands[g_selected]) {
                strncat(desc_buf, "Sub-commands are expanded.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "Press Enter to collapse.", sizeof(desc_buf) - strlen(desc_buf) - 1);
            } else {
                strncat(desc_buf, "This command has sub-commands.\n", sizeof(desc_buf) - strlen(desc_buf) - 1);
                strncat(desc_buf, "Press Enter to expand.", sizeof(desc_buf) - strlen(desc_buf) - 1);
            }
        }
    }

    tui_draw_text_area(&right_win, desc_buf, 0, norm_style);
    // Draw bottom status inside the tile content area
    const char* status_text = "^/v: Move | Enter: Toggle | Ctrl+X: Exit";
    int status_px_y = (cell_y + cell_h - 1) * 8; // bottom-most character row inside content
    int status_px_x = cell_x * 8;
    int status_px_w = cell_w * 8;
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

// GUI key handler: update selection state and stop on Ctrl+X
static void help_gui_key(int tile_idx, int key, void* userdata) {
    if (key == 0x1001) { // Up
        if (g_selected_sub > 0) {
            g_selected_sub--;
        } else if (g_selected > 0) {
            g_selected--;
            g_selected_sub = 0;
            if (g_selected < g_scroll) g_scroll = g_selected;
        }
    } else if (key == 0x1002) { // Down
        if (g_expanded_commands[g_selected] && has_subcommands(g_sorted_cmds[g_selected]->name)) {
            const subcommand_info_t* subcmds = get_subcommands(g_sorted_cmds[g_selected]->name);
            if (subcmds && g_selected_sub < count_subcommands(subcmds)) {
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
        if (g_sorted_cmds) free((void*)g_sorted_cmds);
        g_sorted_cmds = NULL;
        g_cmd_count = 0;
        g_help_running = 0;
    } else if (key == '\n' || key == 13) {
        if (has_subcommands(g_sorted_cmds[g_selected]->name)) {
            g_expanded_commands[g_selected] = !g_expanded_commands[g_selected];
            g_selected_sub = 0;
        }
    }
}