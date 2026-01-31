#include <hal/console.h>
#include <graphics/gfx.h>

void hal_console_putc(char c) {
    /* Keep behavior consistent with the VGA/printf convention: ignore CR. */
    if (c == '\r') {
        return;
    }

    /* Treat DEL as backspace for line editing parity. */
    if ((uint8)c == 0x7Fu) {
        c = '\b';
    }

    gfx_putc(c);
}

void hal_console_write(const char* s) {
    if (!s) return;
    while (*s) {
        hal_console_putc(*s++);
    }
}

void hal_console_write_len(const char* s, uint32 len) {
    if (!s) return;
    for (uint32 i = 0; i < len; i++) {
        hal_console_putc(s[i]);
    }
}

void hal_console_set_rgb(uint8 r, uint8 g, uint8 b) {
    gfx_set_rgb(r, g, b);
}
