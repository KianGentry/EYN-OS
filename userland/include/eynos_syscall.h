// Low-level EYN-OS syscall ABI (int 0x80).
#include <stdint.h>

// eax = syscall number
// ebx/ecx/edx = args 1..3

enum {
    EYN_SYSCALL_WRITE  = 1,
    EYN_SYSCALL_EXIT   = 2,
    EYN_SYSCALL_READ   = 3,
    EYN_SYSCALL_OPEN   = 4,
    EYN_SYSCALL_CLOSE  = 5,
    EYN_SYSCALL_GETKEY = 6,
    EYN_SYSCALL_GETDENTS = 7,

    // GUI / tiling manager integration
    EYN_SYSCALL_GUI_CREATE    = 8,
    EYN_SYSCALL_GUI_SET_TITLE = 9,

    EYN_SYSCALL_GUI_BEGIN      = 10,
    EYN_SYSCALL_GUI_CLEAR      = 11,
    EYN_SYSCALL_GUI_FILL_RECT  = 12,
    EYN_SYSCALL_GUI_DRAW_TEXT  = 13,
    EYN_SYSCALL_GUI_PRESENT    = 14,
    EYN_SYSCALL_GUI_POLL_EVENT = 15,
    EYN_SYSCALL_GUI_WAIT_EVENT = 16,
    EYN_SYSCALL_GUI_ATTACH     = 17,

    EYN_SYSCALL_GUI_DRAW_LINE        = 18,
    EYN_SYSCALL_GUI_GET_CONTENT_SIZE = 19,

    // Set the active bitmap font for a GUI handle (path to .hex). Empty/NULL resets.
    EYN_SYSCALL_GUI_SET_FONT = 20,

    // Write an entire file (create/overwrite) from ring3.
    // args: (const char* path, const void* buf, int len)
    EYN_SYSCALL_WRITEFILE = 21,

    // Cooperative sleep (microseconds)
    EYN_SYSCALL_SLEEP_US = 22,

    // GUI continuous redraw control
    EYN_SYSCALL_GUI_SET_CONTINUOUS_REDRAW = 23,

    // GUI RGB565 blit (userland framebuffer)
    EYN_SYSCALL_GUI_BLIT_RGB565 = 24,

    // Capability-based file descriptor operations
    EYN_SYSCALL_CAP_MINT_FD = 25,
    EYN_SYSCALL_CAP_FD_READ = 26,
    EYN_SYSCALL_CAP_FD_CLOSE = 27,

    // Capability-based GUI operations
    EYN_SYSCALL_CAP_MINT_GUI = 28,
    EYN_SYSCALL_CAP_GUI_BEGIN = 29,
    EYN_SYSCALL_CAP_GUI_CLEAR = 30,
    EYN_SYSCALL_CAP_GUI_FILL_RECT = 31,
    EYN_SYSCALL_CAP_GUI_DRAW_TEXT = 32,
    EYN_SYSCALL_CAP_GUI_PRESENT = 33,
    EYN_SYSCALL_CAP_GUI_POLL_EVENT = 34,
    EYN_SYSCALL_CAP_GUI_WAIT_EVENT = 35,
    EYN_SYSCALL_CAP_GUI_DRAW_LINE = 36,
    EYN_SYSCALL_CAP_GUI_GET_CONTENT_SIZE = 37,
    EYN_SYSCALL_CAP_GUI_SET_TITLE = 38,
    EYN_SYSCALL_CAP_GUI_SET_FONT = 39,
    EYN_SYSCALL_CAP_GUI_SET_CONTINUOUS_REDRAW = 40,
    EYN_SYSCALL_CAP_GUI_BLIT_RGB565 = 41,
    EYN_SYSCALL_CAP_GUI_CLOSE = 42,

    // Capability-based file descriptor write/seek operations
    EYN_SYSCALL_CAP_FD_WRITE = 43,
    EYN_SYSCALL_CAP_FD_SEEK = 44,
};

enum {
    EYN_CAP_OBJ_USER_FD = 1,
    EYN_CAP_OBJ_USER_GUI = 2,
    EYN_CAP_OBJ_THREAD = 3,
    EYN_CAP_OBJ_IPC = 4,
    EYN_CAP_OBJ_MEMORY = 5,
};

enum {
    EYN_CAP_R_READ = 1u << 0,
    EYN_CAP_R_WRITE = 1u << 1,
    EYN_CAP_R_EXEC = 1u << 2,
    EYN_CAP_R_SIGNAL = 1u << 3,
    EYN_CAP_R_GRANT = 1u << 4,
    EYN_CAP_R_CLOSE = 1u << 5,
    EYN_CAP_R_SEEK = 1u << 6,
};

typedef struct {
    uint32_t obj;
    uint32_t type;
    uint32_t rights;
    uint32_t epoch;
    uint32_t tag_lo;
    uint32_t tag_hi;
} eyn_cap_t;

static inline int eyn_syscall3(int n, int a1, const void* a2, int a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall3_pii(int n, const void* a1, int a2, int a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall3_ppi(int n, const void* a1, const void* a2, int a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall3_iip(int n, int a1, int a2, const void* a3) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall1(int n, int a1);
static inline int eyn_syscall0(int n);

static inline int eyn_sys_cap_mint_fd(int fd, uint32_t rights, eyn_cap_t* out_cap) {
    return eyn_syscall3_iip(EYN_SYSCALL_CAP_MINT_FD, fd, (int)rights, out_cap);
}

static inline int eyn_sys_cap_fd_read(const eyn_cap_t* cap, void* buf, int len) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_FD_READ, cap, buf, len);
}

static inline int eyn_sys_cap_fd_close(const eyn_cap_t* cap) {
    return eyn_syscall1(EYN_SYSCALL_CAP_FD_CLOSE, (int)(uintptr_t)cap);
}

static inline int eyn_sys_cap_fd_write(const eyn_cap_t* cap, const void* buf, int len) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_FD_WRITE, cap, buf, len);
}

static inline int eyn_sys_cap_fd_seek(const eyn_cap_t* cap, int offset, int whence) {
    return eyn_syscall3_pii(EYN_SYSCALL_CAP_FD_SEEK, cap, offset, whence);
}

static inline int eyn_sys_cap_mint_gui(int handle, uint32_t rights, eyn_cap_t* out_cap) {
    return eyn_syscall3_iip(EYN_SYSCALL_CAP_MINT_GUI, handle, (int)rights, out_cap);
}

static inline int eyn_sys_cap_gui_begin(const eyn_cap_t* cap) {
    return eyn_syscall1(EYN_SYSCALL_CAP_GUI_BEGIN, (int)(uintptr_t)cap);
}

static inline int eyn_sys_cap_gui_clear(const eyn_cap_t* cap, const void* rgb) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_CLEAR, cap, rgb, 0);
}

static inline int eyn_sys_cap_gui_fill_rect(const eyn_cap_t* cap, const void* rect) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_FILL_RECT, cap, rect, 0);
}

static inline int eyn_sys_cap_gui_draw_text(const eyn_cap_t* cap, const void* text_cmd) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_DRAW_TEXT, cap, text_cmd, 0);
}

static inline int eyn_sys_cap_gui_draw_line(const eyn_cap_t* cap, const void* line_cmd) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_DRAW_LINE, cap, line_cmd, 0);
}

static inline int eyn_sys_cap_gui_present(const eyn_cap_t* cap) {
    return eyn_syscall1(EYN_SYSCALL_CAP_GUI_PRESENT, (int)(uintptr_t)cap);
}

static inline int eyn_sys_cap_gui_get_content_size(const eyn_cap_t* cap, void* out_size) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_GET_CONTENT_SIZE, cap, out_size, 0);
}

static inline int eyn_sys_cap_gui_set_title(const eyn_cap_t* cap, const char* title) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_SET_TITLE, cap, title, 0);
}

static inline int eyn_sys_cap_gui_set_font(const eyn_cap_t* cap, const char* path) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_SET_FONT, cap, path, 0);
}

static inline int eyn_sys_cap_gui_set_continuous_redraw(const eyn_cap_t* cap, int enabled) {
    return eyn_syscall3_pii(EYN_SYSCALL_CAP_GUI_SET_CONTINUOUS_REDRAW, cap, enabled, 0);
}

static inline int eyn_sys_cap_gui_blit_rgb565(const eyn_cap_t* cap, const void* blit_cmd) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_BLIT_RGB565, cap, blit_cmd, 0);
}

static inline int eyn_sys_cap_gui_poll_event(const eyn_cap_t* cap, void* out_event, int out_sz) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_POLL_EVENT, cap, out_event, out_sz);
}

static inline int eyn_sys_cap_gui_wait_event(const eyn_cap_t* cap, void* out_event, int out_sz) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_WAIT_EVENT, cap, out_event, out_sz);
}

static inline int eyn_sys_cap_gui_close(const eyn_cap_t* cap) {
    return eyn_syscall1(EYN_SYSCALL_CAP_GUI_CLOSE, (int)(uintptr_t)cap);
}

static inline int eyn_syscall1(int n, int a1) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline int eyn_syscall0(int n) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(n)
        : "memory"
    );
    return ret;
}
