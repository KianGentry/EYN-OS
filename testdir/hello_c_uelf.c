#include "../userland/eynos_syscall.h"

int main(void) {
    eyn_write_str("Hello from C (.uelf)!\n");
    eyn_write_str("Press q to quit.\n");

    for (;;) {
        int ch = eyn_sys_getkey();
        if (ch == 'q') {
            break;
        }
    }

    eyn_sys_exit(0);
}
