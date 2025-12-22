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
// Stream a single redirected output character directly into a vterm (with color).
// Used to keep the GUI updating while a ring3 task is running.
void vterm_stream_redirect_char(int idx, char ch, int r, int g, int b);
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
void vterm_get_tail_line_color(int idx, int visible_index, int visible_count, int* out_r, int* out_g, int* out_b);
// Get color for a specific character column in the tail view
void vterm_get_tail_char_color(int idx, int visible_index, int visible_count, int char_col, int* out_r, int* out_g, int* out_b);

// Per-line icon metadata for rendering small file/dir icons alongside text.
// Returns NULL if no icon is associated with the line.
// If non-NULL, out_indent_px is typically 10 and out_anchor_col is the text start column.
const char* vterm_get_line_icon_key(int idx, int row, int* out_indent_px, int* out_anchor_col);

// Absolute helpers for rendering/wrapping
// Get per-character color at absolute (row, col) indices
void vterm_get_char_color_abs(int idx, int row, int col, int* out_r, int* out_g, int* out_b);
// Monotonic version counter for each vterm that increments when content changes.
// Use to drive incremental redraws in the tiler.
int vterm_get_version(int idx);

#define TERM_COLS 80
// Increase terminal buffer height so the terminal can fill tall tiles/screens (480px ~= 60 rows)
#define TERM_ROWS 60

#endif // TERMINALS_H
