#include <stddef.h>
#include <stdint.h>

// EYN-OS syscall ABI: int 0x80
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

    // Network syscalls
    EYN_SYSCALL_NET_BIND     = 10,  // Bind UDP port -> socket_id
    EYN_SYSCALL_NET_SENDTO   = 11,  // Send UDP via socket_id
    EYN_SYSCALL_NET_RECVFROM = 12,  // Receive UDP from socket_id
    EYN_SYSCALL_NET_CLOSE    = 13,  // Close socket_id

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

    // Capability-based GUI create/attach (returns caps directly)
    EYN_SYSCALL_CAP_GUI_CREATE = 45,
    EYN_SYSCALL_CAP_GUI_ATTACH = 46,

    // Capability-based file descriptor write/seek operations
    EYN_SYSCALL_CAP_FD_WRITE = 43,
    EYN_SYSCALL_CAP_FD_SEEK = 44,

    // Deterministic execution mode
    EYN_SYSCALL_DET_ENABLE = 47,
    EYN_SYSCALL_DET_STEP = 48,

    // Filesystem mutation helpers
    // args: (const char* path)
    EYN_SYSCALL_MKDIR = 49,
    EYN_SYSCALL_UNLINK = 50,
    EYN_SYSCALL_RMDIR = 51,

    // Query current working directory (vterm cwd)
    // args: (char* buf, int buflen)
    // returns: bytes written excluding NUL, or -1
    EYN_SYSCALL_GETCWD = 52,

    // Low-memory streaming file writer (EYNFS only today)
    // begin: args (const char* path)
    // write: args (int handle, const void* buf, int len) -> bytes written or -1
    // end: args (int handle) -> 0 or -1
    EYN_SYSCALL_EYNFS_STREAM_BEGIN = 53,
    EYN_SYSCALL_EYNFS_STREAM_WRITE = 54,
    EYN_SYSCALL_EYNFS_STREAM_END = 55,

    // Audio (AC97) syscalls
    EYN_SYSCALL_AUDIO_PROBE        = 113,
    EYN_SYSCALL_AUDIO_INIT         = 114,
    EYN_SYSCALL_AUDIO_WRITE        = 115,
    EYN_SYSCALL_AUDIO_STOP         = 116,
    EYN_SYSCALL_AUDIO_IS_AVAILABLE = 117,
    EYN_SYSCALL_AUDIO_WRITE_BULK   = 118,
    EYN_SYSCALL_NET_DNS_RESOLVE    = 119,
    EYN_SYSCALL_NET_TCP_CONNECT    = 120,
    EYN_SYSCALL_NET_TCP_SEND       = 121,
    EYN_SYSCALL_NET_TCP_RECV       = 122,
    EYN_SYSCALL_NET_TCP_CLOSE      = 123,

    // IPC primitives
    EYN_SYSCALL_PIPE = 124,
    EYN_SYSCALL_MKFIFO = 125,
    EYN_SYSCALL_DUP = 126,
    EYN_SYSCALL_DUP2 = 127,
    EYN_SYSCALL_FD_SET_INHERIT = 128,
    EYN_SYSCALL_FD_SET_STDIO = 129,
    EYN_SYSCALL_FD_SET_NONBLOCK = 130,
    EYN_SYSCALL_SPAWN = 131,
    EYN_SYSCALL_WAITPID = 132,
    EYN_SYSCALL_GUI_LOAD_FONT = 137,
    EYN_SYSCALL_GUI_DRAW_TEXT_FONT = 138,
    EYN_SYSCALL_GUI_DRAW_CHAR_FONT = 139,
    EYN_SYSCALL_TTY_SET_MODE = 145,
    EYN_SYSCALL_TTY_GET_MODE = 146,
    EYN_SYSCALL_TTY_SET_WINSIZE = 147,
    EYN_SYSCALL_TTY_GET_WINSIZE = 148,
    EYN_SYSCALL_PTY_OPEN = 149,
};

#define EYN_TTY_MODE_RAW 0x0001

typedef struct {
    uint16_t rows;
    uint16_t cols;
} eyn_tty_winsize_t;

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

static inline int eyn_sys_write(int fd, const void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_WRITE), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_read(int fd, void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_READ), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_tty_set_mode(int mode_flags) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_TTY_SET_MODE), "b"(mode_flags)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_tty_get_mode(void) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_TTY_GET_MODE)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_tty_set_winsize(uint16_t rows, uint16_t cols) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_TTY_SET_WINSIZE), "b"((int)rows), "c"((int)cols)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_tty_get_winsize(eyn_tty_winsize_t* out) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_TTY_GET_WINSIZE), "b"(out)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_pty_open(int out_fds[2]) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_PTY_OPEN), "b"(out_fds)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_getkey(void) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_GETKEY)
        : "memory"
    );
    return ret;
}

__attribute__((noreturn))
static inline void eyn_sys_exit(int code) {
    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(EYN_SYSCALL_EXIT), "b"(code)
        : "memory"
    );
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static inline size_t eyn_strlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static inline void eyn_write_str(const char* s) {
    if (!s) return;
    (void)eyn_sys_write(1, s, (int)eyn_strlen(s));
}

// Network syscalls

// Bind a UDP port and return socket_id (>= 0) or error (< 0)
static inline int eyn_sys_net_bind(uint16_t port) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_NET_BIND), "b"(port)
        : "memory"
    );
    return ret;
}

// Send UDP via socket_id
// dst_ip: string "a.b.c.d"
// Returns bytes sent (>= 0) or error (< 0)
static inline int eyn_sys_net_sendto(int socket_id, const char* dst_ip, uint16_t dst_port, const void* buf, uint32_t len) {
    int ret;
    __asm__ __volatile__(
        "push %%esi\n\t"
        "mov 20(%%esp), %%esi\n\t"
        "int $0x80\n\t"
        "pop %%esi"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_NET_SENDTO), "b"(socket_id), "c"(dst_ip), "d"(dst_port), "m"(buf), "m"(len)
        : "memory"
    );
    return ret;
}

// Receive UDP from socket_id
// src_ip_out: uint8[4] buffer (or NULL)
// src_port_out: uint16* (or NULL)
// Returns 1 if packet received, 0 if none, < 0 on error
static inline int eyn_sys_net_recvfrom(int socket_id, void* buf, uint32_t buflen, void* src_ip_out, void* src_port_out) {
    int ret;
    __asm__ __volatile__(
        "push %%esi\n\t"
        "push %%edi\n\t"
        "mov 28(%%esp), %%esi\n\t"
        "mov 32(%%esp), %%edi\n\t"
        "int $0x80\n\t"
        "pop %%edi\n\t"
        "pop %%esi"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_NET_RECVFROM), "b"(socket_id), "c"(buf), "d"(buflen), "m"(src_ip_out), "m"(src_port_out)
        : "memory"
    );
    return ret;
}

// Close socket
static inline int eyn_sys_net_close(int socket_id) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_NET_CLOSE), "b"(socket_id)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_mint_fd(int fd, uint32_t rights, eyn_cap_t* out_cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_MINT_FD), "b"(fd), "c"(rights), "d"(out_cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_read(const eyn_cap_t* cap, void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_READ), "b"(cap), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_write(const eyn_cap_t* cap, const void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_WRITE), "b"(cap), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_seek(const eyn_cap_t* cap, int offset, int whence) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_SEEK), "b"(cap), "c"(offset), "d"(whence)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_close(const eyn_cap_t* cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_CLOSE), "b"(cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_mint_gui(int handle, uint32_t rights, eyn_cap_t* out_cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_MINT_GUI), "b"(handle), "c"(rights), "d"(out_cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_create(const char* title, const char* status_left, eyn_cap_t* out_cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_CREATE), "b"(title), "c"(status_left), "d"(out_cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_attach(const char* title, const char* status_left, eyn_cap_t* out_cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_ATTACH), "b"(title), "c"(status_left), "d"(out_cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_det_enable(int enabled) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_DET_ENABLE), "b"(enabled ? 1 : 0)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_det_step(uint32_t max_events) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_DET_STEP), "b"(max_events)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_begin(const eyn_cap_t* cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_BEGIN), "b"(cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_clear(const eyn_cap_t* cap, const void* rgb) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_CLEAR), "b"(cap), "c"(rgb)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_fill_rect(const eyn_cap_t* cap, const void* rect) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_FILL_RECT), "b"(cap), "c"(rect)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_draw_text(const eyn_cap_t* cap, const void* text_cmd) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_DRAW_TEXT), "b"(cap), "c"(text_cmd)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_draw_line(const eyn_cap_t* cap, const void* line_cmd) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_DRAW_LINE), "b"(cap), "c"(line_cmd)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_present(const eyn_cap_t* cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_PRESENT), "b"(cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_get_content_size(const eyn_cap_t* cap, void* out_size) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_GET_CONTENT_SIZE), "b"(cap), "c"(out_size)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_set_title(const eyn_cap_t* cap, const char* title) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_SET_TITLE), "b"(cap), "c"(title)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_set_font(const eyn_cap_t* cap, const char* path) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_SET_FONT), "b"(cap), "c"(path)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_set_continuous_redraw(const eyn_cap_t* cap, int enabled) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_SET_CONTINUOUS_REDRAW), "b"(cap), "c"(enabled)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_blit_rgb565(const eyn_cap_t* cap, const void* blit_cmd) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_BLIT_RGB565), "b"(cap), "c"(blit_cmd)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_poll_event(const eyn_cap_t* cap, void* out_event, int out_sz) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_POLL_EVENT), "b"(cap), "c"(out_event), "d"(out_sz)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_wait_event(const eyn_cap_t* cap, void* out_event, int out_sz) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_WAIT_EVENT), "b"(cap), "c"(out_event), "d"(out_sz)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_gui_close(const eyn_cap_t* cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_GUI_CLOSE), "b"(cap)
        : "memory"
    );
    return ret;
}

static inline void eyn_user_read_segments(uint16_t* out_cs, uint16_t* out_ds) {
    uint16_t cs = 0;
    uint16_t ds = 0;
    __asm__ __volatile__(
        "mov %%cs, %0\n\t"
        "mov %%ds, %1\n\t"
        : "=r"(cs), "=r"(ds)
    );
    if (out_cs) *out_cs = cs;
    if (out_ds) *out_ds = ds;
}

static inline int eyn_sys_cap_mint_fd(int fd, uint32_t rights, eyn_cap_t* out_cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_MINT_FD), "b"(fd), "c"(rights), "d"(out_cap)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_read(const eyn_cap_t* cap, void* buf, int len) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_READ), "b"(cap), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int eyn_sys_cap_fd_close(const eyn_cap_t* cap) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(EYN_SYSCALL_CAP_FD_CLOSE), "b"(cap)
        : "memory"
    );
    return ret;
}
