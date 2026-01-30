#include <hal/keyboard.h>

int uart_pl011_getc_nonblock(char* out_c);
int virtio_input_getc_nonblock(char* out_c);

int hal_kbd_getc_nonblock(void) {
    char c;

    /* Prefer the UART so -nographic remains usable. */
    if (uart_pl011_getc_nonblock(&c) == 0) {
        return (int)(uint8)c;
    }

    if (virtio_input_getc_nonblock(&c) == 0) {
        return (int)(uint8)c;
    }

    return 0;
}

uint32 hal_kbd_modifiers(void) {
    /* Not exported by the virtio-input backend yet (best-effort API). */
    return 0;
}
