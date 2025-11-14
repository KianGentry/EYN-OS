#include <linux_syscalls.h>
#include <fs/vfs.h>
#include <string.h>
#include <util.h>
#include <vga.h>
#include <kb.h>

// Very small per-process FD table for now
#define LINUX_MAX_FD 32

typedef struct {
    int in_use;
    char path[128];
    int flags;
    uint32 pos; // current offset for read/write
    uint32 size; // known size if file
} linux_fd;

static linux_fd* get_fd_table(native_process_t* p) {
    if (!p->linux_fd_table) {
        p->linux_fd_table = (void*)malloc(sizeof(linux_fd) * LINUX_MAX_FD);
        if (p->linux_fd_table) memset(p->linux_fd_table, 0, sizeof(linux_fd) * LINUX_MAX_FD);
    }
    return (linux_fd*)p->linux_fd_table;
}

static int fd_alloc(linux_fd* tbl) {
    for (int i = 3; i < LINUX_MAX_FD; i++) if (!tbl[i].in_use) { tbl[i].in_use = 1; return i; }
    return -1;
}

static int sys_write(native_process_t* proc, uint32 fd, const void* buf, uint32 count) {
    if (fd == 1 || fd == 2) {
        // stdout/stderr -> console
        // Print as a string (truncate non-printables and stop at count)
        char out[256];
        uint32 n = (count < 255) ? count : 255;
        for (uint32 i = 0, j = 0; i < n && j < 255; i++) {
            unsigned char ch = ((const unsigned char*)buf)[i];
            if (ch == '\n' || (ch >= 32 && ch <= 126)) out[j++] = (char)ch;
            else {}
            out[j] = 0;
        }
        printf("%s", out);
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
        free(s);
        return (int)n;
    }
    linux_fd* tbl = get_fd_table(proc);
    if (!tbl || fd >= LINUX_MAX_FD || !tbl[fd].in_use) return -1;
    // Map to VFS read by path (inefficient but simple)
    uint32 sz = 0;
    if (vfs_get_file_size(0, tbl[fd].path, &sz) != 0) return -1;
    if (tbl[fd].pos >= sz) return 0;
    uint32 remain = sz - tbl[fd].pos;
    if (count > remain) count = remain;
    // Read slice: VFS currently reads full file; use tmp then copy
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

// Lightweight stubs for common libc expectations
static int sys_set_thread_area(native_process_t* proc, void* user_desc) {
    (void)proc; (void)user_desc;
    // Pretend success; we don't set up real TLS in this emulator
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

static int sys_brk(native_process_t* proc, void* addr) {
    // Minimal brk: track a single top pointer. If addr==0, return current.
    if (proc->brk_end == 0) proc->brk_end = proc->stack_start + 0x100000; // put heap below stack as placeholder
    if (addr == 0) return (int)proc->brk_end;
    uint32 new_end = (uint32)addr;
    // No real allocation; just accept within a conservative range
    if (new_end > proc->brk_end && new_end < proc->brk_end + 0x400000) {
        proc->brk_end = new_end;
    }
    return (int)proc->brk_end;
}

int linux_syscall_dispatch(native_process_t* proc, uint32 regs[8]) {
    uint32 eax = regs[0];
    uint32 ebx = regs[3];
    uint32 ecx = regs[1];
    uint32 edx = regs[2];

    switch (eax) {
        case __NR_write: {
            // ecx = buf (ELF vaddr), translate via segments
            const void* kbuf = (const void*)ecx;
            if (proc->segment_count && ecx >= proc->elf_vaddr_min && ecx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ecx >= sv && ecx + edx <= sv + sm) { kbuf = (const void*)((uint32)proc->segments[s].mem + (ecx - sv)); break; }
                }
            }
            int ret = sys_write(proc, ebx, kbuf, edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_read: {
            void* kbuf = (void*)ecx;
            // translate destination if needed
            if (proc->segment_count && ecx >= proc->elf_vaddr_min && ecx + edx <= proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ecx >= sv && ecx + edx <= sv + sm) { kbuf = (void*)((uint32)proc->segments[s].mem + (ecx - sv)); break; }
                }
            }
            int ret = sys_read(proc, ebx, kbuf, edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_open: {
            // ecx: flags, edx: mode; translate filename pointer
            const char* path = (const char*)ebx;
            if (proc->segment_count && ebx >= proc->elf_vaddr_min && ebx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ebx >= sv && ebx < sv + sm) { path = (const char*)((uint32)proc->segments[s].mem + (ebx - sv)); break; }
                }
            }
            int ret = sys_open(proc, path, (int)ecx, (int)edx);
            regs[0] = ret;
            return ret;
        }
        case __NR_close: {
            int ret = sys_close(proc, ebx);
            regs[0] = ret;
            return ret;
        }
        case __NR_fstat: {
            void* kbuf = (void*)ecx;
            if (proc->segment_count && ecx >= proc->elf_vaddr_min && ecx + sizeof(uint64) <= proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ecx >= sv && ecx < sv + sm) { kbuf = (void*)((uint32)proc->segments[s].mem + (ecx - sv)); break; }
                }
            }
            int ret = sys_fstat(proc, ebx, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_set_thread_area: {
            void* kbuf = (void*)ebx;
            if (proc->segment_count && ebx >= proc->elf_vaddr_min && ebx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ebx >= sv && ebx < sv + sm) { kbuf = (void*)((uint32)proc->segments[s].mem + (ebx - sv)); break; }
                }
            }
            int ret = sys_set_thread_area(proc, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_uname: {
            void* kbuf = (void*)ebx;
            if (proc->segment_count && ebx >= proc->elf_vaddr_min && ebx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ebx >= sv && ebx < sv + sm) { kbuf = (void*)((uint32)proc->segments[s].mem + (ebx - sv)); break; }
                }
            }
            int ret = sys_uname(proc, kbuf);
            regs[0] = ret;
            return ret;
        }
        case __NR_getpid: {
            int ret = sys_getpid(proc);
            regs[0] = ret;
            return ret;
        }
        case __NR_time: {
            uint32* kptr = (uint32*)ebx;
            if (proc->segment_count && ebx && ebx >= proc->elf_vaddr_min && ebx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ebx >= sv && ebx < sv + sm) { kptr = (uint32*)((uint32)proc->segments[s].mem + (ebx - sv)); break; }
                }
            }
            int ret = sys_time(proc, kptr);
            regs[0] = ret;
            return ret;
        }
        case __NR_gettimeofday: {
            void* ktv = (void*)ebx; void* ktz = (void*)ecx;
            if (proc->segment_count) {
                if (ebx && ebx >= proc->elf_vaddr_min && ebx < proc->elf_vaddr_max) {
                    for (int s = 0; s < proc->segment_count; s++) {
                        uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                        if (ebx >= sv && ebx < sv + sm) { ktv = (void*)((uint32)proc->segments[s].mem + (ebx - sv)); break; }
                    }
                }
                if (ecx && ecx >= proc->elf_vaddr_min && ecx < proc->elf_vaddr_max) {
                    for (int s = 0; s < proc->segment_count; s++) {
                        uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                        if (ecx >= sv && ecx < sv + sm) { ktz = (void*)((uint32)proc->segments[s].mem + (ecx - sv)); break; }
                    }
                }
            }
            int ret = sys_gettimeofday(proc, ktv, ktz);
            regs[0] = ret;
            return ret;
        }
        case __NR_clock_gettime: {
            void* kts = (void*)ecx;
            if (proc->segment_count && ecx >= proc->elf_vaddr_min && ecx < proc->elf_vaddr_max) {
                for (int s = 0; s < proc->segment_count; s++) {
                    uint32 sv = proc->segments[s].vaddr, sm = proc->segments[s].memsz;
                    if (ecx >= sv && ecx < sv + sm) { kts = (void*)((uint32)proc->segments[s].mem + (ecx - sv)); break; }
                }
            }
            int ret = sys_clock_gettime(proc, (int)ebx, kts);
            regs[0] = ret;
            return ret;
        }
        case __NR_lseek: {
            int ret = sys_lseek(proc, ebx, (int32)ecx, (int)edx);
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
            int ret = sys_brk(proc, (void*)ebx);
            regs[0] = ret;
            return ret;
        }
        default:
            // Unknown syscall; return -ENOSYS
            regs[0] = (uint32)-38; // -ENOSYS
            return -38;
    }
}
