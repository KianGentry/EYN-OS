#include <eynos_syscall.h>
#include <gui.h>

int gui_create(const char* title, const char* status_left) {
    return eyn_syscall3_pii(EYN_SYSCALL_GUI_CREATE, title, (int)status_left, 0);
}

int gui_set_title(int handle, const char* title) {
    return eyn_syscall3(EYN_SYSCALL_GUI_SET_TITLE, handle, title, 0);
}
