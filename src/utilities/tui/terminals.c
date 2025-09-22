#include <types.h>
#include <vga.h>
#include <string.h>
#include <stdlib.h>
#include <shell.h>
#include <stdint.h>

// shell_current_path is maintained by the main shell code
extern char shell_current_path[128];

// forward decl so prompt printer can call it before it's defined
void vterm_write_char(int idx, char ch);

#define TERM_COLS 80
#define TERM_ROWS 60

#define INPUT_BUF_LEN 200
typedef struct {
    char buf[TERM_ROWS][TERM_COLS+1];
    int cur_x;
    int cur_y;
    int scroll;
    int active;
    // input line buffer and history index for this virtual terminal
    char input_buf[INPUT_BUF_LEN];
    int input_pos;
    int history_idx; // -1 when not browsing
    // saved input when browsing history
    char saved_input[INPUT_BUF_LEN];
    // per-vterm current working directory (isolate cd per terminal)
    char cwd[128];
    // per-line and per-char color metadata for rendering
    uint8_t line_r[TERM_ROWS];
    uint8_t line_g[TERM_ROWS];
    uint8_t line_b[TERM_ROWS];
    // per-character color (row x col)
    uint8_t char_r[TERM_ROWS][TERM_COLS];
    uint8_t char_g[TERM_ROWS][TERM_COLS];
    uint8_t char_b[TERM_ROWS][TERM_COLS];
} vterm_t;

static vterm_t vterms[4];

void vterm_init_all() {
    for (int i = 0; i < 4; ++i) {
        vterms[i].cur_x = 0;
        vterms[i].cur_y = 0;
        vterms[i].scroll = 0;
        vterms[i].active = 0;
        vterms[i].input_buf[0] = '\0';
        vterms[i].input_pos = 0;
        vterms[i].history_idx = -1;
        // default cwd is root
        strncpy(vterms[i].cwd, "/", sizeof(vterms[i].cwd)-1);
        vterms[i].cwd[sizeof(vterms[i].cwd)-1] = '\0';
        for (int r = 0; r < TERM_ROWS; ++r) {
            vterms[i].buf[r][0] = '\0';
            vterms[i].line_r[r] = 200;
            vterms[i].line_g[r] = 200;
            vterms[i].line_b[r] = 200;
            for (int c = 0; c < TERM_COLS; ++c) {
                vterms[i].char_r[r][c] = 200;
                vterms[i].char_g[r][c] = 200;
                vterms[i].char_b[r][c] = 200;
            }
        }
    }
}

    // Print the standard shell prompt into the vterm (drive:path! )
    void vterm_print_prompt(int idx) {
        if (idx < 0 || idx >= 4) return;
        vterm_t* t = &vterms[idx];
        /* Print prompt at the current cursor position */
        extern uint8_t g_current_drive;
        extern uint8 ata_physical_to_logical(uint8 physical_drive);
        uint8 logical = ata_physical_to_logical(g_current_drive);
        char prompt[64];
        int n = snprintf(prompt, sizeof(prompt), "%d:%s! ", logical, t->cwd);
        for (int i = 0; i < n; ++i) {
            // write the char then override its color to match prompt styling
            vterm_write_char(idx, prompt[i]);
            // color drive and leading ':' and '/' as gray, '!' as yellow, rest default
            int drive_and_sep_len = 2; // e.g. "0:"
            // find position of '!' in prompt
            int bang_pos = -1;
            for (int j = 0; j < n; ++j) if (prompt[j] == '!') { bang_pos = j; break; }
            // determine color for this char
            int rr = 200, gg = 200, bb = 200;
            if (i < drive_and_sep_len) { rr = 150; gg = 150; bb = 150; }
            if (i == bang_pos) { rr = 255; gg = 255; bb = 0; }
            // set color for the character we just wrote (cur_x was incremented)
            int written_row = t->cur_y;
            int written_col = t->cur_x - 1;
            if (written_row >= 0 && written_row < TERM_ROWS && written_col >= 0 && written_col < TERM_COLS) {
                t->char_r[written_row][written_col] = rr;
                t->char_g[written_row][written_col] = gg;
                t->char_b[written_row][written_col] = bb;
            }
        }
    }

void vterm_write_char(int idx, char ch) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;
    if (ch == '\n') {
        t->cur_x = 0;
        t->cur_y++;
        if (t->cur_y >= TERM_ROWS) {
            // scroll up: shift lines and color metadata
            for (int r = 1; r < TERM_ROWS; ++r) {
                strcpy(t->buf[r-1], t->buf[r]);
                for (int c = 0; c < TERM_COLS; ++c) {
                    t->char_r[r-1][c] = t->char_r[r][c];
                    t->char_g[r-1][c] = t->char_g[r][c];
                    t->char_b[r-1][c] = t->char_b[r][c];
                }
                t->line_r[r-1] = t->line_r[r];
                t->line_g[r-1] = t->line_g[r];
                t->line_b[r-1] = t->line_b[r];
            }
            // clear last line
            t->buf[TERM_ROWS-1][0] = '\0';
            for (int c = 0; c < TERM_COLS; ++c) {
                t->char_r[TERM_ROWS-1][c] = 200;
                t->char_g[TERM_ROWS-1][c] = 200;
                t->char_b[TERM_ROWS-1][c] = 200;
            }
            t->line_r[TERM_ROWS-1] = 200;
            t->line_g[TERM_ROWS-1] = 200;
            t->line_b[TERM_ROWS-1] = 200;
            t->cur_y = TERM_ROWS-1;
        }
        /* mark the new (now-current) line with default color */
        t->line_r[t->cur_y] = 200;
        t->line_g[t->cur_y] = 200;
        t->line_b[t->cur_y] = 200;
        return;
    }
    if (ch == '\r') return;
    if (t->cur_x >= TERM_COLS) {
        t->cur_x = 0;
        t->cur_y++;
        if (t->cur_y >= TERM_ROWS) {
            for (int r=1;r<TERM_ROWS;++r) {
                strcpy(t->buf[r-1], t->buf[r]);
                for (int c=0;c<TERM_COLS;++c) {
                    t->char_r[r-1][c] = t->char_r[r][c];
                    t->char_g[r-1][c] = t->char_g[r][c];
                    t->char_b[r-1][c] = t->char_b[r][c];
                }
                t->line_r[r-1] = t->line_r[r];
                t->line_g[r-1] = t->line_g[r];
                t->line_b[r-1] = t->line_b[r];
            }
            t->buf[TERM_ROWS-1][0] = '\0';
            for (int c=0;c<TERM_COLS;++c) {
                t->char_r[TERM_ROWS-1][c] = 200;
                t->char_g[TERM_ROWS-1][c] = 200;
                t->char_b[TERM_ROWS-1][c] = 200;
            }
            t->line_r[TERM_ROWS-1] = 200;
            t->line_g[TERM_ROWS-1] = 200;
            t->line_b[TERM_ROWS-1] = 200;
            t->cur_y = TERM_ROWS-1;
        }
    }
    int len = strlen(t->buf[t->cur_y]);
    if (t->cur_x >= len) t->buf[t->cur_y][t->cur_x] = ch;
    else t->buf[t->cur_y][t->cur_x] = ch;
    t->buf[t->cur_y][t->cur_x+1] = '\0';
    /* set per-char color from current redirect color if available (non-zero), else default */
    extern int shell_redirect_color_r;
    extern int shell_redirect_color_g;
    extern int shell_redirect_color_b;
    int use_r = (shell_redirect_color_r > 0) ? shell_redirect_color_r : 200;
    int use_g = (shell_redirect_color_g > 0) ? shell_redirect_color_g : 200;
    int use_b = (shell_redirect_color_b > 0) ? shell_redirect_color_b : 200;
    t->char_r[t->cur_y][t->cur_x] = use_r;
    t->char_g[t->cur_y][t->cur_x] = use_g;
    t->char_b[t->cur_y][t->cur_x] = use_b;
    t->cur_x++;
    /* ensure this line has a sensible default color if not already set */
    if (t->line_r[t->cur_y] == 0 && t->line_g[t->cur_y] == 0 && t->line_b[t->cur_y] == 0) {
        t->line_r[t->cur_y] = 200;
        t->line_g[t->cur_y] = 200;
        t->line_b[t->cur_y] = 200;
    }
}

const char* vterm_get_line(int idx, int row) {
    if (idx < 0 || idx >= 4) return "";
    if (row < 0 || row >= TERM_ROWS) return "";
    return vterms[idx].buf[row];
}

void vterm_feed_input(int idx, int key) {
    // backcompat: simple feed (kept for other users)
    if (key >= 32 && key <= 126) vterm_write_char(idx, (char)key);
    else if (key == '\n' || key == 10) vterm_write_char(idx, '\n');
}

// Helper to append a line to a vterm (used when executing commands)
static void vterm_append_line(int idx, const char* line) {
    if (!line) return;
    vterm_t* t = &vterms[idx];
    // Break line into TERM_COLS chunks
    int len = strlen(line);
    int p = 0;
    while (p < len) {
        int copy = TERM_COLS;
        if (p + copy > len) copy = len - p;
        // shift if at bottom
        if (t->cur_y >= TERM_ROWS) {
            for (int r=1;r<TERM_ROWS;++r) strcpy(t->buf[r-1], t->buf[r]);
            t->buf[TERM_ROWS-1][0] = '\0';
            t->cur_y = TERM_ROWS-1;
        }
        strncpy(t->buf[t->cur_y], line + p, copy);
        t->buf[t->cur_y][copy] = '\0';
        /* Use the current shell redirect color if available; fall back to default */
        extern int shell_redirect_color_r;
        extern int shell_redirect_color_g;
        extern int shell_redirect_color_b;
        int rr = shell_redirect_color_r ? shell_redirect_color_r : 200;
        int gg = shell_redirect_color_g ? shell_redirect_color_g : 200;
        int bb = shell_redirect_color_b ? shell_redirect_color_b : 200;
        t->line_r[t->cur_y] = rr;
        t->line_g[t->cur_y] = gg;
        t->line_b[t->cur_y] = bb;
        for (int c = 0; c < copy; ++c) {
            t->char_r[t->cur_y][c] = rr;
            t->char_g[t->cur_y][c] = gg;
            t->char_b[t->cur_y][c] = bb;
        }
        for (int c = copy; c < TERM_COLS; ++c) {
            t->char_r[t->cur_y][c] = 200;
            t->char_g[t->cur_y][c] = 200;
            t->char_b[t->cur_y][c] = 200;
        }
        t->cur_x = copy;
        t->cur_y++;
        p += copy;
    }
}

    void vterm_clear(int idx) {
        if (idx < 0 || idx >= 4) return;
        vterm_t* t = &vterms[idx];
        for (int r = 0; r < TERM_ROWS; ++r) {
            t->buf[r][0] = '\0';
            t->line_r[r] = 200; t->line_g[r] = 200; t->line_b[r] = 200;
            for (int c = 0; c < TERM_COLS; ++c) {
                t->char_r[r][c] = 200; t->char_g[r][c] = 200; t->char_b[r][c] = 200;
            }
        }
        t->cur_x = 0;
        t->cur_y = 0;
        t->scroll = 0;
        t->input_buf[0] = '\0';
        t->input_pos = 0;
    }

// Each vterm can accept full key handling similar to readStr_with_history: editing, history, enter to execute
void vterm_handle_key(int idx, int key) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;

    // Handle arrow keys: Up/Down for history, Left/Right for cursor movement
    if (key == 0x1001) { // Up
        if (g_command_history.count > 0) {
            if (t->history_idx == -1) {
                // first time browsing - save current input
                strncpy(t->saved_input, t->input_buf, INPUT_BUF_LEN);
                t->history_idx = g_command_history.count - 1;
            } else if (t->history_idx > 0) {
                t->history_idx--;
            }
            // clear current displayed input
            while (t->input_pos > 0) {
                t->input_pos--;
                if (t->cur_x > 0) {
                    t->cur_x--;
                    t->buf[t->cur_y][t->cur_x] = '\0';
                }
            }
            // load history entry (per-vterm browsing)
            strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN);
            t->input_pos = strlen(t->input_buf);
            // echo the history entry and ensure echoed chars use default color
            for (int i = 0; i < t->input_pos; ++i) {
                vterm_write_char(idx, t->input_buf[i]);
                int written_row = t->cur_y;
                int written_col = t->cur_x - 1;
                if (written_row >= 0 && written_row < TERM_ROWS && written_col >= 0 && written_col < TERM_COLS) {
                    t->char_r[written_row][written_col] = 200;
                    t->char_g[written_row][written_col] = 200;
                    t->char_b[written_row][written_col] = 200;
                }
            }
        }
        return;
    }
    if (key == 0x1002) { // Down
        if (t->history_idx != -1) {
            t->history_idx++;
            // clear current displayed input
            while (t->input_pos > 0) {
                t->input_pos--;
                if (t->cur_x > 0) {
                    t->cur_x--;
                    t->buf[t->cur_y][t->cur_x] = '\0';
                }
            }
            if (t->history_idx >= g_command_history.count) {
                // restore saved input
                strncpy(t->input_buf, t->saved_input, INPUT_BUF_LEN);
                t->input_pos = strlen(t->input_buf);
                for (int i = 0; i < t->input_pos; ++i) {
                    vterm_write_char(idx, t->input_buf[i]);
                    int written_row = t->cur_y;
                    int written_col = t->cur_x - 1;
                    if (written_row >= 0 && written_row < TERM_ROWS && written_col >= 0 && written_col < TERM_COLS) {
                        t->char_r[written_row][written_col] = 200;
                        t->char_g[written_row][written_col] = 200;
                        t->char_b[written_row][written_col] = 200;
                    }
                }
                t->history_idx = -1;
                } else {
                // load next history entry
                strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN);
                t->input_pos = strlen(t->input_buf);
                for (int i = 0; i < t->input_pos; ++i) vterm_write_char(idx, t->input_buf[i]);
            }
        }
        return;
    }
    if (key == 0x1003) { // Left
        if (t->input_pos > 0) {
            // move cursor left logically
            if (t->cur_x > 0) t->cur_x--;
            t->input_pos--;
        }
        return;
    }
    if (key == 0x1004) { // Right
        int len = strlen(t->input_buf);
        if (t->input_pos < len) {
            vterm_write_char(idx, t->input_buf[t->input_pos]);
            t->input_pos++;
        }
        return;
    }

    // Printable
    if (key >= 32 && key <= 126) {
        if (t->input_pos < INPUT_BUF_LEN - 1) {
            t->input_buf[t->input_pos++] = (char)key;
            t->input_buf[t->input_pos] = '\0';
            // echo to vterm
            vterm_write_char(idx, (char)key);
        }
        return;
    }
    // Backspace
    if (key == '\b' || key == 8) {
        if (t->input_pos > 0) {
            t->input_pos--;
            t->input_buf[t->input_pos] = '\0';
            // remove last char from display
            // write a backspace effect: move cur_x back and clear
            if (t->cur_x > 0) {
                t->cur_x--;
                t->buf[t->cur_y][t->cur_x] = '\0';
            }
        }
        return;
    }
    // Enter - execute command in this vterm
    if (key == '\n' || key == 10) {
        // append newline visually
        vterm_write_char(idx, '\n');
        // handle command: use existing handle_shell_command, but capture output via shell redirect
        if (strlen(t->input_buf) > 0) {
            // temporarily redirect shell output to vterm buffer by using start_shell_redirect / shell_redirect_buf
            extern void start_shell_redirect(void);
            extern void stop_shell_redirect(void);
            extern char shell_redirect_buf[];
            extern int shell_redirect_pos;
            extern unsigned char shell_redirect_r[];
            extern unsigned char shell_redirect_g[];
            extern unsigned char shell_redirect_b[];
            // Swap global shell_current_path into this vterm's cwd so commands (like cd) operate per-vterm
            extern char shell_current_path[]; // global
            char saved_global_cwd[128];
            strncpy(saved_global_cwd, shell_current_path, sizeof(saved_global_cwd)-1);
            saved_global_cwd[sizeof(saved_global_cwd)-1] = '\0';
            // set global to this vterm's cwd for command execution
            strncpy(shell_current_path, t->cwd, 127);
            shell_current_path[127] = '\0';

            start_shell_redirect();
                // If the command is "clear" (possibly with surrounding spaces), perform vterm_clear instead of global clearScreen
                // Trim leading spaces
                const char* cmd = t->input_buf;
                while (*cmd == ' ' || *cmd == '\t') cmd++;
                // Extract first token
                char first[64];
                int i = 0;
                while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && i < (int)sizeof(first)-1) { first[i] = cmd[i]; i++; }
                first[i] = '\0';
                if (strcmp(first, "clear") == 0) {
                    vterm_clear(idx);
                } else {
                    handle_shell_command(t->input_buf);
                }
            stop_shell_redirect();

            // after command execution, copy back any cwd changes into this vterm and restore global cwd
            strncpy(t->cwd, shell_current_path, sizeof(t->cwd)-1);
            t->cwd[sizeof(t->cwd)-1] = '\0';
            // restore previous global cwd
            strncpy(shell_current_path, saved_global_cwd, sizeof(saved_global_cwd)-1);
            shell_current_path[sizeof(saved_global_cwd)-1] = '\0';
            // append redirected output to vterm
                if (shell_redirect_buf[0]) {
                    // The redirect buffer may contain multiple lines; append each line separately
                    const char* p = shell_redirect_buf;
                    const char* start = p;
                    while (*p) {
                        if (*p == '\n') {
                            int len = p - start;
                            char tmp[TERM_COLS + 1];
                            if (len > TERM_COLS) len = TERM_COLS;
                            strncpy(tmp, start, len);
                            tmp[len] = '\0';
                            // map colors for this chunk from shell_redirect_* arrays
                            int base = start - shell_redirect_buf;
                            // write characters and set per-char colors
                            for (int i = 0; i < len; ++i) {
                                vterm_write_char(idx, tmp[i]);
                                int wr = shell_redirect_r[base + i] ? shell_redirect_r[base + i] : 200;
                                int wg = shell_redirect_g[base + i] ? shell_redirect_g[base + i] : 200;
                                int wb = shell_redirect_b[base + i] ? shell_redirect_b[base + i] : 200;
                                int rr = t->cur_y;
                                int cc = t->cur_x - 1;
                                if (rr >= 0 && rr < TERM_ROWS && cc >= 0 && cc < TERM_COLS) {
                                    t->char_r[rr][cc] = wr;
                                    t->char_g[rr][cc] = wg;
                                    t->char_b[rr][cc] = wb;
                                }
                            }
                            // append newline
                            vterm_write_char(idx, '\n');
                            start = p + 1;
                        }
                        p++;
                    }
                    // If any remaining text after last newline
                    if (start < p) {
                        int len = p - start;
                        char tmp[TERM_COLS + 1];
                        if (len > TERM_COLS) len = TERM_COLS;
                        strncpy(tmp, start, len);
                        tmp[len] = '\0';
                        int base = start - shell_redirect_buf;
                        for (int i = 0; i < len; ++i) {
                            vterm_write_char(idx, tmp[i]);
                            int wr = shell_redirect_r[base + i] ? shell_redirect_r[base + i] : 200;
                            int wg = shell_redirect_g[base + i] ? shell_redirect_g[base + i] : 200;
                            int wb = shell_redirect_b[base + i] ? shell_redirect_b[base + i] : 200;
                            int rr = t->cur_y;
                            int cc = t->cur_x - 1;
                            if (rr >= 0 && rr < TERM_ROWS && cc >= 0 && cc < TERM_COLS) {
                                t->char_r[rr][cc] = wr;
                                t->char_g[rr][cc] = wg;
                                t->char_b[rr][cc] = wb;
                            }
                        }
                    }
                    // reset redirect buffer
                    shell_redirect_pos = 0;
                    shell_redirect_buf[0] = '\0';
                }
            // add to global history
            add_to_history(&g_command_history, t->input_buf);
            // reset this vterm's browsing state so next Up starts from the most-recent entry
            t->history_idx = -1;
        }
        // reset input buffer
        t->input_pos = 0;
        t->input_buf[0] = '\0';
            // After executing, print a fresh prompt line
            vterm_print_prompt(idx);
        return;
    }
    // Arrow keys for local navigation in future (ignored for now)
}

// Return the tail-visible line for a vterm given visible_count.
const char* vterm_get_tail_line(int idx, int visible_index, int visible_count) {
    if (idx < 0 || idx >= 4) return "";
    vterm_t* t = &vterms[idx];
    // Compute which lines are currently visible: last visible_count lines ending at cur_y (current line)
    int end = t->cur_y;
    if (end < 0) end = 0;
    int start = end - visible_count + 1;
    if (start < 0) start = 0;
    int target = start + visible_index;
    if (target < 0 || target >= TERM_ROWS) return "";
    return t->buf[target];
}

void vterm_get_tail_line_color(int idx, int visible_index, int visible_count, int* r, int* g, int* b) {
    if (r) *r = 200; if (g) *g = 200; if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    int end = t->cur_y;
    if (end < 0) end = 0;
    int start = end - visible_count + 1;
    if (start < 0) start = 0;
    int target = start + visible_index;
    if (target < 0 || target >= TERM_ROWS) return;
    if (r) *r = t->line_r[target];
    if (g) *g = t->line_g[target];
    if (b) *b = t->line_b[target];
}

// Return color for a specific character in the tail view
void vterm_get_tail_char_color(int idx, int visible_index, int visible_count, int char_col, int* r, int* g, int* b) {
    if (r) *r = 200; if (g) *g = 200; if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    if (char_col < 0 || char_col >= TERM_COLS) return;
    vterm_t* t = &vterms[idx];
    int end = t->cur_y;
    if (end < 0) end = 0;
    int start = end - visible_count + 1;
    if (start < 0) start = 0;
    int target = start + visible_index;
    if (target < 0 || target >= TERM_ROWS) return;
    if (r) *r = t->char_r[target][char_col];
    if (g) *g = t->char_g[target][char_col];
    if (b) *b = t->char_b[target][char_col];
}

void vterm_set_active(int idx, int active) {
    if (idx < 0 || idx >= 4) return;
    vterms[idx].active = active;
}

int vterm_is_active(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].active;
}
