#include <tui.h>
#include <vga.h>
#include <system.h>
#include <string.h>
#include <stdint.h>
#include <context.h>
#include <misc/sched.h>

// Screen dimensions (to be set by tui_init)
static int tui_screen_width = 80;
static int tui_screen_height = 25;

// Internal cursor position for manual text placement
static int tui_cur_x = 0;
static int tui_cur_y = 0;

// exported flag, updated by tui_read_key
int tui_alt_pressed = 0;
int tui_shift_pressed = 0;

static int tui_ctx_allow(uint32 caps) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, SCHED_COST_CONSOLE);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

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
    if (!tui_ctx_allow(CAP_WRITE_CONSOLE)) return;
    clearScreen();
}

void tui_refresh() {
    // No buffering needed
}

void tui_draw_text(int x, int y, const char* text, tui_style_t style) {
    if (!tui_ctx_allow(CAP_WRITE_CONSOLE)) return;
    int cw = vga_text_cell_w();
    int ch = vga_text_cell_h();
    int rr = 0, gg = 0, bb = 0;
    switch (style.fg_colour) {
        case TUI_COLOUR_YELLOW: rr = 255; gg = 255; bb = 0; break;
        case TUI_COLOUR_RED:    rr = 255; gg = 0;   bb = 0; break;
        case TUI_COLOUR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
        case TUI_COLOUR_WHITE:  rr = 255; gg = 255; bb = 255; break;
        case TUI_COLOUR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
        case TUI_COLOUR_GRAY:   rr = 192; gg = 192; bb = 192; break;
        default:               rr = 255; gg = 255; bb = 255; break;
    }
    // Use side-effect-free pixel-based text to avoid console cursor side effects
    drawTextAt(x * cw, y * ch, text, rr, gg, bb);
}

void tui_draw_window(const tui_window_t* win) {
    if (!tui_ctx_allow(CAP_WRITE_CONSOLE)) return;
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
        if ((i & 0x7) == 0) tui_ctx_allow(CAP_WRITE_CONSOLE);
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
    if (!tui_ctx_allow(CAP_WRITE_CONSOLE)) return;
    int cell_w = vga_text_cell_w();
    int cell_h = vga_text_cell_h();
    int max_lines = win->height - 3;
    int y = win->y + 2;
    int x = win->x + 1;
    int line = 0, col = 0;
    for (int i = 0; text[i] != '\0' && line < max_lines + scroll_offset; ++i) {
        if ((i & 0x3F) == 0) tui_ctx_allow(CAP_WRITE_CONSOLE);
        if (line >= scroll_offset) {
            // Draw this character at the appropriate pixel position directly to avoid
            // any global console cursor side effects.
            char chbuf[2] = {text[i], '\0'};
            int px = (x + col) * cell_w;
            int py = (y + line - scroll_offset) * cell_h;
            int rr = 255, gg = 255, bb = 255;
            switch (style.fg_colour) {
                case TUI_COLOUR_YELLOW: rr = 255; gg = 255; bb = 0; break;
                case TUI_COLOUR_RED:    rr = 255; gg = 0;   bb = 0; break;
                case TUI_COLOUR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
                case TUI_COLOUR_WHITE:  rr = 255; gg = 255; bb = 255; break;
                case TUI_COLOUR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
                case TUI_COLOUR_GRAY:   rr = 192; gg = 192; bb = 192; break;
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
    if (!tui_ctx_allow(CAP_WRITE_CONSOLE)) return;
    int cw = vga_text_cell_w();
    int ch = vga_text_cell_h();
    // If a window is provided, draw the status bar just below the window; otherwise, draw at the screen bottom.
    // Clamp to last visible row (0-based). Previous code used height itself, which is off-by-one.
    int y = (win == NULL) ? (tui_screen_height - 1) : (win->y + win->height - 1);
    int x = (win == NULL) ? 0 : win->x;
    int bar_width = (win == NULL) ? tui_screen_width : win->width;
    // Map style to RGB
    int rr = 255, gg = 255, bb = 255;
    switch (style.fg_colour) {
        case TUI_COLOUR_YELLOW: rr = 255; gg = 255; bb = 0; break;
        case TUI_COLOUR_RED:    rr = 255; gg = 0;   bb = 0; break;
        case TUI_COLOUR_MAGENTA:rr = 255; gg = 0;   bb = 255; break;
        case TUI_COLOUR_WHITE:  rr = 255; gg = 255; bb = 255; break;
        case TUI_COLOUR_BLACK:  rr = 0;   gg = 0;   bb = 0; break;
        case TUI_COLOUR_GRAY:   rr = 192; gg = 192; bb = 192; break;
        default:               rr = 255; gg = 255; bb = 255; break;
    }

    // Convert TUI grid coords to pixel coords
    int px = x * cw;
    int py = y * ch;
    int clip_min = px;
    int clip_max = (x + bar_width) * cw - cw; // last character must fit

    // Draw characters one-by-one clipped to the provided window area
    for (int i = 0; text && text[i]; ++i) {
        int cx = px + i * cw;
        if (cx + (cw - 1) > clip_max) break;
        if (cx < clip_min) continue; // should not happen for left-aligned, but safe
        drawCharAt(cx, py, (int)(unsigned char)text[i], rr, gg, bb);
    }
}

int tui_read_key() {
    static uint8_t ctrl_pressed = 0;
    static uint8_t shift_pressed = 0;
    static uint8_t super_pressed = 0;
    static uint8_t super_used = 0; /* set when another key is pressed while Super is held */
    static uint8_t caps_lock = 0;

    /*
     * Scancode-to-translated-key lookup table.
     *
     * On each key *press*, the translated key code is stored here indexed by
     * scancode (0–83).  On the matching key *release*, the stored value is
     * returned negated so that callers can distinguish press (>0) from
     * release (<0) without re-translating the scancode (which would require
     * knowing the modifier state at press time).
     *
     * Modifier keys (Shift, Ctrl, Super, Alt, CapsLock) are still handled
     * purely via the static flag variables; their releases return 0 as
     * before so existing callers (shell, tiling manager, etc.) are
     * unaffected by this change.
     */
    static int g_scancode_to_key[128];

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
        if (realcode == 42 || realcode == 54) { shift_pressed = 0; tui_shift_pressed = 0; }
        if (realcode == 29) ctrl_pressed = 0;
        if (realcode == 91) {
            int was_clean = super_pressed && !super_used;
            super_pressed = 0;
            if (was_clean) return 0x6001; /* Super alone: toggle start menu */
        }
        if (realcode == 56) tui_alt_pressed = 0; // left Alt release
        /* Return the negated translated key so callers can detect releases. */
        if (realcode < 128 && g_scancode_to_key[realcode]) {
            int released = g_scancode_to_key[realcode];
            g_scancode_to_key[realcode] = 0;
            return -released;
        }
        return 0;
    }

    // Modifiers and toggles
    if (scancode == 42 || scancode == 54) { shift_pressed = 1; tui_shift_pressed = 1; return 0; }
    if (scancode == 29) { ctrl_pressed = 1; return 0; }
    if (scancode == 91) { super_pressed = 1; super_used = 0; return 0; }
    if (scancode == 56) { tui_alt_pressed = 1; return 0; } // left Alt press
    if (scancode == 58) { caps_lock = !caps_lock; return 0; } // Caps Lock toggle

    /* Any non-modifier key while Super is held marks it as "used" (not a solo tap) */
    if (super_pressed) super_used = 1;

    // Ctrl combos
    if (ctrl_pressed) {
        if (scancode == 46) return 0x2206; // Ctrl+C (abort / interrupt)
        if (scancode == 47) return 0x2207; // Ctrl+V (paste)
        if (scancode == 24) return 0x2001; // Ctrl+O (save)
        if (scancode == 31) return 0x2001; // Ctrl+S (save)
        if (scancode == 45) return 0x210B; // Ctrl+X (cut)
        if (scancode == 16) return 0x2101; // Ctrl+Q (quit)
        if (scancode == 13) return 0x2102; // Ctrl+Plus/Equals (zoom in)
        if (scancode == 12) return 0x2103; // Ctrl+Minus (zoom out)
        if (scancode == 30) return 0x2104; // Ctrl+A (select all)
        if (scancode == 38) return 0x2105; // Ctrl+L (select line)
        if (scancode == 17) return 0x2106; // Ctrl+W (toggle whitespace)
        if (scancode == 33) return 0x2107; // Ctrl+F (find)
        if (scancode == 34) return 0x2108; // Ctrl+G (go to line)
        if (scancode == 44) return 0x2109; // Ctrl+Z (undo)
        if (scancode == 21) return 0x210A; // Ctrl+Y (redo)
        if (scancode == 35) return 0x210C; // Ctrl+H (replace / help)
        if (scancode == 49) return 0x210D; // Ctrl+N (new)
        if (scancode == 32) return 0x210E; // Ctrl+D (duplicate line / delete)
    }

    // Function keys F1-F12 (scancodes 59-68 for F1-F10, 87-88 for F11-F12)
    // Encoded as 0x5001..0x500C
    if (scancode >= 59 && scancode <= 68) return 0x5001 + (scancode - 59); // F1..F10
    if (scancode == 87) return 0x500B; // F11
    if (scancode == 88) return 0x500C; // F12

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
        default: break;
    }
    if (is_letter) {
        int upper = (caps_lock && !shift_pressed) || (!caps_lock && shift_pressed);
        int ret = upper ? (base - 32) : base;
        int key = super_pressed ? (0x4000 | ret) : ret;
        if (scancode < 128) g_scancode_to_key[scancode] = key;
        return key;
    }

    // Non-letter keys
    {
    int nk = 0;
    switch (scancode) {
    case 72: nk = (ctrl_pressed ? 0x8000 : 0) | (shift_pressed ? 0x3000 : 0) | (super_pressed ? 0x4000 : 0) | 0x1001; break; // Up
    case 80: nk = (ctrl_pressed ? 0x8000 : 0) | (shift_pressed ? 0x3000 : 0) | (super_pressed ? 0x4000 : 0) | 0x1002; break; // Down
    case 75: nk = (ctrl_pressed ? 0x8000 : 0) | (shift_pressed ? 0x3000 : 0) | (super_pressed ? 0x4000 : 0) | 0x1003; break; // Left
    case 77: nk = (ctrl_pressed ? 0x8000 : 0) | (shift_pressed ? 0x3000 : 0) | (super_pressed ? 0x4000 : 0) | 0x1004; break; // Right
        case 15: nk = '\t'; break;
        case 14: nk = '\b'; break;
        case 28: nk = '\n'; break;
        case 1:  nk = 27; break; // Esc
        case 83: nk = 0x1005; break; // Del
        case 71: nk = 0x1006; break; // Home
        case 73: nk = 0x1008; break; // PgUp
        case 79: nk = 0x1007; break; // End
        case 81: nk = 0x1009; break; // PgDn
        case 2: nk = shift_pressed ? '!' : '1'; break;
        case 3: nk = shift_pressed ? '@' : '2'; break;
        case 4: nk = shift_pressed ? '#' : '3'; break;
        case 5: nk = shift_pressed ? '$' : '4'; break;
        case 6: nk = shift_pressed ? '%' : '5'; break;
        case 7: nk = shift_pressed ? '^' : '6'; break;
        case 8: nk = shift_pressed ? '&' : '7'; break;
        case 9: nk = shift_pressed ? '*' : '8'; break;
        case 10: nk = shift_pressed ? '(' : '9'; break;
        case 11: nk = shift_pressed ? ')' : '0'; break;
        case 12: nk = shift_pressed ? '_' : '-'; break;
        case 13: nk = shift_pressed ? '+' : '='; break;
        case 26: nk = shift_pressed ? '{' : '['; break;
        case 27: nk = shift_pressed ? '}' : ']'; break;
        case 39: nk = shift_pressed ? ':' : ';'; break;
        case 40: nk = shift_pressed ? '"' : '\''; break;
        case 41: nk = shift_pressed ? '~' : '`'; break;
        case 43: nk = shift_pressed ? '|' : '\\'; break;
        case 51: nk = shift_pressed ? '<' : ','; break;
        case 52: nk = shift_pressed ? '>' : '.'; break;
        case 53: nk = shift_pressed ? '?' : '/'; break;
        case 57: nk = ' '; break;
        default: break;
    }
    if (nk) {
        if (scancode < 128) g_scancode_to_key[scancode] = nk;
        return nk;
    }
    }

    return 0;
}