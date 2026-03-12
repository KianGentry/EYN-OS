#ifndef TERMINALS_H
#define TERMINALS_H

void vterm_init_all();
void vterm_write_char(int idx, char ch);
// Erase a single previously echoed output character (used for interactive stdin echo).
void vterm_backspace_output(int idx);
// Get the text for an absolute row index (includes scrollback history).
const char* vterm_get_line(int idx, int row);
void vterm_feed_input(int idx, int key);
void vterm_set_active(int idx, int active);
int vterm_is_active(int idx);
// Move all terminal state from src slot to dst slot and reset src to a fresh state.
void vterm_move_state(int dst_idx, int src_idx);
// Set/Get scrollback offset (0 = follow tail). Positive values scroll up into history.
void vterm_set_scroll(int idx, int scroll);
int vterm_get_scroll(int idx);
// Handle a full key press for the vterm: editing, history, enter to execute command
void vterm_handle_key(int idx, int key);
// Stream a single redirected output character directly into a vterm (with colour).
// Used to keep the GUI updating while a ring3 task is running.
void vterm_stream_redirect_char(int idx, char ch, int r, int g, int b);
// Print the shell prompt into the vterm (e.g. "0:/! ")
void vterm_print_prompt(int idx);
// Clear the virtual terminal buffer and reset cursor
void vterm_clear(int idx);
// Get the current cursor row (absolute row index; includes scrollback history)
int vterm_get_cursor_row(int idx);
// Get the current cursor column (0..TERM_COLS-1)
int vterm_get_cursor_col(int idx);
// Selection helpers (single-line selection for input line)
void vterm_clear_selection(int idx);
int vterm_is_selected(int idx, int row, int col);
// Get the Nth line from the tail when showing last visible lines. 
// visible_index is 0..(visible_count-1), where 0 is the earliest visible line.
const char* vterm_get_tail_line(int idx, int visible_index, int visible_count);
// Get colour for tail line
void vterm_get_tail_line_colour(int idx, int visible_index, int visible_count, int* out_r, int* out_g, int* out_b);
// Get colour for a specific character column in the tail view
void vterm_get_tail_char_colour(int idx, int visible_index, int visible_count, int char_col, int* out_r, int* out_g, int* out_b);

// Per-line icon metadata for rendering small file/dir icons alongside text.
// Icons are anchored to a character column (cell start) and are not meant to
// shift text in pixels. The shell/ls output typically prints one or two leading
// spaces to reserve room for the icon.
//
// Backward-compatible: returns the first icon (if any).
const char* vterm_get_line_icon_key(int idx, int row, int* out_indent_px, int* out_anchor_col);
// New: multiple icons per line (needed for columnar listings).
int vterm_get_line_icon_count(int idx, int row);
const char* vterm_get_line_icon_key_n(int idx, int row, int n, int* out_anchor_col);

// Register an icon on the current output line at the current cursor column.
// Intended for ring3 tasks that print directly into a vterm via syscalls.
void vterm_register_line_icon(int idx, const char* icon_key);

// Absolute helpers for rendering/wrapping
// Get per-character colour at absolute (row, col) indices
void vterm_get_char_colour_abs(int idx, int row, int col, int* out_r, int* out_g, int* out_b);
// Monotonic version counter for each vterm that increments when content changes.
// Use to drive incremental redraws in the tiler.
int vterm_get_version(int idx);

// Get per-vterm current working directory (read-only pointer).
const char* vterm_get_cwd(int idx);
// Set per-vterm current working directory (used by ring3 cd syscall path).
void vterm_set_cwd(int idx, const char* cwd);

#define TERM_COLS 80
// Increase terminal buffer height so the terminal can fill tall tiles/screens (480px ~= 60 rows)
#define TERM_ROWS 60

// -
// User task stdin buffer API
// When a ring3 user task is active and waiting for input, the TUI routes
// keyboard characters to this buffer. The syscall READ (fd=0) consumes from here.
// -

// Clear the stdin buffer for a vterm (called when starting a new user task)
void vterm_stdin_clear(int idx);
// Append a character to the stdin buffer. Returns 1 if newline (line complete), 0 otherwise.
int vterm_stdin_putchar(int idx, char ch);
// Check if a complete line is ready in the stdin buffer
int vterm_stdin_ready(int idx);
// Get pointer to stdin buffer data
const char* vterm_stdin_data(int idx);
// Get length of data in stdin buffer
int vterm_stdin_len(int idx);
// Consume/clear the stdin buffer after the syscall has read the data
void vterm_stdin_consume(int idx);

#endif // TERMINALS_H
