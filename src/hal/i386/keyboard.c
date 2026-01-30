#include <hal/keyboard.h>
#include <kb.h>

int hal_kbd_getc_nonblock(void) {
    return kb_getchar_nonblocking();
}

uint32 hal_kbd_modifiers(void) {
    /* Not available from the existing non-blocking API yet. */
    return 0;
}
