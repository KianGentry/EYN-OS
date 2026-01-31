#include <hal/console.h>
#include <drivers/aarch64/fb_simple.h>
#include <graphics/gfx.h>

void uart_pl011_putc(char c);

static void aarch64_uart_putc(char c) {
    /* Keep UART output readable in common terminal emulators. */
    if (c == '\n') {
        uart_pl011_putc('\r');
    }
    uart_pl011_putc(c);
}

void hal_console_putc(char c) {
    /* Treat DEL as backspace for line editing parity. */
    if ((uint8)c == 0x7Fu) {
        c = '\b';
    }

    /*
     * For UART, backspace is typically cursor-left only; emit "\b \b" so
     * line editing behaves correctly even in -serial stdio/-nographic flows.
     * fb_simple_putc already erases on '\b', so avoid double-erasing there.
     */
    if (c == '\b') {
        aarch64_uart_putc('\b');
        aarch64_uart_putc(' ');
        aarch64_uart_putc('\b');

        if (gfx_ready()) {
            gfx_putc('\b');
        }
        return;
    }

    aarch64_uart_putc(c);

    if (gfx_ready()) {
        gfx_putc(c);
    }
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
    /* Only affects the graphical console; UART output remains uncolored. */
    gfx_set_rgb(r, g, b);
}
