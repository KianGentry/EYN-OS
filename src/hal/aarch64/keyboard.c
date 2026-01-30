#include <hal/keyboard.h>

int uart_pl011_getc_nonblock(char* out_c);
int virtio_input_getkey_nonblock(uint32* out_key, uint32* out_mods);

static uint32 g_mods;

static uint8 g_uart_esc_state;
static char g_uart_csi_buf[8];
static uint8 g_uart_csi_len;

static uint32 uart_key_from_byte(uint8 b) {
    /* Normalize common control bytes into the TUI key encoding. */
    if (b == '\r') return (uint32)'\n';
    if (b == 0x7Fu) return (uint32)'\b';

    /* Best-effort Ctrl combos from terminal control bytes. */
    if (b == 0x03u) return 0x2206u; /* Ctrl+C */
    if (b == 0x16u) return 0x2207u; /* Ctrl+V */
    if (b == 0x11u) return 0x2101u; /* Ctrl+Q */
    if (b == 0x18u) return 0x210Bu; /* Ctrl+X */
    if (b == 0x1Au) return 0x2109u; /* Ctrl+Z */
    if (b == 0x19u) return 0x210Au; /* Ctrl+Y */
    if (b == 0x13u) return 0x2001u; /* Ctrl+S */
    if (b == 0x0Fu) return 0x2001u; /* Ctrl+O */

    return (uint32)b;
}

static uint32 uart_try_read_key(void) {
    /* Parse a minimal VT100/ANSI escape set from the UART byte stream. */
    for (;;) {
        char c;
        if (uart_pl011_getc_nonblock(&c) != 0) {
            return 0;
        }

        uint8 b = (uint8)c;

        if (g_uart_esc_state == 0) {
            if (b == 0x1Bu) {
                g_uart_esc_state = 1;
                continue;
            }
            return uart_key_from_byte(b);
        }

        if (g_uart_esc_state == 1) {
            if (b == (uint8)'[') {
                g_uart_esc_state = 2;
                g_uart_csi_len = 0;
                continue;
            }
            if (b == (uint8)'O') {
                g_uart_esc_state = 3;
                continue;
            }
            /* Unknown escape: treat as Esc and drop this byte. */
            g_uart_esc_state = 0;
            return 27u;
        }

        if (g_uart_esc_state == 3) {
            /* SS3 sequences: ESC O A/B/C/D, ESC O H/F. */
            g_uart_esc_state = 0;
            switch (b) {
                case 'A': return HAL_KEY_UP;
                case 'B': return HAL_KEY_DOWN;
                case 'C': return HAL_KEY_RIGHT;
                case 'D': return HAL_KEY_LEFT;
                case 'H': return HAL_KEY_HOME;
                case 'F': return HAL_KEY_END;
                default: return 0;
            }
        }

        /* CSI sequences: ESC [ ... */
        if (g_uart_esc_state == 2) {
            if (g_uart_csi_len < (uint8)(sizeof(g_uart_csi_buf) - 1u)) {
                g_uart_csi_buf[g_uart_csi_len++] = (char)b;
                g_uart_csi_buf[g_uart_csi_len] = '\0';
            }

            /* Final byte terminates sequence. */
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') {
                g_uart_esc_state = 0;

                if (b == 'A') return HAL_KEY_UP;
                if (b == 'B') return HAL_KEY_DOWN;
                if (b == 'C') return HAL_KEY_RIGHT;
                if (b == 'D') return HAL_KEY_LEFT;
                if (b == 'H') return HAL_KEY_HOME;
                if (b == 'F') return HAL_KEY_END;

                if (b == '~') {
                    /* Numeric parameters like 3~ (Delete), 5~ (PgUp), 6~ (PgDn). */
                    int n = 0;
                    for (uint8 i = 0; i < g_uart_csi_len; i++) {
                        char ch = g_uart_csi_buf[i];
                        if (ch >= '0' && ch <= '9') {
                            n = n * 10 + (ch - '0');
                        }
                    }
                    if (n == 1 || n == 7) return HAL_KEY_HOME;
                    if (n == 4 || n == 8) return HAL_KEY_END;
                    if (n == 3) return HAL_KEY_DELETE;
                    if (n == 5) return HAL_KEY_PGUP;
                    if (n == 6) return HAL_KEY_PGDN;
                }

                return 0;
            }

            /* Need more bytes for this CSI; keep consuming while available. */
            continue;
        }
    }
}

int hal_kbd_getc_nonblock(void) {
    uint32 key = hal_kbd_read_key_nonblock();
    if (key == 0) return 0;
    if (key <= 0xFFu) return (int)key;
    return 0;
}

uint32 hal_kbd_read_key_nonblock(void) {
    /* Prefer UART so -nographic remains usable. */
    uint32 key = uart_try_read_key();
    if (key != 0) return key;

    uint32 mods = 0;
    if (virtio_input_getkey_nonblock(&key, &mods) == 0) {
        g_mods = mods;
        return key;
    }

    return 0;
}

uint32 hal_kbd_modifiers(void) {
    return g_mods;
}
