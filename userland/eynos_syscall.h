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
};

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
