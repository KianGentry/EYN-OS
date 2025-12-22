#include <types.h>
#include <vga.h>
#include <string.h>
#include <shell.h>
#include <stdint.h>
#include <util.h>
// Use the shared terminal API/definitions so TERM_ROWS/TERM_COLS stay consistent
#include <terminals.h>

// shell_current_path is maintained by the main shell code
extern char shell_current_path[128];

// forward decl so prompt printer can call it before it's defined (also in header)
void vterm_write_char(int idx, char ch);

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

    // Per-line icon metadata (used by tiler to draw small file/dir icons)
    // icon_key: base icon name without extension, e.g. "file_txt" or "dir_full"
    char line_icon_key[TERM_ROWS][16];
    uint8_t line_indent_px[TERM_ROWS];
    uint8_t line_icon_anchor_col[TERM_ROWS];

    unsigned int version; // increments when content changes
    // Simple selection for current input line: active flag and [start,end) columns
    int sel_active;
    int sel_start_col;
    int sel_end_col;
    // input editing render anchors
    int input_row;        // row where current input is being edited (same as cur_y at prompt)
    int input_start_col;  // column where input begins (after the prompt)
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
            vterms[i].line_icon_key[r][0] = '\0';
            vterms[i].line_indent_px[r] = 0;
            vterms[i].line_icon_anchor_col[r] = 0;
            for (int c = 0; c < TERM_COLS; ++c) {
                vterms[i].char_r[r][c] = 200;
                vterms[i].char_g[r][c] = 200;
                vterms[i].char_b[r][c] = 200;
            }
        }
        vterms[i].version = 1;
        vterms[i].sel_active = 0;
        vterms[i].sel_start_col = 0;
        vterms[i].sel_end_col = 0;
        vterms[i].input_row = 0;
        vterms[i].input_start_col = 0;
    }
}

// Set scrollback offset for a vterm. 0 = follow the tail (latest). Positive values scroll up.
void vterm_set_scroll(int idx, int scroll) {
    if (idx < 0 || idx >= 4) return;
    if (scroll < 0) scroll = 0;
    // Maximum scroll is up to current cursor row (lines written so far)
    int max_scroll = vterms[idx].cur_y;
    if (scroll > max_scroll) scroll = max_scroll;
    if (vterms[idx].scroll != scroll) {
        vterms[idx].scroll = scroll;
        vterms[idx].version++;
    }
}

int vterm_get_scroll(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].scroll;
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
        // After printing prompt, initialize input edit anchors
        t->input_row = t->cur_y;
        t->input_start_col = t->cur_x;
        t->input_buf[0] = '\0';
        t->input_pos = 0;
        t->sel_active = 0;
    }

// Helper: re-render the current input buffer onto the screen after the prompt
static void vterm_render_input(int idx) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    int row = t->input_row;
    if (row < 0 || row >= TERM_ROWS) return;
    // Copy input buffer into screen buffer at input_start_col
    int plen = t->input_start_col;
    int ilen = strlen(t->input_buf);
    if (plen + ilen >= TERM_COLS) ilen = TERM_COLS - plen - 1;
    for (int i = 0; i < ilen; ++i) {
        t->buf[row][plen + i] = t->input_buf[i];
        t->char_r[row][plen + i] = 200;
        t->char_g[row][plen + i] = 200;
        t->char_b[row][plen + i] = 200;
    }
    // Terminate the line right after input
    t->buf[row][plen + ilen] = '\0';
    // Position caret
    t->cur_y = row;
    t->cur_x = t->input_start_col + t->input_pos;
    t->version++;
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

                // shift per-line icon metadata
                strncpy(t->line_icon_key[r-1], t->line_icon_key[r], sizeof(t->line_icon_key[r-1]) - 1);
                t->line_icon_key[r-1][sizeof(t->line_icon_key[r-1]) - 1] = '\0';
                t->line_indent_px[r-1] = t->line_indent_px[r];
                t->line_icon_anchor_col[r-1] = t->line_icon_anchor_col[r];
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
            t->line_icon_key[TERM_ROWS-1][0] = '\0';
            t->line_indent_px[TERM_ROWS-1] = 0;
            t->line_icon_anchor_col[TERM_ROWS-1] = 0;
            t->cur_y = TERM_ROWS-1;
        }
        /* mark the new (now-current) line with default color */
        t->line_r[t->cur_y] = 200;
        t->line_g[t->cur_y] = 200;
        t->line_b[t->cur_y] = 200;
        // default: no icon metadata for the new line
        t->line_icon_key[t->cur_y][0] = '\0';
        t->line_indent_px[t->cur_y] = 0;
        t->line_icon_anchor_col[t->cur_y] = 0;
        t->version++;
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

                strncpy(t->line_icon_key[r-1], t->line_icon_key[r], sizeof(t->line_icon_key[r-1]) - 1);
                t->line_icon_key[r-1][sizeof(t->line_icon_key[r-1]) - 1] = '\0';
                t->line_indent_px[r-1] = t->line_indent_px[r];
                t->line_icon_anchor_col[r-1] = t->line_icon_anchor_col[r];
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
            t->line_icon_key[TERM_ROWS-1][0] = '\0';
            t->line_indent_px[TERM_ROWS-1] = 0;
            t->line_icon_anchor_col[TERM_ROWS-1] = 0;
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
    t->version++;

    // If a ring3 task is active, mark UI dirty so IRQ0 will repaint all tiles.
    if (g_user_task_active) {
        g_user_task_ui_dirty = 1;
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
    t->version++;
}

const char* vterm_get_line_icon_key(int idx, int row, int* out_indent_px, int* out_anchor_col) {
    if (idx < 0 || idx >= 4) return NULL;
    if (row < 0 || row >= TERM_ROWS) return NULL;
    vterm_t* t = &vterms[idx];
    if (out_indent_px) *out_indent_px = (int)t->line_indent_px[row];
    if (out_anchor_col) *out_anchor_col = (int)t->line_icon_anchor_col[row];
    if (t->line_icon_key[row][0] == '\0') return NULL;
    return t->line_icon_key[row];
}

    void vterm_clear(int idx) {
        if (idx < 0 || idx >= 4) return;
        vterm_t* t = &vterms[idx];
        for (int r = 0; r < TERM_ROWS; ++r) {
            t->buf[r][0] = '\0';
            t->line_r[r] = 200; t->line_g[r] = 200; t->line_b[r] = 200;
            t->line_icon_key[r][0] = '\0';
            t->line_indent_px[r] = 0;
            t->line_icon_anchor_col[r] = 0;
            for (int c = 0; c < TERM_COLS; ++c) {
                t->char_r[r][c] = 200; t->char_g[r][c] = 200; t->char_b[r][c] = 200;
            }
        }
        t->cur_x = 0;
        t->cur_y = 0;
        t->scroll = 0;
        t->input_buf[0] = '\0';
        t->input_pos = 0;
        t->version++;
    }

// Each vterm can accept full key handling similar to readStr_with_history: editing, history, enter to execute
void vterm_handle_key(int idx, int key) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;

    // Handle arrow keys: Up/Down for history, Left/Right for cursor movement
    if (key == 0x2104) { // Ctrl+A -> select all of current input buffer
        int len = strlen(t->input_buf);
        t->sel_active = (len > 0);
        t->sel_start_col = 0;
        t->sel_end_col = len;
        t->version++;
        return;
    }
    if (key == 0x2105) { // Ctrl+L -> select current line (same as all for single-line input)
        int len = strlen(t->input_buf);
        t->sel_active = (len > 0);
        t->sel_start_col = 0;
        t->sel_end_col = len;
        t->version++;
        return;
    }
    // Shift+Arrow selection extension indicated via 0x3000 bit combined with base 0x1001..0x1004
    if ((key & 0x3000) && ((key & 0x0FFF) >= 0x1001 && (key & 0x0FFF) <= 0x1004)) {
        int base = key & 0x0FFF;
        // Start selection if not active
        if (!t->sel_active) { t->sel_active = 1; t->sel_start_col = t->input_pos; t->sel_end_col = t->input_pos; }
        if (base == 0x1003) { // Left: move caret left and extend selection
            if (t->input_pos > 0) {
                t->input_pos--; if (t->cur_x > t->input_start_col) t->cur_x--; t->sel_end_col = t->input_pos;
            }
        } else if (base == 0x1004) { // Right
            int len = strlen(t->input_buf);
            if (t->input_pos < len) { t->input_pos++; t->cur_x = t->input_start_col + t->input_pos; t->sel_end_col = t->input_pos; }
        } else if (base == 0x1001 || base == 0x1002) {
            // Up/Down not meaningful for single-line input; ignore
        }
        t->version++;
        return;
    }

    if (key == 0x1001) { // Up
        if (g_command_history.count > 0) {
            if (t->history_idx == -1) {
                // first time browsing - save current input
                strncpy(t->saved_input, t->input_buf, INPUT_BUF_LEN);
                t->history_idx = g_command_history.count - 1;
            } else if (t->history_idx > 0) {
                t->history_idx--;
            }
            // clear current displayed input (preserve prompt)
            t->input_pos = 0;
            t->buf[t->cur_y][t->input_start_col] = '\0';
            t->cur_x = t->input_start_col;
            // load history entry (per-vterm browsing)
            strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN);
            t->input_pos = strlen(t->input_buf);
            vterm_render_input(idx);
        }
        return;
    }
    if (key == 0x1002) { // Down
        if (t->history_idx != -1) {
            t->history_idx++;
            // clear current displayed input (preserve prompt)
            t->input_pos = 0;
            t->buf[t->cur_y][t->input_start_col] = '\0';
            t->cur_x = t->input_start_col;
            if (t->history_idx >= g_command_history.count) {
                // restore saved input
                strncpy(t->input_buf, t->saved_input, INPUT_BUF_LEN);
                t->input_pos = strlen(t->input_buf);
                vterm_render_input(idx);
                t->history_idx = -1;
                } else {
                // load next history entry
                strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN);
                t->input_pos = strlen(t->input_buf);
                vterm_render_input(idx);
            }
        }
        return;
    }
    if (key == 0x1003) { // Left
        if (t->input_pos > 0) {
            // move cursor left logically
            if (t->cur_x > t->input_start_col) t->cur_x--;
            t->input_pos--;
        }
        // clear selection on plain move
        t->sel_active = 0;
        // force a redraw so caret overlay updates when moving within existing text
        t->version++;
        return;
    }
    if (key == 0x1004) { // Right
        int len = strlen(t->input_buf);
        if (t->input_pos < len) {
            t->input_pos++;
            t->cur_x = t->input_start_col + t->input_pos;
        }
        t->sel_active = 0;
        // force a redraw so caret overlay updates when moving within existing text
        t->version++;
        return;
    }

    // Printable
    if (key >= 32 && key <= 126) {
        int len = strlen(t->input_buf);
        // If selection active, delete selected region first
        if (t->sel_active) {
            int a = t->sel_start_col, b = t->sel_end_col; if (a > b) { int tmp = a; a = b; b = tmp; }
            if (a < 0) a = 0; if (b > len) b = len;
            int tail = len - b;
            memmove(&t->input_buf[a], &t->input_buf[b], tail);
            t->input_buf[a + tail] = '\0';
            t->input_pos = a;
            t->sel_active = 0;
            len = strlen(t->input_buf);
        }
        if (len < INPUT_BUF_LEN - 1) {
            // insert at input_pos
            memmove(&t->input_buf[t->input_pos + 1], &t->input_buf[t->input_pos], len - t->input_pos + 1);
            t->input_buf[t->input_pos] = (char)key;
            t->input_pos++;
            vterm_render_input(idx);
        }
        return;
    }
    // Backspace
    if (key == '\b' || key == 8) {
        int len = strlen(t->input_buf);
        if (t->sel_active) {
            int a = t->sel_start_col, b = t->sel_end_col; if (a > b) { int tmp = a; a = b; b = tmp; }
            if (a < 0) a = 0; if (b > len) b = len;
            int tail = len - b;
            memmove(&t->input_buf[a], &t->input_buf[b], tail);
            t->input_buf[a + tail] = '\0';
            t->input_pos = a;
            t->sel_active = 0;
            vterm_render_input(idx);
            return;
        }
        if (t->input_pos > 0) {
            memmove(&t->input_buf[t->input_pos - 1], &t->input_buf[t->input_pos], len - t->input_pos + 1);
            t->input_pos--;
            vterm_render_input(idx);
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
            extern shell_redirect_icon_t shell_redirect_icons[];
            extern int shell_redirect_icon_count;
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
            // Capture the redirect length before stopping, since stop() resets the position
            int captured_redirect_pos = shell_redirect_pos;
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
                            // Clamp to available redirect data first, then to TERM_COLS
                            int base = start - shell_redirect_buf;
                            int available = captured_redirect_pos - base;
                            if (available < 0) available = 0;
                            if (len > available) len = available;
                            if (len > TERM_COLS) len = TERM_COLS;
                            if (len < 0) len = 0;

                            // If any icon marker falls within this output line, record it on this vterm row.
                            // We store the icon key and shift the line by +10px when rendered.
                            int out_row = t->cur_y;
                            int out_start_col = t->cur_x;
                            const char* icon_key = NULL;
                            for (int ii = 0; ii < shell_redirect_icon_count; ++ii) {
                                int ip = shell_redirect_icons[ii].pos;
                                if (ip >= base && ip < base + len) { icon_key = shell_redirect_icons[ii].ext; break; }
                            }
                            if (icon_key && out_row >= 0 && out_row < TERM_ROWS) {
                                strncpy(t->line_icon_key[out_row], icon_key, sizeof(t->line_icon_key[out_row]) - 1);
                                t->line_icon_key[out_row][sizeof(t->line_icon_key[out_row]) - 1] = '\0';
                                t->line_indent_px[out_row] = 10;
                                t->line_icon_anchor_col[out_row] = (uint8_t)out_start_col;
                            }

                            // write characters with per-char colors
                            for (int i = 0; i < len; ++i) {
                                vterm_write_char(idx, start[i]);
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
                        int base = start - shell_redirect_buf;
                        int available = captured_redirect_pos - base;
                        if (available < 0) available = 0;
                        if (len > available) len = available;
                        if (len > TERM_COLS) len = TERM_COLS;
                        if (len < 0) len = 0;

                        int out_row = t->cur_y;
                        int out_start_col = t->cur_x;
                        const char* icon_key = NULL;
                        for (int ii = 0; ii < shell_redirect_icon_count; ++ii) {
                            int ip = shell_redirect_icons[ii].pos;
                            if (ip >= base && ip < base + len) { icon_key = shell_redirect_icons[ii].ext; break; }
                        }
                        if (icon_key && out_row >= 0 && out_row < TERM_ROWS) {
                            strncpy(t->line_icon_key[out_row], icon_key, sizeof(t->line_icon_key[out_row]) - 1);
                            t->line_icon_key[out_row][sizeof(t->line_icon_key[out_row]) - 1] = '\0';
                            t->line_indent_px[out_row] = 10;
                            t->line_icon_anchor_col[out_row] = (uint8_t)out_start_col;
                        }

                        for (int i = 0; i < len; ++i) {
                            vterm_write_char(idx, start[i]);
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
                    shell_redirect_icon_count = 0;
                }
            // add to global history
            add_to_history(&g_command_history, t->input_buf);
            // reset this vterm's browsing state so next Up starts from the most-recent entry
            t->history_idx = -1;
        }
        // reset input buffer via new prompt
        t->sel_active = 0;
        // After executing, print a fresh prompt line (this will also reset input anchors)
        vterm_print_prompt(idx);
        t->version++;
        return;
    }
    // Arrow keys for local navigation in future (ignored for now)
}

int vterm_get_version(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].version;
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

int vterm_get_cursor_row(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].cur_y;
}

void vterm_get_char_color_abs(int idx, int row, int col, int* r, int* g, int* b) {
    if (r) *r = 200; if (g) *g = 200; if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    if (row < 0 || row >= TERM_ROWS) return;
    if (col < 0 || col >= TERM_COLS) return;
    vterm_t* t = &vterms[idx];
    if (r) *r = t->char_r[row][col];
    if (g) *g = t->char_g[row][col];
    if (b) *b = t->char_b[row][col];
}

int vterm_get_cursor_col(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].cur_x;
}

void vterm_clear_selection(int idx) {
    if (idx < 0 || idx >= 4) return; vterms[idx].sel_active = 0; }

int vterm_is_selected(int idx, int row, int col) {
    if (idx < 0 || idx >= 4) return 0;
    vterm_t* t = &vterms[idx];
    if (!t->sel_active) return 0;
    if (row != t->cur_y) return 0;
    int a = t->sel_start_col; int b = t->sel_end_col; if (a > b) { int tmp = a; a = b; b = tmp; }
    int abs_a = t->input_start_col + a;
    int abs_b = t->input_start_col + b;
    return (col >= abs_a && col < abs_b);
}
