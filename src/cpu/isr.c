#include <isr.h>
#include <vga.h>
#include <idt.h>
#include <multiboot.h>
#include <shell.h>
#include <util.h>
#include <string.h>
#include <kb.h>
#include <panic.h>

#include <sched.h>

#include <tile_manager.h>
#include <terminals.h>
#include <mm/user_access.h>
#include <fs/vfs.h>
#include <fs_commands.h>
extern multiboot_info_t *g_mbi;

// Global error tracking
static volatile int system_error_count = 0;
static volatile int last_error_code = 0;
static volatile uint32 last_error_eip = 0;

// Error severity levels
#define ERROR_FATAL 0
#define ERROR_RECOVERABLE 1
#define ERROR_WARNING 2

// Error context structure
typedef struct {
    int error_code;
    uint32 eip;
    uint32 eflags;
    uint32 esp;
    int severity;
} error_context_t;

// Forward declarations
static void handle_error(int isr_num, error_context_t* ctx);
static int is_recoverable_error(int isr_num);
static void log_error(int isr_num, error_context_t* ctx);
static void attempt_recovery(error_context_t* ctx);

extern void isr_install() 
{
    set_idt_gate(0, (uint32)isr0);
    set_idt_gate(1, (uint32)isr1);
    set_idt_gate(2, (uint32)isr2);
    set_idt_gate(3, (uint32)isr3);
    set_idt_gate(4, (uint32)isr4);
    set_idt_gate(5, (uint32)isr5);
    set_idt_gate(6, (uint32)isr6);
    set_idt_gate(7, (uint32)isr7);
    set_idt_gate(8, (uint32)isr8);
    set_idt_gate(9, (uint32)isr9);
    set_idt_gate(10, (uint32)isr10);
    set_idt_gate(11, (uint32)isr11);
    set_idt_gate(12, (uint32)isr12);
    set_idt_gate(13, (uint32)isr13);
    set_idt_gate(14, (uint32)isr14);
    set_idt_gate(15, (uint32)isr15);
    set_idt_gate(16, (uint32)isr16);
    set_idt_gate(17, (uint32)isr17);
    set_idt_gate(18, (uint32)isr18);
    set_idt_gate(19, (uint32)isr19);
    set_idt_gate(20, (uint32)isr20);
    set_idt_gate(21, (uint32)isr21);
    set_idt_gate(22, (uint32)isr22);
    set_idt_gate(23, (uint32)isr23);
    set_idt_gate(24, (uint32)isr24);
    set_idt_gate(25, (uint32)isr25);
    set_idt_gate(26, (uint32)isr26);
    set_idt_gate(27, (uint32)isr27);
    set_idt_gate(28, (uint32)isr28);
    set_idt_gate(29, (uint32)isr29);
    set_idt_gate(30, (uint32)isr30);
    set_idt_gate(31, (uint32)isr31);
    
    // Set up syscall handler (interrupt 0x80) to the assembly stub
    extern void syscall_entry();
    set_syscall_gate(0x80, (uint32)syscall_entry);

    set_idt(); // Load with ASM
}

// Generic ISR handler that captures context and attempts recovery
static void generic_isr_handler(regs_t* regs) {
    error_context_t ctx;
    int isr_num = (int)regs->int_no;
    ctx.error_code = isr_num;
    ctx.eip = regs->eip;
    ctx.eflags = regs->eflags;
    ctx.esp = ((regs->cs & 3) == 3) ? regs->useresp : 0;
    
    // Determine error severity
    if (is_recoverable_error(isr_num)) {
        ctx.severity = ERROR_RECOVERABLE;
    } else if (isr_num == 0 || isr_num == 6 || isr_num == 8 || isr_num == 13 || isr_num == 14) {
        ctx.severity = ERROR_FATAL;
    } else {
        ctx.severity = ERROR_WARNING;
    }
    
    // Log the error
    log_error(isr_num, &ctx);
    
    // Handle based on severity
    handle_error(isr_num, &ctx);
}

// Determine if an error is potentially recoverable
static int is_recoverable_error(int isr_num) {
    // Most errors are recoverable except critical ones
    switch (isr_num) {
        case 0:  // Division by zero - can be recovered
        case 1:  // Debug - recoverable
        case 3:  // Breakpoint - recoverable
        case 4:  // Overflow - recoverable
        case 5:  // Bounds - recoverable
        case 7:  // No coprocessor - recoverable
        case 9:  // Coprocessor segment overrun - recoverable
        case 10: // Bad TSS - recoverable
        case 11: // Segment not present - recoverable
        case 12: // Stack fault - recoverable
        case 15: // Unknown interrupt - recoverable
        case 16: // Coprocessor fault - recoverable
        case 17: // Alignment check - recoverable
        case 18: // Machine check - recoverable
            return 1;
        case 6:  // Invalid opcode - potentially fatal
        case 8:  // Double fault - fatal
        case 13: // General protection fault - potentially fatal
        case 14: // Page fault - potentially fatal
            return 0;
        default:
            return 1;
    }
}

// Log error with context
static void log_error(int isr_num, error_context_t* ctx) {
    system_error_count++;
    last_error_code = isr_num;
    last_error_eip = ctx->eip;
    
    printf("%c[ERROR] %s (ISR %d) at 0x%X\n", 255, 0, 0, 
           exception_messages[isr_num], isr_num, ctx->eip);
    
    if (ctx->severity == ERROR_FATAL) {
        printf("%c[FATAL] System may be unstable\n", 255, 0, 0);
    } else if (ctx->severity == ERROR_RECOVERABLE) {
        printf("%c[RECOVERABLE] Attempting to continue...\n", 255, 165, 0);
    } else {
        printf("%c[WARNING] Non-critical error\n", 255, 255, 0);
    }
}

// Attempt recovery based on error type
static void attempt_recovery(error_context_t* ctx) {
    switch (ctx->error_code) {
        case 0: // Division by zero
            // Set result to 0 and continue
            printf("%c[RECOVERY] Division by zero handled\n", 0, 255, 0);
            break;
        case 1: // Debug
            printf("%c[RECOVERY] Debug exception handled\n", 0, 255, 0);
            break;
        case 3: // Breakpoint
            printf("%c[RECOVERY] Breakpoint handled\n", 0, 255, 0);
            break;
        case 4: // Overflow
            printf("%c[RECOVERY] Overflow handled\n", 0, 255, 0);
            break;
        case 5: // Bounds
            printf("%c[RECOVERY] Bounds check handled\n", 0, 255, 0);
            break;
        default:
            printf("%c[RECOVERY] Generic error recovery\n", 0, 255, 0);
            break;
    }
}

// Handle error based on severity
static void handle_error(int isr_num, error_context_t* ctx) {
    switch (ctx->severity) {
        case ERROR_FATAL:
            printf("%c[FATAL] Critical error - invoking kernel panic\n", 255, 0, 0);
            printf("%cError count: %d\n", 255, 255, 255, system_error_count);
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "ISR %d at 0x%X", isr_num, ctx->eip);
                PANIC(msg);
            }
            break; // PANIC does not return
            
        case ERROR_RECOVERABLE:
            attempt_recovery(ctx);
            // Return to shell instead of halting
            printf("%c[RECOVERY] Returning to shell...\n", 0, 255, 0);
            // Clear any pending interrupts
            asm("cli");
            asm("sti");
            break;
            
        case ERROR_WARNING:
            printf("%c[WARNING] Non-critical error - continuing\n", 255, 255, 0);
            break;

        default:
            printf("%c[WARNING] Unknown error severity - continuing\n", 255, 255, 0);
            break;
    }
}

// Individual ISR handlers - now use intelligent recovery
void isr_dispatch(regs_t* regs) {
    if (!regs) {
        return;
    }

    /* Page faults should be handled by the VMM (demand paging / COW / swap). */
    if (regs->int_no == 14) {
        extern void page_fault_handler(regs_t* r);
        page_fault_handler(regs);
        return;
    }

    generic_isr_handler(regs);
}

// Error status functions for shell commands
int get_system_error_count() {
    return system_error_count;
}

int get_last_error_code() {
    return last_error_code;
}

uint32 get_last_error_eip() {
    return last_error_eip;
}

// Syscall numbers
#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  2
#define SYSCALL_READ  3
#define SYSCALL_OPEN  4
#define SYSCALL_CLOSE 5
#define SYSCALL_GETKEY 6
#define SYSCALL_GETDENTS 7
// GUI/tiler syscalls (user-mode)
#define SYSCALL_GUI_CREATE 8
#define SYSCALL_GUI_SET_TITLE 9

typedef struct {
    int used;
    int is_dir;
    uint32 offset;
    uint32 dir_pos;
    uint32 size;
    uint8 drive;
    char path[128];
} user_fd_t;

#define USER_FD_MAX 16
static user_fd_t g_user_fds[USER_FD_MAX];

void syscall_reset_user_fds(void) {
    for (int i = 0; i < USER_FD_MAX; ++i) {
        g_user_fds[i].used = 0;
        g_user_fds[i].is_dir = 0;
        g_user_fds[i].offset = 0;
        g_user_fds[i].dir_pos = 0;
        g_user_fds[i].size = 0;
        g_user_fds[i].drive = 0;
        g_user_fds[i].path[0] = '\0';
    }
}

typedef struct {
    int used;
    int tile_idx;
    char* title;
    char* status_left;
} user_gui_t;

// handle 0 is reserved for the current user task's existing tile (g_user_task_term).
#define USER_GUI_MAX 8
static user_gui_t g_user_guis[USER_GUI_MAX];
static char* g_user_self_title = NULL;
static int g_user_self_tile_idx = -1;

static void user_gui_free_entry(user_gui_t* e) {
    if (!e) return;
    if (e->tile_idx >= 0) {
        tile_close(e->tile_idx);
    }
    if (e->title) free(e->title);
    if (e->status_left) free(e->status_left);
    e->used = 0;
    e->tile_idx = -1;
    e->title = NULL;
    e->status_left = NULL;
}

static int user_gui_alloc_handle(void) {
    // start at 1; handle 0 reserved for "self"
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (!g_user_guis[i].used) {
            g_user_guis[i].used = 1;
            g_user_guis[i].tile_idx = -1;
            g_user_guis[i].title = NULL;
            g_user_guis[i].status_left = NULL;
            return i;
        }
    }
    return -1;
}

static user_gui_t* user_gui_get(int handle) {
    if (handle <= 0 || handle >= USER_GUI_MAX) return NULL;
    if (!g_user_guis[handle].used) return NULL;
    return &g_user_guis[handle];
}

void syscall_reset_user_guis(void) {
    // Close/free any created GUI tiles
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (g_user_guis[i].used) {
            user_gui_free_entry(&g_user_guis[i]);
        }
    }
    // Restore the current tile title if the user task changed it.
    if (g_user_self_title) {
        free(g_user_self_title);
        g_user_self_title = NULL;
    }
    if (tile_is_tiling_active() && g_user_self_tile_idx >= 0) {
        tile_set_title_status(g_user_self_tile_idx, "EYN-OS Shell", NULL, NULL);
        tile_invalidate_decorations(g_user_self_tile_idx);
    }
    g_user_self_tile_idx = -1;
}

static int copyin_cstr(char* dst, size_t dstsz, const char* user_src) {
    if (!dst || dstsz == 0 || !user_src) return -1;
    for (size_t i = 0; i < dstsz; ++i) {
        char ch = 0;
        if (copyin(&ch, user_src + i, 1) != 0) return -1;
        dst[i] = ch;
        if (ch == '\0') return 0;
    }
    dst[dstsz - 1] = '\0';
    return -1;
}

static void trim_trailing_crlf(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static char* kstrdup_bounded(const char* s, size_t max_len) {
    if (!s) return NULL;
    size_t n = 0;
    while (n < max_len && s[n] != '\0') {
        n++;
    }
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int user_fd_alloc(void) {
    for (int i = 3; i < USER_FD_MAX; ++i) {
        if (!g_user_fds[i].used) {
            g_user_fds[i].used = 1;
            g_user_fds[i].is_dir = 0;
            g_user_fds[i].offset = 0;
            g_user_fds[i].dir_pos = 0;
            g_user_fds[i].size = 0;
            g_user_fds[i].drive = 0;
            g_user_fds[i].path[0] = '\0';
            return i;
        }
    }
    return -1;
}

static user_fd_t* user_fd_get(int fd) {
    if (fd < 0 || fd >= USER_FD_MAX) return NULL;
    if (!g_user_fds[fd].used) return NULL;
    return &g_user_fds[fd];
}

typedef struct {
    uint8 is_dir;
    uint8 _pad[3];
    uint32 size;
    char name[56];
} eyn_dirent_t;

typedef struct {
    user_fd_t* ufd;
    uint32 cur_index;
    int written;
    int max_entries;
    eyn_dirent_t* user_out;
} syscall_getdents_ctx_t;

static int syscall_getdents_cb(const char* name, int is_dir, uint32 size, void* user) {
    syscall_getdents_ctx_t* c = (syscall_getdents_ctx_t*)user;
    if (!c || !c->ufd) return 1;

    if (c->cur_index < c->ufd->dir_pos) {
        c->cur_index++;
        return 0;
    }

    if (c->written >= c->max_entries) {
        return 1;
    }

    eyn_dirent_t ent;
    memset(&ent, 0, sizeof(ent));
    ent.is_dir = (uint8)(is_dir ? 1 : 0);
    ent.size = size;
    if (name) {
        strncpy(ent.name, name, sizeof(ent.name) - 1);
        ent.name[sizeof(ent.name) - 1] = '\0';
    }

    if (copyout(&c->user_out[c->written], &ent, sizeof(ent)) != 0) {
        return 1;
    }

    c->written++;
    c->ufd->dir_pos++;
    c->cur_index++;
    return 0;
}

// When the tiling manager is active, user-mode programs should print into the
// focused virtual terminal so output is visible in the graphical shell.
static void syscall_console_write(const char* buf, int len) {
    if (!buf || len <= 0) return;
    if (tile_is_tiling_active()) {
        int term = tile_get_focused();
        if (g_user_task_active) {
            term = g_user_task_term;
        }
        if (term < 0) term = 0;
        for (int i = 0; i < len; ++i) {
            vterm_write_char(term, buf[i]);
        }
        if (g_user_task_active) {
            g_user_task_ui_dirty = 1;
        }
        return;
    }
    // Fallback: use kernel printf (serial/text console)
    int pos = 0;
    while (pos < len) {
        int chunk = len - pos;
        if (chunk > 120) chunk = 120;
        char out[121];
        memcpy(out, buf + pos, chunk);
        out[chunk] = '\0';
        printf("%s", out);
        pos += chunk;
    }
}

static void syscall_maybe_render_ui(void) {
    // Intentionally left as a no-op.
    // Rendering from inside the syscall handler is fragile (can be re-entrant
    // w/ IRQ0 and does heavy GUI work while in an interrupt gate). We instead
    // repaint from the PIT IRQ0 path while a user task is active.
}

// Upper bound to keep syscall I/O bounded (protects kernel stack/time).
#define SYSCALL_IO_MAX (64 * 1024)

// Set to 1 to enable verbose syscall I/O debugging on serial.
#ifndef SYSCALL_DEBUG
#define SYSCALL_DEBUG 0
#endif

// C dispatcher called by the assembly stub. Returns value in EAX to user.
uint32 syscall_dispatch(regs_t* regs) {
    uint32 syscall_num = regs->eax;
    uint32 arg1 = regs->ebx;
    uint32 arg2 = regs->ecx;
    uint32 arg3 = regs->edx;

    switch (syscall_num) {
        case SYSCALL_WRITE: {
            if (arg1 == 1) {
                const char* user_buf = (const char*)arg2;
                int len = (int)arg3;
                if (len < 0) {
                    regs->eax = (uint32)-1;
                    break;
                }
                if (len == 0) {
                    regs->eax = 0;
                    break;
                }

                if (len > SYSCALL_IO_MAX) {
                    len = SYSCALL_IO_MAX;
                }

                // Copy user memory in small chunks to avoid faults/overruns.
                int pos = 0;
                while (pos < len) {
                    int chunk = len - pos;
                    if (chunk > 256) chunk = 256;
                    char tmp[256];
                    if (copyin(tmp, user_buf + pos, (size_t)chunk) != 0) {
                        regs->eax = (uint32)-1;
                        return regs->eax;
                    }
                    syscall_console_write(tmp, chunk);
                    pos += chunk;
                }

                syscall_maybe_render_ui();

                regs->eax = (uint32)len; // return bytes written (possibly clamped)
            } else {
                regs->eax = (uint32)-1;
            }
            break;
        }
        case SYSCALL_READ: {
            // read(fd=0, buf=arg2, len=arg3)
            if (arg3 == 0) { regs->eax = 0; break; }
            if (arg2 == 0) { regs->eax = (uint32)-1; break; }
            int fd = (int)arg1;
            int maxlen = (int)arg3;
            if (maxlen < 0) { regs->eax = (uint32)-1; break; }
            if (maxlen > SYSCALL_IO_MAX) maxlen = SYSCALL_IO_MAX;

            // stdin: line read (NUL-terminated for convenience)
            if (fd == 0) {
                static int s_stdin_debug_once = 0;
                if (!s_stdin_debug_once) {
                    s_stdin_debug_once = 1;
                    printf("%c[SYSCALL:READ] fd=0 buf=0x%X len=%d useresp=0x%X\n",
                           255, 255, 0, (unsigned)arg2, maxlen, (unsigned)regs->useresp);
                }

                // When tiling is active, use the vterm stdin buffer instead of
                // polling the keyboard directly. The TUI routes input to this buffer.
                const char* s = NULL;
                int slen = 0;
                
                if (tile_is_tiling_active() && g_user_task_term >= 0) {
                    int term = g_user_task_term;  // Cache to avoid race conditions
                    
                    // Wait for a complete line in the stdin buffer.
                    // This is a busy-wait but the PIT IRQ will feed the buffer
                    // and can also set g_user_interrupt for Ctrl+C.
                    // Enable interrupts so the PIT can fire and process keyboard input.
                    __asm__ __volatile__("sti");
                    while (!vterm_stdin_ready(term)) {
                        if (g_user_interrupt) {
                            // User pressed Ctrl+C - return error
                            regs->eax = (uint32)-1;
                            goto stdin_done;
                        }
                        // Yield to allow IRQ processing (PIT feeds the buffer)
                        __asm__ __volatile__("hlt");
                    }
                    
                    // Disable interrupts while reading buffer to prevent races
                    __asm__ __volatile__("cli");
                    s = vterm_stdin_data(term);
                    slen = vterm_stdin_len(term);
                    
                    if (SYSCALL_DEBUG) {
                        printf("[stdin] len=%d data='%s'\n", slen, s ? s : "(null)");
                    }
                    
                    // Re-enable interrupts
                    __asm__ __volatile__("sti");
                } else {
                    // Non-tiling mode: use direct keyboard polling
                    s = readStr();
                    if (!s) { regs->eax = (uint32)-1; break; }
                    slen = (int)strlen(s);
                }
                
                if (maxlen <= 0) { regs->eax = 0; goto stdin_cleanup; }

                int n = slen;
                if (n > maxlen - 1) n = maxlen - 1;
                if (n < 0) n = 0;

                char* user_dst = (char*)arg2;
                if (copyout(user_dst, s, (size_t)n) != 0) {
                    if (SYSCALL_DEBUG) {
                        printf("[stdin] copyout failed\n");
                    }
                    regs->eax = (uint32)-1;
                    goto stdin_cleanup;
                }
                if (copyout(user_dst + n, "\0", 1) != 0) {
                    if (SYSCALL_DEBUG) {
                        printf("[stdin] copyout NUL failed\n");
                    }
                    regs->eax = (uint32)-1;
                    goto stdin_cleanup;
                }

                if (SYSCALL_DEBUG) {
                    printf("[stdin] returning %d bytes to user\n", n);
                }
                regs->eax = (uint32)n;
                
            stdin_cleanup:
                // Clear the stdin buffer after consuming
                if (tile_is_tiling_active() && g_user_task_term >= 0) {
                    vterm_stdin_consume(g_user_task_term);
                }
            stdin_done:
                break;
            }

            // file read
            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd || ufd->is_dir) { regs->eax = (uint32)-1; break; }

            if (ufd->offset == 0) {
                if (SYSCALL_DEBUG) {
                    printf("[SYSCALL:READ file] fd=%d drive=%d path='%s' size=%d max=%d\n",
                           fd, (int)ufd->drive, ufd->path, (int)ufd->size, maxlen);
                }
            }

            char tmp[256];
            int remaining = maxlen;
            int total = 0;
            char* user_dst = (char*)arg2;
            while (remaining > 0) {
                int chunk = remaining;
                if (chunk > (int)sizeof(tmp)) chunk = (int)sizeof(tmp);
                int n = vfs_read_file_at(ufd->drive, ufd->path, tmp, chunk, ufd->offset);
                if (n < 0) { regs->eax = (uint32)-1; return regs->eax; }
                if (n == 0) {
                    if (total == 0) {
                        if (SYSCALL_DEBUG) {
                            printf("[SYSCALL:READ file] EOF at off=%d (size=%d)\n",
                                   (int)ufd->offset, (int)ufd->size);
                        }
                    }
                    break;
                }
                if (copyout(user_dst + total, tmp, (size_t)n) != 0) { regs->eax = (uint32)-1; return regs->eax; }
                ufd->offset += (uint32)n;
                total += n;
                remaining -= n;
                if (n < chunk) break;
            }
            regs->eax = (uint32)total;
            break;
        }
        case SYSCALL_OPEN: {
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) {
                if (SYSCALL_DEBUG) {
                    printf("[SYSCALL:OPEN] copyin_cstr failed (user_path=0x%X)\n", (unsigned)arg1);
                }
                regs->eax = (uint32)-1;
                break;
            }

            // Be tolerant of user programs that pass newline-terminated paths.
            // This commonly happens when reading a line from stdin.
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            extern uint8 g_current_drive;
            uint8 drive = g_current_drive;

                 // Log the open request even on failure (stdout goes to vterm, but
                 // these diagnostics go to serial so we can see what's happening).
                 vfs_fs_type_t fs = vfs_detect(drive);
                     if (SYSCALL_DEBUG) {
                      printf("[SYSCALL:OPEN] req='%s' cwd='%s' => '%s' drive=%d fs=%d\n",
                          path,
                          cwd ? cwd : "(null)",
                          abspath,
                          (int)drive,
                          (int)fs);
                     }

            vfs_stat_t st;
            int st_rc = vfs_stat(drive, abspath, &st);
            if (st_rc != 0) {
                if (SYSCALL_DEBUG) {
                    printf("[SYSCALL:OPEN] vfs_stat failed rc=%d for '%s' (drive=%d fs=%d)\n",
                           st_rc, abspath, (int)drive, (int)fs);
                }
                regs->eax = (uint32)-1;
                break;
            }

            int fd = user_fd_alloc();
            if (fd < 0) { regs->eax = (uint32)-1; break; }

            user_fd_t* ufd = &g_user_fds[fd];
            ufd->drive = drive;
            strncpy(ufd->path, abspath, sizeof(ufd->path) - 1);
            ufd->path[sizeof(ufd->path) - 1] = '\0';
            ufd->offset = 0;
            ufd->dir_pos = 0;
            ufd->is_dir = (st.type == VFS_NODE_DIR);
            ufd->size = st.size;

                     if (SYSCALL_DEBUG) {
                      printf("[SYSCALL:OPEN] path='%s' cwd='%s' => '%s' drive=%d fd=%d type=%d size=%d\n",
                          path, cwd ? cwd : "(null)", abspath, (int)drive, fd, (int)st.type, (int)st.size);
                     }

            regs->eax = (uint32)fd;
            break;
        }
        case SYSCALL_CLOSE: {
            int fd = (int)arg1;
            if (fd < 0 || fd >= USER_FD_MAX) { regs->eax = (uint32)-1; break; }
            if (fd <= 2) { regs->eax = 0; break; }
            g_user_fds[fd].used = 0;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GETDENTS: {
            int fd = (int)arg1;
            void* user_buf = (void*)arg2;
            int buflen = (int)arg3;
            if (!user_buf || buflen <= 0) { regs->eax = 0; break; }
            if (buflen > SYSCALL_IO_MAX) buflen = SYSCALL_IO_MAX;

            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd || !ufd->is_dir) { regs->eax = (uint32)-1; break; }

            int max_entries = buflen / (int)sizeof(eyn_dirent_t);
            if (max_entries <= 0) { regs->eax = 0; break; }

            syscall_getdents_ctx_t ctx;
            ctx.ufd = ufd;
            ctx.cur_index = 0;
            ctx.written = 0;
            ctx.max_entries = max_entries;
            ctx.user_out = (eyn_dirent_t*)user_buf;

            int rc = vfs_listdir(ufd->drive, ufd->path, syscall_getdents_cb, &ctx);
            if (rc < 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)(ctx.written * (int)sizeof(eyn_dirent_t));
            break;
        }
        case SYSCALL_GUI_CREATE: {
            // gui_create(title_ptr=arg1, status_left_ptr=arg2)
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }

            const char* user_title = (const char*)arg1;
            const char* user_status = (const char*)arg2;
            if (!user_title) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);
            if (!title_tmp[0]) strncpy(title_tmp, "User App", sizeof(title_tmp) - 1);
            title_tmp[sizeof(title_tmp) - 1] = '\0';

            char status_tmp[64];
            status_tmp[0] = '\0';
            if (user_status) {
                if (copyin_cstr(status_tmp, sizeof(status_tmp), user_status) != 0) {
                    status_tmp[0] = '\0';
                }
                trim_trailing_crlf(status_tmp);
            }

            int handle = user_gui_alloc_handle();
            if (handle < 0) { regs->eax = (uint32)-1; break; }

            // Allocate persistent strings (tiler stores pointers)
            user_gui_t* e = &g_user_guis[handle];
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
            if (!e->title) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }

            int tile_idx = tile_create_gui_tile(e->title, e->status_left);
            if (tile_idx < 0) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }
            e->tile_idx = tile_idx;
            tile_invalidate_decorations(tile_idx);
            regs->eax = (uint32)handle;
            break;
        }
        case SYSCALL_GUI_SET_TITLE: {
            // gui_set_title(handle=arg1, title_ptr=arg2)
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const char* user_title = (const char*)arg2;
            if (!user_title) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);

            if (handle == 0) {
                int tile_idx = tile_get_focused();
                if (tile_idx < 0) { regs->eax = (uint32)-1; break; }
                char* new_title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
                if (!new_title) { regs->eax = (uint32)-1; break; }
                if (g_user_self_title) free(g_user_self_title);
                g_user_self_title = new_title;
                g_user_self_tile_idx = tile_idx;
                tile_set_title_status(tile_idx, g_user_self_title, NULL, NULL);
                tile_invalidate_decorations(tile_idx);
                regs->eax = 0;
                break;
            }

            user_gui_t* e = user_gui_get(handle);
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            char* new_title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            if (!new_title) { regs->eax = (uint32)-1; break; }
            if (e->title) free(e->title);
            e->title = new_title;
            tile_set_title_status(e->tile_idx, e->title, e->status_left, NULL);
            tile_invalidate_decorations(e->tile_idx);
            regs->eax = 0;
            break;
        }
        case SYSCALL_EXIT: {
            // Keep this visible in the graphical shell too.
            char msg[64];
            int n = snprintf(msg, sizeof(msg), "[SYSCALL] Program exited with code %d\n", (int)arg1);
            if (n > 0) syscall_console_write(msg, n);
            // Clean up any GUI resources created by this user task.
            syscall_reset_user_guis();
            g_user_task_active = 0;
            g_user_task_term = -1;
            g_user_interrupt = 0;
            g_abort_to_shell = 1;
            syscall_reset_user_fds();
            regs->eax = 0;
            break;
        }
        case SYSCALL_GETKEY: {
            regs->eax = (uint32)kb_getchar_nonblocking();
            break;
        }
        default: {
            printf("%c[SYSCALL] Unknown syscall: %d\n", 255, 0, 0, syscall_num);
            regs->eax = (uint32)-1;
            break;
        }
    }

    return regs->eax;
}


/*To printf the message which defines every exception */
string exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

