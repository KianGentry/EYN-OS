#include <hal/console.h>
#include <vga.h>

static uint8 g_console_r = 255;
static uint8 g_console_g = 255;
static uint8 g_console_b = 255;

void hal_console_putc(char c) {
    /* Keep behavior consistent with the VGA/printf convention: ignore CR. */
    if (c == '\r') {
        return;
    }

    /* Treat DEL as backspace for line editing parity. */
    if ((uint8)c == 0x7Fu) {
        c = '\b';
    }

    drawText((int)(uint8)c, (int)g_console_r, (int)g_console_g, (int)g_console_b);
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
    g_console_r = r;
    g_console_g = g;
    g_console_b = b;
}
