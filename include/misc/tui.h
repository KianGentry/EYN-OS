#ifndef TUI_H
#define TUI_H

#include <stdint.h>

// Color definitions (can be expanded later)
#define TUI_COLOR_DEFAULT 0
#define TUI_COLOR_YELLOW  1
#define TUI_COLOR_RED     2
#define TUI_COLOR_MAGENTA 3
#define TUI_COLOR_WHITE   4
#define TUI_COLOR_BLACK   5
#define TUI_COLOR_GRAY    6

// Style for text
typedef struct {
    uint8_t fg_color;
    uint8_t bg_color;
    uint8_t bold;
} tui_style_t;

// Window definition
typedef struct {
    int x, y, width, height;
    const char* title;
    tui_style_t title_style;
    tui_style_t border_style;
    tui_style_t bg_style;
} tui_window_t;

// Initialize TUI system
void tui_init(int screen_width, int screen_height);

// Draw a window (with titlebar, borders, and automatic separators)
void tui_draw_window(const tui_window_t* win);

// Draw a scrollable/selectable list inside a window
void tui_draw_list(const tui_window_t* win, const char** items, int item_count, int selected_index, int scroll_offset, tui_style_t style, tui_style_t selected_style);

// Draw a text area inside a window (with automatic wrapping)
void tui_draw_text_area(const tui_window_t* win, const char* text, int scroll_offset, tui_style_t style);

// Draw a status/info bar at the bottom of the window or screen
void tui_draw_status_bar(const tui_window_t* win, const char* text, tui_style_t style);

// Draw text at (x, y) with style (for advanced/manual use)
void tui_draw_text(int x, int y, const char* text, tui_style_t style);

// Clear/redraw the screen
void tui_clear();
void tui_refresh();

// Keyboard input (returns key code)
int tui_read_key();

#endif // TUI_H 