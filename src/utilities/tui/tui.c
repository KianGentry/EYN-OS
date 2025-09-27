#include <tui.h>
#include <vga.h>
#include <system.h>
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

// Helper to set the global cursor position for drawText
static void tui_set_cursor(int x, int y) {
    tui_cur_x = x;
    tui_cur_y = y;
    extern int width, height;
    width = x * 8;
    height = y * 8;
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
    tui_set_cursor(x, y);
    int r = 0, g = 0, b = 0;
    switch (style.fg_color) {
        case TUI_COLOR_YELLOW: r = 255; g = 255; b = 0; break;
        case TUI_COLOR_RED:    r = 255; g = 0;   b = 0; break;
        case TUI_COLOR_MAGENTA:r = 255; g = 0;   b = 255; break;
        case TUI_COLOR_WHITE:  r = 255; g = 255; b = 255; break;
        case TUI_COLOR_BLACK:  r = 0;   g = 0;   b = 0; break;
        case TUI_COLOR_GRAY:   r = 192; g = 192; b = 192; break;
        default:               r = 255; g = 255; b = 255; break;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        drawText(text[i], r, g, b);
    }
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
    int max_lines = win->height - 3;
    int y = win->y + 2;
    int x = win->x + 1;
    int line = 0, col = 0;
    for (int i = 0; text[i] != '\0' && line < max_lines + scroll_offset; ++i) {
        if (line >= scroll_offset) {
            if (col == 0) {
                tui_set_cursor(x, y + line - scroll_offset);
            }
            char ch[2] = {text[i], '\0'};
            tui_draw_text(x + col, y + line - scroll_offset, ch, style);
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
    // If a window is provided, draw the status bar just below the window; otherwise, draw at the screen bottom.
    int y = (win == NULL) ? (tui_screen_height) : (win->y + win->height);
    int x = (win == NULL) ? 0 : win->x;
    int width = (win == NULL) ? tui_screen_width : win->width;
    // Map style to RGB
    int r = 255, g = 255, b = 255;
    switch (style.fg_color) {
        case TUI_COLOR_YELLOW: r = 255; g = 255; b = 0; break;
        case TUI_COLOR_RED:    r = 255; g = 0;   b = 0; break;
        case TUI_COLOR_MAGENTA:r = 255; g = 0;   b = 255; break;
        case TUI_COLOR_WHITE:  r = 255; g = 255; b = 255; break;
        case TUI_COLOR_BLACK:  r = 0;   g = 0;   b = 0; break;
        case TUI_COLOR_GRAY:   r = 192; g = 192; b = 192; break;
        default:               r = 255; g = 255; b = 255; break;
    }

    // Convert TUI grid coords to pixel coords
    int px = x * 8;
    int py = y * 8;
    int clip_min = px;
    int clip_max = (x + width) * 8 - 8; // last character must fit 8 pixels

    // Draw characters one-by-one clipped to the provided window area
    for (int i = 0; text && text[i]; ++i) {
        int cx = px + i * 8;
        if (cx + 7 > clip_max) break;
        if (cx < clip_min) continue; // should not happen for left-aligned, but safe
        drawCharAt(cx, py, (int)(unsigned char)text[i], r, g, b);
    }
}

int tui_read_key() {
    static uint8_t ctrl_pressed = 0;
    static uint8_t shift_pressed = 0;
    static uint8_t super_pressed = 0;
    static uint8_t caps_lock = 0;

    // Check controller status (non-blocking)
    uint8_t status = inportb(0x64);
    if (!(status & 0x1)) return 0; // no data waiting

    // If the byte in the output buffer is AUX (mouse) data, do NOT consume it here.
    // Leave it for the mouse driver (ISR or mouse_poll) to process to avoid packet desync.
    if (status & 0x20) return 0;

    // Read one scancode
    uint8_t scancode = inportb(0x60);

    // Key release
    if (scancode & 0x80) {
        uint8_t realcode = scancode & 0x7F;
        if (realcode == 42 || realcode == 54) shift_pressed = 0;
        if (realcode == 29) ctrl_pressed = 0;
        if (realcode == 91) super_pressed = 0;
        if (realcode == 56) tui_alt_pressed = 0; // left Alt release
        return 0;
    }

    // Modifiers and toggles
    if (scancode == 42 || scancode == 54) { shift_pressed = 1; return 0; }
    if (scancode == 29) { ctrl_pressed = 1; return 0; }
    if (scancode == 91) { super_pressed = 1; return 0; }
    if (scancode == 56) { tui_alt_pressed = 1; return 0; } // left Alt press
    if (scancode == 58) { caps_lock = !caps_lock; return 0; } // Caps Lock toggle

    // Ctrl combos
    if (ctrl_pressed) {
        if (scancode == 24) return 0x2001; // Ctrl+O (save)
        if (scancode == 45) return 0x2002; // Ctrl+X (exit)
        if (scancode == 16) return 0x2101; // Ctrl+Q (quit)
        if (scancode == 13) return 0x2102; // Ctrl+Plus/Equals (zoom in)
        if (scancode == 12) return 0x2103; // Ctrl+Minus (zoom out)
    }

    // Letters with Shift/Caps
    int is_letter = 0; char base = 0;
    switch (scancode) {
        case 16: base = 'q'; is_letter = 1; break; case 17: base = 'w'; is_letter = 1; break;
        case 18: base = 'e'; is_letter = 1; break; case 19: base = 'r'; is_letter = 1; break;
        case 20: base = 't'; is_letter = 1; break; case 21: base = 'y'; is_letter = 1; break;
        case 22: base = 'u'; is_letter = 1; break; case 23: base = 'i'; is_letter = 1; break;
        case 24: base = 'o'; is_letter = 1; break; case 25: base = 'p'; is_letter = 1; break;
        case 30: base = 'a'; is_letter = 1; break; case 31: base = 's'; is_letter = 1; break;
        case 32: base = 'd'; is_letter = 1; break; case 33: base = 'f'; is_letter = 1; break;
        case 34: base = 'g'; is_letter = 1; break; case 35: base = 'h'; is_letter = 1; break;
        case 36: base = 'j'; is_letter = 1; break; case 37: base = 'k'; is_letter = 1; break;
        case 38: base = 'l'; is_letter = 1; break; case 44: base = 'z'; is_letter = 1; break;
        case 45: base = 'x'; is_letter = 1; break; case 46: base = 'c'; is_letter = 1; break;
        case 47: base = 'v'; is_letter = 1; break; case 48: base = 'b'; is_letter = 1; break;
        case 49: base = 'n'; is_letter = 1; break; case 50: base = 'm'; is_letter = 1; break;
    }
    if (is_letter) {
        int upper = (caps_lock && !shift_pressed) || (!caps_lock && shift_pressed);
        int ret = upper ? (base - 32) : base;
        return super_pressed ? (0x4000 | ret) : ret;
    }

    // Non-letter keys
    switch (scancode) {
        case 72: return super_pressed ? (0x4000 | 0x1001) : 0x1001; // Up
        case 80: return super_pressed ? (0x4000 | 0x1002) : 0x1002; // Down
        case 75: return super_pressed ? (0x4000 | 0x1003) : 0x1003; // Left
        case 77: return super_pressed ? (0x4000 | 0x1004) : 0x1004; // Right
        case 14: return '\b';
        case 28: return '\n';
        case 1:  return 27; // Esc
        case 83: return 0x1005; // Del
        case 71: return 0x1006; // Home
        case 73: return 0x1008; // PgUp
        case 79: return 0x1007; // End
        case 81: return 0x1009; // PgDn
        case 2: return shift_pressed ? '!' : '1';
        case 3: return shift_pressed ? '@' : '2';
        case 4: return shift_pressed ? '#' : '3';
        case 5: return shift_pressed ? '$' : '4';
        case 6: return shift_pressed ? '%' : '5';
        case 7: return shift_pressed ? '^' : '6';
        case 8: return shift_pressed ? '&' : '7';
        case 9: return shift_pressed ? '*' : '8';
        case 10: return shift_pressed ? '(' : '9';
        case 11: return shift_pressed ? ')' : '0';
        case 12: return shift_pressed ? '_' : '-';
        case 13: return shift_pressed ? '+' : '=';
        case 26: return shift_pressed ? '{' : '[';
        case 27: return shift_pressed ? '}' : ']';
        case 39: return shift_pressed ? ':' : ';';
        case 40: return shift_pressed ? '"' : '\'';
        case 41: return shift_pressed ? '~' : '`';
        case 43: return shift_pressed ? '|' : '\\';
        case 51: return shift_pressed ? '<' : ',';
        case 52: return shift_pressed ? '>' : '.';
        case 53: return shift_pressed ? '?' : '/';
        case 57: return ' ';
    }

    return 0;
}