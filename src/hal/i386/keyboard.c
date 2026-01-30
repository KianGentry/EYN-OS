#include <hal/keyboard.h>
#include <system.h>

static uint8 g_ctrl_pressed = 0;
static uint8 g_shift_pressed = 0;
static uint8 g_super_pressed = 0;
static uint8 g_alt_pressed = 0;
static uint8 g_caps_lock = 0;

static uint32 kbd_mods_to_mask(void) {
    uint32 m = 0;
    if (g_shift_pressed) m |= HAL_KBD_MOD_SHIFT;
    if (g_ctrl_pressed) m |= HAL_KBD_MOD_CTRL;
    if (g_alt_pressed) m |= HAL_KBD_MOD_ALT;
    if (g_super_pressed) m |= HAL_KBD_MOD_SUPER;
    if (g_caps_lock) m |= HAL_KBD_MOD_CAPS;
    return m;
}

uint32 hal_kbd_read_key_nonblock(void) {
    /* Check controller status (non-blocking). */
    uint8 status = inportb(0x64);
    if (!(status & 0x1)) return 0; /* no data */

    /* If the byte in the output buffer is AUX (mouse) data, do NOT consume it. */
    if (status & 0x20) return 0;

    /* Read one scancode. */
    uint8 scancode = inportb(0x60);

    /* Key release. */
    if (scancode & 0x80) {
        uint8 realcode = scancode & 0x7F;
        if (realcode == 42 || realcode == 54) g_shift_pressed = 0;
        if (realcode == 29) g_ctrl_pressed = 0;
        if (realcode == 91) g_super_pressed = 0;
        if (realcode == 56) g_alt_pressed = 0;
        return 0;
    }

    /* Modifiers and toggles. */
    if (scancode == 42 || scancode == 54) { g_shift_pressed = 1; return 0; }
    if (scancode == 29) { g_ctrl_pressed = 1; return 0; }
    if (scancode == 91) { g_super_pressed = 1; return 0; }
    if (scancode == 56) { g_alt_pressed = 1; return 0; }
    if (scancode == 58) { g_caps_lock = !g_caps_lock; return 0; }

    /* Ctrl combos (match existing TUI encoding). */
    if (g_ctrl_pressed) {
        if (scancode == 46) return 0x2206u; /* Ctrl+C */
        if (scancode == 47) return 0x2207u; /* Ctrl+V */
        if (scancode == 24) return 0x2001u; /* Ctrl+O */
        if (scancode == 31) return 0x2001u; /* Ctrl+S */
        if (scancode == 45) return 0x210Bu; /* Ctrl+X */
        if (scancode == 16) return 0x2101u; /* Ctrl+Q */
        if (scancode == 13) return 0x2102u; /* Ctrl+Plus/Equals */
        if (scancode == 12) return 0x2103u; /* Ctrl+Minus */
        if (scancode == 30) return 0x2104u; /* Ctrl+A */
        if (scancode == 38) return 0x2105u; /* Ctrl+L */
        if (scancode == 17) return 0x2106u; /* Ctrl+W */
        if (scancode == 33) return 0x2107u; /* Ctrl+F */
        if (scancode == 34) return 0x2108u; /* Ctrl+G */
        if (scancode == 44) return 0x2109u; /* Ctrl+Z */
        if (scancode == 21) return 0x210Au; /* Ctrl+Y */
    }

    /* Letters with Shift/Caps. */
    int is_letter = 0;
    char base = 0;
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
        int upper = (g_caps_lock && !g_shift_pressed) || (!g_caps_lock && g_shift_pressed);
        uint32 ret = (uint32)(unsigned char)(upper ? (base - 32) : base);
        if (g_super_pressed) ret |= HAL_KEY_FLAG_SUPER;
        return ret;
    }

    /* Non-letter keys. */
    uint32 key = 0;
    switch (scancode) {
        case 72: key = HAL_KEY_UP; break;
        case 80: key = HAL_KEY_DOWN; break;
        case 75: key = HAL_KEY_LEFT; break;
        case 77: key = HAL_KEY_RIGHT; break;
        case 83: key = HAL_KEY_DELETE; break;
        case 71: key = HAL_KEY_HOME; break;
        case 79: key = HAL_KEY_END; break;
        case 73: key = HAL_KEY_PGUP; break;
        case 81: key = HAL_KEY_PGDN; break;

        case 15: key = (uint32)'\t'; break;
        case 14: key = (uint32)'\b'; break;
        case 28: key = (uint32)'\n'; break;
        case 1:  key = 27u; break; /* Esc */

        case 2: key = (uint32)(g_shift_pressed ? '!' : '1'); break;
        case 3: key = (uint32)(g_shift_pressed ? '@' : '2'); break;
        case 4: key = (uint32)(g_shift_pressed ? '#' : '3'); break;
        case 5: key = (uint32)(g_shift_pressed ? '$' : '4'); break;
        case 6: key = (uint32)(g_shift_pressed ? '%' : '5'); break;
        case 7: key = (uint32)(g_shift_pressed ? '^' : '6'); break;
        case 8: key = (uint32)(g_shift_pressed ? '&' : '7'); break;
        case 9: key = (uint32)(g_shift_pressed ? '*' : '8'); break;
        case 10: key = (uint32)(g_shift_pressed ? '(' : '9'); break;
        case 11: key = (uint32)(g_shift_pressed ? ')' : '0'); break;
        case 12: key = (uint32)(g_shift_pressed ? '_' : '-'); break;
        case 13: key = (uint32)(g_shift_pressed ? '+' : '='); break;
        case 26: key = (uint32)(g_shift_pressed ? '{' : '['); break;
        case 27: key = (uint32)(g_shift_pressed ? '}' : ']'); break;
        case 39: key = (uint32)(g_shift_pressed ? ':' : ';'); break;
        case 40: key = (uint32)(g_shift_pressed ? '"' : '\''); break;
        case 41: key = (uint32)(g_shift_pressed ? '~' : '`'); break;
        case 43: key = (uint32)(g_shift_pressed ? '|' : '\\'); break;
        case 51: key = (uint32)(g_shift_pressed ? '<' : ','); break;
        case 52: key = (uint32)(g_shift_pressed ? '>' : '.'); break;
        case 53: key = (uint32)(g_shift_pressed ? '?' : '/'); break;
        case 57: key = (uint32)' '; break;
        default: key = 0; break;
    }

    if (key == 0) return 0;

    /* Match TUI behavior: Shift flag is only encoded for arrow selection extension. */
    if (g_shift_pressed && (key >= HAL_KEY_UP && key <= HAL_KEY_RIGHT)) {
        key |= HAL_KEY_FLAG_SHIFTSEL;
    }
    if (g_super_pressed) {
        key |= HAL_KEY_FLAG_SUPER;
    }

    return key;
}

int hal_kbd_getc_nonblock(void) {
    uint32 key = hal_kbd_read_key_nonblock();
    if (key == 0) return 0;
    if (key <= 0xFFu) return (int)key;
    return 0;
}

uint32 hal_kbd_modifiers(void) {
    return kbd_mods_to_mask();
}
