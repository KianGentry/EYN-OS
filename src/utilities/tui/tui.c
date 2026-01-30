#include <tui.h>
#include <vga.h>
#include <hal/keyboard.h>
#include <string.h>
#include <stdint.h>

// Screen dimensions (to be set by tui_init)
static int tui_screen_width = 80;
static int tui_screen_height = 25;

// Internal cursor position for manual text placement
static int tui_cur_x = 0;
static int tui_cur_y = 0;

// exported flag, updated by tui_read_key
int tui_alt_pressed = 0;
int tui_shift_pressed = 0;

// Helper to compute pixel position for cell coords (kept for potential future use)
static __attribute__((unused)) void tui_set_cursor(int x, int y) {
    tui_cur_x = x;
    tui_cur_y = y;
    // Intentionally avoid touching global VGA cursor (width/height) to prevent side effects.
}

void tui_init(int screen_width, int screen_height) {
    tui_screen_width = screen_width;
    tui_screen_height = screen_height;
}

void tui_clear() {
    clearScreen();
}

void tui_refresh() {
    // No buffering needed
}

void tui_draw_text(int x, int y, const char* text, tui_style_t style) {
    int cw = vga_text_cell_w();
    int ch = vga_text_cell_h();
    int rr = 0, gg = 0, bb = 0;
    switch (style.fg_color) {
        case TUI_COLOR_YELLOW: rr = 255; gg = 255; bb = 0; break;
        case TUI_COLOR_RED:    rr = 255; gg = 0;   bb = 0; break;
        case TUI_COLOR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
        case TUI_COLOR_WHITE:  rr = 255; gg = 255; bb = 255; break;
        case TUI_COLOR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
        case TUI_COLOR_GRAY:   rr = 192; gg = 192; bb = 192; break;
        default:               rr = 255; gg = 255; bb = 255; break;
    }
    // Use side-effect-free pixel-based text to avoid console cursor side effects
    drawTextAt(x * cw, y * ch, text, rr, gg, bb);
}

void tui_draw_window(const tui_window_t* win) {
    // Draw titlebar
    int title_len = strlen(win->title);
    char titlebar[win->width + 1];
    int pad = (win->width - title_len) / 2;
    for (int i = 0; i < win->width; ++i) titlebar[i] = ' ';
    titlebar[win->width] = '\0';
    for (int i = 0; i < title_len && (pad + i) < win->width; ++i) {
        titlebar[pad + i] = win->title[i];
    }
    tui_draw_text(win->x, win->y, titlebar, win->title_style);
    // Draw separator under titlebar (between borders)
    char sep[win->width + 1];
    sep[0] = '|';
    for (int i = 1; i < win->width - 1; ++i) sep[i] = '-';
    sep[win->width - 1] = '|';
    sep[win->width] = '\0';
    tui_draw_text(win->x, win->y + 1, sep, win->border_style);
    // Draw left/right borders
    int bottom = win->y + win->height - 1;
    for (int i = 2; win->y + i < bottom; ++i) {
        tui_draw_text(win->x, win->y + i, "|", win->border_style);
        tui_draw_text(win->x + win->width - 1, win->y + i, "|", win->border_style);
    }
    // Draw bottom border (between borders)
    char bot[win->width + 1];
    bot[0] = '|';
    for (int i = 1; i < win->width - 1; ++i) bot[i] = '-';
    bot[win->width - 1] = '|';
    bot[win->width] = '\0';
    tui_draw_text(win->x, bottom, bot, win->border_style);
}

void tui_draw_list(const tui_window_t* win, const char** items, int item_count, int selected_index, int scroll_offset, tui_style_t style, tui_style_t selected_style) {
    int max_visible = win->height - 3;
    for (int i = 0; i < max_visible && (i + scroll_offset) < item_count; ++i) {
        int idx = i + scroll_offset;
        if (idx == selected_index) {
            tui_draw_text(win->x + 1, win->y + 2 + i, "!", selected_style);
            tui_draw_text(win->x + 2, win->y + 2 + i, items[idx], style);
        } else {
            tui_draw_text(win->x + 1, win->y + 2 + i, items[idx], style);
        }
    }
}

void tui_draw_text_area(const tui_window_t* win, const char* text, int scroll_offset, tui_style_t style) {
    int cell_w = vga_text_cell_w();
    int cell_h = vga_text_cell_h();
    int max_lines = win->height - 3;
    int y = win->y + 2;
    int x = win->x + 1;
    int line = 0, col = 0;
    for (int i = 0; text[i] != '\0' && line < max_lines + scroll_offset; ++i) {
        if (line >= scroll_offset) {
            char chbuf[2] = {text[i], '\0'};
            int px = (x + col) * cell_w;
            int py = (y + line - scroll_offset) * cell_h;
            int rr = 255, gg = 255, bb = 255;
            switch (style.fg_color) {
                case TUI_COLOR_YELLOW: rr = 255; gg = 255; bb = 0; break;
                case TUI_COLOR_RED:    rr = 255; gg = 0;   bb = 0; break;
                case TUI_COLOR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
                case TUI_COLOR_WHITE:  rr = 255; gg = 255; bb = 255; break;
                case TUI_COLOR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
                case TUI_COLOR_GRAY:   rr = 192; gg = 192; bb = 192; break;
                default:               rr = 255; gg = 255; bb = 255; break;
            }
            drawTextAt(px, py, chbuf, rr, gg, bb);
        }
        if (text[i] == '\n' || col >= win->width - 3) {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
}

void tui_draw_status_bar(const tui_window_t* win, const char* text, tui_style_t style) {
    int cw = vga_text_cell_w();
    int ch = vga_text_cell_h();
    int y = (win == NULL) ? (tui_screen_height - 1) : (win->y + win->height - 1);
    int x = (win == NULL) ? 0 : win->x;
    int bar_width = (win == NULL) ? tui_screen_width : win->width;
    int rr = 255, gg = 255, bb = 255;
    switch (style.fg_color) {
        case TUI_COLOR_YELLOW: rr = 255; gg = 255; bb = 0; break;
        case TUI_COLOR_RED:    rr = 255; gg = 0;   bb = 0; break;
        case TUI_COLOR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
        case TUI_COLOR_WHITE:  rr = 255; gg = 255; bb = 255; break;
        case TUI_COLOR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
        case TUI_COLOR_GRAY:   rr = 192; gg = 192; bb = 192; break;
        default:               rr = 255; gg = 255; bb = 255; break;
    }

    int px = x * cw;
    int py = y * ch;
    int clip_min = px;
    int clip_max = (x + bar_width) * cw - cw;
    for (int i = 0; text && text[i]; ++i) {
        int cx = px + i * cw;
        if (cx + (cw - 1) > clip_max) break;
        if (cx < clip_min) continue;
        drawCharAt(cx, py, (int)(unsigned char)text[i], rr, gg, bb);
    }
}

int tui_read_key() {
    uint32 mods = hal_kbd_modifiers();
    tui_alt_pressed = (mods & HAL_KBD_MOD_ALT) ? 1 : 0;
    tui_shift_pressed = (mods & HAL_KBD_MOD_SHIFT) ? 1 : 0;
    return (int)hal_kbd_read_key_nonblock();
}