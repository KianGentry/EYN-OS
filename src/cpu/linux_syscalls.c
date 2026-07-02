#include <linux_syscalls.h>
#include <fs/vfs.h>
#include <string.h>
#include <util.h>
#include <vga.h>
#include <kb.h>
#include <network/netstack.h>
#include <context.h>
#include <misc/sched.h>
#include <mm/vmm.h>
#include <mm/user_access.h>
#include <cpu/user_elf.h>
#include <fs_commands.h>
#include <terminals.h>

// Very small per-process FD table for now
#define LINUX_MAX_FD 32

/* Standard Linux errno values */
#define EINVAL      22
#define ENOMEM      12
#define EBADF        9
#define ECHILD      10
#define ENOENT       2
#define ENOSYS      38
#define EFAULT      14

#define EAGAIN      11

#define LINUX_WNOHANG 0x1
#define LINUX_CLONE_SIGNAL_MASK 0x000000FFu

extern uint8 g_current_drive;

#define LINUX_MAX_CHILD_TRACK 64
typedef struct {
    uint8 used;
    uint32 parent_pid;
    uint32 child_pid;
} linux_child_link_t;

static linux_child_link_t g_linux_child_links[LINUX_MAX_CHILD_TRACK];

static int linux_child_register(uint32 parent_pid, uint32 child_pid) {
    if (parent_pid == 0 || child_pid == 0) return -1;
    for (int i = 0; i < LINUX_MAX_CHILD_TRACK; ++i) {
        if (!g_linux_child_links[i].used) {
            g_linux_child_links[i].used = 1;
            g_linux_child_links[i].parent_pid = parent_pid;
            g_linux_child_links[i].child_pid = child_pid;
            return 0;
        }
    }
    return -1;
}

static int linux_child_find_slot(uint32 parent_pid, int pid_filter) {
    for (int i = 0; i < LINUX_MAX_CHILD_TRACK; ++i) {
        if (!g_linux_child_links[i].used) continue;
        if (g_linux_child_links[i].parent_pid != parent_pid) continue;
        if (pid_filter > 0 && g_linux_child_links[i].child_pid != (uint32)pid_filter) continue;
        return i;
    }
    return -1;
}

static int linux_child_find_reapable_slot(uint32 parent_pid, int pid_filter) {
    for (int i = 0; i < LINUX_MAX_CHILD_TRACK; ++i) {
        if (!g_linux_child_links[i].used) continue;
        if (g_linux_child_links[i].parent_pid != parent_pid) continue;
        if (pid_filter > 0 && g_linux_child_links[i].child_pid != (uint32)pid_filter) continue;
        if (!native_process_is_active(g_linux_child_links[i].child_pid)) {
            return i;
        }
    }
    return -1;
}

static void linux_child_drop_slot(int slot) {
    if (slot < 0 || slot >= LINUX_MAX_CHILD_TRACK) return;
    memset(&g_linux_child_links[slot], 0, sizeof(g_linux_child_links[slot]));
}

/*
 * ABI-INVARIANT: Linux-compat user pointers are 32-bit guest virtual addrs.
 *
 * Why: Native/Linux shim processes currently run with i386-style pointer ABI.
 * Invariant: Pointer arguments that feed native_process_t address fields must
 * round-trip through uint32.
 * Breakage if violated: Silent truncation can move process break bookkeeping
 * to unrelated addresses.
 */
static int linux_addr_to_u32(const void* addr, uint32* out) {
    if (!out) return -1;
    uintptr raw = (uintptr)addr;
    uint32 narrowed = (uint32)raw;
    if ((uintptr)narrowed != raw) return -1;
    *out = narrowed;
    return 0;
}

static int linux_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static int linux_copyin_cstr(char* out, uint32 out_size, const char* user_ptr) {
    if (!out || out_size == 0 || !user_ptr) return -1;
    for (uint32 i = 0; i < out_size; ++i) {
        char ch = '\0';
        if (copyin(&ch, (const void*)(user_ptr + i), sizeof(ch)) != 0) return -1;
        out[i] = ch;
        if (ch == '\0') return 0;
    }
    out[out_size - 1u] = '\0';
    return 0;
}

typedef struct {
    int in_use;
    char path[128];
    int flags;
    uint32 pos; // current offset for read/write
    uint32 size; // known size if file
} linux_fd;

// Epoll event entry: tracks a file descriptor and its registered events
typedef struct {
    int fd;           // file descriptor being monitored
    uint32 events;    // event mask (EPOLLIN, EPOLLOUT, etc.)
    uint64 data;      // opaque user data (64-bit for compat)
} epoll_event_entry_t;

// Epoll instance: created by epoll_create, tracks multiple FDs
#define MAX_EPOLL_EVENTS 32
typedef struct {
    int in_use;
    epoll_event_entry_t events[MAX_EPOLL_EVENTS];
    int event_count;
} epoll_instance_t;

#define MAX_EPOLL_INSTANCES 8

static int epoll_alloc(epoll_instance_t* tbl) {
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (!tbl[i].in_use) { tbl[i].in_use = 1; tbl[i].event_count = 0; return 100 + i; } // use 100+ for epoll FDs
    }
    return -1;
}

static epoll_instance_t* epoll_get(epoll_instance_t* tbl, int epfd) {
    int idx = epfd - 100;
    if (idx < 0 || idx >= MAX_EPOLL_INSTANCES || !tbl[idx].in_use) return NULL;
    return &tbl[idx];
}

static int linux_user_range_in_segment(uint32 start, uint32 size, uint32 seg_base, uint32 seg_size) {
    uint64 begin = (uint64)start;
    uint64 end = begin + (uint64)size;
    uint64 seg_begin = (uint64)seg_base;
    uint64 seg_end = seg_begin + (uint64)seg_size;
    return begin >= seg_begin && end <= seg_end;
}

static void* linux_translate_user_ptr(native_process_t* proc, uint32 user_addr, uint32 size) {
    if (user_addr == 0) return NULL;
    if (!proc || proc->segment_count <= 0) return (void*)(uintptr)user_addr;
    if (user_addr < proc->elf_vaddr_min || user_addr >= proc->elf_vaddr_max) {
        return (void*)(uintptr)user_addr;
    }

    for (int s = 0; s < proc->segment_count; s++) {
        uint32 sv = proc->segments[s].vaddr;
        uint32 sm = proc->segments[s].memsz;
        if (linux_user_range_in_segment(user_addr, size, sv, sm)) {
            uintptr base = (uintptr)proc->segments[s].mem;
            return (void*)(base + (uintptr)(user_addr - sv));
        }
    }

    return (void*)(uintptr)user_addr;
}

static const void* linux_translate_user_const_ptr(native_process_t* proc, uint32 user_addr, uint32 size) {
    return (const void*)linux_translate_user_ptr(proc, user_addr, size);
}

static linux_fd* get_fd_table(native_process_t* p) {
    if (!p->linux_fd_table) {
        if (!linux_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
        // Allocate space for both FD table and epoll instances together
        uint32 total_size = (sizeof(linux_fd) * LINUX_MAX_FD) + (sizeof(epoll_instance_t) * MAX_EPOLL_INSTANCES);
        p->linux_fd_table = (void*)malloc(total_size);
        if (p->linux_fd_table) memset(p->linux_fd_table, 0, total_size);
    }
    return (linux_fd*)p->linux_fd_table;
}

static epoll_instance_t* get_epoll_table(native_process_t* p) {
    if (!p) return NULL;
    // Ensure FD table is allocated (which also allocates epoll space)
    if (!p->linux_fd_table) {
        if (!get_fd_table(p)) return NULL;
    }
    // Epoll instances are stored right after the FD table
    return (epoll_instance_t*)((char*)p->linux_fd_table + (sizeof(linux_fd) * LINUX_MAX_FD));
}

static int fd_alloc(linux_fd* tbl) {
    for (int i = 3; i < LINUX_MAX_FD; i++) if (!tbl[i].in_use) { tbl[i].in_use = 1; return i; }
    return -1;
}

static int sys_write(native_process_t* proc, uint32 fd, const void* buf, uint32 count) {
    if (fd == 1 || fd == 2) {
        if (!linux_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) return -1;

        // Copy buffer from user space first (max 256 bytes)
        char kernel_buf[256];
        uint32 to_read = (count < 256) ? count : 256;
        
        // Safely read from user memory
        if (copyin(kernel_buf, buf, to_read) != 0) {
            return -14; // EFAULT - bad address
        }

        // Write to console, filtering printables only
        for (uint32 i = 0; i < to_read; i++) {
            unsigned char ch = (unsigned char)kernel_buf[i];
            // Only output newlines and printable ASCII - using a simple mechanism
            if (ch == '\n') {
                printf("\n");
            } else if (ch >= 32 && ch <= 126) {
                printf("%c", ch);
            }
        }

        return (int)count;
    }
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -1;
    // naive: append into file via VFS_write_file (no append capability per-offset yet)
    // For now, we ignore writes to regular files to keep scope small
    return (int)count;
}

static int sys_read(native_process_t* proc, uint32 fd, void* buf, uint32 count) {
    if (fd == 0) {
        // stdin via readStr(); non-blocking minimal
        string s = readStr();
        if (!s) return 0;
        uint32 n = (uint32)strlen(s);
        if (n > count) n = count;
        memcpy(buf, s, n);
        return (int)n;
    }
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -1;
    if (!linux_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    // Map to VFS read by path (inefficient but simple)
    uint32 sz = 0;
    if (vfs_get_file_size(0, tbl[fd].path, &sz) != 0) return -1;
    if (tbl[fd].pos >= sz) return 0;
    uint32 remain = sz - tbl[fd].pos;
    if (count > remain) count = remain;
    // Read slice: VFS currently reads full file; use tmp then copy
    if (!linux_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    void* tmp = malloc(sz);
    if (!tmp) return -1;
    int got = vfs_read_file(0, tbl[fd].path, tmp, sz);
    if (got < 0) { free(tmp); return -1; }
    memcpy(buf, (uint8_t*)tmp + tbl[fd].pos, count);
    tbl[fd].pos += count;
    free(tmp);
    return (int)count;
}

static int sys_open(native_process_t* proc, const char* path, int flags, int mode) {
    (void)mode;
    if (!linux_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl) return -1;
    int fd = fd_alloc(tbl);
    if (fd < 0) return -1;
    safe_strcpy(tbl[fd].path, path, sizeof(tbl[fd].path));
    tbl[fd].flags = flags;
    tbl[fd].pos = 0;
    vfs_stat_t st;
    if (vfs_stat(0, path, &st) == 0) tbl[fd].size = st.size; else tbl[fd].size = 0;
    return fd;
}

static int sys_close(native_process_t* proc, uint32 fd) {
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -1;
    memset(&tbl[fd], 0, sizeof(tbl[fd]));
    return 0;
}

static int sys_fstat(native_process_t* proc, uint32 fd, void* statbuf) {
    (void)proc;
    if (!statbuf) return -1;
    if (!linux_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    // Try to populate a Linux-ish i386 struct stat with minimal fields
    // struct stat (i386) layout varies; we fill st_mode, st_size, st_blksize, st_blocks, and set a few zeros.
    // We assume callers only check S_IFCHR for fds 1/2 or size>0 for files.
    memset(statbuf, 0, 128);
    unsigned short* st_mode = (unsigned short*)((char*)statbuf + 16);
    unsigned long* st_size = (unsigned long*)((char*)statbuf + 44);
    unsigned long* st_blksize = (unsigned long*)((char*)statbuf + 60);
    unsigned long* st_blocks = (unsigned long*)((char*)statbuf + 64);

    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || (!tbl[fd].in_use && fd > 2)) {
        *st_mode = 0; return -1;
    }
    if (fd == 1 || fd == 2) {
        // character device (console)
        *st_mode = 0x2000 | 0666; // S_IFCHR
        *st_blksize = 4096;
        *st_blocks = 0;
        *st_size = 0;
        return 0;
    }
    // File: query size via vfs
    vfs_stat_t st;
    if (vfs_stat(0, tbl[fd].path, &st) == 0) {
        *st_mode = 0x8000 | 0644; // S_IFREG
        *st_size = st.size;
        *st_blksize = 4096;
        *st_blocks = (st.size + 511) / 512;
        return 0;
    }
    return -1;
}

static int sys_lseek(native_process_t* proc, uint32 fd, int32 offset, int whence) {
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -1;
    if (!linux_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    uint32 base = 0;
    uint32 sz = 0;
    if (vfs_get_file_size(0, tbl[fd].path, &sz) != 0) sz = tbl[fd].size;
    switch (whence) {
        case 0: // SEEK_SET
            base = 0; break;
        case 1: // SEEK_CUR
            base = tbl[fd].pos; break;
        case 2: // SEEK_END
            base = sz; break;
        default:
            return -1;
    }
    int64_t np = (int64_t)base + (int64_t)offset;
    if (np < 0) np = 0;
    if ((uint64_t)np > (uint64_t)0x7fffffff) np = 0x7fffffff;
    tbl[fd].pos = (uint32)np;
    return (int)tbl[fd].pos;
}

static int sys_wait4(native_process_t* proc, int pid, int* kstatus, int options, void* krusage) {
    (void)krusage;

    if (!proc || proc->pid == 0) return -ECHILD;
    if (pid == 0 || pid < -1) return -EINVAL;

    int flags = 0;
    if (options & LINUX_WNOHANG) flags |= USER_TASK_WAIT_NOHANG;

    uint32 parent_pid = proc->pid;

    int slot = linux_child_find_reapable_slot(parent_pid, pid);
    if (slot >= 0) {
        int waited_pid = (int)g_linux_child_links[slot].child_pid;
        linux_child_drop_slot(slot);
        if (kstatus) *kstatus = 0;
        return waited_pid;
    }

    int active_slot = linux_child_find_slot(parent_pid, pid);
    if (active_slot < 0) return -ECHILD;

    if (flags & USER_TASK_WAIT_NOHANG) {
        return 0;
    }

    uint32 wait_pid = g_linux_child_links[active_slot].child_pid;
    while (native_process_is_active(wait_pid)) {
        sched_on_timeslice_end();
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");
    }

    slot = linux_child_find_slot(parent_pid, (int)wait_pid);
    if (slot >= 0) linux_child_drop_slot(slot);
    if (kstatus) *kstatus = 0;
    return (int)wait_pid;
}

static int linux_spawn_native_child(native_process_t* proc) {
    if (!proc || !proc->name[0]) return -EINVAL;
    uint32 child_pid = 0;
    exec_result_t rc = native_spawn(proc->name, &child_pid);
    if (rc != EXEC_SUCCESS || child_pid == 0) return -EAGAIN;
    if (linux_child_register(proc->pid, child_pid) != 0) return -EAGAIN;
    return (int)child_pid;
}

static void linux_release_process_image(native_process_t* process) {
    if (!process) return;

    if (process->data_start) {
        int data_is_offset_in_code = 0;
        if (process->code_start && process->data_start >= process->code_start &&
            process->data_start < process->code_start + process->code_size) {
            data_is_offset_in_code = 1;
        }
        if (!data_is_offset_in_code) {
            free((void*)process->data_start);
        }
        process->data_start = 0;
    }

    if (process->stack_start) {
        free((void*)process->stack_start);
        process->stack_start = 0;
    }

    if (process->segment_count) {
        int code_is_segment = 0;
        for (int s = 0; s < process->segment_count; s++) {
            if (process->segments[s].mem) {
                if ((uint32)process->segments[s].mem == process->code_start) code_is_segment = 1;
                free(process->segments[s].mem);
                process->segments[s].mem = NULL;
            }
        }
        process->segment_count = 0;
        if (process->code_start && !code_is_segment) {
            free((void*)process->code_start);
        }
        process->code_start = 0;
    } else {
        if (process->code_start) {
            free((void*)process->code_start);
            process->code_start = 0;
        }
    }

    if (process->address_space) {
        destroy_address_space(process->address_space);
        process->address_space = NULL;
    }
}

static int linux_execve_replace_from_user(native_process_t* proc, uint32 path_ptr, uint32 argv_ptr) {
    (void)argv_ptr;

    if (!proc) return -EINVAL;

    if (!path_ptr) return -EFAULT;

    char path_buf[128];
    if (linux_copyin_cstr(path_buf, sizeof(path_buf), (const char*)(uintptr)path_ptr) != 0) {
        return -EFAULT;
    }

    const char* cwd = "/";
    if (g_user_task_active) {
        const char* t = vterm_get_cwd(g_user_task_term);
        if (t && t[0] == '/') cwd = t;
    }
    char abspath[128];
    resolve_path(path_buf, cwd, abspath, sizeof(abspath));

    native_process_t image;
    exec_result_t load_rc = native_load_program(abspath, &image);
    if (load_rc != EXEC_SUCCESS) return -ENOENT;

    uint32 keep_pid = proc->pid;
    uint8 keep_owned = proc->owned;
    void* keep_fd_table = proc->linux_fd_table;

    linux_release_process_image(proc);

    if (image.linux_fd_table && image.linux_fd_table != keep_fd_table) {
        free(image.linux_fd_table);
        image.linux_fd_table = NULL;
    }

    *proc = image;
    proc->pid = keep_pid;
    proc->owned = keep_owned;
    proc->linux_fd_table = keep_fd_table;
    proc->linux_compat_mode = 1;
    proc->linux_execve_pending = 1;
    proc->active = 1;
    return 0;
}

// Lightweight stubs for common libc expectations
static int sys_set_thread_area(native_process_t* proc, void* user_desc) {
    if (!proc || !user_desc) return -EINVAL;

    /* i386 Linux user_desc structure (12 bytes):
     * struct user_desc {
     *     unsigned int entry_number;      // +0: GDT entry slot (typically 6-7)
     *     unsigned int base_addr;         // +4: TLS base address
     *     unsigned int limit;             // +8: size/limit (optional)
     *     ... bitfields ...
     * };
     */
    uint32* desc = (uint32*)user_desc;
    uint32 entry_number = desc[0];
    uint32 base_addr = desc[1];
    
    /* Only support entry numbers 6-7 (thread-local storage slots) */
    if (entry_number < 6 || entry_number > 7) return -EINVAL;

    /* Store TLS base for later use by dynamic linker */
    proc->tls_base = base_addr;

    /* On a real system, we would allocate/configure a GDT descriptor.
     * For now, store the base and libc will read it via syscall or direct access.
     * musl libc accesses %gs:0 for the TCB (thread control block), which is
     * the base_addr we just stored.
     */

    return 0;
}

static int sys_uname(native_process_t* proc, void* utsname_buf) {
    (void)proc;
    if (!utsname_buf) return -1;
    // struct utsname has 5 strings; we fill them with constants.
    // We assume Linux i386 layout with 65 bytes per field typically.
    char (*fields)[65] = (char (*)[65])utsname_buf;
    safe_strcpy(fields[0], "EYN-OS", 65);    // sysname
    safe_strcpy(fields[1], "eyn-host", 65);  // nodename
    safe_strcpy(fields[2], "0.1", 65);       // release
    safe_strcpy(fields[3], "0.1", 65);       // version
    safe_strcpy(fields[4], "i686", 65);      // machine
    return 0;
}

static int sys_getpid(native_process_t* proc) {
    if (!proc || proc->pid == 0) return 1;
    return (int)proc->pid;
}

static int sys_getppid(native_process_t* proc) {
    (void)proc;
    return 1;
}

static int sys_getuid(native_process_t* proc) {
    (void)proc;
    return 0;
}

static int sys_getgid(native_process_t* proc) {
    (void)proc;
    return 0;
}

static int sys_gettid(native_process_t* proc) {
    return sys_getpid(proc);
}

static int sys_set_tid_address(native_process_t* proc, uint32* tidptr) {
    int tid = sys_gettid(proc);
    if (tidptr) *tidptr = (uint32)tid;
    return tid;
}

static int sys_time(native_process_t* proc, uint32* tloc) {
    (void)proc;
    uint32 now = 1730500000u; // fixed epoch-like value (~2024-11), OK for hello
    if (tloc) *tloc = now;
    return (int)now;
}

static int sys_gettimeofday(native_process_t* proc, void* tv, void* tz) {
    (void)proc; (void)tz;
    if (!tv) return -1;
    // struct timeval { long tv_sec; long tv_usec; }
    uint32* p = (uint32*)tv;
    p[0] = 1730500000u; // seconds
    p[1] = 0;           // usec
    return 0;
}

static int sys_clock_gettime(native_process_t* proc, int clk_id, void* ts) {
    (void)proc; (void)clk_id;
    if (!ts) return -1;
    // struct timespec { long tv_sec; long tv_nsec; }
    uint32* p = (uint32*)ts;
    p[0] = 1730500000u; // seconds
    p[1] = 0;           // nsec
    return 0;
}

static int sys_clock_getres(native_process_t* proc, int clk_id, void* ts) {
    (void)proc; (void)clk_id;
    if (!ts) return -1;
    // struct timespec { long tv_sec; long tv_nsec; }
    uint32* p = (uint32*)ts;
    p[0] = 0;        // sec
    p[1] = 1000000u; // 1ms nominal resolution
    return 0;
}

static int sys_nanosleep(native_process_t* proc, const void* req, void* rem) {
    (void)proc;
    if (!req) return -1;
    if (rem) {
        uint32* r = (uint32*)rem;
        r[0] = 0;
        r[1] = 0;
    }
    return 0;
}

static int sys_clock_nanosleep(native_process_t* proc, int clk_id, int flags, const void* req, void* rem) {
    (void)clk_id;
    (void)flags;
    return sys_nanosleep(proc, req, rem);
}

static int sys_rt_sigaction(native_process_t* proc, int signum, const void* act, void* oldact, uint32 sigsetsize) {
    (void)proc;
    (void)signum;
    (void)act;
    (void)sigsetsize;
    if (oldact) memset(oldact, 0, 16);
    return 0;
}

static int sys_rt_sigprocmask(native_process_t* proc, int how, const void* set, void* oldset, uint32 sigsetsize) {
    (void)proc;
    (void)how;
    (void)set;
    (void)sigsetsize;
    if (oldset) memset(oldset, 0, sizeof(uint32));
    return 0;
}

static int sys_sigaltstack(native_process_t* proc, const void* ss, void* old_ss) {
    (void)proc;
    (void)ss;
    if (old_ss) memset(old_ss, 0, 12);
    return 0;
}

static int sys_futex(native_process_t* proc,
                     int32* uaddr,
                     int op,
                     int val,
                     const void* timeout,
                     int32* uaddr2,
                     int val3) {
    (void)proc;
    (void)uaddr;
    (void)timeout;
    (void)uaddr2;
    (void)val3;
    // Single-thread-friendly futex shim: report success without blocking.
    switch (op & 0x7f) {
        case 0: // FUTEX_WAIT
            return 0;
        case 1: // FUTEX_WAKE
            return (val > 0) ? 1 : 0;
        default:
            return 0;
    }
}

static int sys_tgkill(native_process_t* proc, int tgid, int tid, int sig) {
    (void)proc;
    (void)tgid;
    (void)tid;
    (void)sig;
    return 0;
}

static int sys_brk(native_process_t* proc, void* addr) {
    // Minimal brk: track a single top pointer. If addr==0, return current.
    if (proc->brk_end == 0) proc->brk_end = proc->stack_start + 0x100000; // put heap below stack as placeholder
    if (addr == 0) return (int)proc->brk_end;
    uint32 new_end = 0;
    if (linux_addr_to_u32(addr, &new_end) != 0) {
        return (int)proc->brk_end;
    }
    // No real allocation; just accept within a conservative range
    if (new_end > proc->brk_end && new_end < proc->brk_end + 0x400000) {
        proc->brk_end = new_end;
    }
    return (int)proc->brk_end;
}

// --- VMM-backed mmap/munmap/mprotect support ---

/*
 * Create or return the address_space_t for a native process.
 * This is lazily allocated on first mmap and is separate from the
 * segment-based model, allowing coexistence.
 */
static address_space_t* linux_get_or_create_address_space(native_process_t* proc) {
    if (!proc) return NULL;
    if (proc->address_space) return proc->address_space;

    extern address_space_t* create_address_space(void);
    address_space_t* as = create_address_space();
    if (!as) return NULL;

    proc->address_space = as;
    return as;
}

static int sys_mmap_vmm(native_process_t* proc, uint32 addr, uint32 length, int prot, int flags, int fd, uint32 offset) {
    if (!proc || length == 0) return -ENOMEM;

    uint32 len_aligned = (length + PAGE_SIZE - 1) & PAGE_MASK;
    if (len_aligned == 0) return -ENOMEM;

    address_space_t* as = linux_get_or_create_address_space(proc);
    if (!as) return -ENOMEM;

    linux_fd* tbl = NULL;
    const char* file_path = NULL;
    uint32 file_size = 0;
    int file_backed = 0;

    if (!(flags & MAP_ANONYMOUS)) {
        tbl = get_fd_table(proc);
        if (!tbl || fd < 0 || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -EBADF;
        file_path = tbl[fd].path;
        if (!file_path || !file_path[0]) return -EBADF;
        file_backed = 1;
        if (tbl[fd].size > 0) {
            file_size = tbl[fd].size;
        } else {
            if (vfs_get_file_size(0, file_path, &file_size) != 0) {
                return -EBADF;
            }
        }
    }

    /* Determine target address: if addr is 0, pick one in shared region */
    uint32 target_addr = addr;
    if (target_addr == 0) {
        /* Use a simple allocator: USER_SHARED_BASE + offset; could be smarter */
        static uint32 next_mmap_va = USER_SHARED_BASE;
        target_addr = next_mmap_va;
        next_mmap_va += len_aligned;
        if (next_mmap_va >= USER_SHARED_END) return -ENOMEM;  /* Out of shared space */
    }

    /* Convert prot to PTE flags */
    uint32 pte_flags = PTE_USER | PTE_PRESENT;
    if (prot & PROT_WRITE) pte_flags |= PTE_RW;
    /* PROT_EXEC is noted but doesn't affect i386 paging (NX not available) */

    /* Map each page */
    uint32 mapped = 0;
    for (uint32 va = target_addr; va < target_addr + len_aligned; va += PAGE_SIZE) {
        uint32 frame = frame_alloc();
        if (frame == 0) {
            /* Out of memory: unwind partial mapping */
            for (uint32 undo = target_addr; undo < va; undo += PAGE_SIZE) {
                pte_t* pte = vmm_walk_page_tables(as, undo, 0);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint32 prev_frame = *pte & PTE_FRAME_MASK;
                    vmm_unmap_page(as, undo);
                    frame_free(prev_frame);
                }
            }
            return -ENOMEM;
        }

        /* Zero the frame */
        memset((void*)(KERNEL_BASE + frame), 0, PAGE_SIZE);

        if (file_backed) {
            uint32 file_off = offset + (va - target_addr);
            if (file_off < file_size) {
                uint32 to_copy = file_size - file_off;
                if (to_copy > PAGE_SIZE) to_copy = PAGE_SIZE;
                int got = vfs_read_file_at(0, file_path, (void*)(KERNEL_BASE + frame), (int)to_copy, file_off);
                if (got < 0) {
                    frame_free(frame);
                    for (uint32 undo = target_addr; undo < va; undo += PAGE_SIZE) {
                        pte_t* pte = vmm_walk_page_tables(as, undo, 0);
                        if (pte && (*pte & PTE_PRESENT)) {
                            uint32 prev_frame = *pte & PTE_FRAME_MASK;
                            vmm_unmap_page(as, undo);
                            frame_free(prev_frame);
                        }
                    }
                    return -EBADF;
                }
            }
        }

        /* Map into address space */
        if (vmm_map_page(as, va, frame, pte_flags) != 0) {
            frame_free(frame);
            for (uint32 undo = target_addr; undo < va; undo += PAGE_SIZE) {
                pte_t* pte = vmm_walk_page_tables(as, undo, 0);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint32 prev_frame = *pte & PTE_FRAME_MASK;
                    vmm_unmap_page(as, undo);
                    frame_free(prev_frame);
                }
            }
            return -ENOMEM;
        }
        mapped += PAGE_SIZE;
    }

    return (int)target_addr;
}

static int sys_munmap_vmm(native_process_t* proc, uint32 addr, uint32 length) {
    if (!proc || length == 0 || !proc->address_space) return -EINVAL;

    address_space_t* as = proc->address_space;
    uint32 len_aligned = (length + PAGE_SIZE - 1) & PAGE_MASK;

    /* Unmap each page */
    for (uint32 va = addr; va < addr + len_aligned; va += PAGE_SIZE) {
        vmm_unmap_page(as, va);
    }

    return 0;
}

static int sys_mprotect_vmm(native_process_t* proc, uint32 addr, uint32 length, int prot) {
    if (!proc || length == 0 || !proc->address_space) return -EINVAL;

    address_space_t* as = proc->address_space;
    uint32 len_aligned = (length + PAGE_SIZE - 1) & PAGE_MASK;

    /* Convert prot to PTE flags */
    uint32 pte_flags = PTE_USER | PTE_PRESENT;
    if (prot & PROT_WRITE) pte_flags |= PTE_RW;

    /* Update each page's protection */
    for (uint32 va = addr; va < addr + len_aligned; va += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            uint32 frame = *pte & PTE_FRAME_MASK;
            *pte = (frame & PTE_FRAME_MASK) | (pte_flags & 0xFFF);
            vm_invalidate_page((void*)va);
        }
    }

    return 0;
}

// --- Networking Syscalls ---


/*
 * Lightweight native-process mmap support (anonymous-only)
 * Notes:
 * - This implements a minimal mmap/munmap/mprotect for the native_process_t
 *   loader model used by the current user-mode emulator. It does not touch
 *   the full VMM address_space_t mapping APIs; that will be added when the
 *   kernel switches to a full user process model.
 */

static inline uint32 align_up_u32(uint32 x, uint32 a) { return (x + a - 1) & ~(a - 1); }

static int sys_mmap(native_process_t* proc, uint32 addr, uint32 length, int prot, int flags, int fd, uint32 offset) {
    (void)offset; (void)fd;
    if (!proc || length == 0) return -1;
    uint32 len = align_up_u32(length, PAGE_SIZE);

    /* Only support anonymous private mappings for now */
    if (!(flags & MAP_ANONYMOUS)) return -1;

    if (!linux_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    void* mem = malloc(len);
    if (!mem) return -1;
    memset(mem, 0, len);

    /* Find free segment slot */
    int slot = -1;
    for (int s = 0; s < 8; s++) {
        if (proc->segments[s].mem == NULL || proc->segments[s].memsz == 0) { slot = s; break; }
    }
    if (slot < 0) { free(mem); return -1; }

    uint32 vaddr = addr;
    if (vaddr == 0) {
        if (proc->elf_vaddr_max > proc->elf_vaddr_min) vaddr = align_up_u32(proc->elf_vaddr_max, PAGE_SIZE);
        else vaddr = USER_SHARED_BASE; /* fallback into shared region */
    }

    proc->segments[slot].vaddr = vaddr;
    proc->segments[slot].memsz = len;
    proc->segments[slot].filesz = 0;
    proc->segments[slot].mem = mem;
    proc->segments[slot].flags = (uint32)prot;
    if (slot >= proc->segment_count) proc->segment_count = slot + 1;

    if (proc->elf_vaddr_min == 0 || vaddr < proc->elf_vaddr_min) proc->elf_vaddr_min = vaddr;
    if (proc->elf_vaddr_max < vaddr + len) proc->elf_vaddr_max = vaddr + len;

    return (int)vaddr;
}

static int sys_munmap(native_process_t* proc, uint32 addr, uint32 length) {
    if (!proc || length == 0) return -1;
    uint32 end = addr + length;
    for (int s = 0; s < proc->segment_count; s++) {
        uint32 sv = proc->segments[s].vaddr;
        uint32 sm = proc->segments[s].memsz;
        if (sv == addr && sm == length) {
            if (proc->segments[s].mem) free(proc->segments[s].mem);
            proc->segments[s].mem = NULL;
            proc->segments[s].memsz = 0;
            proc->segments[s].filesz = 0;
            proc->segments[s].vaddr = 0;
            proc->segments[s].flags = 0;
            /* Shrink segment_count if trailing slots are empty */
            int new_count = proc->segment_count;
            while (new_count > 0 && (proc->segments[new_count-1].mem == NULL || proc->segments[new_count-1].memsz == 0)) new_count--;
            proc->segment_count = new_count;
            /* Recompute elf_vaddr_min/max */
            uint32 min = 0, max = 0;
            for (int i = 0; i < proc->segment_count; i++) {
                if (proc->segments[i].memsz == 0) continue;
                if (min == 0 || proc->segments[i].vaddr < min) min = proc->segments[i].vaddr;
                if (proc->segments[i].vaddr + proc->segments[i].memsz > max) max = proc->segments[i].vaddr + proc->segments[i].memsz;
            }
            proc->elf_vaddr_min = min;
            proc->elf_vaddr_max = max;
            return 0;
        }
    }
    return -1;
}

static int sys_mprotect(native_process_t* proc, uint32 addr, uint32 length, int prot) {
    if (!proc || length == 0) return -1;
    for (int s = 0; s < proc->segment_count; s++) {
        uint32 sv = proc->segments[s].vaddr;
        uint32 sm = proc->segments[s].memsz;
        if (addr >= sv && (addr + length) <= (sv + sm)) {
            proc->segments[s].flags = (uint32)prot;
            return 0;
        }
    }
    return -1;
}

static int sys_net_socket(native_process_t* proc) {
    (void)proc;
    // For now, we only support UDP sockets; no socket type argument needed
    // Just return a success indicator; actual socket creation happens in bind
    return 0; // Placeholder; real socket created on bind
}

static int sys_net_bind(native_process_t* proc, uint16 port) {
    (void)proc;
    // Bind a UDP socket to a port; returns socket_id >= 0 on success
    return net_udp_bind(port);
}

static int sys_net_sendto(native_process_t* proc, int socket_id, const char* dst_ip_str, uint16 dst_port, const void* buf, uint32 len) {
    (void)proc;
    if (!buf || !dst_ip_str) return -1;
    
    // Parse dst_ip_str (format: "a.b.c.d") - manual parsing since we can't use libc sscanf
    uint8 dst_ip[4];
    int parts[4] = {0,0,0,0};
    int part_idx = 0;
    for (const char* p = dst_ip_str; *p && part_idx < 4; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[part_idx] = parts[part_idx] * 10 + (*p - '0');
            if (parts[part_idx] > 255) return -2;
        } else if (*p == '.') {
            part_idx++;
        } else {
            return -2; // Invalid character
        }
    }
    if (part_idx != 3) return -2; // Need exactly 4 parts
    for (int i = 0; i < 4; i++) dst_ip[i] = (uint8)parts[i];

    uint8 src_ip[4];
    net_get_local_ip(src_ip);

    return net_udp_send_socket(socket_id, src_ip, dst_ip, dst_port, (const uint8*)buf, len, 800000);
}

static int sys_net_recvfrom(native_process_t* proc, int socket_id, void* buf, uint32 buflen, void* src_ip_out, void* src_port_out) {
    (void)proc;
    if (!buf) return -1;

    net_udp_rx_packet pkt;
    int rc = net_udp_recv_socket(socket_id, &pkt);
    if (rc <= 0) return rc; // 0 = no packet, <0 = error

    // Copy payload to user buffer
    uint32 copy_len = pkt.payload_len;
    if (copy_len > buflen) copy_len = buflen;
    memcpy(buf, pkt.payload, copy_len);

    // Optionally return source IP and port
    if (src_ip_out) {
        uint8* ip_out = (uint8*)src_ip_out;
        for (int i = 0; i < 4; i++) ip_out[i] = pkt.src_ip[i];
    }
    if (src_port_out) {
        *(uint16*)src_port_out = pkt.src_port;
    }

    return (int)copy_len; // Return number of bytes received
}

static int sys_net_close(native_process_t* proc, int socket_id) {
    (void)proc;
    return net_udp_close(socket_id);
}

// ============ Epoll syscalls (211-213) ============

static int sys_epoll_create(native_process_t* proc, int size) {
    (void)size; // size is ignored in modern Linux
    if (!proc) return -1;
    if (!linux_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    
    epoll_instance_t* tbl = get_epoll_table(proc);
    if (!tbl) return -1;
    
    int epfd = epoll_alloc(tbl);
    return epfd;
}

static int sys_epoll_ctl(native_process_t* proc, int epfd, int op, int fd, const void* kevent) {
    if (!proc) return -1;
    
    epoll_instance_t* tbl = get_epoll_table(proc);
    if (!tbl) return -1;
    
    epoll_instance_t* ep = epoll_get(tbl, epfd);
    if (!ep) return -1; // -EBADF
    
    // Parse the epoll_event structure from user memory
    // struct epoll_event { uint32 events; uint64 data; }
    if (!kevent) return -1;
    
    const uint32* events_ptr = (const uint32*)kevent;
    const uint64* data_ptr = (const uint64*)((const uint32*)kevent + 1);
    uint32 events = *events_ptr;
    uint64 data = *data_ptr;
    
    switch (op) {
        case EPOLL_CTL_ADD: {
            // Add a new FD to watch
            if (ep->event_count >= MAX_EPOLL_EVENTS) return -1; // -ENOMEM
            ep->events[ep->event_count].fd = fd;
            ep->events[ep->event_count].events = events;
            ep->events[ep->event_count].data = data;
            ep->event_count++;
            return 0;
        }
        case EPOLL_CTL_DEL: {
            // Remove FD from watch
            for (int i = 0; i < ep->event_count; i++) {
                if (ep->events[i].fd == fd) {
                    // Swap with last and decrement
                    ep->events[i] = ep->events[ep->event_count - 1];
                    ep->event_count--;
                    return 0;
                }
            }
            return -1; // -ENOENT
        }
        case EPOLL_CTL_MOD: {
            // Modify events for existing FD
            for (int i = 0; i < ep->event_count; i++) {
                if (ep->events[i].fd == fd) {
                    ep->events[i].events = events;
                    ep->events[i].data = data;
                    return 0;
                }
            }
            return -1; // -ENOENT
        }
        default:
            return -1; // -EINVAL
    }
}

static int sys_epoll_wait(native_process_t* proc, int epfd, void* kevents, int maxevents, int timeout) {
    if (!proc || !kevents || maxevents <= 0) return -1;
    
    epoll_instance_t* tbl = get_epoll_table(proc);
    if (!tbl) return -1;
    
    epoll_instance_t* ep = epoll_get(tbl, epfd);
    if (!ep) return -1; // -EBADF
    
    // Minimal implementation: return all registered events ready immediately
    // For a full implementation, would need actual I/O readiness checks
    // This works for programs that just want to wait with a timeout
    
    int ret_count = 0;
    int out_count = (ep->event_count < maxevents) ? ep->event_count : maxevents;
    
    // Copy events to user buffer: struct epoll_event { uint32 events; uint64 data; }
    // Each entry is 12 bytes (4 + 8)
    uint8_t* out_buf = (uint8_t*)kevents;
    for (int i = 0; i < out_count; i++) {
        uint32* events_ptr = (uint32*)(out_buf + i * 12);
        uint64* data_ptr = (uint64*)(out_buf + i * 12 + 4);
        *events_ptr = ep->events[i].events;
        *data_ptr = ep->events[i].data;
        ret_count++;
    }
    
    // Handle timeout: if positive, we could sleep; if 0, return immediately; if -1, block forever
    // For now, always return immediately with available events
    (void)timeout;
    
    return ret_count;
}

// Initialize standard file descriptors (stdin, stdout, stderr) for a Linux process
void linux_init_stdio(native_process_t* proc) {
    if (!proc) return;

    // Get the FD table (this will allocate if needed)
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl) {
        return;
    }

    // Mark stdin (0), stdout (1), stderr (2) as in use
    tbl[0].in_use = 1;
    tbl[0].flags = 0; // Read-only
    tbl[0].pos = 0;
    tbl[0].size = 0;
    safe_strcpy(tbl[0].path, "stdin", sizeof(tbl[0].path));
    
    tbl[1].in_use = 1;
    tbl[1].flags = 1; // Write
    tbl[1].pos = 0;
    tbl[1].size = 0;
    safe_strcpy(tbl[1].path, "stdout", sizeof(tbl[1].path));
    
    tbl[2].in_use = 1;
    tbl[2].flags = 1; // Write
    tbl[2].pos = 0;
    tbl[2].size = 0;
    safe_strcpy(tbl[2].path, "stderr", sizeof(tbl[2].path));
}

int linux_syscall_dispatch(native_process_t* proc, uint32 regs[8]) {
    uint32 eax = regs[0];
    uint32 ebx = regs[3];
    uint32 ecx = regs[1];
    uint32 edx = regs[2];

    switch (eax) {
        case __NR_write: {
            const void* kbuf = linux_translate_user_const_ptr(proc, ecx, edx);
            int ret = sys_write(proc, ebx, kbuf, edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_read: {
            void* kbuf = linux_translate_user_ptr(proc, ecx, edx);
            int ret = sys_read(proc, ebx, kbuf, edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_open: {
            const char* path = (const char*)linux_translate_user_const_ptr(proc, ebx, 1u);
            int ret = sys_open(proc, path, (int)ecx, (int)edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_close: {
            int ret = sys_close(proc, ebx);
            regs[0] = ret;
            return ret;
        }
        case __NR_fork:
        case __NR_vfork: {
            int ret = linux_spawn_native_child(proc);
            regs[0] = (uint32)ret;
            return ret;
        }
        case __NR_clone: {
            uint32 flags = ebx;
            if ((flags & ~LINUX_CLONE_SIGNAL_MASK) != 0u) {
                regs[0] = (uint32)-ENOSYS;
                return -ENOSYS;
            }
            int ret = linux_spawn_native_child(proc);
            regs[0] = (uint32)ret;
            return ret;
        }
        case __NR_execve: {
            int ret = linux_execve_replace_from_user(proc, ebx, ecx);
            regs[0] = (uint32)ret;
            return ret;
        }
        case __NR_fstat: {
            void* kbuf = linux_translate_user_ptr(proc, ecx, (uint32)sizeof(uint64));
            int ret = sys_fstat(proc, ebx, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_waitpid: {
            int* kstatus = (int*)linux_translate_user_ptr(proc, ecx, (uint32)sizeof(int));
            int ret = sys_wait4(proc, (int)ebx, kstatus, (int)edx, NULL);
            regs[0] = ret;
            return ret;
        }
        case __NR_wait4: {
            int* kstatus = (int*)linux_translate_user_ptr(proc, ecx, (uint32)sizeof(int));
            void* krusage = linux_translate_user_ptr(proc, regs[6], 1u);
            int ret = sys_wait4(proc, (int)ebx, kstatus, (int)edx, krusage);
            regs[0] = ret;
            return ret;
        }
        case __NR_set_thread_area: {
            void* kbuf = linux_translate_user_ptr(proc, ebx, 1u);
            int ret = sys_set_thread_area(proc, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_uname: {
            void* kbuf = linux_translate_user_ptr(proc, ebx, 65u * 5u);
            int ret = sys_uname(proc, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_getpid: {
            int ret = sys_getpid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_getppid: {
            int ret = sys_getppid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_getuid:
        case __NR_geteuid: {
            int ret = sys_getuid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_getgid:
        case __NR_getegid: {
            int ret = sys_getgid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_gettid: {
            int ret = sys_gettid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_time: {
            uint32* kptr = (uint32*)linux_translate_user_ptr(proc, ebx, (uint32)sizeof(uint32));
            int ret = sys_time(proc, kptr);
            regs[0] = ret;
            return ret;
        }
        case __NR_gettimeofday: {
            void* ktv = linux_translate_user_ptr(proc, ebx, (uint32)(2u * sizeof(uint32)));
            void* ktz = linux_translate_user_ptr(proc, ecx, 1u);
            int ret = sys_gettimeofday(proc, ktv, ktz);
            regs[0] = ret;
            return ret;
        }
        case __NR_clock_gettime: {
            void* kts = linux_translate_user_ptr(proc, ecx, (uint32)(2u * sizeof(uint32)));
            int ret = sys_clock_gettime(proc, (int)ebx, kts);
            regs[0] = ret;
            return ret;
        }
        case __NR_clock_getres: {
            void* kts = linux_translate_user_ptr(proc, ecx, (uint32)(2u * sizeof(uint32)));
            int ret = sys_clock_getres(proc, (int)ebx, kts);
            regs[0] = ret;
            return ret;
        }
        case __NR_nanosleep: {
            const void* kreq = linux_translate_user_const_ptr(proc, ebx, (uint32)(2u * sizeof(uint32)));
            void* krem = linux_translate_user_ptr(proc, ecx, (uint32)(2u * sizeof(uint32)));
            int ret = sys_nanosleep(proc, kreq, krem);
            regs[0] = ret;
            return ret;
        }
        case __NR_clock_nanosleep: {
            const void* kreq = linux_translate_user_const_ptr(proc, regs[6], (uint32)(2u * sizeof(uint32)));
            void* krem = linux_translate_user_ptr(proc, regs[7], (uint32)(2u * sizeof(uint32)));
            int ret = sys_clock_nanosleep(proc, (int)ebx, (int)ecx, kreq, krem);
            regs[0] = ret;
            return ret;
        }
        case __NR_rt_sigaction: {
            const void* kact = linux_translate_user_const_ptr(proc, ecx, 16u);
            void* kold = linux_translate_user_ptr(proc, edx, 16u);
            int ret = sys_rt_sigaction(proc, (int)ebx, kact, kold, regs[6]);
            regs[0] = ret;
            return ret;
        }
        case __NR_rt_sigprocmask: {
            const void* kset = linux_translate_user_const_ptr(proc, ecx, sizeof(uint32));
            void* kold = linux_translate_user_ptr(proc, edx, sizeof(uint32));
            int ret = sys_rt_sigprocmask(proc, (int)ebx, kset, kold, regs[6]);
            regs[0] = ret;
            return ret;
        }
        case __NR_sigaltstack: {
            const void* kss = linux_translate_user_const_ptr(proc, ebx, 12u);
            void* kold = linux_translate_user_ptr(proc, ecx, 12u);
            int ret = sys_sigaltstack(proc, kss, kold);
            regs[0] = ret;
            return ret;
        }
        case __NR_futex: {
            int32* kuaddr = (int32*)linux_translate_user_ptr(proc, ebx, sizeof(int32));
            const void* ktimeout = linux_translate_user_const_ptr(proc, regs[6], (uint32)(2u * sizeof(uint32)));
            int32* kuaddr2 = (int32*)linux_translate_user_ptr(proc, regs[7], sizeof(int32));
            int ret = sys_futex(proc, kuaddr, (int)ecx, (int)edx, ktimeout, kuaddr2, (int)regs[5]);
            regs[0] = ret;
            return ret;
        }
        case __NR_lseek: {
            int ret = sys_lseek(proc, ebx, (int32)ecx, (int)edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_set_tid_address: {
            uint32* ktid = (uint32*)linux_translate_user_ptr(proc, ebx, sizeof(uint32));
            int ret = sys_set_tid_address(proc, ktid);
            regs[0] = ret;
            return ret;
        }
        case __NR_tgkill: {
            int ret = sys_tgkill(proc, (int)ebx, (int)ecx, (int)edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_exit:
        case __NR_exit_group: {
            proc->active = 0;
            regs[0] = 0;
            return 0;
        }
        case __NR_brk: {
            int ret = sys_brk(proc, (void*)(uintptr)ebx);
            regs[0] = ret;
            return ret;
        }
        case __NR_mmap:
        case __NR_mmap2: {
            /* mmap(addr, len, prot, flags, fd, offset)
             * i386 syscall args: ebx, ecx, edx, esi, edi, ebp
             */
            uint32 addr = ebx;
            uint32 len = ecx;
            int prot = (int)edx;
            uint32 flags = regs[6];  /* esi */
            int fd = (int)regs[7];   /* edi */
            uint32 off = regs[5];    /* ebp */
            int ret = sys_mmap_vmm(proc, addr, len, prot, (int)flags, fd, off);
            regs[0] = (uint32)ret;
            return ret;
        }
        case __NR_munmap: {
            uint32 addr = ebx;
            uint32 len = ecx;
            int ret = sys_munmap_vmm(proc, addr, len);
            regs[0] = ret;
            return ret;
        }
        case __NR_mprotect: {
            uint32 addr = ebx;
            uint32 len = ecx;
            int prot = (int)edx;
            int ret = sys_mprotect_vmm(proc, addr, len, prot);
            regs[0] = ret;
            return ret;
        }
        // Networking syscalls
        case __NR_net_socket: {
            int ret = sys_net_socket(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_net_bind: {
            // ebx = port
            uint16 port = (uint16)ebx;
            int ret = sys_net_bind(proc, port);
            regs[0] = ret;
            return ret;
        }
        case __NR_net_sendto: {
            // ebx = socket_id, ecx = dst_ip_str, edx = dst_port, esi = buf, edi = len
            uint32 esi = regs[6];
            uint32 edi = regs[7];
            
            const char* dst_ip_str = (const char*)linux_translate_user_const_ptr(proc, ecx, 1u);
            const void* buf = linux_translate_user_const_ptr(proc, esi, edi);
            
            int ret = sys_net_sendto(proc, (int)ebx, dst_ip_str, (uint16)edx, buf, edi);
            regs[0] = ret;
            return ret;
        }
        case __NR_net_recvfrom: {
            // ebx = socket_id, ecx = buf, edx = buflen, esi = src_ip_out, edi = src_port_out
            uint32 esi = regs[6];
            uint32 edi = regs[7];
            
            void* buf = linux_translate_user_ptr(proc, ecx, edx);
            void* src_ip_out = linux_translate_user_ptr(proc, esi, 4u);
            void* src_port_out = linux_translate_user_ptr(proc, edi, (uint32)sizeof(uint16));
            
            int ret = sys_net_recvfrom(proc, (int)ebx, buf, edx, src_ip_out, src_port_out);
            regs[0] = ret;
            return ret;
        }
        case __NR_net_close: {
            // ebx = socket_id
            int ret = sys_net_close(proc, (int)ebx);
            regs[0] = ret;
            return ret;
        }
        case __NR_epoll_create: {
            // ebx = size (ignored)
            int ret = sys_epoll_create(proc, (int)ebx);
            regs[0] = ret;
            return ret;
        }
        case __NR_epoll_ctl: {
            // ebx = epfd, ecx = op, edx = fd, esi = events pointer
            uint32 esi = regs[6];
            const void* kevents = linux_translate_user_const_ptr(proc, esi, (uint32)(4u + 8u)); // epoll_event size
            int ret = sys_epoll_ctl(proc, (int)ebx, (int)ecx, (int)edx, kevents);
            regs[0] = ret;
            return ret;
        }
        case __NR_epoll_wait: {
            // ebx = epfd, ecx = events pointer, edx = maxevents, esi = timeout
            uint32 esi = regs[6];
            void* kevents = linux_translate_user_ptr(proc, ecx, edx * 12u); // each epoll_event is 12 bytes
            int ret = sys_epoll_wait(proc, (int)ebx, kevents, (int)edx, (int)esi);
            regs[0] = ret;
            return ret;
        }
        default:
            // Unknown syscall; return -ENOSYS
            regs[0] = (uint32)-38; // -ENOSYS
            return -38;
    }
}
