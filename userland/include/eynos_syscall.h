// Low-level EYN-OS syscall ABI (int 0x80).
#ifndef EYNOS_SYSCALL_H
#define EYNOS_SYSCALL_H

#include <stddef.h>
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

    // Set the active font for a GUI handle (path to .hex/.otf/.ttf). Empty/NULL resets.
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

    EYN_SYSCALL_DRIVE_SET_LOGICAL = 56,
    EYN_SYSCALL_DRIVE_GET_LOGICAL = 57,
    EYN_SYSCALL_DRIVE_GET_COUNT = 58,
    EYN_SYSCALL_DRIVE_IS_PRESENT = 59,
    EYN_SYSCALL_INIT_SERVICES = 60,
    EYN_SYSCALL_SERIAL_WRITE_COM1 = 61,
    EYN_SYSCALL_SHELL_LOG_SET = 62,
    EYN_SYSCALL_SHELL_LOG_GET = 63,
    EYN_SYSCALL_CRASHLOG_COUNT = 64,
    EYN_SYSCALL_CRASHLOG_INFO = 65,
    EYN_SYSCALL_CRASHLOG_DATA = 66,
    EYN_SYSCALL_CRASHLOG_CLEAR = 67,
    EYN_SYSCALL_SHELL_MIGRATED_DISPATCH = 68,

    EYN_SYSCALL_PCI_GET_COUNT = 69,
    EYN_SYSCALL_PCI_GET_ENTRY = 70,
    EYN_SYSCALL_E1000_PROBE = 71,
    EYN_SYSCALL_E1000_INIT = 72,
    EYN_SYSCALL_NETCFG_GET = 73,
    EYN_SYSCALL_NETCFG_SET = 74,
    EYN_SYSCALL_NETCFG_DEFAULTS = 75,
    EYN_SYSCALL_NET_IS_INITED = 76,
    EYN_SYSCALL_NET_GET_MAC = 77,
    EYN_SYSCALL_NET_GET_ARP = 78,
    EYN_SYSCALL_NET_GET_UDP_STATS = 79,
    EYN_SYSCALL_NET_GET_IP_STATS = 80,
    EYN_SYSCALL_NET_GET_SOCKETS = 81,
    EYN_SYSCALL_NET_PING = 82,
    EYN_SYSCALL_FS_CHECK_INTEGRITY = 83,
    EYN_SYSCALL_FS_FATFIX = 84,
    EYN_SYSCALL_VTERM_CLEAR = 85,
    EYN_SYSCALL_HISTORY_COUNT = 86,
    EYN_SYSCALL_HISTORY_ENTRY = 87,
    EYN_SYSCALL_HISTORY_CLEAR = 88,
    EYN_SYSCALL_BG_JOB_COUNT = 89,
    EYN_SYSCALL_BG_JOB_INFO = 90,
    EYN_SYSCALL_TILING_START = 91,
    EYN_SYSCALL_SETBG_PATH = 92,
    EYN_SYSCALL_CLEARBG_FOCUSED = 93,
    EYN_SYSCALL_SETFONT_PATH = 94,

    EYN_SYSCALL_CHDIR = 95,
    EYN_SYSCALL_RUN = 96,
    EYN_SYSCALL_WRITE_EDITOR = 97,
    EYN_SYSCALL_MMAP = 98,
    EYN_SYSCALL_MUNMAP = 99,
    EYN_SYSCALL_MSYNC = 100,
    EYN_SYSCALL_PAGING_GUARDS = 101,
    EYN_SYSCALL_PANIC = 102,
    EYN_SYSCALL_PF = 103,
    EYN_SYSCALL_RING3 = 104,

    // GUI icon drawing -- renders a named icon from the kernel-side icon cache
    EYN_SYSCALL_GUI_DRAW_ICON = 105,

    // GUI outlined rectangle (1px border, no fill)
    EYN_SYSCALL_GUI_OUTLINE_RECT = 106,

    // GUI single-character draw (more efficient than draw_text for per-char rendering)
    EYN_SYSCALL_GUI_DRAW_CHAR = 107,

    // GUI font metrics query (returns char width and height in pixels)
    EYN_SYSCALL_GUI_GET_FONT_METRICS = 108,

    /*
     * ABI-INVARIANT: GET_TICKS_MS (109) and LSEEK (110) are locked.
     *
     * GET_TICKS_MS: Returns uint32 milliseconds since kernel boot.
     *   args: none
     *   returns: ms elapsed (wraps at ~49.7 days)
     *
     * LSEEK: Reposition an open file descriptor's read offset.
     *   args: (int fd, int32 offset, int whence)
     *         whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
     *   returns: new offset >= 0, or -1 on error
     */
    EYN_SYSCALL_GET_TICKS_MS = 109,
    EYN_SYSCALL_LSEEK        = 110,
    /*
     * EYN_SYSCALL_GUI_WARP_MOUSE (111)
     * gui_warp_mouse(handle, content_x, content_y)
     * Moves the physical cursor to (content_x, content_y) within the window
     * content area.  Accumulated mouse deltas are zeroed after the warp.
     * Returns 0 on success, -1 on error.
     */
    EYN_SYSCALL_GUI_WARP_MOUSE = 111,
    /*
     * EYN_SYSCALL_GUI_SET_CURSOR_VISIBLE (112)
     * gui_set_cursor_visible(handle, visible)
     * Shows (visible=1) or hides (visible=0) the mouse cursor sprite.
     * Intended for games that grab the mouse via gui_warp_mouse.
     * Returns 0 on success, -1 on error.
     */
    EYN_SYSCALL_GUI_SET_CURSOR_VISIBLE = 112,

    /* ---- Audio (AC97) syscalls ---- */

    /*
     * EYN_SYSCALL_AUDIO_PROBE (113)
     * Detect whether an AC97 audio controller is present.
     * Returns 0 if found, -1 if absent.
     */
    EYN_SYSCALL_AUDIO_PROBE = 113,

    /*
     * EYN_SYSCALL_AUDIO_INIT (114)
     * Initialise the AC97 controller (DMA, codec, IRQ).
     * Returns 0 on success, -1 on failure.
     */
    EYN_SYSCALL_AUDIO_INIT = 114,

    /*
     * EYN_SYSCALL_AUDIO_WRITE (115)
     * audio_write(const void* buf, int size_bytes)
     * Submit 16-bit signed LE stereo PCM at 48 kHz for playback.
     * size_bytes is clamped to 4096.
     * Returns 0 on success, -1 on error.
     */
    EYN_SYSCALL_AUDIO_WRITE = 115,

    /*
     * EYN_SYSCALL_AUDIO_STOP (116)
     * Stop playback and silence output.
     * Returns 0.
     */
    EYN_SYSCALL_AUDIO_STOP = 116,

    /*
     * EYN_SYSCALL_AUDIO_IS_AVAILABLE (117)
     * Returns 1 if audio is initialised, 0 otherwise.
     */
    EYN_SYSCALL_AUDIO_IS_AVAILABLE = 117,

    /*
     * EYN_SYSCALL_AUDIO_WRITE_BULK (118)
     * audio_write_bulk(const void* buf, int total_size_bytes)
     * Submit up to 32 KB of 48 kHz stereo s16le PCM in one syscall.
     * The kernel splits the buffer into 4 KB DMA chunks and queues them.
     * Returns the number of 4 KB chunks queued (>= 0), or -1 on error.
     */
    EYN_SYSCALL_AUDIO_WRITE_BULK = 118,
    EYN_SYSCALL_NET_DNS_RESOLVE = 119,
    EYN_SYSCALL_NET_TCP_CONNECT = 120,
    EYN_SYSCALL_NET_TCP_SEND = 121,
    EYN_SYSCALL_NET_TCP_RECV = 122,
    EYN_SYSCALL_NET_TCP_CLOSE = 123,

    // IPC primitives
    // PIPE: args (int out_fds[2]) -> 0 or -1; out_fds[0]=read end, out_fds[1]=write end
    EYN_SYSCALL_PIPE = 124,
    // MKFIFO: args (const char* path) -> 0 or -1 (kernel-runtime FIFO namespace)
    EYN_SYSCALL_MKFIFO = 125,
    // DUP: args (int oldfd) -> new fd or -1
    EYN_SYSCALL_DUP = 126,
    // DUP2: args (int oldfd, int newfd) -> newfd or -1
    EYN_SYSCALL_DUP2 = 127,
    // FD inheritance mode for run/spawn flows: args (int enabled) -> previous mode
    EYN_SYSCALL_FD_SET_INHERIT = 128,
    // Explicit stdio remap for next/active task: args (int in_fd, int out_fd, int err_fd)
    EYN_SYSCALL_FD_SET_STDIO = 129,
    // Toggle non-blocking mode for a specific FD: args (int fd, int enabled)
    EYN_SYSCALL_FD_SET_NONBLOCK = 130,
    // Spawn a user program: args (const char* path, const char* const* argv, int argc)
    EYN_SYSCALL_SPAWN = 131,
    // Wait for a spawned PID: args (int pid, int* out_status, int flags)
    EYN_SYSCALL_WAITPID = 132,

    // Installer disk management syscalls
    EYN_SYSCALL_INSTALLER_PREPARE_DRIVE = 133,
    EYN_SYSCALL_INSTALLER_FORMAT_EYNFS_PARTITION = 134,
    EYN_SYSCALL_INSTALLER_WRITE_SECTOR = 135,
    EYN_SYSCALL_INSTALLER_GET_PARTITIONS = 136,

    // GUI per-window multi-font support
    // load: args (int handle, const char* font_path) -> font_id [1..8] or -1
    EYN_SYSCALL_GUI_LOAD_FONT = 137,
    // draw text with a specific loaded font id (0 = window default)
    EYN_SYSCALL_GUI_DRAW_TEXT_FONT = 138,
    // draw char with a specific loaded font id (0 = window default)
    EYN_SYSCALL_GUI_DRAW_CHAR_FONT = 139,
    // Set/get compositor workspace display profile (scale + aspect).
    EYN_SYSCALL_SET_DISPLAY_PROFILE = 140,
    EYN_SYSCALL_GET_DISPLAY_PROFILE = 141,
    // Runtime hardware display mode switch/query.
    EYN_SYSCALL_SET_DISPLAY_MODE = 142,
    EYN_SYSCALL_GET_DISPLAY_MODE = 143,

    // Post a kernel-rendered desktop notification toast.
    EYN_SYSCALL_NOTIFY_POST = 144,

    // Minimal TTY/PTY control for interactive clients.
    // set mode: args (int mode_flags) -> previous mode flags
    EYN_SYSCALL_TTY_SET_MODE = 145,
    // get mode: args () -> current mode flags
    EYN_SYSCALL_TTY_GET_MODE = 146,
    // set winsize: args (uint16 rows, uint16 cols)
    EYN_SYSCALL_TTY_SET_WINSIZE = 147,
    // get winsize: args (eyn_tty_winsize_t* out)
    EYN_SYSCALL_TTY_GET_WINSIZE = 148,
    // allocate PTY endpoints: args (int out_fds[2]) -> 0 or -1
    EYN_SYSCALL_PTY_OPEN = 149,
};

#define EYN_TTY_MODE_RAW 0x0001

typedef struct {
    uint16_t rows;
    uint16_t cols;
} eyn_tty_winsize_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t bar0_is_io;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t _pad;
    uint32_t bar0_base;
} eyn_pci_entry_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t _pad0;
    uint32_t bar0;
    uint32_t ctrl;
    uint32_t status;
    uint8_t mac[6];
    uint8_t _pad1[2];
    int32_t link_up;
} eyn_e1000_probe_info_t;

typedef struct {
    uint8_t local_ip[4];
    uint8_t gateway_ip[4];
    uint8_t netmask[4];
    uint8_t dns_ip[4];
} eyn_net_config_t;

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
    uint8_t _pad;
} eyn_net_arp_entry_t;

typedef struct {
    uint32_t udp_rx_enqueued;
    uint32_t udp_rx_dropped;
    uint32_t udp_rx_truncated;
    uint32_t udp_rx_bad_checksum;
    uint32_t udp_tx_checksums;
} eyn_net_udp_stats_t;

typedef struct {
    uint32_t ipv4_rx_fragments;
    uint32_t ipv4_rx_frag_dropped;
} eyn_net_ip_stats_t;

typedef struct {
    uint8_t bound;
    uint8_t _pad0;
    uint16_t port;
    uint32_t queued;
    uint32_t dropped;
} eyn_net_socket_info_t;

typedef struct {
    int32_t pid;
    int32_t status;
    int32_t active;
    char command[96];
} eyn_bg_job_info_t;

typedef struct {
    uint8_t present;
    uint8_t type;
    uint8_t bootable;
    uint8_t _pad;
    uint32_t lba_start;
    uint32_t sector_count;
} eyn_installer_partition_t;

typedef struct {
    uint32_t logical_drive;
    uint32_t physical_drive;
    uint32_t partition_count;
    eyn_installer_partition_t partitions[4];
} eyn_installer_partitions_t;

typedef struct {
    int32_t fb_w;
    int32_t fb_h;
    int32_t workspace_w;
    int32_t workspace_h;
    int32_t scale_pct;
    int32_t aspect_mode;
} eyn_display_profile_t;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t bpp;
    int32_t persist;
} eyn_display_mode_set_t;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t bpp;
    int32_t can_switch;
} eyn_display_mode_t;

#define EYN_ASPECT_NATIVE 0
#define EYN_ASPECT_4_3 1
#define EYN_ASPECT_16_10 2
#define EYN_ASPECT_16_9 3
#define EYN_ASPECT_21_9 4
#define EYN_ASPECT_1_1 5

typedef struct {
    uint32_t obj_type;
    uint32_t obj_id;
    uint32_t epoch;
    uint32_t data_len;
    uint32_t checksum;
} eyn_crashlog_record_info_t;

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

#ifdef __chibicc__
/*
 * chibicc does not support inline assembly.  The syscall primitives are
 * provided as assembly stubs by the chibicc UELF runtime.
 */
int eyn_syscall3(int n, int a1, const void* a2, int a3);
int eyn_syscall3_pii(int n, const void* a1, int a2, int a3);
int eyn_syscall3_ppi(int n, const void* a1, const void* a2, int a3);
int eyn_syscall3_iip(int n, int a1, int a2, const void* a3);
int eyn_syscall3_iii(int n, int a1, int a2, int a3);
int eyn_syscall1(int n, int a1);
int eyn_syscall0(int n);

static void eyn_user_read_segments(uint16_t* out_cs, uint16_t* out_ds) {
    if (out_cs) *out_cs = 0;
    if (out_ds) *out_ds = 0;
}

#else /* GCC -- inline assembly path */

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

/*
 * Three-integer argument syscall variant.
 * Used by SYSCALL_LSEEK (fd, offset, whence) where all args are integers.
 */
static inline int eyn_syscall3_iii(int n, int a1, int a2, int a3) {
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

static inline int eyn_sys_cap_gui_create(const char* title, const char* status_left, eyn_cap_t* out_cap) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_CREATE, title, status_left, (int)(uintptr_t)out_cap);
}

static inline int eyn_sys_cap_gui_attach(const char* title, const char* status_left, eyn_cap_t* out_cap) {
    return eyn_syscall3_ppi(EYN_SYSCALL_CAP_GUI_ATTACH, title, status_left, (int)(uintptr_t)out_cap);
}

static inline int eyn_sys_det_enable(int enabled) {
    return eyn_syscall1(EYN_SYSCALL_DET_ENABLE, enabled ? 1 : 0);
}

static inline int eyn_sys_det_step(uint32_t max_events) {
    return eyn_syscall1(EYN_SYSCALL_DET_STEP, (int)max_events);
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

static inline int eyn_sys_drive_set_logical(uint32_t logical_drive) {
    return eyn_syscall1(EYN_SYSCALL_DRIVE_SET_LOGICAL, (int)logical_drive);
}

static inline int eyn_sys_drive_get_logical(void) {
    return eyn_syscall0(EYN_SYSCALL_DRIVE_GET_LOGICAL);
}

static inline int eyn_sys_drive_get_count(void) {
    return eyn_syscall0(EYN_SYSCALL_DRIVE_GET_COUNT);
}

static inline int eyn_sys_drive_is_present(uint32_t logical_drive) {
    return eyn_syscall1(EYN_SYSCALL_DRIVE_IS_PRESENT, (int)logical_drive);
}

static inline int eyn_sys_init_services(void) {
    return eyn_syscall0(EYN_SYSCALL_INIT_SERVICES);
}

static inline int eyn_sys_serial_write_com1(const void* buf, int len) {
    return eyn_syscall3_pii(EYN_SYSCALL_SERIAL_WRITE_COM1, buf, len, 0);
}

static inline int eyn_sys_shell_log_set(int enabled) {
    return eyn_syscall1(EYN_SYSCALL_SHELL_LOG_SET, enabled ? 1 : 0);
}

static inline int eyn_sys_shell_log_get(void) {
    return eyn_syscall0(EYN_SYSCALL_SHELL_LOG_GET);
}

static inline int eyn_sys_crashlog_count(void) {
    return eyn_syscall0(EYN_SYSCALL_CRASHLOG_COUNT);
}

static inline int eyn_sys_crashlog_info(uint32_t index, eyn_crashlog_record_info_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_CRASHLOG_INFO, (int)index, 0, out);
}

static inline int eyn_sys_crashlog_data(uint32_t index, void* out, int out_cap) {
    return eyn_syscall3_iip(EYN_SYSCALL_CRASHLOG_DATA, (int)index, out_cap, out);
}

static inline int eyn_sys_crashlog_clear(void) {
    return eyn_syscall0(EYN_SYSCALL_CRASHLOG_CLEAR);
}

static inline int eyn_sys_shell_migrated_dispatch(const char* command_name, const char* raw_line) {
    return eyn_syscall3_ppi(EYN_SYSCALL_SHELL_MIGRATED_DISPATCH, command_name, raw_line, 0);
}

static inline int eyn_sys_pci_get_count(int net_only) {
    return eyn_syscall1(EYN_SYSCALL_PCI_GET_COUNT, net_only ? 1 : 0);
}

static inline int eyn_sys_pci_get_entry(int net_only, int index, eyn_pci_entry_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_PCI_GET_ENTRY, net_only ? 1 : 0, index, out);
}

static inline int eyn_sys_e1000_probe(eyn_e1000_probe_info_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_E1000_PROBE, 0, 0, out);
}

static inline int eyn_sys_e1000_init(void) {
    return eyn_syscall0(EYN_SYSCALL_E1000_INIT);
}

static inline int eyn_sys_netcfg_get(eyn_net_config_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_NETCFG_GET, 0, 0, out);
}

static inline int eyn_sys_netcfg_set(const eyn_net_config_t* in) {
    return eyn_syscall3_ppi(EYN_SYSCALL_NETCFG_SET, in, 0, 0);
}

static inline int eyn_sys_netcfg_defaults(void) {
    return eyn_syscall0(EYN_SYSCALL_NETCFG_DEFAULTS);
}

static inline int eyn_sys_net_is_inited(void) {
    return eyn_syscall0(EYN_SYSCALL_NET_IS_INITED);
}

static inline int eyn_sys_net_get_mac(uint8_t out_mac[6]) {
    return eyn_syscall3_iip(EYN_SYSCALL_NET_GET_MAC, 0, 0, out_mac);
}

static inline int eyn_sys_net_get_arp(eyn_net_arp_entry_t* out, int out_cap) {
    return eyn_syscall3_iip(EYN_SYSCALL_NET_GET_ARP, out_cap, 0, out);
}

static inline int eyn_sys_net_get_udp_stats(eyn_net_udp_stats_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_NET_GET_UDP_STATS, 0, 0, out);
}

static inline int eyn_sys_net_get_ip_stats(eyn_net_ip_stats_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_NET_GET_IP_STATS, 0, 0, out);
}

static inline int eyn_sys_net_get_sockets(eyn_net_socket_info_t* out, int out_cap) {
    return eyn_syscall3_iip(EYN_SYSCALL_NET_GET_SOCKETS, out_cap, 0, out);
}

static inline int eyn_sys_net_ping(const uint8_t dst_ip[4], const uint8_t local_ip[4], int count) {
    return eyn_syscall3_ppi(EYN_SYSCALL_NET_PING, dst_ip, local_ip, count);
}

// Resolve hostname to IPv4 using kernel DNS resolver.
static inline int eyn_sys_net_dns_resolve(const char* name, uint8_t out_ip[4]) {
    return eyn_syscall3_ppi(EYN_SYSCALL_NET_DNS_RESOLVE, name, out_ip, 0);
}

// Establish a TCP connection to dst_ip:dst_port (local_port=0 for ephemeral).
static inline int eyn_sys_net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t local_port) {
    return eyn_syscall3_pii(EYN_SYSCALL_NET_TCP_CONNECT, dst_ip, (int)dst_port, (int)local_port);
}

// Send payload on the current TCP connection.
static inline int eyn_sys_net_tcp_send(const void* buf, uint32_t len) {
    return eyn_syscall3_pii(EYN_SYSCALL_NET_TCP_SEND, buf, (int)len, 0);
}

// Receive payload from the current TCP connection.
// Returns bytes received, 0 if none, -2 if closed, or <0 on error.
static inline int eyn_sys_net_tcp_recv(void* buf, uint32_t buflen) {
    return eyn_syscall3_pii(EYN_SYSCALL_NET_TCP_RECV, buf, (int)buflen, 0);
}

// Close the current TCP connection.
static inline int eyn_sys_net_tcp_close(void) {
    return eyn_syscall1(EYN_SYSCALL_NET_TCP_CLOSE, 0);
}

static inline int eyn_sys_fs_check_integrity(void) {
    return eyn_syscall0(EYN_SYSCALL_FS_CHECK_INTEGRITY);
}

static inline int eyn_sys_fs_fatfix(const char* path) {
    return eyn_syscall1(EYN_SYSCALL_FS_FATFIX, (int)(uintptr_t)path);
}

static inline int eyn_sys_vterm_clear(void) {
    return eyn_syscall0(EYN_SYSCALL_VTERM_CLEAR);
}

static inline int eyn_sys_history_count(void) {
    return eyn_syscall0(EYN_SYSCALL_HISTORY_COUNT);
}

static inline int eyn_sys_history_entry(int index, char* out, int out_len) {
    return eyn_syscall3_iip(EYN_SYSCALL_HISTORY_ENTRY, index, out_len, out);
}

static inline int eyn_sys_history_clear(void) {
    return eyn_syscall0(EYN_SYSCALL_HISTORY_CLEAR);
}

static inline int eyn_sys_bg_job_count(void) {
    return eyn_syscall0(EYN_SYSCALL_BG_JOB_COUNT);
}

static inline int eyn_sys_bg_job_info(int index, eyn_bg_job_info_t* out) {
    return eyn_syscall3_iip(EYN_SYSCALL_BG_JOB_INFO, index, 0, out);
}

static inline int eyn_sys_tiling_start(void) {
    return eyn_syscall0(EYN_SYSCALL_TILING_START);
}

static inline int eyn_sys_setbg_path(const char* path) {
    return eyn_syscall1(EYN_SYSCALL_SETBG_PATH, (int)(uintptr_t)path);
}

static inline int eyn_sys_setbg_path_mode(const char* path, int mode) {
    return eyn_syscall3_pii(EYN_SYSCALL_SETBG_PATH, path, mode, 0);
}

static inline int eyn_sys_clearbg_focused(void) {
    return eyn_syscall0(EYN_SYSCALL_CLEARBG_FOCUSED);
}

static inline int eyn_sys_setfont_path(const char* path) {
    return eyn_syscall1(EYN_SYSCALL_SETFONT_PATH, (int)(uintptr_t)path);
}

static inline int eyn_sys_set_display_profile(int scale_pct, int aspect_mode, int persist) {
    return eyn_syscall3_iii(EYN_SYSCALL_SET_DISPLAY_PROFILE, scale_pct, aspect_mode, persist ? 1 : 0);
}

static inline int eyn_sys_get_display_profile(eyn_display_profile_t* out) {
    return eyn_syscall1(EYN_SYSCALL_GET_DISPLAY_PROFILE, (int)(uintptr_t)out);
}

static inline int eyn_sys_set_display_mode(const eyn_display_mode_set_t* req) {
    return eyn_syscall1(EYN_SYSCALL_SET_DISPLAY_MODE, (int)(uintptr_t)req);
}

static inline int eyn_sys_get_display_mode(eyn_display_mode_t* out) {
    return eyn_syscall1(EYN_SYSCALL_GET_DISPLAY_MODE, (int)(uintptr_t)out);
}

static inline int eyn_sys_notify_post(const char* title,
                                      const char* message,
                                      int level,
                                      uint32_t timeout_ms) {
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    if (timeout_ms > 0x00FFFFFFu) timeout_ms = 0x00FFFFFFu;

    uint32_t packed = (timeout_ms << 8) | ((uint32_t)level & 0xFFu);
    return eyn_syscall3_ppi(EYN_SYSCALL_NOTIFY_POST, title, message, (int)packed);
}

static inline int eyn_sys_tty_set_mode(int mode_flags) {
    return eyn_syscall1(EYN_SYSCALL_TTY_SET_MODE, mode_flags);
}

static inline int eyn_sys_tty_get_mode(void) {
    return eyn_syscall0(EYN_SYSCALL_TTY_GET_MODE);
}

static inline int eyn_sys_tty_set_winsize(uint16_t rows, uint16_t cols) {
    return eyn_syscall3_iii(EYN_SYSCALL_TTY_SET_WINSIZE, (int)rows, (int)cols, 0);
}

static inline int eyn_sys_tty_get_winsize(eyn_tty_winsize_t* out) {
    return eyn_syscall1(EYN_SYSCALL_TTY_GET_WINSIZE, (int)(uintptr_t)out);
}

static inline int eyn_sys_pty_open(int out_fds[2]) {
    return eyn_syscall1(EYN_SYSCALL_PTY_OPEN, (int)(uintptr_t)out_fds);
}

static inline int eyn_sys_chdir(const char* path) {
    return eyn_syscall1(EYN_SYSCALL_CHDIR, (int)(uintptr_t)path);
}

static inline int eyn_sys_run(const char* raw_args) {
    return eyn_syscall1(EYN_SYSCALL_RUN, (int)(uintptr_t)raw_args);
}

static inline int eyn_sys_write_editor(const char* path) {
    return eyn_syscall1(EYN_SYSCALL_WRITE_EDITOR, (int)(uintptr_t)path);
}

static inline void* eyn_sys_mmap(const char* path, size_t* out_size, int read_only) {
    int ret = eyn_syscall3_ppi(EYN_SYSCALL_MMAP, path, out_size, read_only ? 1 : 0);
    if (ret < 0) return (void*)0;
    return (void*)(uintptr_t)(uint32_t)ret;
}

static inline int eyn_sys_munmap(void* addr) {
    return eyn_syscall1(EYN_SYSCALL_MUNMAP, (int)(uintptr_t)addr);
}

static inline int eyn_sys_msync(void* addr) {
    return eyn_syscall1(EYN_SYSCALL_MSYNC, (int)(uintptr_t)addr);
}

static inline int eyn_sys_paging_guards(void) {
    return eyn_syscall0(EYN_SYSCALL_PAGING_GUARDS);
}

static inline int eyn_sys_trigger_panic(int confirmed_yes) {
    return eyn_syscall1(EYN_SYSCALL_PANIC, confirmed_yes ? 1 : 0);
}

static inline int eyn_sys_trigger_pf(uint32_t addr, int mode, int confirmed_yes) {
    return eyn_syscall3(EYN_SYSCALL_PF, (int)addr, (const void*)(uintptr_t)mode, confirmed_yes ? 1 : 0);
}

static inline int eyn_sys_ring3_test(int confirmed_yes) {
    return eyn_syscall1(EYN_SYSCALL_RING3, confirmed_yes ? 1 : 0);
}

/* ---- Audio syscall wrappers ---- */

static inline int eyn_sys_audio_probe(void) {
    return eyn_syscall0(EYN_SYSCALL_AUDIO_PROBE);
}

static inline int eyn_sys_audio_init(void) {
    return eyn_syscall0(EYN_SYSCALL_AUDIO_INIT);
}

static inline int eyn_sys_audio_write(const void* buf, int size_bytes) {
    return eyn_syscall3_pii(EYN_SYSCALL_AUDIO_WRITE, buf, size_bytes, 0);
}

static inline int eyn_sys_audio_stop(void) {
    return eyn_syscall0(EYN_SYSCALL_AUDIO_STOP);
}

static inline int eyn_sys_audio_is_available(void) {
    return eyn_syscall0(EYN_SYSCALL_AUDIO_IS_AVAILABLE);
}

static inline int eyn_sys_audio_write_bulk(const void* buf, int total_size_bytes) {
    return eyn_syscall3_pii(EYN_SYSCALL_AUDIO_WRITE_BULK, buf, total_size_bytes, 0);
}

static inline int eyn_sys_installer_prepare_drive(uint32_t logical_drive) {
    return eyn_syscall1(EYN_SYSCALL_INSTALLER_PREPARE_DRIVE, (int)logical_drive);
}

static inline int eyn_sys_installer_format_eynfs_partition(uint32_t logical_drive, uint32_t partition_num) {
    return eyn_syscall3_iii(EYN_SYSCALL_INSTALLER_FORMAT_EYNFS_PARTITION,
                            (int)logical_drive,
                            (int)partition_num,
                            0);
}

static inline int eyn_sys_installer_write_sector(uint32_t logical_drive, uint32_t lba, const void* sector512) {
    return eyn_syscall3_iip(EYN_SYSCALL_INSTALLER_WRITE_SECTOR,
                            (int)logical_drive,
                            (int)lba,
                            sector512);
}

static inline int eyn_sys_installer_get_partitions(uint32_t logical_drive, eyn_installer_partitions_t* out) {
    return eyn_syscall3_iii(EYN_SYSCALL_INSTALLER_GET_PARTITIONS,
                            (int)logical_drive,
                            (int)(uintptr_t)out,
                            (int)sizeof(*out));
}

#ifndef __chibicc__
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
#endif

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

#endif /* !__chibicc__ */

#endif /* EYNOS_SYSCALL_H */
