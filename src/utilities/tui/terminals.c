#include <misc/types.h>
#include <vga.h>
#include <string.h>
#include <shell.h>
#include <stdint.h>
#include <util.h>
// Use the shared terminal API/definitions so TERM_ROWS/TERM_COLS stay consistent
#include <terminals.h>
#include <context.h>
#include <misc/sched.h>
#include <utilities/shell/pipeline.h>
#include <fs/vfs.h>

// shell_current_path is maintained by the main shell code
extern char shell_current_path[128];

#define INPUT_BUF_LEN 200
#define VTERM_MAX_LINE_ICONS 16
#define VTERM_HISTORY_ROWS 256

static inline int vterm_row_slot(int abs_row) {
    if (abs_row < 0) return 0;
    return abs_row % VTERM_HISTORY_ROWS;
}
typedef struct {
    // Ring buffer of rows: stores visible rows + scrollback.
    char buf[VTERM_HISTORY_ROWS][TERM_COLS+1];
    int cur_x;
    // Absolute cursor row (monotonic; rendering uses absolute indices).
    int cur_y;
    // Oldest absolute row still retained in the ring buffer.
    int head_row;
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
    // per-line and per-char colour metadata for rendering
    uint8_t line_r[VTERM_HISTORY_ROWS];
    uint8_t line_g[VTERM_HISTORY_ROWS];
    uint8_t line_b[VTERM_HISTORY_ROWS];
    // per-character colour (row x col)
    uint8_t char_r[VTERM_HISTORY_ROWS][TERM_COLS];
    uint8_t char_g[VTERM_HISTORY_ROWS][TERM_COLS];
    uint8_t char_b[VTERM_HISTORY_ROWS][TERM_COLS];

    // Per-line icon metadata (used by tiler to draw small file/dir icons)
    // icon_key: base icon name without extension, e.g. "file_txt" or "dir_full"
    uint8_t line_icon_count[VTERM_HISTORY_ROWS];
    char line_icon_key[VTERM_HISTORY_ROWS][VTERM_MAX_LINE_ICONS][16];
    uint8_t line_icon_anchor_col[VTERM_HISTORY_ROWS][VTERM_MAX_LINE_ICONS];
    // Legacy: previously used to shift the whole line right in pixels.
    // Now expected to be 0; reserve space in the text stream with leading spaces instead.
    uint8_t line_indent_px[VTERM_HISTORY_ROWS];

    unsigned int version; // increments when content changes
    // Simple selection for current input line: active flag and [start,end) columns
    int sel_active;
    int sel_start_col;
    int sel_end_col;
    // input editing render anchors
    int input_row;        // absolute row where current input is being edited
    int input_start_col;  // column where input begins (after the prompt)

    // Stdin buffer for ring3 user tasks: when a user task is waiting for stdin input,
    // the TUI routes keyboard characters here. The syscall handler consumes from this buffer.
    char stdin_buf[256];
    volatile int stdin_len;        // current bytes in stdin_buf
    volatile int stdin_ready;      // 1 when line is complete (Enter pressed), 0 otherwise
} vterm_t;

static vterm_t vterms[4];

static int vterm_ctx_allow(uint32 caps) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, SCHED_COST_CONSOLE);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void vterm_clear_line_icons(vterm_t* t, int row) {
    if (!t) return;
    if (row < 0 || row >= VTERM_HISTORY_ROWS) return;
    t->line_icon_count[row] = 0;
    for (int i = 0; i < VTERM_MAX_LINE_ICONS; ++i) {
        t->line_icon_key[row][i][0] = '\0';
        t->line_icon_anchor_col[row][i] = 0;
    }
    t->line_indent_px[row] = 0;
}

const char* vterm_get_cwd(int idx) {
    if (idx < 0 || idx >= 4) return "/";
    return vterms[idx].cwd;
}

void vterm_set_cwd(int idx, const char* cwd) {
    if (idx < 0 || idx >= 4) return;
    if (!cwd || cwd[0] != '/') {
        strncpy(vterms[idx].cwd, "/", sizeof(vterms[idx].cwd) - 1);
        vterms[idx].cwd[sizeof(vterms[idx].cwd) - 1] = '\0';
        return;
    }
    strncpy(vterms[idx].cwd, cwd, sizeof(vterms[idx].cwd) - 1);
    vterms[idx].cwd[sizeof(vterms[idx].cwd) - 1] = '\0';
}

void vterm_init_all() {
    for (int i = 0; i < 4; ++i) {
        vterms[i].cur_x = 0;
        vterms[i].cur_y = 0;
        vterms[i].head_row = 0;
        vterms[i].scroll = 0;
        vterms[i].active = 0;
        vterms[i].input_buf[0] = '\0';
        vterms[i].input_pos = 0;
        vterms[i].history_idx = -1;
        // default cwd is root
        strncpy(vterms[i].cwd, "/", sizeof(vterms[i].cwd)-1);
        vterms[i].cwd[sizeof(vterms[i].cwd)-1] = '\0';
        for (int r = 0; r < VTERM_HISTORY_ROWS; ++r) {
            vterms[i].buf[r][0] = '\0';
            vterms[i].line_r[r] = 200;
            vterms[i].line_g[r] = 200;
            vterms[i].line_b[r] = 200;
            vterm_clear_line_icons(&vterms[i], r);
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
        // Initialize stdin buffer for user tasks
        vterms[i].stdin_buf[0] = '\0';
        vterms[i].stdin_len = 0;
        vterms[i].stdin_ready = 0;
    }
}

// Set scrollback offset for a vterm. 0 = follow the tail (latest). Positive values scroll up.
void vterm_set_scroll(int idx, int scroll) {
    if (idx < 0 || idx >= 4) return;
    if (scroll < 0) scroll = 0;
    // Maximum scroll is limited by how much history we retain.
    int max_scroll = vterms[idx].cur_y - vterms[idx].head_row;
    if (max_scroll < 0) max_scroll = 0;
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
        char prompt[64];
        int n;
        if (g_current_drive == VFS_DRIVE_RAM) {
            n = snprintf(prompt, sizeof(prompt), "RAM:%s! ", t->cwd);
        } else {
            uint8 logical = ata_physical_to_logical(g_current_drive);
            if (logical == 0xFF) logical = 0;
            n = snprintf(prompt, sizeof(prompt), "%d:%s! ", logical, t->cwd);
        }
        for (int i = 0; i < n; ++i) {
            if ((i & 0x7) == 0) vterm_ctx_allow(CAP_WRITE_CONSOLE);
            // write the char then override its colour to match prompt styling
            vterm_write_char(idx, prompt[i]);
            // colour drive and leading ':' and '/' as gray, '!' as yellow, rest default
            int drive_and_sep_len = 2; // e.g. "0:"
            // find position of '!' in prompt
            int bang_pos = -1;
            for (int j = 0; j < n; ++j) if (prompt[j] == '!') { bang_pos = j; break; }
            // determine colour for this char
            int rr = 200, gg = 200, bb = 200;
            if (i < drive_and_sep_len) { rr = 150; gg = 150; bb = 150; }
            if (i == bang_pos) { rr = 255; gg = 255; bb = 0; }
            // set colour for the character we just wrote (cur_x was incremented)
            int written_row = t->cur_y;
            int written_col = t->cur_x - 1;
            if (written_row >= t->head_row && written_row <= t->cur_y && written_col >= 0 && written_col < TERM_COLS) {
                int slot = vterm_row_slot(written_row);
                t->char_r[slot][written_col] = rr;
                t->char_g[slot][written_col] = gg;
                t->char_b[slot][written_col] = bb;
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
    int abs_row = t->input_row;
    if (abs_row < t->head_row || abs_row > t->cur_y) return;
    int row = vterm_row_slot(abs_row);
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
    t->cur_y = abs_row;
    t->cur_x = t->input_start_col + t->input_pos;
    t->version++;
}

void vterm_write_char(int idx, char ch) {
    if (!vterm_ctx_allow(CAP_WRITE_CONSOLE)) return;
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;
    // New output should follow the tail (cancel scrollback view).
    if (t->scroll != 0) {
        t->scroll = 0;
        t->version++;
    }

    // Carriage return: move to column 0 on the current row so callers can
    // update in-place status/progress lines.
    if (ch == '\r') {
        t->cur_x = 0;
        t->version++;
        return;
    }

    // Explicit newline
    if (ch == '\n') {
        t->cur_x = 0;
        t->cur_y++;
        if (t->cur_y - t->head_row >= VTERM_HISTORY_ROWS) {
            t->head_row = t->cur_y - (VTERM_HISTORY_ROWS - 1);
        }
        int row = vterm_row_slot(t->cur_y);
        t->buf[row][0] = '\0';
        t->line_r[row] = 200;
        t->line_g[row] = 200;
        t->line_b[row] = 200;
        for (int c = 0; c < TERM_COLS; ++c) {
            t->char_r[row][c] = 200;
            t->char_g[row][c] = 200;
            t->char_b[row][c] = 200;
        }
        vterm_clear_line_icons(t, row);
        t->version++;
        return;
    }

    // Soft wrap
    if (t->cur_x >= TERM_COLS) {
        t->cur_x = 0;
        t->cur_y++;
        if (t->cur_y - t->head_row >= VTERM_HISTORY_ROWS) {
            t->head_row = t->cur_y - (VTERM_HISTORY_ROWS - 1);
        }
        int row = vterm_row_slot(t->cur_y);
        t->buf[row][0] = '\0';
        t->line_r[row] = 200;
        t->line_g[row] = 200;
        t->line_b[row] = 200;
        for (int c = 0; c < TERM_COLS; ++c) {
            t->char_r[row][c] = 200;
            t->char_g[row][c] = 200;
            t->char_b[row][c] = 200;
        }
        vterm_clear_line_icons(t, row);
    }

    int row = vterm_row_slot(t->cur_y);
    t->buf[row][t->cur_x] = ch;
    t->buf[row][t->cur_x + 1] = '\0';
    /* set per-char colour from current redirect colour if available (non-zero), else default */
    int use_r = (shell_redirect_colour_r > 0) ? shell_redirect_colour_r : 200;
    int use_g = (shell_redirect_colour_g > 0) ? shell_redirect_colour_g : 200;
    int use_b = (shell_redirect_colour_b > 0) ? shell_redirect_colour_b : 200;
    t->char_r[row][t->cur_x] = use_r;
    t->char_g[row][t->cur_x] = use_g;
    t->char_b[row][t->cur_x] = use_b;
    t->cur_x++;
    /* ensure this line has a sensible default colour if not already set */
    if (t->line_r[row] == 0 && t->line_g[row] == 0 && t->line_b[row] == 0) {
        t->line_r[row] = 200;
        t->line_g[row] = 200;
        t->line_b[row] = 200;
    }
    t->version++;

    // If a ring3 task is active, mark UI dirty so IRQ0 will repaint all tiles.
    if (g_user_task_active) {
        g_user_task_ui_dirty = 1;
    }
}

void vterm_register_line_icon(int idx, const char* icon_key) {
    if (!vterm_ctx_allow(CAP_WRITE_CONSOLE)) return;
    if (idx < 0 || idx >= 4) return;
    if (!icon_key || !icon_key[0]) return;

    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;

    int slot = vterm_row_slot(t->cur_y);
    int slot_idx = (int)t->line_icon_count[slot];
    if (slot_idx >= VTERM_MAX_LINE_ICONS) return;

    strncpy(t->line_icon_key[slot][slot_idx], icon_key, sizeof(t->line_icon_key[slot][slot_idx]) - 1);
    t->line_icon_key[slot][slot_idx][sizeof(t->line_icon_key[slot][slot_idx]) - 1] = '\0';
    if (t->cur_x < 0) t->cur_x = 0;
    if (t->cur_x >= TERM_COLS) t->cur_x = TERM_COLS - 1;
    t->line_icon_anchor_col[slot][slot_idx] = (uint8_t)t->cur_x;
    t->line_icon_count[slot] = (uint8_t)(slot_idx + 1);
    t->line_indent_px[slot] = 0;

    t->version++;

    if (g_user_task_active) {
        g_user_task_ui_dirty = 1;
    }
}

void vterm_backspace_output(int idx) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    if (!t->active) t->active = 1;

    // Only support simple "erase previous char on current line".
    if (t->cur_x <= 0) return;

    // Move cursor back one and erase that character.
    t->cur_x--;
    if (t->cur_x < 0) t->cur_x = 0;
    int row = vterm_row_slot(t->cur_y);
    t->buf[row][t->cur_x] = '\0';

    // Reset per-char colour at erased position to the line default.
    uint8 rr = t->line_r[row] ? t->line_r[row] : 200;
    uint8 gg = t->line_g[row] ? t->line_g[row] : 200;
    uint8 bb = t->line_b[row] ? t->line_b[row] : 200;
    t->char_r[row][t->cur_x] = rr;
    t->char_g[row][t->cur_x] = gg;
    t->char_b[row][t->cur_x] = bb;

    t->version++;

    // If a ring3 task is active, mark UI dirty so IRQ0 will repaint all tiles.
    if (g_user_task_active) {
        g_user_task_ui_dirty = 1;
    }
}

const char* vterm_get_line(int idx, int row) {
    if (idx < 0 || idx >= 4) return "";
    vterm_t* t = &vterms[idx];
    if (row < t->head_row || row > t->cur_y) return "";
    return t->buf[vterm_row_slot(row)];
}

void vterm_feed_input(int idx, int key) {
    // backcompat: simple feed (kept for other users)
    if (key >= 32 && key <= 126) vterm_write_char(idx, (char)key);
    else if (key == '\n' || key == 10) vterm_write_char(idx, '\n');
}


const char* vterm_get_line_icon_key(int idx, int row, int* out_indent_px, int* out_anchor_col) {
    if (idx < 0 || idx >= 4) return NULL;
    vterm_t* t = &vterms[idx];
    if (row < t->head_row || row > t->cur_y) return NULL;
    int slot = vterm_row_slot(row);
    if (out_indent_px) *out_indent_px = (int)t->line_indent_px[slot];
    if (t->line_icon_count[slot] == 0) return NULL;
    if (out_anchor_col) *out_anchor_col = (int)t->line_icon_anchor_col[slot][0];
    if (t->line_icon_key[slot][0][0] == '\0') return NULL;
    return t->line_icon_key[slot][0];
}

int vterm_get_line_icon_count(int idx, int row) {
    if (idx < 0 || idx >= 4) return 0;
    vterm_t* t = &vterms[idx];
    if (row < t->head_row || row > t->cur_y) return 0;
    return (int)t->line_icon_count[vterm_row_slot(row)];
}

const char* vterm_get_line_icon_key_n(int idx, int row, int n, int* out_anchor_col) {
    if (idx < 0 || idx >= 4) return NULL;
    if (n < 0 || n >= VTERM_MAX_LINE_ICONS) return NULL;
    vterm_t* t = &vterms[idx];
    if (row < t->head_row || row > t->cur_y) return NULL;
    int slot = vterm_row_slot(row);
    if (n >= (int)t->line_icon_count[slot]) return NULL;
    if (out_anchor_col) *out_anchor_col = (int)t->line_icon_anchor_col[slot][n];
    if (t->line_icon_key[slot][n][0] == '\0') return NULL;
    return t->line_icon_key[slot][n];
}

    void vterm_clear(int idx) {
        if (idx < 0 || idx >= 4) return;
        vterm_t* t = &vterms[idx];
        for (int r = 0; r < VTERM_HISTORY_ROWS; ++r) {
            t->buf[r][0] = '\0';
            t->line_r[r] = 200; t->line_g[r] = 200; t->line_b[r] = 200;
            vterm_clear_line_icons(t, r);
            for (int c = 0; c < TERM_COLS; ++c) {
                t->char_r[r][c] = 200; t->char_g[r][c] = 200; t->char_b[r][c] = 200;
            }
        }
        t->cur_x = 0;
        t->cur_y = 0;
        t->head_row = 0;
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
                strncpy(t->saved_input, t->input_buf, INPUT_BUF_LEN - 1);
                t->saved_input[INPUT_BUF_LEN - 1] = '\0';
                t->history_idx = g_command_history.count - 1;
            } else if (t->history_idx > 0) {
                t->history_idx--;
            }
            // clear current displayed input (preserve prompt)
            t->input_pos = 0;
            t->buf[vterm_row_slot(t->cur_y)][t->input_start_col] = '\0';
            t->cur_x = t->input_start_col;
            // load history entry (per-vterm browsing)
            strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN - 1);
            t->input_buf[INPUT_BUF_LEN - 1] = '\0';
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
            t->buf[vterm_row_slot(t->cur_y)][t->input_start_col] = '\0';
            t->cur_x = t->input_start_col;
            if (t->history_idx >= g_command_history.count) {
                // restore saved input
                strncpy(t->input_buf, t->saved_input, INPUT_BUF_LEN - 1);
                t->input_buf[INPUT_BUF_LEN - 1] = '\0';
                t->input_pos = strlen(t->input_buf);
                vterm_render_input(idx);
                t->history_idx = -1;
                } else {
                // load next history entry
                strncpy(t->input_buf, g_command_history.commands[t->history_idx], INPUT_BUF_LEN - 1);
                t->input_buf[INPUT_BUF_LEN - 1] = '\0';
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
            if (a < 0) a = 0;
            if (b > len) b = len;
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
            if (a < 0) a = 0;
            if (b > len) b = len;
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
            while (pipeline_resume_pending()) {
                // Drain any previously armed pipeline before processing input.
            }

            char raw_input[INPUT_BUF_LEN];
            strncpy(raw_input, t->input_buf, sizeof(raw_input) - 1);
            raw_input[sizeof(raw_input) - 1] = '\0';

            // Add to global history before command execution so parser/tokenizer mutations
            // in command handlers cannot alter what Up/Down navigation recalls.
            add_to_history(&g_command_history, raw_input);

            // temporarily redirect shell output to vterm buffer by using start_shell_redirect / shell_redirect_buf
            // Swap global shell_current_path into this vterm's cwd so commands (like cd) operate per-vterm
            char saved_global_cwd[128];
            strncpy(saved_global_cwd, shell_current_path, sizeof(saved_global_cwd)-1);
            saved_global_cwd[sizeof(saved_global_cwd)-1] = '\0';
            // set global to this vterm's cwd for command execution
            strncpy(shell_current_path, t->cwd, 127);
            shell_current_path[127] = '\0';

            start_shell_redirect();
                handle_shell_command(t->input_buf);
                while (pipeline_resume_pending()) {
                    // Continue newly armed pipelines in tiling-terminal mode.
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
                            // Icons are anchored to the character column where the marker falls.
                            int out_row = t->cur_y;
                            int out_start_col = t->cur_x;
                            {
                                int out_slot = vterm_row_slot(out_row);
                                vterm_clear_line_icons(t, out_slot);
                                for (int ii = 0; ii < shell_redirect_icon_count; ++ii) {
                                    int ip = shell_redirect_icons[ii].pos;
                                    if (ip < base || ip >= base + len) continue;
                                    int rel = ip - base;
                                    int anchor = out_start_col + rel;
                                    if (anchor < 0 || anchor >= TERM_COLS) continue;
                                    int slot_idx = (int)t->line_icon_count[out_slot];
                                    if (slot_idx >= VTERM_MAX_LINE_ICONS) break;
                                    strncpy(t->line_icon_key[out_slot][slot_idx], shell_redirect_icons[ii].ext, sizeof(t->line_icon_key[out_slot][slot_idx]) - 1);
                                    t->line_icon_key[out_slot][slot_idx][sizeof(t->line_icon_key[out_slot][slot_idx]) - 1] = '\0';
                                    t->line_icon_anchor_col[out_slot][slot_idx] = (uint8_t)anchor;
                                    t->line_icon_count[out_slot] = (uint8_t)(slot_idx + 1);
                                }
                                // No pixel indentation; reserve space in the text stream with leading spaces.
                                t->line_indent_px[out_slot] = 0;
                            }

                            // write characters with per-char colours
                            for (int i = 0; i < len; ++i) {
                                vterm_write_char(idx, start[i]);
                                int wr = shell_redirect_r[base + i] ? shell_redirect_r[base + i] : 200;
                                int wg = shell_redirect_g[base + i] ? shell_redirect_g[base + i] : 200;
                                int wb = shell_redirect_b[base + i] ? shell_redirect_b[base + i] : 200;
                                int rr = t->cur_y;
                                int cc = t->cur_x - 1;
                                if (cc >= 0 && cc < TERM_COLS) {
                                    int rslot = vterm_row_slot(rr);
                                    t->char_r[rslot][cc] = wr;
                                    t->char_g[rslot][cc] = wg;
                                    t->char_b[rslot][cc] = wb;
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
                        {
                            int out_slot = vterm_row_slot(out_row);
                            vterm_clear_line_icons(t, out_slot);
                            for (int ii = 0; ii < shell_redirect_icon_count; ++ii) {
                                int ip = shell_redirect_icons[ii].pos;
                                if (ip < base || ip >= base + len) continue;
                                int rel = ip - base;
                                int anchor = out_start_col + rel;
                                if (anchor < 0 || anchor >= TERM_COLS) continue;
                                int slot_idx = (int)t->line_icon_count[out_slot];
                                if (slot_idx >= VTERM_MAX_LINE_ICONS) break;
                                strncpy(t->line_icon_key[out_slot][slot_idx], shell_redirect_icons[ii].ext, sizeof(t->line_icon_key[out_slot][slot_idx]) - 1);
                                t->line_icon_key[out_slot][slot_idx][sizeof(t->line_icon_key[out_slot][slot_idx]) - 1] = '\0';
                                t->line_icon_anchor_col[out_slot][slot_idx] = (uint8_t)anchor;
                                t->line_icon_count[out_slot] = (uint8_t)(slot_idx + 1);
                            }
                            t->line_indent_px[out_slot] = 0;
                        }

                        for (int i = 0; i < len; ++i) {
                            vterm_write_char(idx, start[i]);
                            int wr = shell_redirect_r[base + i] ? shell_redirect_r[base + i] : 200;
                            int wg = shell_redirect_g[base + i] ? shell_redirect_g[base + i] : 200;
                            int wb = shell_redirect_b[base + i] ? shell_redirect_b[base + i] : 200;
                            int rr = t->cur_y;
                            int cc = t->cur_x - 1;
                            if (cc >= 0 && cc < TERM_COLS) {
                                int rslot = vterm_row_slot(rr);
                                t->char_r[rslot][cc] = wr;
                                t->char_g[rslot][cc] = wg;
                                t->char_b[rslot][cc] = wb;
                            }
                        }
                    }
                    // reset redirect buffer
                    shell_redirect_pos = 0;
                    shell_redirect_buf[0] = '\0';
                    shell_redirect_icon_count = 0;
                }
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
    // Compute which lines are currently visible: last visible_count lines ending at cur_y
    int end = t->cur_y;
    if (end < t->head_row) end = t->head_row;
    int start = end - visible_count + 1;
    if (start < t->head_row) start = t->head_row;
    int target = start + visible_index;
    if (target < t->head_row || target > t->cur_y) return "";
    return t->buf[vterm_row_slot(target)];
}

void vterm_get_tail_line_colour(int idx, int visible_index, int visible_count, int* r, int* g, int* b) {
    if (r) *r = 200;
    if (g) *g = 200;
    if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    int end = t->cur_y;
    if (end < t->head_row) end = t->head_row;
    int start = end - visible_count + 1;
    if (start < t->head_row) start = t->head_row;
    int target = start + visible_index;
    if (target < t->head_row || target > t->cur_y) return;
    int slot = vterm_row_slot(target);
    if (r) *r = t->line_r[slot];
    if (g) *g = t->line_g[slot];
    if (b) *b = t->line_b[slot];
}

// Return colour for a specific character in the tail view
void vterm_get_tail_char_colour(int idx, int visible_index, int visible_count, int char_col, int* r, int* g, int* b) {
    if (r) *r = 200;
    if (g) *g = 200;
    if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    if (char_col < 0 || char_col >= TERM_COLS) return;
    vterm_t* t = &vterms[idx];
    int end = t->cur_y;
    if (end < t->head_row) end = t->head_row;
    int start = end - visible_count + 1;
    if (start < t->head_row) start = t->head_row;
    int target = start + visible_index;
    if (target < t->head_row || target > t->cur_y) return;
    int slot = vterm_row_slot(target);
    if (r) *r = t->char_r[slot][char_col];
    if (g) *g = t->char_g[slot][char_col];
    if (b) *b = t->char_b[slot][char_col];
}

void vterm_set_active(int idx, int active) {
    if (idx < 0 || idx >= 4) return;
    vterms[idx].active = active;
}

int vterm_is_active(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].active;
}

void vterm_move_state(int dst_idx, int src_idx) {
    if (dst_idx < 0 || dst_idx >= 4) return;
    if (src_idx < 0 || src_idx >= 4) return;
    if (dst_idx == src_idx) return;

    vterms[dst_idx] = vterms[src_idx];

    vterm_t* src = &vterms[src_idx];
    src->cur_x = 0;
    src->cur_y = 0;
    src->head_row = 0;
    src->scroll = 0;
    src->active = 0;
    src->input_buf[0] = '\0';
    src->input_pos = 0;
    src->history_idx = -1;
    src->saved_input[0] = '\0';
    strncpy(src->cwd, "/", sizeof(src->cwd) - 1);
    src->cwd[sizeof(src->cwd) - 1] = '\0';
    for (int r = 0; r < VTERM_HISTORY_ROWS; ++r) {
        src->buf[r][0] = '\0';
        src->line_r[r] = 200;
        src->line_g[r] = 200;
        src->line_b[r] = 200;
        vterm_clear_line_icons(src, r);
        for (int c = 0; c < TERM_COLS; ++c) {
            src->char_r[r][c] = 200;
            src->char_g[r][c] = 200;
            src->char_b[r][c] = 200;
        }
    }
    src->version++;
    src->sel_active = 0;
    src->sel_start_col = 0;
    src->sel_end_col = 0;
    src->input_row = 0;
    src->input_start_col = 0;
    src->stdin_buf[0] = '\0';
    src->stdin_len = 0;
    src->stdin_ready = 0;
}

int vterm_get_cursor_row(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].cur_y;
}

void vterm_get_char_colour_abs(int idx, int row, int col, int* r, int* g, int* b) {
    if (r) *r = 200;
    if (g) *g = 200;
    if (b) *b = 200;
    if (idx < 0 || idx >= 4) return;
    if (col < 0 || col >= TERM_COLS) return;
    vterm_t* t = &vterms[idx];
    if (row < t->head_row || row > t->cur_y) return;
    int slot = vterm_row_slot(row);
    if (r) *r = t->char_r[slot][col];
    if (g) *g = t->char_g[slot][col];
    if (b) *b = t->char_b[slot][col];
}

int vterm_get_cursor_col(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].cur_x;
}

void vterm_clear_selection(int idx) {
    if (idx < 0 || idx >= 4) return;
    vterms[idx].sel_active = 0;
}

int vterm_is_selected(int idx, int row, int col) {
    if (idx < 0 || idx >= 4) return 0;
    vterm_t* t = &vterms[idx];
    if (!t->sel_active) return 0;
    if (row != t->input_row) return 0;
    int a = t->sel_start_col; int b = t->sel_end_col; if (a > b) { int tmp = a; a = b; b = tmp; }
    int abs_a = t->input_start_col + a;
    int abs_b = t->input_start_col + b;
    return (col >= abs_a && col < abs_b);
}


// User task stdin buffer API
// When a ring3 user task is active and waiting for input, the TUI routes
// keyboard characters to this buffer. The syscall READ (fd=0) consumes from here.


// Clear the stdin buffer for a vterm (called when starting a new user task)
void vterm_stdin_clear(int idx) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    t->stdin_buf[0] = '\0';
    t->stdin_len = 0;
    t->stdin_ready = 0;
}

// Append a character to the stdin buffer (called from TUI input handler).
// Returns 1 if the character was a newline (line is complete), 0 otherwise.
int vterm_stdin_putchar(int idx, char ch) {
    if (idx < 0 || idx >= 4) return 0;
    vterm_t* t = &vterms[idx];
    
    // Handle backspace
    if (ch == '\b' || ch == 127) {
        if (t->stdin_len > 0) {
            t->stdin_len--;
            t->stdin_buf[t->stdin_len] = '\0';
        }
        return 0;
    }
    
    // Handle newline - mark line as ready
    if (ch == '\n' || ch == '\r') {
        if (t->stdin_len < (int)sizeof(t->stdin_buf) - 1) {
            t->stdin_buf[t->stdin_len++] = '\n';
            t->stdin_buf[t->stdin_len] = '\0';
        }
        t->stdin_ready = 1;
        return 1;
    }
    
    // Append printable character
    if (t->stdin_len < (int)sizeof(t->stdin_buf) - 2) {  // Leave room for \n and \0
        t->stdin_buf[t->stdin_len++] = ch;
        t->stdin_buf[t->stdin_len] = '\0';
    }
    return 0;
}

// Check if a complete line is ready in the stdin buffer
int vterm_stdin_ready(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].stdin_ready;
}

// Get pointer to stdin buffer data (for copying to user space)
const char* vterm_stdin_data(int idx) {
    if (idx < 0 || idx >= 4) return "";
    return vterms[idx].stdin_buf;
}

// Get length of data in stdin buffer
int vterm_stdin_len(int idx) {
    if (idx < 0 || idx >= 4) return 0;
    return vterms[idx].stdin_len;
}

// Consume/clear the stdin buffer after the syscall has read the data
void vterm_stdin_consume(int idx) {
    if (idx < 0 || idx >= 4) return;
    vterm_t* t = &vterms[idx];
    t->stdin_buf[0] = '\0';
    t->stdin_len = 0;
    t->stdin_ready = 0;
}

