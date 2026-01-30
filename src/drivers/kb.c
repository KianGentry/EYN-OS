#include <kb.h>
#include <hal/keyboard.h>
#include <util.h>
#include <vga.h>

int kb_getchar_nonblocking(void) {
    return hal_kbd_getc_nonblock();
}

string readStr(void) {
    // Avoid heap usage: stdin reads can occur while the heap is unhealthy.
    // Single-core kernel: a single static buffer is sufficient here.
    static char buffstr_storage[200];
    string buffstr = buffstr_storage;
    uint32 i = 0;

    buffstr[0] = '\0';

    for (;;) {
        uint32 key = hal_kbd_read_key_nonblock();
        if (!key) continue;

        // Ctrl+C aborts
        if (key == HAL_KEY_CTRL_C || key == 0x03u) {
            g_user_interrupt = 1;
            buffstr[0] = '\0';
            printf("^C\n");
            return buffstr;
        }

        // Legacy line input only accepts ASCII bytes.
        if (key > 0xFFu) continue;
        char ch = (char)key;

        if (ch == '\n') {
            drawText('\n', 255, 255, 255);
            buffstr[i] = '\0';
            return buffstr;
        }

        if (ch == '\b') {
            if (i > 0) {
                drawText('\b', 255, 255, 255);
                i--;
                buffstr[i] = '\0';
            }
            continue;
        }

        if (i >= (sizeof(buffstr_storage) - 1)) continue;

        if ((unsigned char)ch >= 32u || ch == '\t') {
            drawText(ch, 255, 255, 255);
            buffstr[i++] = ch;
            buffstr[i] = '\0';
        }
    }
}

void poll_keyboard_for_ctrl_c(void) {
    // Note: consumes one pending key event per call (same behavior as the old
    // scancode version). Intended for long-running loops only.
    uint32 key = hal_kbd_read_key_nonblock();
    if (key == HAL_KEY_CTRL_C || key == 0x03u) {
        g_user_interrupt = 1;
    }
}