#ifndef TERMINALS_H
#define TERMINALS_H

void vterm_init_all();
void vterm_write_char(int idx, char ch);
const char* vterm_get_line(int idx, int row);
void vterm_feed_input(int idx, int key);
void vterm_set_active(int idx, int active);
int vterm_is_active(int idx);
// Set/Get scrollback offset (0 = follow tail). Positive values scroll up into history.
void vterm_set_scroll(int idx, int scroll);
int vterm_get_scroll(int idx);
// Handle a full key press for the vterm: editing, history, enter to execute command
void vterm_handle_key(int idx, int key);
// Print the shell prompt into the vterm (e.g. "0:/! ")
void vterm_print_prompt(int idx);
// Clear the virtual terminal buffer and reset cursor
void vterm_clear(int idx);
// Get the current cursor row (0..TERM_ROWS-1)
int vterm_get_cursor_row(int idx);
// Get the current cursor column (0..TERM_COLS-1)
int vterm_get_cursor_col(int idx);
// Selection helpers (single-line selection for input line)
void vterm_clear_selection(int idx);
int vterm_is_selected(int idx, int row, int col);
// Get the Nth line from the tail when showing last visible lines. 
// visible_index is 0..(visible_count-1), where 0 is the earliest visible line.
const char* vterm_get_tail_line(int idx, int visible_index, int visible_count);
// Get color for tail line
void vterm_get_tail_line_color(int idx, int visible_index, int visible_count, int* r, int* g, int* b);
// Get color for a specific character column in the tail view
void vterm_get_tail_char_color(int idx, int visible_index, int visible_count, int char_col, int* r, int* g, int* b);

// Absolute helpers for rendering/wrapping
// Get per-character color at absolute (row, col) indices
void vterm_get_char_color_abs(int idx, int row, int col, int* r, int* g, int* b);
// Monotonic version counter for each vterm that increments when content changes.
// Use to drive incremental redraws in the tiler.
int vterm_get_version(int idx);
// Get a pointer to the absolute line buffer (same as vterm_get_line)
// and optionally the line length via strlen if needed.

#define TERM_COLS 80
#define TERM_ROWS 24

#endif // TERMINALS_H
