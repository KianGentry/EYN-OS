#include <isr.h>
#include <vga.h>
#include <idt.h>
#include <multiboot.h>
#include <shell.h>
#include <util.h>
#include <string.h>
#include <kb.h>
#include <panic.h>
#include <watchdog.h>
#include <capabilities.h>

#include <misc/sched.h>

#include <tile_manager.h>
#include <terminals.h>
#include <mm/user_access.h>
#include <fs/vfs.h>
#include <fs_commands.h>
#include <eynfs.h>
#include <context.h>
#include <ata.h>
#include <serial.h>
#include <crashlog.h>
#include <pipeline.h>
#include <drivers/pci.h>
#include <drivers/e1000.h>
#include <drivers/ac97.h>
#include <network/netstack.h>
#include <predictive_memory.h>
#include <run_command.h>
#include <paging.h>
#include <drivers/mouse.h>
#include <cpu/user_elf.h>
#include <partition.h>

extern background_process_t g_background_processes[MAX_BACKGROUND_PROCESSES];

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
extern void fpu_handle_nm(void);
static uint32 syscall_dispatch_core(regs_t* regs,
                                    uint32 syscall_num,
                                    uintptr arg1,
                                    uintptr arg2,
                                    uintptr arg3,
                                    uintptr arg4,
                                    uintptr arg5);

extern void isr_install() 
{
    set_idt_gate(0, (uintptr)isr0);
    set_idt_gate(1, (uintptr)isr1);
    set_idt_gate(2, (uintptr)isr2);
    set_idt_gate(3, (uintptr)isr3);
    set_idt_gate(4, (uintptr)isr4);
    set_idt_gate(5, (uintptr)isr5);
    set_idt_gate(6, (uintptr)isr6);
    set_idt_gate(7, (uintptr)isr7);
    set_idt_gate(8, (uintptr)isr8);
    set_idt_gate(9, (uintptr)isr9);
    set_idt_gate(10, (uintptr)isr10);
    set_idt_gate(11, (uintptr)isr11);
    set_idt_gate(12, (uintptr)isr12);
    set_idt_gate(13, (uintptr)isr13);
    set_idt_gate(14, (uintptr)isr14);
    set_idt_gate(15, (uintptr)isr15);
    set_idt_gate(16, (uintptr)isr16);
    set_idt_gate(17, (uintptr)isr17);
    set_idt_gate(18, (uintptr)isr18);
    set_idt_gate(19, (uintptr)isr19);
    set_idt_gate(20, (uintptr)isr20);
    set_idt_gate(21, (uintptr)isr21);
    set_idt_gate(22, (uintptr)isr22);
    set_idt_gate(23, (uintptr)isr23);
    set_idt_gate(24, (uintptr)isr24);
    set_idt_gate(25, (uintptr)isr25);
    set_idt_gate(26, (uintptr)isr26);
    set_idt_gate(27, (uintptr)isr27);
    set_idt_gate(28, (uintptr)isr28);
    set_idt_gate(29, (uintptr)isr29);
    set_idt_gate(30, (uintptr)isr30);
    set_idt_gate(31, (uintptr)isr31);
    
    // Set up syscall handler (interrupt 0x80) to the assembly stub
    extern void syscall_entry();
    set_syscall_gate(0x80, (uintptr)syscall_entry);

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

    // ISR 7: #NM (Device Not Available). Used for lazy x87 enabling (CR0.TS).
    // Even if we don't use lazy switching yet, handling this avoids spurious
    // fatal errors if firmware/boot code left TS set.
    if (regs->int_no == 7) {
        fpu_handle_nm();
        return;
    }

    /* Page faults should be handled by the VMM (demand paging / COW / swap). */
    if (regs->int_no == 14) {
        page_fault_handler(regs);
        return;
    }

    generic_isr_handler(regs);
}

#if defined(EYNOS_ARCH_AMD64)
/*
 * ABI-INVARIANT: amd64 entry stubs bridge into 32-bit-compatible core paths.
 *
 * Why: Current paging/syscall core APIs and saved-reg compatibility structs are
 * 32-bit shaped.
 * Invariant: Values copied from 64-bit frames must round-trip through uint32
 * before entering those paths.
 * Breakage if violated: Silent truncation can misroute page-fault handling or
 * syscall dispatch state.
 */
static int isr_amd64_u32_from_uintptr(uintptr value, uint32* out) {
    if (!out) return -1;
    uint32 narrowed = (uint32)value;
    if ((uintptr)narrowed != value) return -1;
    *out = narrowed;
    return 0;
}

void isr_amd64_dispatch_frame(const amd64_interrupt_frame_t* frame) {
    if (!frame) {
        return;
    }

    if (frame->vector == 13u && (frame->cs & 3u) == 3u) {
        printf("\n%c*** General Protection Fault (user) ***\n", 255, 0, 0);
        printf("RIP: 0x%08X  ERR: 0x%08X\n",
               (unsigned)((uint32)frame->rip),
               (unsigned)((uint32)frame->error_code));
        printf("Terminating user process\n");
        int exiting_pid = user_task_get_running_pid();
        if (exiting_pid > 0) {
            syscall_cleanup_user_resources_for_pid(exiting_pid);
        }
        user_task_notify_exit(-13);
        g_user_task_active = 0;
        g_user_task_term = -1;
        g_user_interrupt = 0;
        g_abort_to_shell = 1;
        return;
    }

    if (frame->vector == 7u) {
        fpu_handle_nm();
        return;
    }

    if (frame->vector == 14u) {
        uintptr fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

        uint32 fault_addr32 = 0;
        uint32 rip32 = 0;
        if (isr_amd64_u32_from_uintptr(fault_addr, &fault_addr32) != 0 ||
            isr_amd64_u32_from_uintptr((uintptr)frame->rip, &rip32) != 0) {
            PANIC("amd64 page-fault frame exceeds 32-bit VMM contract");
        }

        if ((frame->cs & 3u) != 3u) {
            PANICF("PAGE FAULT (kernel): addr=0x%08X rip=0x%08X err=0x%08X cs=0x%08X",
                   (unsigned)fault_addr32,
                   (unsigned)rip32,
                   (unsigned)((uint32)frame->error_code),
                   (unsigned)((uint32)frame->cs));
        }

        vmm_page_fault_handler((uint32)frame->error_code,
                               fault_addr32,
                               rip32);
        return;
    }

    regs_t synthetic_regs;
    memset(&synthetic_regs, 0, sizeof(synthetic_regs));

    uint32 frame_vector32 = 0;
    uint32 frame_error32 = 0;
    uint32 frame_rip32 = 0;
    if (isr_amd64_u32_from_uintptr((uintptr)frame->vector, &frame_vector32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->error_code, &frame_error32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->rip, &frame_rip32) != 0) {
        PANIC("amd64 interrupt frame exceeds 32-bit register bridge contract");
    }

    synthetic_regs.int_no = frame_vector32;
    synthetic_regs.err_code = frame_error32;
    synthetic_regs.eip = frame_rip32;
    synthetic_regs.cs = (uint32)frame->cs;
    synthetic_regs.eflags = (uint32)frame->rflags;

    generic_isr_handler(&synthetic_regs);
}

uint64 syscall_dispatch_amd64_frame(const amd64_syscall_frame_t* frame) {
    if (!frame) {
        return (uint64)-1;
    }

    regs_t synthetic_regs;
    memset(&synthetic_regs, 0, sizeof(synthetic_regs));

    uint32 syscall_no32 = 0;
    uint32 arg1_32 = 0;
    uint32 arg2_32 = 0;
    uint32 arg3_32 = 0;
    uint32 rip32 = 0;
    if (isr_amd64_u32_from_uintptr((uintptr)frame->syscall_no, &syscall_no32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->arg1, &arg1_32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->arg2, &arg2_32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->arg3, &arg3_32) != 0 ||
        isr_amd64_u32_from_uintptr((uintptr)frame->rip, &rip32) != 0) {
        return (uint64)-1;
    }

    synthetic_regs.eax = syscall_no32;
    synthetic_regs.ebx = arg1_32;
    synthetic_regs.ecx = arg2_32;
    synthetic_regs.edx = arg3_32;
    synthetic_regs.eip = rip32;
    synthetic_regs.cs = (uint32)frame->cs;
    synthetic_regs.eflags = (uint32)frame->rflags;

    return (uint64)syscall_dispatch_core(&synthetic_regs,
                                         syscall_no32,
                                         (uintptr)frame->arg1,
                                         (uintptr)frame->arg2,
                                         (uintptr)frame->arg3,
                                         (uintptr)frame->arg4,
                                         (uintptr)frame->arg5);
}
#endif

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
#define SYSCALL_GUI_BEGIN 10
#define SYSCALL_GUI_CLEAR 11
#define SYSCALL_GUI_FILL_RECT 12
#define SYSCALL_GUI_DRAW_TEXT 13
#define SYSCALL_GUI_PRESENT 14
#define SYSCALL_GUI_POLL_EVENT 15
#define SYSCALL_GUI_WAIT_EVENT 16
#define SYSCALL_GUI_ATTACH 17
#define SYSCALL_GUI_DRAW_LINE 18
#define SYSCALL_GUI_GET_CONTENT_SIZE 19
#define SYSCALL_GUI_SET_FONT 20

// Write an entire file (create/overwrite) from ring3.
// args: (const char* path, const void* buf, int len)
#define SYSCALL_WRITEFILE 21

// Filesystem mutation helpers from ring3.
// args: (const char* path)
#define SYSCALL_MKDIR 49
#define SYSCALL_UNLINK 50
#define SYSCALL_RMDIR 51
#define SYSCALL_GETCWD 52
#define SYSCALL_EYNFS_STREAM_BEGIN 53
#define SYSCALL_EYNFS_STREAM_WRITE 54
#define SYSCALL_EYNFS_STREAM_END 55
#define SYSCALL_DRIVE_SET_LOGICAL 56
#define SYSCALL_DRIVE_GET_LOGICAL 57
#define SYSCALL_DRIVE_GET_COUNT 58
#define SYSCALL_DRIVE_IS_PRESENT 59
#define SYSCALL_INIT_SERVICES 60
#define SYSCALL_SERIAL_WRITE_COM1 61
#define SYSCALL_SHELL_LOG_SET 62
#define SYSCALL_SHELL_LOG_GET 63
#define SYSCALL_CRASHLOG_COUNT 64
#define SYSCALL_CRASHLOG_INFO 65
#define SYSCALL_CRASHLOG_DATA 66
#define SYSCALL_CRASHLOG_CLEAR 67
#define SYSCALL_SHELL_MIGRATED_DISPATCH 68
#define SYSCALL_PCI_GET_COUNT 69
#define SYSCALL_PCI_GET_ENTRY 70
#define SYSCALL_E1000_PROBE 71
#define SYSCALL_E1000_INIT 72
#define SYSCALL_NETCFG_GET 73
#define SYSCALL_NETCFG_SET 74
#define SYSCALL_NETCFG_DEFAULTS 75
#define SYSCALL_NET_IS_INITED 76
#define SYSCALL_NET_GET_MAC 77
#define SYSCALL_NET_GET_ARP 78
#define SYSCALL_NET_GET_UDP_STATS 79
#define SYSCALL_NET_GET_IP_STATS 80
#define SYSCALL_NET_GET_SOCKETS 81
#define SYSCALL_NET_PING 82
#define SYSCALL_FS_CHECK_INTEGRITY 83
#define SYSCALL_FS_FATFIX 84
#define SYSCALL_VTERM_CLEAR 85
#define SYSCALL_HISTORY_COUNT 86
#define SYSCALL_HISTORY_ENTRY 87
#define SYSCALL_HISTORY_CLEAR 88
#define SYSCALL_BG_JOB_COUNT 89
#define SYSCALL_BG_JOB_INFO 90
#define SYSCALL_TILING_START 91
#define SYSCALL_SETBG_PATH 92
#define SYSCALL_CLEARBG_FOCUSED 93
#define SYSCALL_SETFONT_PATH 94
#define SYSCALL_CHDIR 95
#define SYSCALL_RUN 96
#define SYSCALL_MMAP 98
#define SYSCALL_MUNMAP 99
#define SYSCALL_MSYNC 100
#define SYSCALL_PAGING_GUARDS 101
#define SYSCALL_PANIC 102
#define SYSCALL_PF 103
#define SYSCALL_RING3 104

// GUI icon drawing (kernel-side icon cache)
#define SYSCALL_GUI_DRAW_ICON 105

// GUI outlined rectangle (border only, no fill)
#define SYSCALL_GUI_OUTLINE_RECT 106

// GUI single-character draw (faster than draw_text for char-at-a-time rendering)
#define SYSCALL_GUI_DRAW_CHAR 107

// GUI font metrics query (returns cell width and height)
#define SYSCALL_GUI_GET_FONT_METRICS 108

/*
 * SYSCALL_GET_TICKS_MS (109): Return milliseconds elapsed since kernel boot.
 *
 * Why: Needed by time-sensitive userland programs (e.g. game engines) that
 *      require a monotonic millisecond clock without needing a real-time source.
 * Invariant: Derived from the scheduler's PIT tick counter and the configured
 *            Hz rate.  Resolution is 1000/hz ms (10ms at the default 100Hz).
 * Breakage if changed: Any program using I_GetTime() or gettimeofday() via the
 *                      libc shim will get incorrect timing.
 * ABI-sensitive: Yes (number 109 is locked once published).
 * Security-critical: No (timing info is not privileged on bare-metal).
 */
#define SYSCALL_GET_TICKS_MS 109

/*
 * SYSCALL_LSEEK (110): Reposition the read/write offset of an open FD.
 *
 * Why: fseek/ftell/rewind in the C library require repositioning the kernel-
 *      side file offset.  Without this, programs that seek within WAD/data
 *      files (e.g. DOOM) cannot read non-sequential data.
 * Invariant: whence encoding is POSIX: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
 *            The offset stored in user_fd_t.offset is the authoritative
 *            read position used by SYSCALL_READ.
 * Breakage if changed: Breaks fseek/ftell and any program relying on
 *                      seekable file I/O.
 * ABI-sensitive: Yes (number 110 is locked once published).
 */
#define SYSCALL_LSEEK 110

/*
 * SYSCALL_GUI_WARP_MOUSE (111): Teleport the physical mouse cursor to a
 * window-content-relative position.
 *
 * Why: Game ports (e.g. DOOM) need to re-centre the cursor each tic so
 *      that there is always room for the mouse to move in every direction.
 *      Without this, the cursor hits screen borders and generates zero
 *      deltas, making mouse-look unusable.
 * Invariant: The position is clamped to the same mouse_min/max_x/y that
 *            the PS/2 driver applies to hardware packets.  The accumulated
 *            delta registers are zeroed after the warp so the first real
 *            motion event after a warp reports only post-warp movement.
 * Breakage if changed:
 *   Any user program that calls gui_warp_mouse would mis-position the cursor.
 * ABI-sensitive: Yes (number 111 is locked once published).
 * Security: Capability check matches the existing mouse-read path.
 */
#define SYSCALL_GUI_WARP_MOUSE 111

/*
 * SYSCALL_GUI_SET_CURSOR_VISIBLE (112): Show or hide the mouse cursor sprite.
 *
 * Why: Games that grab the mouse (via gui_warp_mouse) should hide the
 *      cursor so the REI sprite does not jitter at the warp centre.
 * Invariant: The visibility flag is kernel-global.  Hiding the cursor
 *            suppresses the REI overlay in both the tiling manager
 *            main loop and the ring-3 render-once path.
 * Breakage if changed:
 *   User programs relying on cursor visibility control would break.
 * ABI-sensitive: Yes (number 112 is locked once published).
 * Security: Same capability requirements as gui_warp_mouse.
 */
#define SYSCALL_GUI_SET_CURSOR_VISIBLE 112
#define SYSCALL_GUI_LOAD_FONT 137
#define SYSCALL_GUI_DRAW_TEXT_FONT 138
#define SYSCALL_GUI_DRAW_CHAR_FONT 139
#define SYSCALL_SET_DISPLAY_PROFILE 140
#define SYSCALL_GET_DISPLAY_PROFILE 141
#define SYSCALL_SET_DISPLAY_MODE 142
#define SYSCALL_GET_DISPLAY_MODE 143

/*
 * SYSCALL_AUDIO_PROBE (113): Detect whether an AC97 audio controller is
 * present on the PCI bus.
 *
 * args: none
 * returns: 0 if found, -1 if absent.
 * ABI-sensitive: Yes (number 113 is locked once published).
 */
#define SYSCALL_AUDIO_PROBE 113

/*
 * SYSCALL_AUDIO_INIT (114): Initialise the AC97 audio controller.
 *
 * Allocates DMA buffers, programs the codec, and registers the IRQ handler.
 * Must be called after a successful AUDIO_PROBE.
 * args: none
 * returns: 0 on success, -1 on failure.
 * ABI-sensitive: Yes (number 114 is locked once published).
 */
#define SYSCALL_AUDIO_INIT 114

/*
 * SYSCALL_AUDIO_WRITE (115): Submit a buffer of PCM data for playback.
 *
 * args: (const void* buf, int size_bytes)
 *   buf:  pointer to 16-bit signed LE stereo PCM at 48 kHz.
 *   size: byte length, clamped to AC97_DMA_BUF_SIZE (4096).
 * returns: 0 on success, -1 on error.
 * ABI-sensitive: Yes (number 115 is locked once published).
 * SECURITY-INVARIANT: buf is validated and copied from user space.
 */
#define SYSCALL_AUDIO_WRITE 115

/*
 * SYSCALL_AUDIO_STOP (116): Stop audio playback and silence the output.
 *
 * args: none
 * returns: 0 always.
 * ABI-sensitive: Yes (number 116 is locked once published).
 */
#define SYSCALL_AUDIO_STOP 116

/*
 * SYSCALL_AUDIO_IS_AVAILABLE (117): Query whether audio is initialised.
 *
 * args: none
 * returns: 1 if audio is available, 0 if not.
 * ABI-sensitive: Yes (number 117 is locked once published).
 */
#define SYSCALL_AUDIO_IS_AVAILABLE 117

/*
 * SYSCALL_AUDIO_WRITE_BULK (118): Submit a large PCM buffer for playback.
 *
 * The kernel splits the user buffer into AC97_DMA_BUF_SIZE (4 KB) chunks
 * and queues as many as the ring can accept in one syscall, then restarts
 * DMA at most once.  This reduces per-buffer syscall overhead from N
 * round-trips to 1.
 *
 * args: (const void* buf, int total_size_bytes)
 *   buf:        pointer to 16-bit signed LE stereo PCM at 48 kHz.
 *   total_size: total byte length (capped to 32 768 = 8 DMA buffers).
 * returns: number of 4 KB chunks queued (>= 0), or -1 on error.
 *          0 means the ring was already full.
 * ABI-sensitive: Yes (number 118 is locked once published).
 * SECURITY-INVARIANT: buf is validated and copied per-chunk from user space.
 */
#define SYSCALL_AUDIO_WRITE_BULK 118

/*
 * ABI-INVARIANT: Networking syscalls for DNS + TCP (119-123).
 *
 * Why: Provide userland access to DNS resolution and minimal TCP streams.
 * Invariant: Numbers are locked once published; userland headers must match.
 * Breakage if changed: User binaries will invoke the wrong syscalls.
 * ABI-sensitive: Yes.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (controls network access surface).
 */
#define SYSCALL_NET_DNS_RESOLVE 119
#define SYSCALL_NET_TCP_CONNECT 120
#define SYSCALL_NET_TCP_SEND 121
#define SYSCALL_NET_TCP_RECV 122
#define SYSCALL_NET_TCP_CLOSE 123

/* IPC primitives */
#define SYSCALL_PIPE 124
#define SYSCALL_MKFIFO 125
#define SYSCALL_DUP 126
#define SYSCALL_DUP2 127
#define SYSCALL_FD_SET_INHERIT 128
#define SYSCALL_FD_SET_STDIO 129
#define SYSCALL_FD_SET_NONBLOCK 130
#define SYSCALL_SPAWN 131
#define SYSCALL_WAITPID 132
#define SYSCALL_INSTALLER_PREPARE_DRIVE 133
#define SYSCALL_INSTALLER_FORMAT_EYNFS_PARTITION 134
#define SYSCALL_INSTALLER_WRITE_SECTOR 135
#define SYSCALL_INSTALLER_GET_PARTITIONS 136

#define FIFO_METADATA_MARKER "EYNFIFO1\n"
#define FIFO_METADATA_MARKER_LEN 9

/* open(2) flag bits used by SYSCALL_OPEN for FIFO endpoint selection. */
#define OPEN_FLAG_WRONLY 0x0001
#define OPEN_FLAG_RDWR 0x0002
#define OPEN_FLAG_NONBLOCK 0x0800

/* Bounded spill capacity used by pipeline spool fallback when pipe ring fills. */
#define USER_PIPE_SPOOL_CAP_BYTES 16384

// Cooperative scheduling from userland
#define SYSCALL_SLEEP_US 22

// GUI continuous redraw control
#define SYSCALL_GUI_SET_CONTINUOUS_REDRAW 23

// GUI RGB565 blit (userland framebuffer)
#define SYSCALL_GUI_BLIT_RGB565 24

// Capability-based file descriptor operations
#define SYSCALL_CAP_MINT_FD 25
#define SYSCALL_CAP_FD_READ 26
#define SYSCALL_CAP_FD_CLOSE 27

// Capability-based GUI operations
#define SYSCALL_CAP_MINT_GUI 28
#define SYSCALL_CAP_GUI_BEGIN 29
#define SYSCALL_CAP_GUI_CLEAR 30
#define SYSCALL_CAP_GUI_FILL_RECT 31
#define SYSCALL_CAP_GUI_DRAW_TEXT 32
#define SYSCALL_CAP_GUI_PRESENT 33
#define SYSCALL_CAP_GUI_POLL_EVENT 34
#define SYSCALL_CAP_GUI_WAIT_EVENT 35
#define SYSCALL_CAP_GUI_DRAW_LINE 36
#define SYSCALL_CAP_GUI_GET_CONTENT_SIZE 37
#define SYSCALL_CAP_GUI_SET_TITLE 38
#define SYSCALL_CAP_GUI_SET_FONT 39
#define SYSCALL_CAP_GUI_SET_CONTINUOUS_REDRAW 40
#define SYSCALL_CAP_GUI_BLIT_RGB565 41
#define SYSCALL_CAP_GUI_CLOSE 42

// Capability-based file descriptor write/seek operations
#define SYSCALL_CAP_FD_WRITE 43
#define SYSCALL_CAP_FD_SEEK 44

// Capability-based GUI creation/attach (returns caps directly)
#define SYSCALL_CAP_GUI_CREATE 45
#define SYSCALL_CAP_GUI_ATTACH 46

// Deterministic execution mode
#define SYSCALL_DET_ENABLE 47
#define SYSCALL_DET_STEP 48

static int syscall_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void syscall_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

typedef struct {
    uint32 type;
    int32 a;
    int32 b;
    int32 c;
    int32 d;
} user_gui_event_t;

enum {
    USER_GUI_EVENT_NONE = 0,
    USER_GUI_EVENT_KEY = 1,
    USER_GUI_EVENT_MOUSE = 2,
    USER_GUI_EVENT_CLOSE = 3,
    USER_GUI_EVENT_KEY_UP = 4,
};

typedef struct {
    uint32 type;
    // Common
    int x;
    int y;
    int w;
    int h;
    int x2;
    int y2;
    uint8 r;
    uint8 g;
    uint8 b;
    int font_slot;
    char text[64];
} user_gui_cmd_t;

typedef struct {
    int fb_w;
    int fb_h;
    int workspace_w;
    int workspace_h;
    int scale_pct;
    int aspect_mode;
} syscall_display_profile_t;

typedef struct {
    int width;
    int height;
    int bpp;
    int persist;
} syscall_display_mode_set_t;

typedef struct {
    int width;
    int height;
    int bpp;
    int can_switch;
} syscall_display_mode_t;

/*
 * SECURITY-INVARIANT: Per-GUI command buffer slot count.
 *
 * Why: Bounds kernel memory per user GUI while allowing text-heavy clients
 * (notably syntax-highlighting editors) to emit a full frame without dropping
 * draw commands.
 * Invariant: cmd_count must stay within [0, USER_GUI_CMD_MAX].
 * Breakage if changed:
 *   - Increasing raises per-handle kernel memory linearly.
 *   - Decreasing can reintroduce silent draw drops/flicker in rich UIs.
 * ABI-sensitive: Yes (user-visible per-frame draw throughput ceiling).
 * Security-critical: Yes (resource exhaustion boundary).
 */
#define USER_GUI_CMD_MAX 512

enum {
    USER_GUI_CMD_CLEAR = 1,
    USER_GUI_CMD_FILL_RECT = 2,
    USER_GUI_CMD_TEXT = 3,
    USER_GUI_CMD_LINE = 4,
    USER_GUI_CMD_ICON = 5,
    USER_GUI_CMD_OUTLINE_RECT = 6,
    USER_GUI_CMD_CHAR = 7,
};

typedef enum {
    USER_FD_KIND_FILE = 0,
    USER_FD_KIND_PIPE = 1,
} user_fd_kind_t;

typedef enum {
    USER_PIPE_END_READ = 0,
    USER_PIPE_END_WRITE = 1,
} user_pipe_end_t;

/*
 * SECURITY-INVARIANT: Maximum concurrent kernel IPC pipe objects.
 *
 * Why: Binds static kernel memory for pipe buffers and metadata on a low-RAM
 * target. Pipe objects are shared by anonymous pipes and named FIFOs.
 * Breakage if changed:
 *   - Increasing grows kernel .bss linearly by USER_PIPE_BUFFER_BYTES.
 *   - Decreasing reduces IPC concurrency and can make pipe()/mkfifo() fail.
 * ABI-sensitive: No (kernel-internal capacity).
 * Security-critical: Yes (resource exhaustion boundary).
 */
#define USER_PIPE_MAX 24

/*
 * SECURITY-INVARIANT: Per-pipe ring buffer payload capacity.
 *
 * Why: Keeps memory bounded while allowing shell pipelines and short control
 * messages to stream between processes without heap churn.
 * Breakage if changed:
 *   - Increasing raises static memory use by USER_PIPE_MAX * delta.
 *   - Decreasing increases short-write behavior under bursty producers.
 * ABI-sensitive: Behavioral (write/read return lengths), but not numeric ABI.
 * Security-critical: Yes (prevents unbounded kernel buffering).
 */
#define USER_PIPE_BUFFER_BYTES 4096

/*
 * ABI-INVARIANT: Ticket donation amount for blocked pipe endpoints.
 *
 * Why: Improves pipeline responsiveness by temporarily boosting the likely
 * unblocking peer when read/write must wait.
 * Invariant: Donation is transient and cleared when wait condition resolves.
 * ABI-sensitive: Behavioral (scheduler latency), not numeric syscall ABI.
 */
#define USER_TASK_PIPE_DONATION_TICKETS 24u

typedef struct {
    int used;
    int persistent_fifo;
    int spool_enabled;
    int reader_refs;
    int writer_refs;
    uint32 read_pos;
    uint32 write_pos;
    uint32 bytes_used;
    uint8* spool_buf;
    uint32 spool_cap;
    uint32 spool_read_pos;
    uint32 spool_write_pos;
    uint32 spool_bytes;
    int last_reader_pid;
    int last_writer_pid;
    char fifo_path[128];
    uint8 buffer[USER_PIPE_BUFFER_BYTES];
} user_pipe_t;

typedef struct {
    int used;
    int is_dir;
    int nonblock;
    int kind;
    int pipe_id;
    int pipe_end;
    uint32 offset;
    uint32 dir_pos;
    uint32 size;
    uint8 drive;
    char path[128];

    /*
     * EYNFS streaming cursor -- avoids re-traversing the full block chain from
     * first_block on every read call.  O(28 000) block reads for a 14 MB WAD
     * drops to O(1) for sequential reads once the cursor is primed.
     *
     * eynfs_first_block  -- first_block from the dir entry, populated at open
     *                      time.  0 = not an EYNFS file (FAT32 or directory);
     *                      fall back to vfs_read_file_at() in that case.
     * cur_block          -- EYNFS block number where the cursor currently sits.
     *                      0 = not yet initialised; eynfs_read_file_fast()
     *                      will restart from eynfs_first_block.
     * cur_block_off      -- file byte offset at the START of cur_block's data
     *                      payload (i.e. 4 bytes past the chain pointer).
     *
     * On any backwards seek that takes offset below cur_block_off, the fast
     * reader automatically falls back to traversal from first_block and the
     * cursor is updated to the new position.
     */
    uint32 eynfs_first_block;
    uint32 cur_block;
    uint32 cur_block_off;

    /*
     * Per-FD reusable kernel read buffer.
     *
     * Why: syscall_read_file previously called malloc(maxlen)+free on every
     *      read call.  For REIV video frames (~40-80 KB each) decoded at
     *      30 fps, that is 30 malloc/free cycles per second through the
     *      kernel heap allocator.  malloc/free itself is fast, but every
     *      free() triggers merge_free_blocks() which scans the heap.  A
     *      reusable per-FD buffer eliminates this churn entirely.
     *
     * Invariant: kbuf is NULL when kbuf_size is 0.  kbuf_size reflects
     *            the allocated capacity in bytes.  The buffer is only grown
     *            (reallocated), never shrunk, until close.
     * Lifetime: allocated on first read of a given FD; freed on close.
     * Breakage if removed: reverts to per-read heap churn.
     * ABI-sensitive: No.  Kernel-internal only.
     */
    uint8* kbuf;
    uint32 kbuf_size;
} user_fd_t;

#define USER_FD_MAX 16
static user_fd_t g_user_fds[USER_FD_MAX];
static user_pipe_t g_user_pipes[USER_PIPE_MAX];

static int g_user_fd_inherit_mode = 0;
static int g_user_stdio_stdin_fd = 0;
static int g_user_stdio_stdout_fd = 1;
static int g_user_stdio_stderr_fd = 2;

/*
 * SECURITY-INVARIANT: Maximum number of concurrent ring3 EYNFS stream writers.
 *
 * Why: Bounds per-task kernel memory usage and prevents stream-handle exhaustion.
 * Invariant: Each active stream reserves an `eynfs_stream_t` and can create/overwrite
 *           a file on disk. Handles are small integers in [0, USER_STREAM_MAX).
 * Breakage if changed:
 *   - Increasing: increases kernel .bss footprint (low-RAM boot sensitive).
 *   - Decreasing: reduces concurrency for user utilities (copy/move).
 * ABI-sensitive: Yes (user-visible via failure to begin additional streams).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (resource exhaustion boundary).
 */
#define USER_STREAM_MAX 4

typedef struct {
    int used;
    eynfs_stream_t s;
} user_stream_t;

static user_stream_t g_user_streams[USER_STREAM_MAX];

void syscall_reset_user_streams(void) {
    for (int i = 0; i < USER_STREAM_MAX; ++i) {
        g_user_streams[i].used = 0;
        memset(&g_user_streams[i].s, 0, sizeof(g_user_streams[i].s));
    }
}

static user_stream_t* user_stream_get(int handle) {
    if (handle < 0 || handle >= USER_STREAM_MAX) return NULL;
    if (!g_user_streams[handle].used) return NULL;
    return &g_user_streams[handle];
}

static void user_pipe_maybe_destroy(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= USER_PIPE_MAX) return;
    user_pipe_t* p = &g_user_pipes[pipe_id];
    if (!p->used) return;
    if (p->persistent_fifo) {
        if (p->reader_refs == 0 && p->writer_refs == 0) {
            p->bytes_used = 0;
            p->read_pos = 0;
            p->write_pos = 0;
            p->spool_read_pos = 0;
            p->spool_write_pos = 0;
            p->spool_bytes = 0;
            p->last_reader_pid = 0;
            p->last_writer_pid = 0;
        }
        return;
    }
    if (p->reader_refs == 0 && p->writer_refs == 0) {
        if (p->spool_buf) {
            free(p->spool_buf);
            p->spool_buf = NULL;
        }
        memset(p, 0, sizeof(*p));
    }
}

static int user_pipe_alloc(int persistent_fifo, const char* fifo_path) {
    for (int i = 0; i < USER_PIPE_MAX; ++i) {
        if (g_user_pipes[i].used) continue;
        memset(&g_user_pipes[i], 0, sizeof(g_user_pipes[i]));
        g_user_pipes[i].used = 1;
        g_user_pipes[i].persistent_fifo = persistent_fifo ? 1 : 0;
        if (persistent_fifo && fifo_path) {
            strncpy(g_user_pipes[i].fifo_path, fifo_path, sizeof(g_user_pipes[i].fifo_path) - 1);
            g_user_pipes[i].fifo_path[sizeof(g_user_pipes[i].fifo_path) - 1] = '\0';
        }
        return i;
    }
    return -1;
}

static int user_pipe_find_fifo_by_path(const char* path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < USER_PIPE_MAX; ++i) {
        if (!g_user_pipes[i].used || !g_user_pipes[i].persistent_fifo) continue;
        if (strcmp(g_user_pipes[i].fifo_path, path) == 0) return i;
    }
    return -1;
}

static int user_pipe_set_spool(int pipe_id, int enabled) {
    if (pipe_id < 0 || pipe_id >= USER_PIPE_MAX) return -1;
    user_pipe_t* p = &g_user_pipes[pipe_id];
    if (!p->used) return -1;

    if (!enabled) {
        p->spool_enabled = 0;
        p->spool_read_pos = 0;
        p->spool_write_pos = 0;
        p->spool_bytes = 0;
        if (p->spool_buf) {
            free(p->spool_buf);
            p->spool_buf = NULL;
        }
        p->spool_cap = 0;
        return 0;
    }

    if (!p->spool_buf) {
        p->spool_buf = (uint8*)malloc(USER_PIPE_SPOOL_CAP_BYTES);
        if (!p->spool_buf) return -1;
        p->spool_cap = USER_PIPE_SPOOL_CAP_BYTES;
        p->spool_read_pos = 0;
        p->spool_write_pos = 0;
        p->spool_bytes = 0;
    }
    p->spool_enabled = 1;
    return 0;
}

static int user_pipe_read(int pipe_id, void* user_dst, int maxlen, int nonblock) {
    if (pipe_id < 0 || pipe_id >= USER_PIPE_MAX || maxlen < 0 || !user_dst) return -1;
    user_pipe_t* p = &g_user_pipes[pipe_id];
    if (!p->used) return -1;
    if (maxlen == 0) return 0;

    int donated = 0;
    int running_pid = user_task_get_running_pid();
    if (running_pid > 0) {
        p->last_reader_pid = running_pid;
    }

    while (p->bytes_used == 0 && p->spool_bytes == 0) {
        if (p->writer_refs == 0) {
            if (donated) user_task_clear_running_donation();
            // POSIX-style EOF: no writers and no buffered bytes.
            return 0;
        }
        if (nonblock) {
            if (donated) user_task_clear_running_donation();
            return -1;
        }
        if (g_user_interrupt) {
            if (donated) user_task_clear_running_donation();
            return -1;
        }
        if (running_pid > 0 && !donated && p->last_writer_pid > 0 && p->last_writer_pid != running_pid) {
            if (user_task_donate_running_to_pid(p->last_writer_pid, (uint16)USER_TASK_PIPE_DONATION_TICKETS) == 0) {
                donated = 1;
            }
        }
        watchdog_kick("pipe-read");
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");
    }

    if (donated) user_task_clear_running_donation();

    int n = 0;
    while (n < maxlen && p->bytes_used > 0) {
        uint8 b = p->buffer[p->read_pos];
        if (copyout((uint8*)user_dst + n, &b, 1) != 0) return -1;
        p->read_pos = (p->read_pos + 1u) % USER_PIPE_BUFFER_BYTES;
        p->bytes_used--;
        n++;
    }

    while (n < maxlen && p->spool_bytes > 0) {
        uint8 b = p->spool_buf[p->spool_read_pos];
        if (copyout((uint8*)user_dst + n, &b, 1) != 0) return -1;
        p->spool_read_pos = (p->spool_read_pos + 1u) % p->spool_cap;
        p->spool_bytes--;
        n++;
    }
    return n;
}

static int user_pipe_write(int pipe_id, const void* user_src, int len, int nonblock) {
    if (pipe_id < 0 || pipe_id >= USER_PIPE_MAX || len < 0 || !user_src) return -1;
    user_pipe_t* p = &g_user_pipes[pipe_id];
    if (!p->used) return -1;
    if (len == 0) return 0;

    int donated = 0;
    int running_pid = user_task_get_running_pid();
    if (running_pid > 0) {
        p->last_writer_pid = running_pid;
    }

    if (p->reader_refs == 0) {
        if (donated) user_task_clear_running_donation();
        // POSIX-like broken pipe behavior: no readers present.
        return -1;
    }

    int n = 0;
    while (n < len) {
        while (p->bytes_used >= USER_PIPE_BUFFER_BYTES && (!p->spool_enabled || p->spool_bytes >= p->spool_cap)) {
            if (p->reader_refs == 0) {
                if (donated) user_task_clear_running_donation();
                return (n > 0) ? n : -1;
            }
            if (nonblock) {
                if (donated) user_task_clear_running_donation();
                return (n > 0) ? n : -1;
            }
            if (g_user_interrupt) {
                if (donated) user_task_clear_running_donation();
                return (n > 0) ? n : -1;
            }
            if (running_pid > 0 && !donated && p->last_reader_pid > 0 && p->last_reader_pid != running_pid) {
                if (user_task_donate_running_to_pid(p->last_reader_pid, (uint16)USER_TASK_PIPE_DONATION_TICKETS) == 0) {
                    donated = 1;
                }
            }
            watchdog_kick("pipe-write");
            __asm__ __volatile__("sti");
            __asm__ __volatile__("hlt");
        }

        if (donated) {
            user_task_clear_running_donation();
            donated = 0;
        }

        uint8 b = 0;
        if (copyin(&b, (const uint8*)user_src + n, 1) != 0) return -1;
        if (p->bytes_used < USER_PIPE_BUFFER_BYTES) {
            p->buffer[p->write_pos] = b;
            p->write_pos = (p->write_pos + 1u) % USER_PIPE_BUFFER_BYTES;
            p->bytes_used++;
        } else if (p->spool_enabled && p->spool_buf && p->spool_bytes < p->spool_cap) {
            p->spool_buf[p->spool_write_pos] = b;
            p->spool_write_pos = (p->spool_write_pos + 1u) % p->spool_cap;
            p->spool_bytes++;
        } else {
            if (nonblock) return (n > 0) ? n : -1;
            continue;
        }
        n++;
    }

    if (donated) user_task_clear_running_donation();
    return n;
}

static int user_fd_release(int fd) {
    if (fd < 0 || fd >= USER_FD_MAX) return -1;
    user_fd_t* ufd = &g_user_fds[fd];
    if (!ufd->used) return 0;

    cap_revoke_object(ufd, CAP_OBJ_USER_FD);
    if (ufd->kind == USER_FD_KIND_PIPE && ufd->pipe_id >= 0 && ufd->pipe_id < USER_PIPE_MAX) {
        user_pipe_t* p = &g_user_pipes[ufd->pipe_id];
        if (p->used) {
            if (ufd->pipe_end == USER_PIPE_END_READ) {
                if (p->reader_refs > 0) p->reader_refs--;
            } else if (ufd->pipe_end == USER_PIPE_END_WRITE) {
                if (p->writer_refs > 0) p->writer_refs--;
            }
            user_pipe_maybe_destroy(ufd->pipe_id);
        }
    }

    if (ufd->kbuf) {
        free(ufd->kbuf);
        ufd->kbuf = NULL;
    }
    ufd->kbuf_size = 0;
    ufd->used = 0;
    ufd->is_dir = 0;
    ufd->nonblock = 0;
    ufd->kind = USER_FD_KIND_FILE;
    ufd->pipe_id = -1;
    ufd->pipe_end = -1;
    ufd->offset = 0;
    ufd->dir_pos = 0;
    ufd->size = 0;
    ufd->drive = 0;
    ufd->path[0] = '\0';
    ufd->eynfs_first_block = 0;
    ufd->cur_block = 0;
    ufd->cur_block_off = 0;
    return 0;
}

static int user_fd_duplicate_into(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= USER_FD_MAX || newfd < 0 || newfd >= USER_FD_MAX) return -1;
    if (!g_user_fds[oldfd].used) return -1;
    if (newfd <= 2) return -1;

    if (oldfd == newfd) return newfd;
    if (g_user_fds[newfd].used) {
        if (user_fd_release(newfd) != 0) return -1;
    }

    user_fd_t* src = &g_user_fds[oldfd];
    user_fd_t* dst = &g_user_fds[newfd];
    memset(dst, 0, sizeof(*dst));

    dst->used = 1;
    dst->is_dir = src->is_dir;
    dst->nonblock = src->nonblock;
    dst->kind = src->kind;
    dst->pipe_id = src->pipe_id;
    dst->pipe_end = src->pipe_end;
    dst->offset = src->offset;
    dst->dir_pos = src->dir_pos;
    dst->size = src->size;
    dst->drive = src->drive;
    dst->eynfs_first_block = src->eynfs_first_block;
    dst->cur_block = src->cur_block;
    dst->cur_block_off = src->cur_block_off;
    dst->kbuf = NULL;
    dst->kbuf_size = 0;
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    dst->path[sizeof(dst->path) - 1] = '\0';

    if (dst->kind == USER_FD_KIND_PIPE && dst->pipe_id >= 0 && dst->pipe_id < USER_PIPE_MAX) {
        user_pipe_t* p = &g_user_pipes[dst->pipe_id];
        if (!p->used) {
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
        if (dst->pipe_end == USER_PIPE_END_READ) p->reader_refs++;
        else if (dst->pipe_end == USER_PIPE_END_WRITE) p->writer_refs++;
    }

    return newfd;
}

static int user_pipe_get_or_create_fifo(const char* path) {
    int fifo_id = user_pipe_find_fifo_by_path(path);
    if (fifo_id >= 0) return fifo_id;

    vfs_stat_t st;
    if (vfs_stat(get_current_physical_drive(), path, &st) != 0 || st.type != VFS_NODE_FIFO) {
        return -1;
    }

    return user_pipe_alloc(1, path);
}

void syscall_set_user_fd_inherit_mode(int enabled) {
    g_user_fd_inherit_mode = enabled ? 1 : 0;
}

int syscall_get_user_fd_inherit_mode(void) {
    return g_user_fd_inherit_mode;
}

void syscall_set_user_stdio_fds(int stdin_fd, int stdout_fd, int stderr_fd) {
    g_user_stdio_stdin_fd = stdin_fd;
    g_user_stdio_stdout_fd = stdout_fd;
    g_user_stdio_stderr_fd = stderr_fd;
}

void syscall_reset_user_stdio_fds(void) {
    g_user_stdio_stdin_fd = 0;
    g_user_stdio_stdout_fd = 1;
    g_user_stdio_stderr_fd = 2;
}

int syscall_kernel_close_user_fd(int fd) {
    if (fd <= 2) return 0;
    return user_fd_release(fd);
}

int syscall_kernel_set_user_fd_nonblock(int fd, int enabled) {
    if (fd < 0 || fd >= USER_FD_MAX) return -1;
    if (fd <= 2) return -1;
    if (!g_user_fds[fd].used) return -1;
    g_user_fds[fd].nonblock = enabled ? 1 : 0;
    return 0;
}

int syscall_kernel_set_user_pipe_spool(int fd, int enabled) {
    if (fd < 0 || fd >= USER_FD_MAX) return -1;
    if (fd <= 2) return -1;
    if (!g_user_fds[fd].used) return -1;
    if (g_user_fds[fd].kind != USER_FD_KIND_PIPE) return -1;
    return user_pipe_set_spool(g_user_fds[fd].pipe_id, enabled);
}

int syscall_kernel_pipe_create(int* out_read_fd, int* out_write_fd) {
    if (!out_read_fd || !out_write_fd) return -1;
    int pipe_id = user_pipe_alloc(0, NULL);
    if (pipe_id < 0) return -1;

    int read_fd = -1;
    int write_fd = -1;
    for (int i = 3; i < USER_FD_MAX; ++i) {
        if (!g_user_fds[i].used) { read_fd = i; break; }
    }
    if (read_fd < 0) {
        memset(&g_user_pipes[pipe_id], 0, sizeof(g_user_pipes[pipe_id]));
        return -1;
    }
    for (int i = read_fd + 1; i < USER_FD_MAX; ++i) {
        if (!g_user_fds[i].used) { write_fd = i; break; }
    }
    if (write_fd < 0) {
        memset(&g_user_pipes[pipe_id], 0, sizeof(g_user_pipes[pipe_id]));
        return -1;
    }

    memset(&g_user_fds[read_fd], 0, sizeof(g_user_fds[read_fd]));
    g_user_fds[read_fd].used = 1;
    g_user_fds[read_fd].kind = USER_FD_KIND_PIPE;
    g_user_fds[read_fd].pipe_id = pipe_id;
    g_user_fds[read_fd].pipe_end = USER_PIPE_END_READ;

    memset(&g_user_fds[write_fd], 0, sizeof(g_user_fds[write_fd]));
    g_user_fds[write_fd].used = 1;
    g_user_fds[write_fd].kind = USER_FD_KIND_PIPE;
    g_user_fds[write_fd].pipe_id = pipe_id;
    g_user_fds[write_fd].pipe_end = USER_PIPE_END_WRITE;

    g_user_pipes[pipe_id].reader_refs = 1;
    g_user_pipes[pipe_id].writer_refs = 1;

    *out_read_fd = read_fd;
    *out_write_fd = write_fd;
    return 0;
}

void syscall_reset_user_fds(void) {
    for (int i = 0; i < USER_FD_MAX; ++i) {
        (void)user_fd_release(i);
    }
    for (int i = 0; i < USER_PIPE_MAX; ++i) {
        memset(&g_user_pipes[i], 0, sizeof(g_user_pipes[i]));
    }
}

typedef struct {
    int used;
    int tile_idx;
    /*
     * Floating-window fields.
     * When is_floating is non-zero, win_id holds the index returned by
     * wm_create_window() and tile_idx is unused (-1).  All GUI syscalls
     * branch on is_floating to dispatch to the correct subsystem.
     * ABI-INVARIANT: These fields are kernel-internal and not visible to ring3.
     */
    int win_id;
    int is_floating;
    char* title;
    char* status_left;

    /* Owning ring3 pid. Used to isolate GUI handles across concurrent tasks. */
    int owner_pid;

    /*
     * Non-zero when this entry is a "self attach" binding to the task's
     * existing shell tile (gui_attach handle 0 view).
     */
    int self_attached_tile;

    // Current font handle for this GUI context (0 = built-in fallback).
    int font_handle;

    /*
     * SECURITY-INVARIANT: Per-window additional GUI font slots.
     *
     * Why: Allows mixed-font rendering in one window while bounding kernel
     * memory and open-font references on low-RAM systems.
     * Invariant: Valid user slot ids are 1..USER_GUI_EXTRA_FONTS and map to
     *            extra_font_handles[slot-1]. Slot 0 means the window default.
     * Breakage if changed:
     *   - Increasing grows per-window retained font references and memory.
     *   - Decreasing can break user programs relying on higher slot ids.
     * ABI-sensitive: Yes (user-visible slot range and behavior).
     * Security-critical: Yes (resource-exhaustion boundary).
     */
#define USER_GUI_EXTRA_FONTS 8
    int extra_font_handles[USER_GUI_EXTRA_FONTS];

    // Immediate-mode command list. Draw callback replays this list.
    /*
     * SECURITY-INVARIANT: Per-GUI command buffer upper bound.
     *
     * Why: Bounds kernel memory per GUI handle. Each gui_fill_rect, gui_draw_text,
     *      gui_draw_line, or gui_clear consumes one slot.
     * Invariant: User programs that exceed this limit silently lose draw commands
     *            (syscall returns -1). Callers should minimise per-frame commands.
     * Breakage if changed:
    *   - Increasing: linearly grows per-user-GUI kernel memory (~96 bytes/slot).
    *     512 slots ≈ 48 KB -- acceptable on 9 MB target for current GUI load.
     *   - Decreasing: existing GUI programs may silently lose draw calls.
     * ABI-sensitive: Yes (user-visible limit on draw throughput per frame).
     */
    int cmd_count;
    user_gui_cmd_t cmds[USER_GUI_CMD_MAX];
    uint8 frame_pending;

    // Input event ring buffer.
    uint8 ev_head;
    uint8 ev_tail;
    user_gui_event_t ev[64];

    // Optional RGB565 blit buffer (userland-provided)
    uint16* blit_buf;
    int blit_w;
    int blit_h;
    int blit_dst_w;
    int blit_dst_h;
} user_gui_t;

#define USER_GUI_EVENT_CAP 64
#define USER_GUI_EVENT_MASK (USER_GUI_EVENT_CAP - 1)

static user_gui_t* user_gui_get(int handle);
static user_gui_t* user_gui_get_raw(int handle);

// handle 0 remains the public "self" handle in userland and is resolved per pid.
#define USER_GUI_MAX 8
static user_gui_t g_user_guis[USER_GUI_MAX];

static void user_gui_release_fonts(user_gui_t* e) {
    if (!e) return;
    if (e->font_handle > 0) {
        vga_font_release(e->font_handle);
        e->font_handle = 0;
    }
    for (int i = 0; i < USER_GUI_EXTRA_FONTS; ++i) {
        if (e->extra_font_handles[i] > 0) {
            vga_font_release(e->extra_font_handles[i]);
            e->extra_font_handles[i] = 0;
        }
    }
}

static int user_gui_font_handle_from_slot(const user_gui_t* e, int slot) {
    if (!e) return 0;
    if (slot <= 0) return e->font_handle;
    if (slot > USER_GUI_EXTRA_FONTS) return 0;
    return e->extra_font_handles[slot - 1];
}

static void user_gui_free_entry(user_gui_t* e) {
    if (!e) return;

    int self_attached = e->self_attached_tile;

    cap_revoke_object(e, CAP_OBJ_USER_GUI);

    user_gui_release_fonts(e);
    if (e->blit_buf) {
        free(e->blit_buf);
        e->blit_buf = NULL;
    }
    e->blit_w = 0;
    e->blit_h = 0;
    e->blit_dst_w = 0;
    e->blit_dst_h = 0;
    if (e->is_floating) {
        /* Floating window: unregister callbacks then close the window.
         * wm_close_window will skip its veto path because we already cleared
         * close_cb in wm_unregister_gui_client before reaching here. */
        if (e->win_id >= 0) {
            wm_unregister_gui_client(e->win_id);
            wm_close_window(e->win_id);
        }
    } else {
        if (e->tile_idx >= 0) {
            tile_unregister_gui_client(e->tile_idx);
            if (self_attached) {
                tile_set_title_status(e->tile_idx, "EYN-OS Shell", NULL, NULL);
                tile_invalidate_decorations(e->tile_idx);
                tile_invalidate_gui(e->tile_idx);
            } else {
                tile_close(e->tile_idx);
            }
        }
    }
    if (e->title) free(e->title);
    if (e->status_left) free(e->status_left);
    e->used = 0;
    e->tile_idx = -1;
    e->win_id = -1;
    e->is_floating = 0;
    e->title = NULL;
    e->status_left = NULL;
    e->owner_pid = 0;
    e->self_attached_tile = 0;

    e->cmd_count = 0;
    e->frame_pending = 0;
    e->ev_head = 0;
    e->ev_tail = 0;
}

static int user_gui_alloc_handle(int owner_pid, int self_attached_tile) {
    // start at 1; handle 0 reserved for "self"
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (!g_user_guis[i].used) {
            memset(&g_user_guis[i], 0, sizeof(g_user_guis[i]));
            g_user_guis[i].used = 1;
            g_user_guis[i].tile_idx = -1;
            g_user_guis[i].win_id = -1;
            g_user_guis[i].is_floating = 0;
            g_user_guis[i].title = NULL;
            g_user_guis[i].status_left = NULL;
            g_user_guis[i].blit_buf = NULL;
            g_user_guis[i].blit_w = 0;
            g_user_guis[i].blit_h = 0;
            g_user_guis[i].blit_dst_w = 0;
            g_user_guis[i].blit_dst_h = 0;
            g_user_guis[i].cmd_count = 0;
            g_user_guis[i].frame_pending = 0;
            g_user_guis[i].ev_head = 0;
            g_user_guis[i].ev_tail = 0;
            g_user_guis[i].owner_pid = owner_pid;
            g_user_guis[i].self_attached_tile = self_attached_tile ? 1 : 0;
            return i;
        }
    }
    return -1;
}

static int user_gui_find_self_handle_by_pid(int pid) {
    if (pid <= 0) return -1;
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (!g_user_guis[i].used) continue;
        if (!g_user_guis[i].self_attached_tile) continue;
        if (g_user_guis[i].owner_pid != pid) continue;
        return i;
    }
    return -1;
}

static user_gui_t* user_gui_get_raw(int handle) {
    if (handle < 0 || handle >= USER_GUI_MAX) return NULL;
    if (!g_user_guis[handle].used) return NULL;
    return &g_user_guis[handle];
}

static void user_gui_flush_pending_frame(user_gui_t* e) {
    if (!e || !e->frame_pending) return;
    if (e->is_floating) {
        if (e->win_id < 0) return;
        wm_invalidate_window(e->win_id);
    } else {
        if (e->tile_idx < 0) return;
        tile_invalidate_gui(e->tile_idx);
        tile_render_once();
    }
    e->frame_pending = 0;
}

static void user_gui_push_event(user_gui_t* e, const user_gui_event_t* ev) {
    if (!e || !ev) return;
    __asm__ __volatile__("cli");
    uint8 head = e->ev_head & USER_GUI_EVENT_MASK;
    uint8 tail = e->ev_tail & USER_GUI_EVENT_MASK;
    uint8 next = (uint8)((head + 1u) & USER_GUI_EVENT_MASK);
    if (next == tail) {
        // drop oldest
        tail = (uint8)((tail + 1u) & USER_GUI_EVENT_MASK);
        e->ev_tail = tail;
    }
    e->ev[head] = *ev;
    e->ev_head = next;
    __asm__ __volatile__("sti");
}

static int user_gui_pop_event(user_gui_t* e, user_gui_event_t* out) {
    if (!e || !out) return 0;
    __asm__ __volatile__("cli");
    uint8 head = e->ev_head & USER_GUI_EVENT_MASK;
    uint8 tail = e->ev_tail & USER_GUI_EVENT_MASK;
    if (tail == head) {
        __asm__ __volatile__("sti");
        return 0;
    }
    *out = e->ev[tail];
    e->ev_tail = (uint8)((tail + 1u) & USER_GUI_EVENT_MASK);
    __asm__ __volatile__("sti");
    return 1;
}

static void user_gui_draw_cb(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    int handle = (int)(uintptr)userdata;
    user_gui_t* e = user_gui_get_raw(handle);
    /* tile_idx carries win_id when called from the floating window manager */
    if (!e || (e->is_floating ? e->win_id != tile_idx : e->tile_idx != tile_idx)) return;
    if (content_w <= 0 || content_h <= 0) return;

    int clip_l = content_x;
    int clip_t = content_y;
    int clip_r = content_x + content_w;
    int clip_b = content_y + content_h;

    if (e->blit_buf && e->blit_w > 0 && e->blit_h > 0) {
        int dst_w = (e->blit_dst_w > 0) ? e->blit_dst_w : content_w;
        int dst_h = (e->blit_dst_h > 0) ? e->blit_dst_h : content_h;
        if (dst_w > content_w) dst_w = content_w;
        if (dst_h > content_h) dst_h = content_h;
        if (dst_w > 0 && dst_h > 0) {
            if (dst_w == e->blit_w && dst_h == e->blit_h) {
                vga_blit_rgb565_bb(content_x, content_y, e->blit_buf, e->blit_w, e->blit_h);
            } else {
                vga_blit_rgb565_scaled_bb(content_x, content_y, dst_w, dst_h, e->blit_buf, e->blit_w, e->blit_h);
            }
            vga_mark_dirty_rect(content_x, content_y, dst_w, dst_h);
        }
    }

    for (int i = 0; i < e->cmd_count; ++i) {
        const user_gui_cmd_t* c = &e->cmds[i];
        if (c->type == USER_GUI_CMD_CLEAR) {
            drawRect(content_x, content_y, content_w, content_h, c->r, c->g, c->b);
            continue;
        }
        if (c->type == USER_GUI_CMD_FILL_RECT) {
            int x = content_x + c->x;
            int y = content_y + c->y;
            int w = c->w;
            int h = c->h;
            if (w <= 0 || h <= 0) continue;
            drawRect(x, y, w, h, c->r, c->g, c->b);
            continue;
        }
        if (c->type == USER_GUI_CMD_TEXT) {
            int x = content_x + c->x;
            int y = content_y + c->y;
            int draw_font = user_gui_font_handle_from_slot(e, c->font_slot);
            int line_h = vga_font_line_height(draw_font);
            if (line_h <= 0) line_h = 8;
            if (y < clip_t || y + line_h > clip_b) {
                continue;
            }
            int pen_x = x;
            for (int j = 0; c->text[j] && j < (int)sizeof(c->text); ++j) {
                int ch = (int)(unsigned char)c->text[j];
                int adv = vga_font_char_advance(draw_font, ch);
                if (adv <= 0) adv = 8;
                if (pen_x >= clip_r) break;
                if (pen_x >= clip_l && pen_x + adv <= clip_r) {
                    drawCharAt_font(draw_font, pen_x, y, ch, c->r, c->g, c->b);
                }
                pen_x += adv;
            }
            continue;
        }

        if (c->type == USER_GUI_CMD_LINE) {
            int x1 = content_x + c->x;
            int y1 = content_y + c->y;
            int x2 = content_x + c->x2;
            int y2 = content_y + c->y2;
            drawLine(x1, y1, x2, y2, c->r, c->g, c->b);
            continue;
        }
        if (c->type == USER_GUI_CMD_OUTLINE_RECT) {
            int x = content_x + c->x;
            int y = content_y + c->y;
            int w = c->w;
            int h = c->h;
            if (w > 0 && h > 0) {
                drawLine(x, y, x + w - 1, y, c->r, c->g, c->b);             /* top    */
                drawLine(x, y + h - 1, x + w - 1, y + h - 1, c->r, c->g, c->b); /* bottom */
                drawLine(x, y, x, y + h - 1, c->r, c->g, c->b);             /* left   */
                drawLine(x + w - 1, y, x + w - 1, y + h - 1, c->r, c->g, c->b); /* right  */
            }
            continue;
        }
        if (c->type == USER_GUI_CMD_CHAR) {
            int x = content_x + c->x;
            int y = content_y + c->y;
            int draw_font = user_gui_font_handle_from_slot(e, c->font_slot);
            int line_h = vga_font_line_height(draw_font);
            if (line_h <= 0) line_h = 8;
            int adv = vga_font_char_advance(draw_font, c->w);
            if (adv <= 0) adv = 8;
            if (x >= clip_l && x + adv <= clip_r && y >= clip_t && y + line_h <= clip_b) {
                drawCharAt_font(draw_font, x, y, c->w, c->r, c->g, c->b);
            }
            continue;
        }
        if (c->type == USER_GUI_CMD_ICON) {
            /* Render a named file icon from the kernel-side icon cache. */
            int x = content_x + c->x;
            int y = content_y + c->y;
            tile_draw_file_icon(c->text, x, y);
            continue;
        }
    }
}

static void user_gui_key_cb(int tile_idx, int key, void* userdata) {
    int handle = (int)(uintptr)userdata;
    user_gui_t* e = user_gui_get_raw(handle);
    if (!e || (e->is_floating ? e->win_id != tile_idx : e->tile_idx != tile_idx)) return;
    user_gui_event_t ev;
    /*
     * Negative key values indicate key releases (from tui_read_key).
     * Translate to a KEY_UP event with the positive key code.
     */
    if (key < 0) {
        ev.type = USER_GUI_EVENT_KEY_UP;
        ev.a = (int32)(-key);
    } else {
        ev.type = USER_GUI_EVENT_KEY;
        ev.a = (int32)key;
    }
    ev.b = 0;
    ev.c = 0;
    ev.d = 0;
    user_gui_push_event(e, &ev);
}

/*
 * Close-request callback registered with the tiling manager.
 * Instead of directly closing, push a CLOSE event into the userland
 * event queue so the ring3 program can save state / confirm / clean up.
 * Returns 0 (veto) to prevent the tiler from destroying the tile --
 * the user program is expected to call exit() when ready.
 */
static int user_gui_close_cb(int tile_idx, void* userdata) {
    int handle = (int)(uintptr)userdata;
    user_gui_t* e = user_gui_get_raw(handle);
    if (!e || (e->is_floating ? e->win_id != tile_idx : e->tile_idx != tile_idx)) return 1; /* not ours -- allow close */
    user_gui_event_t ev;
    ev.type = USER_GUI_EVENT_CLOSE;
    ev.a = 0;
    ev.b = 0;
    ev.c = 0;
    ev.d = 0;
    user_gui_push_event(e, &ev);
    return 0; /* veto: let the user program handle it */
}

static void user_gui_mouse_cb(int tile_idx, const mouse_event_t* me, void* userdata) {
    if (!me) return;
    int handle = (int)(uintptr)userdata;
    user_gui_t* e = user_gui_get_raw(handle);
    if (!e || (e->is_floating ? e->win_id != tile_idx : e->tile_idx != tile_idx)) return;

    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (e->is_floating) {
        wm_get_content_rect(e->win_id, &cx, &cy, &cw, &ch);
    } else {
        tile_get_content_rect(tile_idx, &cx, &cy, &cw, &ch);
    }
    int rx = me->x - cx;
    int ry = me->y - cy;

    user_gui_event_t ev;
    ev.type = USER_GUI_EVENT_MOUSE;
    ev.a = (int32)rx;
    ev.b = (int32)ry;
    ev.c = (int32)me->buttons;
    ev.d = (int32)me->wheel_delta;
    user_gui_push_event(e, &ev);
}

static user_gui_t* user_gui_get(int handle) {
    int pid = user_task_get_running_pid();
    int resolved = handle;

    if (handle == 0) {
        if (pid <= 0) return NULL;
        resolved = user_gui_find_self_handle_by_pid(pid);
        if (resolved < 0) return NULL;
    }

    if (resolved < 0 || resolved >= USER_GUI_MAX) return NULL;
    if (!g_user_guis[resolved].used) return NULL;

    if (pid > 0 && g_user_guis[resolved].owner_pid > 0 && g_user_guis[resolved].owner_pid != pid) {
        return NULL;
    }

    return &g_user_guis[resolved];
}

/*
 * Returns non-zero when a GUI handle has an active backing resource.
 * Floating-window handles are live when win_id >= 0; tile-backed handles
 * are live when tile_idx >= 0.  Use this instead of (e->tile_idx < 0)
 * so all GUI syscall validation is floating-window-aware.
 */
static inline int user_gui_active(const user_gui_t* e) {
    if (!e || !e->used) return 0;
    return e->is_floating ? (e->win_id >= 0) : (e->tile_idx >= 0);
}

void syscall_reset_user_guis(void) {
    // Close/free all active user GUI entries (including per-pid self attachments).
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (g_user_guis[i].used) {
            user_gui_free_entry(&g_user_guis[i]);
        }
    }

    if (g_user_guis[0].used) {
        user_gui_free_entry(&g_user_guis[0]);
    }
}

void syscall_cleanup_user_resources_for_pid(int pid) {
    if (pid <= 0) return;

    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (!g_user_guis[i].used) continue;
        if (g_user_guis[i].owner_pid != pid) continue;
        user_gui_free_entry(&g_user_guis[i]);
    }
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

/* Parse explicit drive prefixes. Supported forms:
 *   RAM:/path  -> VFS_DRIVE_RAM
 *   N:/path    -> logical ATA drive N
 * Returns 1 when a prefix is present; 0 otherwise.
 */
static int syscall_parse_explicit_drive_prefix(const char* path, uint8* out_drive, const char** out_relpath) {
    if (!path || !out_drive || !out_relpath) return 0;

    if (path[0] == 'R' && path[1] == 'A' && path[2] == 'M' && path[3] == ':' && path[4] == '/') {
        *out_drive = VFS_DRIVE_RAM;
        *out_relpath = path + 4; /* keep leading '/' */
        return 1;
    }

    if (path[0] < '0' || path[0] > '9') return 0;

    uint32 logical = 0;
    int i = 0;
    while (path[i] >= '0' && path[i] <= '9') {
        logical = (logical * 10u) + (uint32)(path[i] - '0');
        if (logical > 255u) return 0;
        i++;
    }
    if (path[i] != ':' || path[i + 1] != '/') return 0;

    uint8 phys = ata_logical_to_physical((uint8)logical);
    if (phys == 0xFFu) return 0;

    *out_drive = phys;
    *out_relpath = path + i + 1; /* keep leading '/' */
    return 1;
}

typedef struct {
    uint8 present;
    uint8 type;
    uint8 bootable;
    uint8 _pad;
    uint32 lba_start;
    uint32 sector_count;
} syscall_installer_partition_t;

typedef struct {
    uint32 logical_drive;
    uint32 physical_drive;
    uint32 partition_count;
    syscall_installer_partition_t partitions[4];
} syscall_installer_partitions_t;

static int syscall_installer_get_disk_table(uint8 logical, disk_info_t* out_disk, uint8* out_physical) {
    if (!out_disk || !out_physical) return -1;
    uint8 physical = ata_logical_to_physical(logical);
    if (physical == 0xFFu) return -1;
    if (partition_read_table(physical, out_disk) != 0) return -1;
    *out_physical = physical;
    return 0;
}

/* Format an existing partition entry as EYNFS (partition_num: 1..4). */
static int syscall_installer_format_eynfs_partition(uint8 physical_drive, uint8 partition_num) {
    if (partition_num < 1 || partition_num > 4) return -1;

    disk_info_t disk;
    if (partition_read_table(physical_drive, &disk) != 0) return -1;

    partition_info_t* part = &disk.partitions[partition_num - 1];
    if (part->type == PART_TYPE_EMPTY || part->sector_count < 8u) return -1;

    uint32 start_lba = part->lba_start;
    uint32 total_blocks = part->sector_count;
    uint32 bitmap_bytes = (total_blocks + 7u) / 8u;
    uint32 bitmap_blocks = (bitmap_bytes + 511u) / 512u;
    uint32 superblock_block = 0;
    uint32 bitmap_block = 1;
    uint32 nametable_block = bitmap_block + bitmap_blocks;
    uint32 rootdir_block = nametable_block + 1u;

    if (rootdir_block >= total_blocks) return -1;

    uint8 sector[512];
    memset(sector, 0, sizeof(sector));

    eynfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = EYNFS_MAGIC;
    sb.version = EYNFS_VERSION;
    sb.block_size = EYNFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.root_dir_block = rootdir_block;
    sb.free_block_map = bitmap_block;
    sb.name_table_block = nametable_block;

    memcpy(sector, &sb, sizeof(sb));
    if (ata_write_sector(physical_drive, start_lba + superblock_block, sector) != 0) return -1;

    for (uint32 b = 0; b < bitmap_blocks; ++b) {
        memset(sector, 0, sizeof(sector));
        for (uint32 reserved = 0; reserved <= rootdir_block; ++reserved) {
            uint32 byte_index = reserved / 8u;
            uint32 bit_index = reserved % 8u;
            uint32 byte_block = byte_index / 512u;
            uint32 byte_off = byte_index % 512u;
            if (byte_block == b) sector[byte_off] |= (uint8)(1u << bit_index);
        }
        if (ata_write_sector(physical_drive, start_lba + bitmap_block + b, sector) != 0) return -1;
    }

    memset(sector, 0, sizeof(sector));
    if (ata_write_sector(physical_drive, start_lba + nametable_block, sector) != 0) return -1;
    if (ata_write_sector(physical_drive, start_lba + rootdir_block, sector) != 0) return -1;

    return 0;
}

/* Create a single bootable EYNFS partition spanning the disk and format it. */
static int syscall_installer_prepare_drive(uint8 logical_drive) {
    uint8 physical = ata_logical_to_physical(logical_drive);
    if (physical == 0xFFu) return -1;

    drive_info_t* info = ata_get_drive_info(physical);
    if (!info || !info->present || info->sectors <= 4096u) return -1;

    disk_info_t disk;
    memset(&disk, 0, sizeof(disk));
    disk.drive = physical;
    disk.has_valid_mbr = 1;
    disk.partition_count = 1;

    disk.partitions[0].drive = physical;
    disk.partitions[0].partition_num = 0;
    disk.partitions[0].bootable = 1;
    disk.partitions[0].type = PART_TYPE_EYNFS;
    disk.partitions[0].lba_start = 2048u;
    disk.partitions[0].sector_count = info->sectors - 2048u;
    disk.partitions[0].size_mb = disk.partitions[0].sector_count / 2048u;

    if (partition_write_table(physical, &disk) != 0) return -1;
    if (syscall_installer_format_eynfs_partition(physical, 1) != 0) return -1;

    /*
     * FS-INVARIANT: Installer format path performs raw ATA writes that bypass
     * EYNFS cache helpers.
     *
     * Why: partition_write_table()/syscall_installer_format_eynfs_partition()
     * write sectors directly. Any previously cached EYNFS directory/data blocks
     * for this drive may now be stale and must not be reused.
     *
     * Breakage if omitted: eynfs_create_entry() can observe stale root directory
     * entries (e.g. old /test.txt), causing false "already exists" failures on
     * the first installer copy pass after a reformat.
     */
    eynfs_cache_clear();
    return 0;
}

static void trim_trailing_crlf(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

/*
 * ABI-INVARIANT: User syscall address values are 32-bit virtual addresses.
 *
 * Why: Userland ABI remains i386-sized even when the kernel is built amd64.
 * Invariant: Any pointer returned to or consumed from user syscall arguments
 * must round-trip through uint32 without loss.
 * Breakage if violated: Silent truncation can retarget memory operations to
 * the wrong object/address.
 */
static int syscall_addr_to_u32(uintptr addr, uint32* out) {
    if (!out) return -1;
    uint32 narrowed = (uint32)addr;
    if ((uintptr)narrowed != addr) return -1;
    *out = narrowed;
    return 0;
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
            /* Defensive: free any leftover kbuf from a prior close that
             * forgot to release it (e.g. raw used=0 without free). */
            if (g_user_fds[i].kbuf) { free(g_user_fds[i].kbuf); g_user_fds[i].kbuf = NULL; }
            g_user_fds[i].kbuf_size = 0;
            g_user_fds[i].used = 1;
            g_user_fds[i].is_dir = 0;
            g_user_fds[i].kind = USER_FD_KIND_FILE;
            g_user_fds[i].pipe_id = -1;
            g_user_fds[i].pipe_end = -1;
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

static int user_fd_index_from_ptr(const user_fd_t* ptr) {
    if (!ptr) return -1;
    uintptr base = (uintptr)&g_user_fds[0];
    uintptr end = (uintptr)&g_user_fds[USER_FD_MAX];
    uintptr p = (uintptr)ptr;
    if (p < base || p >= end) return -1;
    uintptr off = p - base;
    if (off % sizeof(user_fd_t) != 0) return -1;
    int idx = (int)(off / sizeof(user_fd_t));
    if (idx < 0 || idx >= USER_FD_MAX) return -1;
    if (&g_user_fds[idx] != ptr) return -1;
    return idx;
}

static int syscall_cap_copyin(const void* user_cap_ptr, cap_t* out) {
    if (!user_cap_ptr || !out) return -1;
    return copyin(out, user_cap_ptr, sizeof(*out));
}

static user_fd_t* user_fd_from_cap(const cap_t* cap, uint32 required_rights, int* out_fd) {
    if (!cap) return NULL;
    user_fd_t* ufd = (user_fd_t*)(uintptr)cap->obj;
    int idx = user_fd_index_from_ptr(ufd);
    if (idx < 0) return NULL;
    if (!g_user_fds[idx].used) return NULL;
    if (!cap_validate(cap, ufd, CAP_OBJ_USER_FD, required_rights)) return NULL;
    if (out_fd) *out_fd = idx;
    return ufd;
}

static int user_gui_index_from_ptr(const user_gui_t* ptr) {
    if (!ptr) return -1;
    uintptr base = (uintptr)&g_user_guis[0];
    uintptr end = (uintptr)&g_user_guis[USER_GUI_MAX];
    uintptr p = (uintptr)ptr;
    if (p < base || p >= end) return -1;
    uintptr off = p - base;
    if (off % sizeof(user_gui_t) != 0) return -1;
    int idx = (int)(off / sizeof(user_gui_t));
    if (idx < 0 || idx >= USER_GUI_MAX) return -1;
    if (&g_user_guis[idx] != ptr) return -1;
    return idx;
}

static user_gui_t* user_gui_from_cap(const cap_t* cap, uint32 required_rights, int* out_handle) {
    if (!cap) return NULL;
    user_gui_t* e = (user_gui_t*)(uintptr)cap->obj;
    int idx = user_gui_index_from_ptr(e);
    if (idx < 0) return NULL;
    if (!g_user_guis[idx].used) return NULL;
    int pid = user_task_get_running_pid();
    if (pid > 0 && g_user_guis[idx].owner_pid > 0 && g_user_guis[idx].owner_pid != pid) return NULL;
    if (!cap_validate(cap, e, CAP_OBJ_USER_GUI, required_rights)) return NULL;
    if (out_handle) *out_handle = idx;
    return e;
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

static int syscall_current_task_term(void) {
    int term = user_task_get_running_term();
    if (term >= 0) return term;
    return g_user_task_term;
}

// When the tiling manager is active, user-mode programs print into their
// bound terminal (tracked per task slot), not the currently focused tile.
static void syscall_console_write(const char* buf, int len) {
    if (!buf || len <= 0) return;

    // For user-task output, explicitly control the vterm colour so it does not
    // inherit shell capture/redirect colours. Programs can change it by writing
    // the byte sequence: 0xFF, r, g, b.
    extern volatile int g_user_task_colour_r;
    extern volatile int g_user_task_colour_g;
    extern volatile int g_user_task_colour_b;
    extern volatile uint8 g_user_task_colour_state;
    extern volatile uint8 g_user_task_colour_bytes[3];
    extern volatile uint8 g_user_task_icon_state;
    extern volatile uint8 g_user_task_icon_bytes[16];

    int prev_r = shell_redirect_colour_r;
    int prev_g = shell_redirect_colour_g;
    int prev_b = shell_redirect_colour_b;

    // Start with current user-task colour (default white).
    shell_redirect_colour_r = (int)g_user_task_colour_r;
    shell_redirect_colour_g = (int)g_user_task_colour_g;
    shell_redirect_colour_b = (int)g_user_task_colour_b;

    // Control sequences for ring3 stdout/stderr.
    //  - 0xFF,r,g,b: set current colour
    //  - 0xFE,<16 bytes>: register line icon key
    const uint8 CTRL_COLOUR = 0xFF;
    const uint8 CTRL_ICON = 0xFE;
    if (tile_is_tiling_active()) {
        int term = tile_get_focused();
        if (g_user_task_active) {
            term = syscall_current_task_term();
        }
        if (term < 0) term = 0;
        for (int i = 0; i < len; ++i) {
            uint8 ch = (uint8)buf[i];

            if (g_user_task_colour_state != 0) {
                // Collect r,g,b across bytes (sequence may be split across writes).
                int idx = (int)g_user_task_colour_state - 1;
                if (idx >= 0 && idx < 3) {
                    g_user_task_colour_bytes[idx] = ch;
                }
                g_user_task_colour_state++;
                if (g_user_task_colour_state == 4) {
                    g_user_task_colour_r = (int)g_user_task_colour_bytes[0];
                    g_user_task_colour_g = (int)g_user_task_colour_bytes[1];
                    g_user_task_colour_b = (int)g_user_task_colour_bytes[2];
                    shell_redirect_colour_r = (int)g_user_task_colour_r;
                    shell_redirect_colour_g = (int)g_user_task_colour_g;
                    shell_redirect_colour_b = (int)g_user_task_colour_b;
                    g_user_task_colour_state = 0;
                }
                continue;
            }

            if (g_user_task_icon_state != 0) {
                int idx = (int)g_user_task_icon_state - 1;
                if (idx >= 0 && idx < 16) {
                    g_user_task_icon_bytes[idx] = ch;
                }
                g_user_task_icon_state++;
                if (g_user_task_icon_state == 17) {
                    char icon_key[16];
                    memcpy(icon_key, (const void*)g_user_task_icon_bytes, 16);
                    icon_key[15] = '\0';
                    if (icon_key[0]) {
                        vterm_register_line_icon(term, icon_key);
                    }
                    g_user_task_icon_state = 0;
                }
                continue;
            }

            if (ch == CTRL_COLOUR) {
                g_user_task_colour_state = 1;
                continue;
            }
            if (ch == CTRL_ICON) {
                g_user_task_icon_state = 1;
                continue;
            }

            vterm_write_char(term, (char)ch);
        }
        if (g_user_task_active) {
            g_user_task_ui_dirty = 1;
        }
        shell_redirect_colour_r = prev_r;
        shell_redirect_colour_g = prev_g;
        shell_redirect_colour_b = prev_b;
        return;
    }
    // Fallback: use kernel printf (serial/text console)
    char out[121];
    int out_len = 0;

    #define FLUSH_OUT() do { \
        if (out_len > 0) { \
            out[out_len] = '\0'; \
            printf("%s", out); \
            out_len = 0; \
        } \
    } while (0)

    for (int i = 0; i < len; ++i) {
        uint8 ch = (uint8)buf[i];

        if (g_user_task_colour_state != 0) {
            int idx = (int)g_user_task_colour_state - 1;
            if (idx >= 0 && idx < 3) g_user_task_colour_bytes[idx] = ch;
            g_user_task_colour_state++;
            if (g_user_task_colour_state == 4) {
                g_user_task_colour_r = (int)g_user_task_colour_bytes[0];
                g_user_task_colour_g = (int)g_user_task_colour_bytes[1];
                g_user_task_colour_b = (int)g_user_task_colour_bytes[2];
                g_user_task_colour_state = 0;
            }
            continue;
        }

        if (g_user_task_icon_state != 0) {
            int idx = (int)g_user_task_icon_state - 1;
            if (idx >= 0 && idx < 16) g_user_task_icon_bytes[idx] = ch;
            g_user_task_icon_state++;
            if (g_user_task_icon_state == 17) {
                g_user_task_icon_state = 0;
            }
            continue;
        }

        if (ch == CTRL_COLOUR) { FLUSH_OUT(); g_user_task_colour_state = 1; continue; }
        if (ch == CTRL_ICON) { FLUSH_OUT(); g_user_task_icon_state = 1; continue; }

        out[out_len++] = (char)ch;
        if (out_len >= 120) {
            FLUSH_OUT();
        }
    }

    FLUSH_OUT();
    #undef FLUSH_OUT

    shell_redirect_colour_r = prev_r;
    shell_redirect_colour_g = prev_g;
    shell_redirect_colour_b = prev_b;
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

// Focused tracing for file creation/writes from ring3.
#ifndef SYSCALL_WRITEFILE_DEBUG
#define SYSCALL_WRITEFILE_DEBUG 0
#endif

#ifndef GUI_HANDLE_SYSCALLS_DISABLED
/*
 * ABI-INVARIANT: Legacy GUI handle syscalls.
 *
 * Why: Existing userland programs (including the bundled testdir UELFs) use
 * the original GUI handle syscalls (EYN_SYSCALL_GUI_ATTACH/BEGIN/CLEAR/...).
 * Disabling them makes those binaries fail with "gui_attach failed".
 *
 * Security note: Capability-based GUI syscalls exist (EYN_SYSCALL_CAP_GUI_*).
 * Hardened builds can disable legacy handle syscalls by defining
 * GUI_HANDLE_SYSCALLS_DISABLED=1 at build time.
 */
#define GUI_HANDLE_SYSCALLS_DISABLED 0
#endif

/*
 * syscall_read_file -- kernel-side handler for SYSCALL_READ on a regular file.
 *
 * Replaced the old 256-byte inner loop that called vfs_read_file_at() for
 * each tiny chunk, re-traversing the EYNFS block chain from first_block every
 * 256 bytes.  For a 14 MB WAD that loop caused ~2.24 million block reads just
 * to stream the lump directory.
 *
 * New approach:
 *   1. Allocate a single kernel buffer of exactly maxlen bytes.
 *   2. For EYNFS files (eynfs_first_block != 0): call eynfs_read_file_fast()
 *      which uses the per-FD block cursor for O(1) continuation on sequential
 *      reads and only re-traverses on backwards seeks.
 *   3. For FAT32 / other files (eynfs_first_block == 0): single
 *      vfs_read_file_at() call -- still eliminates the inner loop that was
 *      previously O(maxlen/256) re-traversals.
 *   4. copyout the full result, update offset, free kernel buffer.
 */
static int syscall_read_file(user_fd_t* ufd, char* user_dst, int maxlen) {
    if (!ufd || !user_dst || maxlen <= 0) return 0;
    if (ufd->kind != USER_FD_KIND_FILE) return -1;
    if (ufd->is_dir) return -1;

    if (ufd->offset == 0 && SYSCALL_DEBUG) {
        printf("[SYSCALL:READ file] drive=%d path='%s' size=%d max=%d\n",
               (int)ufd->drive, ufd->path, (int)ufd->size, maxlen);
    }

    /*
     * Grow the per-FD reusable kernel buffer only when needed.
     *
     * LOW-MEM INVARIANT: read(2) should degrade to smaller chunks under
     * memory pressure rather than fail with -1 when a larger temporary buffer
     * cannot be allocated. Returning fewer bytes than requested is valid POSIX
     * behavior and lets userland stream loops continue progress.
     */
    if ((uint32)maxlen > ufd->kbuf_size) {
        uint32 target = (uint32)maxlen;
        if (target > SYSCALL_IO_MAX) target = SYSCALL_IO_MAX;
        if (target < 256u) target = 256u;

        if (ufd->kbuf) { free(ufd->kbuf); ufd->kbuf = NULL; }

        while (target >= 256u) {
            ufd->kbuf = (uint8*)malloc((size_t)target);
            if (ufd->kbuf) {
                ufd->kbuf_size = target;
                break;
            }
            target >>= 1;
        }

        if (!ufd->kbuf) {
            ufd->kbuf_size = 0;
            return -1;
        }
    }

    if ((uint32)maxlen > ufd->kbuf_size) {
        maxlen = (int)ufd->kbuf_size;
    }
    uint8 *kbuf = ufd->kbuf;

    int n;
    if (ufd->eynfs_first_block != 0) {
        /* EYNFS fast path: cursor-aware read, zero traversal for sequential. */
        n = eynfs_read_file_fast(ufd->drive,
                                 ufd->eynfs_first_block,
                                 ufd->size,
                                 kbuf,
                                 (size_t)maxlen,
                                 (size_t)ufd->offset,
                                 &ufd->cur_block,
                                 &ufd->cur_block_off);
    } else {
        /* Fallback for FAT32 or any non-EYNFS filesystem. */
        n = vfs_read_file_at(ufd->drive, ufd->path, kbuf, maxlen, ufd->offset);
    }

    if (n > 0) {
        if (copyout(user_dst, kbuf, (size_t)n) != 0) {
            return -1;
        }
        ufd->offset += (uint32)n;
    } else if (n == 0 && SYSCALL_DEBUG) {
        printf("[SYSCALL:READ file] EOF at off=%d (size=%d)\n",
               (int)ufd->offset, (int)ufd->size);
    }

    /* kbuf is retained for the next read on this FD -- no free here. */
    return (n < 0) ? -1 : n;
}

static int syscall_write_file_from_fd(user_fd_t* ufd, const void* user_src, int len) {
    if (!ufd || !user_src || len < 0) return -1;
    if (ufd->kind != USER_FD_KIND_FILE) return -1;
    if (ufd->is_dir) return -1;

    if (ufd->offset != 0) {
        // Current VFS has no per-offset write; only support overwrite from start.
        return -1;
    }

    const int max_len = 256 * 1024;
    if (len > max_len) return -1;

    uint8* kbuf = (uint8*)malloc((size_t)len);
    if (!kbuf) return -1;
    if (copyin(kbuf, user_src, (size_t)len) != 0) {
        free(kbuf);
        return -1;
    }

    int written = vfs_write_file(ufd->drive, ufd->path, kbuf, (uint32)len);
    free(kbuf);
    if (written < 0) return -1;

    ufd->offset = (uint32)written;
    ufd->size = (uint32)written;
    return written;
}

static int syscall_seek_fd(user_fd_t* ufd, int32 offset, int whence) {
    if (!ufd || ufd->is_dir || ufd->kind != USER_FD_KIND_FILE) return -1;

    int64 base = 0;
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int64)ufd->offset;
    } else if (whence == 2) {
        base = (int64)ufd->size;
    } else {
        return -1;
    }

    int64 next = base + (int64)offset;
    if (next < 0) return -1;
    if (next > (int64)ufd->size) {
        next = (int64)ufd->size;
    }
    ufd->offset = (uint32)next;
    /*
     * EYNFS block cursor management.
     *
     * For FORWARD seeks (new offset >= cur_block_off), the cursor is
     * preserved.  eynfs_read_file_fast() already handles forward jumps
     * by traversing from the cursor instead of from the file start,
     * avoiding an O(offset/block_size) chain walk.
     *
     * For BACKWARD seeks (new offset < cur_block_off), the cursor must
     * be reset because the singly-linked block chain can only be walked
     * forward.  eynfs_read_file_fast() will re-traverse from first_block.
     *
     * This is critical for audio streaming: an unnecessary cursor reset
     * on a forward seek forces a full chain walk on the next read,
     * which for a 9.5 MB file at 5 seconds in means walking ~1900
     * blocks and consuming all CPU time.
     */
    if (ufd->cur_block != 0 && (uint32)next >= ufd->cur_block_off) {
        /* Forward seek -- cursor stays valid. */
    } else {
        /* Backward seek or uninitialised cursor -- reset. */
        ufd->cur_block     = 0;
        ufd->cur_block_off = 0;
    }
    return (int)ufd->offset;
}

static int syscall_shell_migrated_dispatch(const char* user_cmd, const char* user_raw) {
    if (!user_cmd || !user_raw) return -1;

    char cmd_name[24];
    char raw_line[256];
    if (copyin_cstr(cmd_name, sizeof(cmd_name), user_cmd) != 0) return -1;
    if (copyin_cstr(raw_line, sizeof(raw_line), user_raw) != 0) return -1;
    if (!cmd_name[0]) return -1;

    (void)cmd_name;
    (void)raw_line;
    return -1;
}

typedef struct {
    uint8 bus;
    uint8 device;
    uint8 function;
    uint8 class_code;
    uint8 subclass;
    uint8 prog_if;
    uint8 header_type;
    uint8 bar0_is_io;
    uint16 vendor_id;
    uint16 device_id;
    uint16 command;
    uint16 _pad;
    uint32 bar0_base;
} syscall_pci_entry_t;

typedef struct {
    int filter_net_only;
    int count;
} syscall_pci_count_ctx_t;

static void syscall_pci_count_cb(const pci_device_info* info, void* user) {
    syscall_pci_count_ctx_t* ctx = (syscall_pci_count_ctx_t*)user;
    if (!ctx || !info) return;
    if (ctx->filter_net_only && info->class_code != 0x02) return;
    ctx->count++;
}

typedef struct {
    int filter_net_only;
    int target_index;
    int current_index;
    int found;
    syscall_pci_entry_t out;
} syscall_pci_entry_ctx_t;

static void syscall_pci_entry_cb(const pci_device_info* info, void* user) {
    syscall_pci_entry_ctx_t* ctx = (syscall_pci_entry_ctx_t*)user;
    if (!ctx || !info) return;
    if (ctx->filter_net_only && info->class_code != 0x02) return;
    if (ctx->current_index != ctx->target_index) {
        ctx->current_index++;
        return;
    }

    uint16 command = pci_read_config_word(info->bus, info->device, info->function, 0x04);
    uint32 bar0 = pci_read_config_dword(info->bus, info->device, info->function, 0x10);
    uint8 bar0_is_io = (uint8)(bar0 & 0x1u);
    uint32 bar0_base = bar0_is_io ? (bar0 & ~0x3u) : (bar0 & ~0xFu);

    memset(&ctx->out, 0, sizeof(ctx->out));
    ctx->out.bus = info->bus;
    ctx->out.device = info->device;
    ctx->out.function = info->function;
    ctx->out.class_code = info->class_code;
    ctx->out.subclass = info->subclass;
    ctx->out.prog_if = info->prog_if;
    ctx->out.header_type = info->header_type;
    ctx->out.bar0_is_io = bar0_is_io;
    ctx->out.vendor_id = info->vendor_id;
    ctx->out.device_id = info->device_id;
    ctx->out.command = command;
    ctx->out.bar0_base = bar0_base;
    ctx->found = 1;
}

typedef struct {
    uint8 bus;
    uint8 device;
    uint8 function;
    uint8 _pad0;
    uint32 bar0;
    uint32 ctrl;
    uint32 status;
    uint8 mac[6];
    uint8 _pad1[2];
    int32 link_up;
} syscall_e1000_probe_t;

typedef struct {
    uint8 local_ip[4];
    uint8 gateway_ip[4];
    uint8 netmask[4];
    uint8 dns_ip[4];
} syscall_net_config_t;

typedef struct {
    uint8 ip[4];
    uint8 mac[6];
    uint8 valid;
    uint8 _pad;
} syscall_net_arp_entry_t;

typedef struct {
    uint32 udp_rx_enqueued;
    uint32 udp_rx_dropped;
    uint32 udp_rx_truncated;
    uint32 udp_rx_bad_checksum;
    uint32 udp_tx_checksums;
} syscall_net_udp_stats_t;

typedef struct {
    uint32 ipv4_rx_fragments;
    uint32 ipv4_rx_frag_dropped;
} syscall_net_ip_stats_t;

typedef struct {
    uint8 bound;
    uint8 _pad0;
    uint16 port;
    uint32 queued;
    uint32 dropped;
} syscall_net_socket_info_t;

typedef struct {
    int32 pid;
    int32 status;
    int32 active;
    char command[96];
} syscall_bg_job_info_t;

extern uint8 g_current_drive;

// C dispatcher called by the assembly stub. Returns value in EAX to user.
static uint32 syscall_dispatch_core(regs_t* regs,
                                    uint32 syscall_num,
                                    uintptr arg1,
                                    uintptr arg2,
                                    uintptr arg3,
                                    uintptr arg4,
                                    uintptr arg5) {
    (void)arg4;
    (void)arg5;

    if (g_user_task_active) {
        user_task_capture_syscall_frame(regs);
    }

    switch (syscall_num) {
        case SYSCALL_EYNFS_STREAM_BEGIN: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }

            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            if (!cwd || cwd[0] != '/') cwd = "/";

            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            uint8 drive = g_current_drive;

            // Only support EYNFS for streaming writes today.
            eynfs_superblock_t sb;
            if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
                regs->eax = (uint32)-1;
                break;
            }

            int handle = -1;
            for (int i = 0; i < USER_STREAM_MAX; ++i) {
                if (!g_user_streams[i].used) { handle = i; break; }
            }
            if (handle < 0) { regs->eax = (uint32)-1; break; }

            g_user_streams[handle].used = 1;
            if (eynfs_stream_begin(drive, abspath, &g_user_streams[handle].s) != 0) {
                g_user_streams[handle].used = 0;
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = (uint32)handle;
            break;
        }
        case SYSCALL_EYNFS_STREAM_WRITE: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_buf = (const void*)arg2;
            int len = (int)arg3;
            if (!user_buf || len < 0) { regs->eax = (uint32)-1; break; }
            if (len == 0) { regs->eax = 0; break; }

            user_stream_t* us = user_stream_get(handle);
            if (!us) { regs->eax = (uint32)-1; break; }

            // Copy from user memory in bounded chunks.
            int total = 0;
            const uint8* src = (const uint8*)user_buf;
            while (total < len) {
                int remaining = len - total;
                int chunk = remaining;
                if (chunk > 1024) chunk = 1024;
                uint8 tmp[1024];
                if (copyin(tmp, src + total, (size_t)chunk) != 0) { regs->eax = (uint32)-1; return regs->eax; }
                int w = eynfs_stream_write(&us->s, tmp, (size_t)chunk);
                if (w != chunk) { regs->eax = (uint32)-1; break; }
                total += chunk;
            }
            regs->eax = (regs->eax == (uint32)-1) ? (uint32)-1 : (uint32)total;
            break;
        }
        case SYSCALL_EYNFS_STREAM_END: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            user_stream_t* us = user_stream_get(handle);
            if (!us) { regs->eax = (uint32)-1; break; }

            int rc = eynfs_stream_end(&us->s);
            us->used = 0;
            memset(&us->s, 0, sizeof(us->s));
            regs->eax = (rc == 0) ? 0 : (uint32)-1;
            break;
        }
        case SYSCALL_GETCWD: {
            // getcwd(buf=arg1, buflen=arg2) -> returns bytes written (excluding NUL)
            // Used by ring3 utilities (e.g. /binaries/pwd).
            char* user_buf = (char*)arg1;
            int buflen = (int)arg2;
            if (!user_buf || buflen <= 0) { regs->eax = (uint32)-1; break; }

            const char* cwd = "/";
            if (g_user_task_active) {
                const char* t = vterm_get_cwd(g_user_task_term);
                if (t && t[0] == '/') {
                    cwd = t;
                }
            }

            int n = (int)strlen(cwd);
            if (n >= buflen) n = buflen - 1;
            if (n < 0) n = 0;

            if (copyout(user_buf, cwd, (size_t)n) != 0) { regs->eax = (uint32)-1; break; }
            if (copyout(user_buf + n, "\0", 1) != 0) { regs->eax = (uint32)-1; break; }

            regs->eax = (uint32)n;
            break;
        }
        case SYSCALL_INSTALLER_PREPARE_DRIVE: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint8 logical = (uint8)arg1;
            regs->eax = (uint32)syscall_installer_prepare_drive(logical);
            break;
        }
        case SYSCALL_INSTALLER_FORMAT_EYNFS_PARTITION: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint8 logical = (uint8)arg1;
            uint8 part_num = (uint8)arg2;
            uint8 physical = ata_logical_to_physical(logical);
            if (physical == 0xFFu) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)syscall_installer_format_eynfs_partition(physical, part_num);
            break;
        }
        case SYSCALL_INSTALLER_WRITE_SECTOR: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint8 logical = (uint8)arg1;
            uint32 lba = (uint32)arg2;
            const void* user_buf = (const void*)arg3;
            if (!user_buf) { regs->eax = (uint32)-1; break; }
            uint8 physical = ata_logical_to_physical(logical);
            if (physical == 0xFFu) { regs->eax = (uint32)-1; break; }

            uint8 sector[512];
            if (copyin(sector, user_buf, sizeof(sector)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (ata_write_sector(physical, lba, sector) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_INSTALLER_GET_PARTITIONS: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint8 logical = (uint8)arg1;
            void* user_out = (void*)arg2;
            int out_len = (int)arg3;
            if (!user_out || out_len < (int)sizeof(syscall_installer_partitions_t)) { regs->eax = (uint32)-1; break; }

            disk_info_t disk;
            uint8 physical = 0;
            if (syscall_installer_get_disk_table(logical, &disk, &physical) != 0) { regs->eax = (uint32)-1; break; }

            syscall_installer_partitions_t out;
            memset(&out, 0, sizeof(out));
            out.logical_drive = logical;
            out.physical_drive = physical;
            out.partition_count = disk.partition_count;
            for (int i = 0; i < 4; ++i) {
                out.partitions[i].present = (disk.partitions[i].type != PART_TYPE_EMPTY && disk.partitions[i].sector_count > 0) ? 1u : 0u;
                out.partitions[i].type = disk.partitions[i].type;
                out.partitions[i].bootable = disk.partitions[i].bootable ? 1u : 0u;
                out.partitions[i].lba_start = disk.partitions[i].lba_start;
                out.partitions[i].sector_count = disk.partitions[i].sector_count;
            }

            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_DRIVE_SET_LOGICAL: {
            if (!syscall_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint8 logical = (uint8)arg1;
            uint8 physical = ata_logical_to_physical(logical);
            if (physical == 0xFFu) { regs->eax = (uint32)-1; break; }
            g_current_drive = physical;
            regs->eax = (uint32)logical;
            break;
        }
        case SYSCALL_DRIVE_GET_LOGICAL: {
            if (!syscall_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)ata_physical_to_logical(g_current_drive);
            break;
        }
        case SYSCALL_DRIVE_GET_COUNT: {
            if (!syscall_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)ata_get_num_logical_drives();
            break;
        }
        case SYSCALL_DRIVE_IS_PRESENT: {
            if (!syscall_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = ata_logical_drive_present((uint8)arg1) ? 1u : 0u;
            break;
        }
        case SYSCALL_INIT_SERVICES: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            ata_init_drives();
            regs->eax = 0;
            break;
        }
        case SYSCALL_SERIAL_WRITE_COM1: {
            if (!syscall_ctx_allow(CAP_DEV_SERIAL, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const char* user_buf = (const char*)arg1;
            int len = (int)arg2;
            if (!user_buf || len < 0) { regs->eax = (uint32)-1; break; }
            if (len == 0) { regs->eax = 0; break; }
            if (len > SYSCALL_IO_MAX) len = SYSCALL_IO_MAX;

            int total = 0;
            while (total < len) {
                int chunk = len - total;
                if (chunk > 256) chunk = 256;
                char tmp[256];
                if (copyin(tmp, user_buf + total, (size_t)chunk) != 0) { regs->eax = (uint32)-1; return regs->eax; }
                int w = serial_write(SERIAL_COM1, tmp, chunk);
                if (w != chunk) { regs->eax = (uint32)-1; return regs->eax; }
                total += chunk;
            }
            regs->eax = (uint32)total;
            break;
        }
        case SYSCALL_SHELL_LOG_SET: {
            if (!syscall_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            if ((int)arg1 != 0) shell_log_enable();
            else shell_log_disable();
            regs->eax = (uint32)(shell_log_is_enabled() ? 1 : 0);
            break;
        }
        case SYSCALL_SHELL_LOG_GET: {
            regs->eax = (uint32)(shell_log_is_enabled() ? 1 : 0);
            break;
        }
        case SYSCALL_CRASHLOG_COUNT: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int count = crashlog_get_record_count();
            regs->eax = (count < 0) ? (uint32)-1 : (uint32)count;
            break;
        }
        case SYSCALL_CRASHLOG_INFO: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint32 index = (uint32)arg1;
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            crashlog_record_info_t info;
            if (crashlog_get_record_info(index, &info) != 0) { regs->eax = (uint32)-1; break; }
            if (copyout(user_out, &info, sizeof(info)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CRASHLOG_DATA: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint32 index = (uint32)arg1;
            void* user_out = (void*)arg2;
            int out_cap = (int)arg3;
            if (!user_out || out_cap <= 0) { regs->eax = (uint32)-1; break; }
            if (out_cap > CRASHLOG_MAX_DATA) out_cap = CRASHLOG_MAX_DATA;

            uint8 data[CRASHLOG_MAX_DATA];
            int n = crashlog_get_record_data(index, data, (uint32)out_cap);
            if (n < 0) { regs->eax = (uint32)-1; break; }
            if (n > 0 && copyout(user_out, data, (size_t)n) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)n;
            break;
        }
        case SYSCALL_CRASHLOG_CLEAR: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (crashlog_clear() == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_SHELL_MIGRATED_DISPATCH: {
            const char* user_cmd = (const char*)arg1;
            const char* user_raw = (const char*)arg2;
            regs->eax = (syscall_shell_migrated_dispatch(user_cmd, user_raw) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_PCI_GET_COUNT: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            syscall_pci_count_ctx_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.filter_net_only = ((int)arg1 != 0) ? 1 : 0;
            pci_enumerate(syscall_pci_count_cb, &ctx);
            regs->eax = (uint32)ctx.count;
            break;
        }
        case SYSCALL_PCI_GET_ENTRY: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int filter_net_only = ((int)arg1 != 0) ? 1 : 0;
            int target_index = (int)arg2;
            void* user_out = (void*)arg3;
            if (!user_out || target_index < 0) { regs->eax = (uint32)-1; break; }

            syscall_pci_entry_ctx_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.filter_net_only = filter_net_only;
            ctx.target_index = target_index;
            pci_enumerate(syscall_pci_entry_cb, &ctx);
            if (!ctx.found) { regs->eax = (uint32)-1; break; }
            if (copyout(user_out, &ctx.out, sizeof(ctx.out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_E1000_PROBE: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            e1000_probe_info info;
            if (e1000_probe(&info) != 0) { regs->eax = (uint32)-1; break; }
            syscall_e1000_probe_t out;
            memset(&out, 0, sizeof(out));
            out.bus = info.bus;
            out.device = info.device;
            out.function = info.function;
            out.bar0 = info.bar0;
            out.ctrl = info.ctrl;
            out.status = info.status;
            for (int i = 0; i < 6; i++) out.mac[i] = info.mac[i];
            out.link_up = info.link_up;
            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_E1000_INIT: {
            if (!syscall_ctx_allow(CAP_DEV_NET | CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)net_init_e1000_default();
            break;
        }
        case SYSCALL_NETCFG_GET: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            net_config cfg;
            net_config_get(&cfg);
            syscall_net_config_t out;
            memset(&out, 0, sizeof(out));
            for (int i = 0; i < 4; i++) {
                out.local_ip[i] = cfg.local_ip[i];
                out.gateway_ip[i] = cfg.gateway_ip[i];
                out.netmask[i] = cfg.netmask[i];
                out.dns_ip[i] = cfg.dns_ip[i];
            }
            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_NETCFG_SET: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_in = (const void*)arg1;
            if (!user_in) { regs->eax = (uint32)-1; break; }
            syscall_net_config_t in;
            if (copyin(&in, user_in, sizeof(in)) != 0) { regs->eax = (uint32)-1; break; }
            net_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            for (int i = 0; i < 4; i++) {
                cfg.local_ip[i] = in.local_ip[i];
                cfg.gateway_ip[i] = in.gateway_ip[i];
                cfg.netmask[i] = in.netmask[i];
                cfg.dns_ip[i] = in.dns_ip[i];
            }
            regs->eax = (uint32)net_config_set(&cfg);
            break;
        }
        case SYSCALL_NETCFG_DEFAULTS: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            net_config_set_defaults();
            regs->eax = 0;
            break;
        }
        case SYSCALL_NET_IS_INITED: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)(net_is_inited() ? 1 : 0);
            break;
        }
        case SYSCALL_NET_GET_MAC: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            uint8 mac[6];
            if (net_get_mac(mac) != 0) { regs->eax = (uint32)-1; break; }
            if (copyout(user_out, mac, 6) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_NET_GET_ARP: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int out_cap = (int)arg2;
            void* user_out = (void*)arg3;
            if (!user_out || out_cap <= 0) { regs->eax = (uint32)-1; break; }
            if (out_cap > 32) out_cap = 32;

            net_arp_entry entries[32];
            uint32 n = net_get_arp_cache(entries, (uint32)out_cap);
            if (n > (uint32)out_cap) n = (uint32)out_cap;

            syscall_net_arp_entry_t out_entries[32];
            for (uint32 i = 0; i < n; i++) {
                memset(&out_entries[i], 0, sizeof(out_entries[i]));
                for (int k = 0; k < 4; k++) out_entries[i].ip[k] = entries[i].ip[k];
                for (int k = 0; k < 6; k++) out_entries[i].mac[k] = entries[i].mac[k];
                out_entries[i].valid = entries[i].valid;
            }
            if (copyout(user_out, out_entries, sizeof(out_entries[0]) * n) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = n;
            break;
        }
        case SYSCALL_NET_GET_UDP_STATS: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            net_udp_stats st = net_udp_get_stats();
            syscall_net_udp_stats_t out;
            out.udp_rx_enqueued = st.udp_rx_enqueued;
            out.udp_rx_dropped = st.udp_rx_dropped;
            out.udp_rx_truncated = st.udp_rx_truncated;
            out.udp_rx_bad_checksum = st.udp_rx_bad_checksum;
            out.udp_tx_checksums = st.udp_tx_checksums;
            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_NET_GET_IP_STATS: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_out = (void*)arg3;
            if (!user_out) { regs->eax = (uint32)-1; break; }
            net_ip_stats st = net_ip_get_stats();
            syscall_net_ip_stats_t out;
            out.ipv4_rx_fragments = st.ipv4_rx_fragments;
            out.ipv4_rx_frag_dropped = st.ipv4_rx_frag_dropped;
            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_NET_GET_SOCKETS: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int out_cap = (int)arg2;
            void* user_out = (void*)arg3;
            if (!user_out || out_cap <= 0) { regs->eax = (uint32)-1; break; }
            if (out_cap > 16) out_cap = 16;
            net_socket_info sockets[16];
            uint32 n = net_get_sockets(sockets, (uint32)out_cap);
            if (n > (uint32)out_cap) n = (uint32)out_cap;
            syscall_net_socket_info_t out[16];
            for (uint32 i = 0; i < n; i++) {
                memset(&out[i], 0, sizeof(out[i]));
                out[i].bound = sockets[i].bound;
                out[i].port = sockets[i].port;
                out[i].queued = sockets[i].queued;
                out[i].dropped = sockets[i].dropped;
            }
            if (copyout(user_out, out, sizeof(out[0]) * n) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = n;
            break;
        }
        case SYSCALL_NET_PING: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_dst = (const void*)arg1;
            const void* user_local = (const void*)arg2;
            int count = (int)arg3;
            if (!user_dst || !user_local) { regs->eax = (uint32)-1; break; }
            if (count <= 0) count = 4;
            if (count > 64) count = 64;
            uint8 dst_ip[4];
            uint8 local_ip[4];
            if (copyin(dst_ip, user_dst, 4) != 0) { regs->eax = (uint32)-1; break; }
            if (copyin(local_ip, user_local, 4) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)net_icmp_ping(local_ip, dst_ip, count, 0);
            break;
        }
        case SYSCALL_NET_DNS_RESOLVE: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_name = (const char*)arg1;
            void* user_out = (void*)arg2;
            if (!user_name || !user_out) { regs->eax = (uint32)-1; break; }

            char name[256];
            if (copyin_cstr(name, sizeof(name), user_name) != 0) { regs->eax = (uint32)-1; break; }

            uint8 ip[4];
            int rc = net_dns_resolve(name, ip, 0);
            if (rc != 0) { regs->eax = (uint32)rc; break; }
            if (copyout(user_out, ip, 4) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_NET_TCP_CONNECT: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_dst = (const void*)arg1;
            uint16 dst_port = (uint16)arg2;
            uint16 local_port = (uint16)arg3;
            if (!user_dst) { regs->eax = (uint32)-1; break; }

            uint8 dst_ip[4];
            if (copyin(dst_ip, user_dst, 4) != 0) { regs->eax = (uint32)-1; break; }

            uint8 local_ip[4];
            net_get_local_ip(local_ip);
            regs->eax = (uint32)net_tcp_connect(local_ip, local_port, dst_ip, dst_port, 0);
            break;
        }
        case SYSCALL_NET_TCP_SEND: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_buf = (const void*)arg1;
            uint32 len = (uint32)arg2;
            if (!user_buf) { regs->eax = (uint32)-1; break; }
            if (len > NET_TCP_MAX_PAYLOAD) { regs->eax = (uint32)-2; break; }

            uint8 buf[NET_TCP_MAX_PAYLOAD];
            if (len != 0u && copyin(buf, user_buf, (size_t)len) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)net_tcp_send_current(buf, len);
            break;
        }
        case SYSCALL_NET_TCP_RECV: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            void* user_buf = (void*)arg1;
            uint32 buflen = (uint32)arg2;
            if (!user_buf) { regs->eax = (uint32)-1; break; }

            net_tcp_rx_packet pkt;
            int rc = net_tcp_recv(&pkt);
            if (rc <= 0) {
                if (rc == 0 && net_tcp_is_closed()) { regs->eax = (uint32)-2; break; }
                regs->eax = (uint32)rc;
                break;
            }

            uint32 copy_len = pkt.payload_len;
            if (copy_len > buflen) copy_len = buflen;
            if (copy_len != 0u && copyout(user_buf, pkt.payload, (size_t)copy_len) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)copy_len;
            break;
        }
        case SYSCALL_NET_TCP_CLOSE: {
            if (!syscall_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)net_tcp_close();
            break;
        }
        case SYSCALL_FS_CHECK_INTEGRITY: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)check_filesystem_integrity(g_current_drive);
            break;
        }
        case SYSCALL_FS_FATFIX: {
            if (!syscall_ctx_allow(CAP_DEV_DISK | CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }
            char path[256];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)fatfix_repair_path(g_current_drive, path);
            break;
        }
        case SYSCALL_VTERM_CLEAR: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (!g_user_task_active || g_user_task_term < 0) { regs->eax = (uint32)-1; break; }
            vterm_clear(g_user_task_term);
            regs->eax = 0;
            break;
        }
        case SYSCALL_HISTORY_COUNT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)g_command_history.count;
            break;
        }
        case SYSCALL_HISTORY_ENTRY: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int index = (int)arg1;
            int out_len = (int)arg2;
            char* user_out = (char*)arg3;
            if (!user_out || out_len <= 0) { regs->eax = (uint32)-1; break; }
            if (index < 0 || index >= g_command_history.count) { regs->eax = (uint32)-1; break; }

            const char* src = g_command_history.commands[index];
            int n = (int)strlen(src);
            if (n >= out_len) n = out_len - 1;
            if (n < 0) n = 0;
            if (copyout(user_out, src, (size_t)n) != 0) { regs->eax = (uint32)-1; break; }
            if (copyout(user_out + n, "\0", 1) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)n;
            break;
        }
        case SYSCALL_HISTORY_CLEAR: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            clear_history(&g_command_history);
            regs->eax = 0;
            break;
        }
        case SYSCALL_BG_JOB_COUNT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int count = 0;
            for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
                if (g_background_processes[i].active) count++;
            }
            regs->eax = (uint32)count;
            break;
        }
        case SYSCALL_BG_JOB_INFO: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int target = (int)arg1;
            void* user_out = (void*)arg3;
            if (!user_out || target < 0) { regs->eax = (uint32)-1; break; }

            int seen = 0;
            int found = -1;
            for (int i = 0; i < MAX_BACKGROUND_PROCESSES; i++) {
                if (!g_background_processes[i].active) continue;
                if (seen == target) { found = i; break; }
                seen++;
            }
            if (found < 0) { regs->eax = (uint32)-1; break; }

            syscall_bg_job_info_t out;
            memset(&out, 0, sizeof(out));
            out.pid = g_background_processes[found].pid;
            out.status = g_background_processes[found].status;
            out.active = g_background_processes[found].active;
            if (g_background_processes[found].command) {
                strncpy(out.command, g_background_processes[found].command, sizeof(out.command) - 1);
                out.command[sizeof(out.command) - 1] = '\0';
            }

            if (copyout(user_out, &out, sizeof(out)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_TILING_START: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            start_tiling_manager();
            regs->eax = 0;
            break;
        }
        case SYSCALL_SETBG_PATH: {
            if (!syscall_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }
            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }

            const char* cwd = "/";
            if (g_user_task_active) {
                const char* t = vterm_get_cwd(g_user_task_term);
                if (t && t[0] == '/') cwd = t;
            }

            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            vfs_stat_t st;
            if (vfs_stat(g_current_drive, abspath, &st) != 0 || st.type != VFS_NODE_FILE) { regs->eax = (uint32)-1; break; }
            if (st.size == 0 || st.size > 512 * 1024) { regs->eax = (uint32)-1; break; }

            uint8* buf = (uint8*)malloc(st.size);
            if (!buf) { regs->eax = (uint32)-1; break; }
            int br = vfs_read_file(g_current_drive, abspath, (char*)buf, (int)st.size);
            if (br <= 0) { free(buf); regs->eax = (uint32)-1; break; }

            rei_image_t* img = (rei_image_t*)malloc(sizeof(rei_image_t));
            if (!img) { free(buf); regs->eax = (uint32)-1; break; }
            if (rei_parse_image(buf, br, img) != 0) { free(buf); free(img); regs->eax = (uint32)-1; break; }
            free(buf);

            int focused = tile_get_focused();
            if (focused < 0) { rei_free_image(img); free(img); regs->eax = (uint32)-1; break; }

            regs->eax = (tile_begin_set_background_from_rei(focused, img) == 0) ? 0u : (uint32)-1;
            if (regs->eax != 0u) {
                rei_free_image(img);
                free(img);
            }
            break;
        }
        case SYSCALL_CLEARBG_FOCUSED: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int focused = tile_get_focused();
            if (focused < 0) { regs->eax = (uint32)-1; break; }
            tile_clear_background(focused);
            regs->eax = 0;
            break;
        }
        case SYSCALL_SETFONT_PATH: {
            if (!syscall_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }
            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }

            if (strcmp(path, "builtin") == 0 || path[0] == '\0') {
                regs->eax = (vga_system_font_set(g_current_drive, "builtin") == 0) ? 0u : (uint32)-1;
                if (regs->eax == 0u) tile_render_once();
                break;
            }

            const char* cwd = "/";
            if (g_user_task_active) {
                const char* t = vterm_get_cwd(g_user_task_term);
                if (t && t[0] == '/') cwd = t;
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));
            regs->eax = (vga_system_font_set(g_current_drive, abspath) == 0) ? 0u : (uint32)-1;
            if (regs->eax == 0u) tile_render_once();
            break;
        }
        case SYSCALL_SET_DISPLAY_PROFILE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int scale_pct = (int)arg1;
            int aspect_mode = (int)arg2;
            int persist = (int)arg3;
            regs->eax = (tiler_set_display_profile(scale_pct, aspect_mode, persist) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_GET_DISPLAY_PROFILE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            syscall_display_profile_t* user_out = (syscall_display_profile_t*)arg1;
            if (!user_out) { regs->eax = (uint32)-1; break; }

            tiler_display_profile_t profile;
            memset(&profile, 0, sizeof(profile));
            tiler_get_display_profile(&profile);

            syscall_display_profile_t out;
            out.fb_w = profile.fb_w;
            out.fb_h = profile.fb_h;
            out.workspace_w = profile.workspace_w;
            out.workspace_h = profile.workspace_h;
            out.scale_pct = profile.scale_pct;
            out.aspect_mode = profile.aspect_mode;

            regs->eax = (copyout(user_out, &out, sizeof(out)) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_SET_DISPLAY_MODE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const syscall_display_mode_set_t* user_in = (const syscall_display_mode_set_t*)arg1;
            if (!user_in) { regs->eax = (uint32)-1; break; }

            syscall_display_mode_set_t in;
            if (copyin(&in, user_in, sizeof(in)) != 0) { regs->eax = (uint32)-1; break; }

            regs->eax = (tiler_set_display_mode(in.width, in.height, in.bpp, in.persist) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_GET_DISPLAY_MODE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            syscall_display_mode_t* user_out = (syscall_display_mode_t*)arg1;
            if (!user_out) { regs->eax = (uint32)-1; break; }

            tiler_display_mode_t mode;
            memset(&mode, 0, sizeof(mode));
            tiler_get_display_mode(&mode);

            syscall_display_mode_t out;
            out.width = mode.width;
            out.height = mode.height;
            out.bpp = mode.bpp;
            out.can_switch = mode.can_switch;

            regs->eax = (copyout(user_out, &out, sizeof(out)) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_CHDIR: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            if (!g_user_task_active || g_user_task_term < 0) { regs->eax = (uint32)-1; break; }

            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }

            const char* cwd = vterm_get_cwd(g_user_task_term);
            if (!cwd || cwd[0] != '/') cwd = "/";

            uint8 drive = g_current_drive;
            const char* explicit_path = NULL;
            char abspath[128];
            if (syscall_parse_explicit_drive_prefix(path, &drive, &explicit_path)) {
                strncpy(abspath, explicit_path, sizeof(abspath) - 1);
                abspath[sizeof(abspath) - 1] = '\0';
            } else {
                resolve_path(path, cwd, abspath, sizeof(abspath));
            }

            if (strcmp(abspath, "/") == 0) {
                vterm_set_cwd(g_user_task_term, "/");
                regs->eax = 0;
                break;
            }

            vfs_stat_t st;
            if (vfs_stat(drive, abspath, &st) != 0 || st.type != VFS_NODE_DIR) {
                regs->eax = (uint32)-1;
                break;
            }

            vterm_set_cwd(g_user_task_term, abspath);
            regs->eax = 0;
            break;
        }
        case SYSCALL_RUN: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_raw = (const char*)arg1;
            if (!user_raw) { regs->eax = (uint32)-1; break; }

            char raw_args[192];
            if (copyin_cstr(raw_args, sizeof(raw_args), user_raw) != 0) { regs->eax = (uint32)-1; break; }

            char full_line[196];
            full_line[0] = 'r';
            full_line[1] = 'u';
            full_line[2] = 'n';
            full_line[3] = ' ';
            full_line[4] = '\0';
            strncat(full_line, raw_args, sizeof(full_line) - strlen(full_line) - 1);
            run_command(full_line);
            regs->eax = 0;
            break;
        }
        case SYSCALL_MMAP: {
            int read_only = ((int)arg3 != 0) ? 1 : 0;
            uint32 caps = read_only ? (CAP_READ_FS | CAP_ALLOC_MEMORY) : (CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY);
            if (!syscall_ctx_allow(caps, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }

            const char* user_path = (const char*)arg1;
            size_t* user_out_size = (size_t*)arg2;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }

            void* mapped_addr = NULL;
            size_t mapped_size = 0;
            if (eynfs_mmap(path, &mapped_addr, &mapped_size, (uint8_t)read_only) != 0 || !mapped_addr) {
                regs->eax = (uint32)-1;
                break;
            }

            if (user_out_size && copyout(user_out_size, &mapped_size, sizeof(mapped_size)) != 0) {
                (void)eynfs_munmap(mapped_addr, mapped_size);
                regs->eax = (uint32)-1;
                break;
            }

            uint32 user_addr = 0;
            if (syscall_addr_to_u32((uintptr)mapped_addr, &user_addr) != 0) {
                (void)eynfs_munmap(mapped_addr, mapped_size);
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = user_addr;
            break;
        }
        case SYSCALL_MUNMAP: {
            if (!syscall_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) { regs->eax = (uint32)-1; break; }
            uint32 user_addr = 0;
            if (syscall_addr_to_u32(arg1, &user_addr) != 0) { regs->eax = (uint32)-1; break; }
            void* addr = (void*)(uintptr)user_addr;
            regs->eax = (eynfs_munmap(addr, 0) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_MSYNC: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            uint32 user_addr = 0;
            if (syscall_addr_to_u32(arg1, &user_addr) != 0) { regs->eax = (uint32)-1; break; }
            void* addr = (void*)(uintptr)user_addr;
            regs->eax = (eynfs_msync(addr, 0) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_PAGING_GUARDS: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            paging_install_null_guard();
            paging_protect_kernel_text_ro();
            regs->eax = 0;
            break;
        }
        case SYSCALL_PANIC: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if ((int)arg1 != 1) { regs->eax = (uint32)-1; break; }
            PANIC("manual panic via ring3 syscall");
            regs->eax = 0;
            break;
        }
        case SYSCALL_PF: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if ((int)arg3 != 1) { regs->eax = (uint32)-1; break; }

            uint32 user_addr = 0;
            if (syscall_addr_to_u32(arg1, &user_addr) != 0) { regs->eax = (uint32)-1; break; }

            uintptr addr = (uintptr)user_addr;
            int mode = (int)arg2;
            if (mode == 1) {
                *(volatile uint32*)addr = 0x12345678u;
            } else if (mode == 2) {
                void (*fn)(void) = (void (*)(void))addr;
                fn();
            } else {
                volatile uint32 tmp = *(volatile uint32*)addr;
                (void)tmp;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_RING3: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) { regs->eax = (uint32)-1; break; }
            if ((int)arg1 != 1) { regs->eax = (uint32)-1; break; }

            regs->eax = (uint32)-1;
            break;
        }
        case SYSCALL_WRITE: {
            const char* user_buf = (const char*)arg2;
            int len = (int)arg3;
            int req_fd = (int)arg1;
            int fd = req_fd;
            if (req_fd == 1) fd = g_user_stdio_stdout_fd;
            else if (req_fd == 2) fd = g_user_stdio_stderr_fd;

            /*
             * SECURITY-INVARIANT: Stdout/stderr remap must never permanently
             * black-hole user output.
             *
             * If a remapped fd points at a closed/non-existent descriptor,
             * fall back to the canonical console fd so interactive commands
             * remain observable.
             */
            if ((req_fd == 1 || req_fd == 2) && fd > 2) {
                if (!user_fd_get(fd)) {
                    fd = req_fd;
                }
            }

            if (len < 0 || !user_buf) { regs->eax = (uint32)-1; break; }
            if (len == 0) { regs->eax = 0; break; }
            if (len > SYSCALL_IO_MAX) len = SYSCALL_IO_MAX;

            if (fd == 1 || fd == 2) {
                if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
                int pos = 0;
                while (pos < len) {
                    syscall_ctx_account(SCHED_COST_CONSOLE);
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
                regs->eax = (uint32)len;
                break;
            }

            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd) { regs->eax = (uint32)-1; break; }
            if (ufd->kind == USER_FD_KIND_PIPE) {
                int n = user_pipe_write(ufd->pipe_id, user_buf, len, ufd->nonblock);
                regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
                break;
            }

            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int n = syscall_write_file_from_fd(ufd, user_buf, len);
            regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
            break;
        }
        case SYSCALL_READ: {
            if (arg3 == 0) { regs->eax = 0; break; }
            if (arg2 == 0) { regs->eax = (uint32)-1; break; }
            int req_fd = (int)arg1;
            int fd = (req_fd == 0) ? g_user_stdio_stdin_fd : req_fd;
            int maxlen = (int)arg3;
            if (maxlen < 0) { regs->eax = (uint32)-1; break; }
            if (maxlen > SYSCALL_IO_MAX) maxlen = SYSCALL_IO_MAX;

            /* Same defensive fallback as SYSCALL_WRITE for remapped stdin. */
            if (req_fd == 0 && fd > 2) {
                if (!user_fd_get(fd)) {
                    fd = 0;
                }
            }

            if (fd == 0) {
                if (!syscall_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
                static int s_stdin_debug_once = 0;
                int stdin_term = syscall_current_task_term();
                if (!s_stdin_debug_once) {
                    s_stdin_debug_once = 1;
                    printf("%c[SYSCALL:READ] fd=0 buf=0x%X len=%d useresp=0x%X\n",
                           255, 255, 0, (unsigned)arg2, maxlen, (unsigned)regs->useresp);
                }

                // When tiling is active, use the vterm stdin buffer instead of
                // polling the keyboard directly. The TUI routes input to this buffer.
                const char* s = NULL;
                int slen = 0;
                
                if (tile_is_tiling_active() && stdin_term >= 0) {
                    int term = stdin_term;  // Cache to avoid race conditions
                    
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
                        // A ring3 task may legitimately block waiting for input;
                        // keep the watchdog from firing while we're alive and
                        // PIT IRQ0 is pumping UI/input.
                        watchdog_kick("sys-read");
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
                if (tile_is_tiling_active() && stdin_term >= 0) {
                    vterm_stdin_consume(stdin_term);
                }
            stdin_done:
                break;
            }

            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd) { regs->eax = (uint32)-1; break; }

            if (ufd->kind == USER_FD_KIND_PIPE) {
                int n = user_pipe_read(ufd->pipe_id, (char*)arg2, maxlen, ufd->nonblock);
                regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
                break;
            }

            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            if (ufd->is_dir) { regs->eax = (uint32)-1; break; }

            int n = syscall_read_file(ufd, (char*)arg2, maxlen);
            regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
            break;
        }
        case SYSCALL_OPEN: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            int open_flags = (int)arg2;
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
            uint8 drive = g_current_drive;
            const char* explicit_path = NULL;
            char abspath[128];
            if (syscall_parse_explicit_drive_prefix(path, &drive, &explicit_path)) {
                strncpy(abspath, explicit_path, sizeof(abspath) - 1);
                abspath[sizeof(abspath) - 1] = '\0';
            } else {
                resolve_path(path, cwd, abspath, sizeof(abspath));
            }

            int fifo_id = user_pipe_get_or_create_fifo(abspath);
            if (fifo_id >= 0) {
                int fd = user_fd_alloc();
                if (fd < 0) { regs->eax = (uint32)-1; break; }

                user_fd_t* ufd = &g_user_fds[fd];
                ufd->kind = USER_FD_KIND_PIPE;
                ufd->pipe_id = fifo_id;
                ufd->pipe_end = ((open_flags & (OPEN_FLAG_WRONLY | OPEN_FLAG_RDWR)) != 0)
                              ? USER_PIPE_END_WRITE
                              : USER_PIPE_END_READ;
                ufd->nonblock = ((open_flags & OPEN_FLAG_NONBLOCK) != 0) ? 1 : 0;
                ufd->is_dir = 0;
                ufd->size = 0;
                ufd->offset = 0;
                ufd->dir_pos = 0;
                ufd->path[0] = '\0';
                ufd->drive = drive;
                if (ufd->pipe_end == USER_PIPE_END_WRITE) g_user_pipes[fifo_id].writer_refs++;
                else g_user_pipes[fifo_id].reader_refs++;

                regs->eax = (uint32)fd;
                break;
            }

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
            ufd->kind = USER_FD_KIND_FILE;
            ufd->pipe_id = -1;
            ufd->pipe_end = -1;
            ufd->drive = drive;
            strncpy(ufd->path, abspath, sizeof(ufd->path) - 1);
            ufd->path[sizeof(ufd->path) - 1] = '\0';
            ufd->offset = 0;
            ufd->dir_pos = 0;
            ufd->is_dir = (st.type == VFS_NODE_DIR);
            ufd->nonblock = 0;
            ufd->size = st.size;

            /* Pre-populate the EYNFS block cursor so sequential reads pay
             * zero traversal overhead after the first call.
             *
             * SECURITY-INVARIANT: This fast-path is ATA-backed EYNFS only.
             * RAM:/ is served by the VFS RAM backend and must not call
             * eynfs_read_superblock()/eynfs_traverse_path(), which assume an
             * ATA-readable superblock LBA and can fault in kernel mode when
             * given a non-ATA pseudo-drive.
             */
            ufd->eynfs_first_block = 0;
            ufd->cur_block = 0;
            ufd->cur_block_off = 0;
            if (!ufd->is_dir && fs == VFS_FS_EYNFS && drive != VFS_DRIVE_RAM) {
                eynfs_superblock_t sb_tmp;
                eynfs_dir_entry_t ent_tmp;
                uint32_t pb_tmp = 0, ei_tmp = 0;
                if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb_tmp) == 0 &&
                    sb_tmp.magic == EYNFS_MAGIC &&
                    eynfs_traverse_path(drive, &sb_tmp, abspath, &ent_tmp, &pb_tmp, &ei_tmp) == 0 &&
                    ent_tmp.type == EYNFS_TYPE_FILE) {
                    ufd->eynfs_first_block = ent_tmp.first_block;
                }
            }

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
            regs->eax = (user_fd_release(fd) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_DUP: {
            int oldfd_req = (int)arg1;
            int oldfd = oldfd_req;
            if (oldfd_req == 0) oldfd = g_user_stdio_stdin_fd;
            else if (oldfd_req == 1) oldfd = g_user_stdio_stdout_fd;
            else if (oldfd_req == 2) oldfd = g_user_stdio_stderr_fd;

            if (oldfd <= 2 || oldfd < 0 || oldfd >= USER_FD_MAX || !g_user_fds[oldfd].used) {
                regs->eax = (uint32)-1;
                break;
            }

            int newfd = -1;
            for (int i = 3; i < USER_FD_MAX; ++i) {
                if (!g_user_fds[i].used) {
                    newfd = i;
                    break;
                }
            }
            if (newfd < 0) {
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = (uint32)user_fd_duplicate_into(oldfd, newfd);
            break;
        }
        case SYSCALL_DUP2: {
            int oldfd_req = (int)arg1;
            int newfd = (int)arg2;
            int oldfd = oldfd_req;
            if (oldfd_req == 0) oldfd = g_user_stdio_stdin_fd;
            else if (oldfd_req == 1) oldfd = g_user_stdio_stdout_fd;
            else if (oldfd_req == 2) oldfd = g_user_stdio_stderr_fd;

            if (oldfd <= 2 || oldfd < 0 || oldfd >= USER_FD_MAX || !g_user_fds[oldfd].used) {
                regs->eax = (uint32)-1;
                break;
            }
            if (newfd < 0 || newfd >= USER_FD_MAX) {
                regs->eax = (uint32)-1;
                break;
            }

            if (newfd == 0) {
                g_user_stdio_stdin_fd = oldfd;
                regs->eax = 0;
                break;
            }
            if (newfd == 1) {
                g_user_stdio_stdout_fd = oldfd;
                regs->eax = 1;
                break;
            }
            if (newfd == 2) {
                g_user_stdio_stderr_fd = oldfd;
                regs->eax = 2;
                break;
            }

            regs->eax = (uint32)user_fd_duplicate_into(oldfd, newfd);
            break;
        }
        case SYSCALL_PIPE: {
            int* user_pipefd = (int*)arg1;
            if (!user_pipefd) { regs->eax = (uint32)-1; break; }
            int read_fd = -1;
            int write_fd = -1;
            if (syscall_kernel_pipe_create(&read_fd, &write_fd) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            int out[2];
            out[0] = read_fd;
            out[1] = write_fd;
            if (copyout(user_pipefd, out, sizeof(out)) != 0) {
                (void)user_fd_release(read_fd);
                (void)user_fd_release(write_fd);
                regs->eax = (uint32)-1;
                break;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_MKFIFO: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            vfs_stat_t existing;
            if (vfs_stat(g_current_drive, abspath, &existing) == 0) {
                if (existing.type == VFS_NODE_FIFO) {
                    if (user_pipe_get_or_create_fifo(abspath) >= 0) {
                        regs->eax = 0;
                        break;
                    }
                }
                regs->eax = (uint32)-1;
                break;
            }

            if (user_pipe_find_fifo_by_path(abspath) >= 0) {
                regs->eax = 0;
                break;
            }

            /* Ensure parent path exists in VFS to keep FIFO namespace intuitive. */
            char parent[128];
            strncpy(parent, abspath, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char* slash = strrchr(parent, '/');
            if (!slash) { regs->eax = (uint32)-1; break; }
            if (slash == parent) {
                parent[1] = '\0';
            } else {
                *slash = '\0';
            }

            vfs_stat_t pst;
            if (vfs_stat(g_current_drive, parent, &pst) != 0 || pst.type != VFS_NODE_DIR) {
                regs->eax = (uint32)-1;
                break;
            }

            int marker_rc = vfs_write_file(g_current_drive,
                                           abspath,
                                           FIFO_METADATA_MARKER,
                                           FIFO_METADATA_MARKER_LEN);
            if (marker_rc < 0) {
                regs->eax = (uint32)-1;
                break;
            }

            int pipe_id = user_pipe_alloc(1, abspath);
            regs->eax = (pipe_id >= 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_FD_SET_INHERIT: {
            int prev = g_user_fd_inherit_mode;
            syscall_set_user_fd_inherit_mode(((int)arg1) != 0 ? 1 : 0);
            regs->eax = (uint32)prev;
            break;
        }
        case SYSCALL_FD_SET_STDIO: {
            int in_fd = (int)arg1;
            int out_fd = (int)arg2;
            int err_fd = (int)arg3;

            if (in_fd < 0 || in_fd >= USER_FD_MAX ||
                out_fd < 0 || out_fd >= USER_FD_MAX ||
                err_fd < 0 || err_fd >= USER_FD_MAX) {
                regs->eax = (uint32)-1;
                break;
            }

            if (in_fd > 2 && !g_user_fds[in_fd].used) { regs->eax = (uint32)-1; break; }
            if (out_fd > 2 && !g_user_fds[out_fd].used) { regs->eax = (uint32)-1; break; }
            if (err_fd > 2 && !g_user_fds[err_fd].used) { regs->eax = (uint32)-1; break; }

            syscall_set_user_stdio_fds(in_fd, out_fd, err_fd);
            regs->eax = 0;
            break;
        }
        case SYSCALL_FD_SET_NONBLOCK: {
            int fd = (int)arg1;
            int enabled = (int)arg2;
            regs->eax = (syscall_kernel_set_user_fd_nonblock(fd, enabled ? 1 : 0) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_SPAWN: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }

            const char* user_path = (const char*)arg1;
            const char* const* user_argv = (const char* const*)arg2;
            int argc = (int)arg3;
            if (!user_path || argc < 0) { regs->eax = (uint32)-1; break; }
            if (argc > 0 && !user_argv) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }

            const int max_args = 16;
            const int max_arg_len = 64;
            char arg_buf[16][64];
            const char* kargv[16];
            int kargc = 0;

            if (argc > max_args) argc = max_args;
            for (int i = 0; i < argc; ++i) {
                const char* user_arg_ptr = NULL;
                if (copyin(&user_arg_ptr, user_argv + i, sizeof(user_arg_ptr)) != 0) {
                    regs->eax = (uint32)-1;
                    return regs->eax;
                }
                if (!user_arg_ptr) continue;
                if (copyin_cstr(arg_buf[kargc], (size_t)max_arg_len, user_arg_ptr) != 0) {
                    regs->eax = (uint32)-1;
                    return regs->eax;
                }
                kargv[kargc] = arg_buf[kargc];
                kargc++;
            }

            const char* cwd = "/";
            if (g_user_task_active) {
                const char* t = vterm_get_cwd(g_user_task_term);
                if (t && t[0] == '/') cwd = t;
            }

            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            int pid = user_task_spawn_argv(g_current_drive, abspath, kargc, kargv);
            regs->eax = (pid > 0) ? (uint32)pid : (uint32)-1;
            break;
        }
        case SYSCALL_WAITPID: {
            int pid = (int)arg1;
            int* user_status = (int*)arg2;
            int flags = (int)arg3;
            int status = 0;
            int waited = user_task_waitpid(pid, &status, flags);
            if (waited < 0) {
                regs->eax = (uint32)-1;
                break;
            }
            if (waited > 0 && user_status) {
                if (copyout(user_status, &status, sizeof(status)) != 0) {
                    regs->eax = (uint32)-1;
                    break;
                }
            }
            regs->eax = (uint32)waited;
            break;
        }
        case SYSCALL_CAP_MINT_FD: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            int fd = (int)arg1;
            uint32 req_rights = (uint32)arg2;
            void* user_cap_out = (void*)arg3;
            if (!user_cap_out) { regs->eax = (uint32)-1; break; }

            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd) { regs->eax = (uint32)-1; break; }

            uint32 allowed = CAP_R_READ | CAP_R_WRITE | CAP_R_SEEK | CAP_R_CLOSE;
            uint32 rights = req_rights ? (req_rights & allowed) : allowed;
            if (rights == 0) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (cap_mint(&cap, ufd, CAP_OBJ_USER_FD, rights) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            if (copyout(user_cap_out, &cap, sizeof(cap)) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_FD_READ: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            char* user_buf = (char*)arg2;
            int maxlen = (int)arg3;
            if (maxlen < 0) { regs->eax = (uint32)-1; break; }
            if (maxlen > SYSCALL_IO_MAX) maxlen = SYSCALL_IO_MAX;

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            user_fd_t* ufd = user_fd_from_cap(&cap, CAP_R_READ, NULL);
            if (!ufd) { regs->eax = (uint32)-1; break; }

            int n = (ufd->kind == USER_FD_KIND_PIPE)
                ? user_pipe_read(ufd->pipe_id, user_buf, maxlen, ufd->nonblock)
                : syscall_read_file(ufd, user_buf, maxlen);
            regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
            break;
        }
        case SYSCALL_CAP_FD_CLOSE: {
            const void* user_cap_ptr = (const void*)arg1;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            int fd = -1;
            user_fd_t* ufd = user_fd_from_cap(&cap, CAP_R_CLOSE, &fd);
            if (!ufd || fd <= 2) { regs->eax = (uint32)-1; break; }
            regs->eax = (user_fd_release(fd) == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_CAP_FD_WRITE: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_buf = (const void*)arg2;
            int len = (int)arg3;
            if (len < 0) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            user_fd_t* ufd = user_fd_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!ufd) { regs->eax = (uint32)-1; break; }

            int written = (ufd->kind == USER_FD_KIND_PIPE)
                        ? user_pipe_write(ufd->pipe_id, user_buf, len, ufd->nonblock)
                        : syscall_write_file_from_fd(ufd, user_buf, len);
            regs->eax = (written < 0) ? (uint32)-1 : (uint32)written;
            break;
        }
        case SYSCALL_CAP_FD_SEEK: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            int32 offset = (int32)arg2;
            int whence = (int)arg3;

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            user_fd_t* ufd = user_fd_from_cap(&cap, CAP_R_SEEK, NULL);
            if (!ufd) { regs->eax = (uint32)-1; break; }

            int next = syscall_seek_fd(ufd, offset, whence);
            regs->eax = (next < 0) ? (uint32)-1 : (uint32)next;
            break;
        }
        case SYSCALL_GETDENTS: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
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

        case SYSCALL_WRITEFILE: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            const void* user_buf = (const void*)arg2;
            int len = (int)arg3;

            if (!user_path || !user_buf || len < 0) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            if (!cwd || cwd[0] != '/') {
                cwd = "/";
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            uint8 drive = g_current_drive;

            if (SYSCALL_WRITEFILE_DEBUG) {
                printf("[SYSCALL:WRITEFILE] req='%s' cwd='%s' => '%s' len=%d drive=%d\n",
                       path, cwd ? cwd : "(null)", abspath, len, (int)drive);
            }

            // Special-case empty writes: still create/truncate the file.
            // `vfs_write_file` requires a non-NULL buffer pointer even when size==0.
            if (len == 0) {
                const uint8 dummy = 0;
                int written = vfs_write_file(drive, abspath, &dummy, 0);
                regs->eax = (written < 0) ? (uint32)-1 : (uint32)written;
                break;
            }

            // Stream the user buffer into the filesystem to avoid large kernel allocations.
            // For now, implement streaming writes on EYNFS. FAT32 falls back to the
            // whole-buffer write path.
            if (vfs_detect(drive) == VFS_FS_EYNFS) {
                eynfs_stream_t s;
                if (eynfs_stream_begin(drive, abspath, &s) != 0) {
                    if (SYSCALL_WRITEFILE_DEBUG) {
                        printf("[SYSCALL:WRITEFILE] eynfs_stream_begin failed for '%s'\n", abspath);
                    }
                    regs->eax = (uint32)-1;
                    break;
                }

                int rc = 0;
                int pos = 0;
                while (pos < len) {
                    int chunk = len - pos;
                    if (chunk > 4096) chunk = 4096;
                    uint8 tmp[4096];
                    if (copyin(tmp, (const uint8*)user_buf + pos, (size_t)chunk) != 0) {
                        rc = -1;
                        break;
                    }
                    if (eynfs_stream_write(&s, tmp, (size_t)chunk) < 0) {
                        rc = -1;
                        break;
                    }
                    pos += chunk;
                }

                if (rc == 0) {
                    if (eynfs_stream_end(&s) != 0) {
                        rc = -1;
                    }
                }

                if (SYSCALL_WRITEFILE_DEBUG) {
                    printf("[SYSCALL:WRITEFILE] eynfs rc=%d wrote=%d\n", rc, (rc == 0) ? len : -1);
                }
                regs->eax = (rc == 0) ? (uint32)len : (uint32)-1;
                break;
            }

            // FAT32 (or unknown FS): fall back to whole-buffer write.
            // Keep memory usage bounded; EYN-OS often runs with small RAM.
            const int max_len = 256 * 1024;
            if (len > max_len) {
                regs->eax = (uint32)-1;
                break;
            }

            uint8* kbuf = (uint8*)malloc((size_t)len);
            if (!kbuf) { regs->eax = (uint32)-1; break; }
            if (copyin(kbuf, user_buf, (size_t)len) != 0) {
                free(kbuf);
                regs->eax = (uint32)-1;
                break;
            }

            int written = vfs_write_file(drive, abspath, kbuf, (uint32)len);
            free(kbuf);
            if (SYSCALL_WRITEFILE_DEBUG) {
                printf("[SYSCALL:WRITEFILE] vfs_write_file rc=%d\n", written);
            }
            regs->eax = (uint32)written;
            break;
        }

        case SYSCALL_MKDIR: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            if (!cwd || cwd[0] != '/') {
                cwd = "/";
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            uint8 drive = g_current_drive;

            int rc = vfs_mkdir(drive, abspath);
            regs->eax = (rc == 0) ? 0u : (uint32)-1;
            break;
        }

        case SYSCALL_UNLINK: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            if (!cwd || cwd[0] != '/') {
                cwd = "/";
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            uint8 drive = g_current_drive;

            int rc = vfs_unlink(drive, abspath);
            regs->eax = (rc == 0) ? 0u : (uint32)-1;
            break;
        }

        case SYSCALL_RMDIR: {
            if (!syscall_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const char* user_path = (const char*)arg1;
            if (!user_path) { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(path);

            const char* cwd = "/";
            if (g_user_task_active) {
                cwd = vterm_get_cwd(g_user_task_term);
            }
            if (!cwd || cwd[0] != '/') {
                cwd = "/";
            }
            char abspath[128];
            resolve_path(path, cwd, abspath, sizeof(abspath));

            uint8 drive = g_current_drive;

            int rc = vfs_rmdir(drive, abspath);
            regs->eax = (rc == 0) ? 0u : (uint32)-1;
            break;
        }
        case SYSCALL_SLEEP_US: {
            uint32 usec = (uint32)arg1;
            // Cooperative sleep to allow other tasks (UI/tiler) to run.
            // Uses a busy-wait fallback if no timer is configured.
            sched_sleep_us(usec);
            regs->eax = 0;
            break;
        }
        case SYSCALL_DET_ENABLE: {
            int enabled = (int)arg1;
            sched_det_enable(enabled ? 1 : 0);
            regs->eax = 0;
            break;
        }
        case SYSCALL_DET_STEP: {
            uint32 max_events = (uint32)arg1;
            int processed = sched_det_step(max_events);
            regs->eax = (uint32)processed;
            break;
        }
        case SYSCALL_GUI_CREATE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_create(title_ptr=arg1, status_left_ptr=arg2)
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int owner_pid = user_task_get_running_pid();
            if (owner_pid <= 0) { regs->eax = (uint32)-1; break; }

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

            int handle = user_gui_alloc_handle(owner_pid, 0);
            if (handle < 0) { regs->eax = (uint32)-1; break; }

            // Allocate persistent strings (tiler stores pointers)
            user_gui_t* e = &g_user_guis[handle];
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
			e->font_handle = vga_system_font_acquire();
            if (!e->title) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }

            int new_win = wm_create_window(e->title, 100 + (handle - 1) * 24,
                                           60  + (handle - 1) * 24, 700, 480,
                                           e->status_left);
            if (new_win < 0) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }
            e->win_id = new_win;
            e->is_floating = 1;
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;
            wm_register_gui_client2(new_win, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uintptr)handle);
            wm_register_gui_close_cb(new_win, user_gui_close_cb);
            wm_invalidate_window(new_win);
            regs->eax = (uint32)handle;
            break;
        }
        case SYSCALL_CAP_GUI_CREATE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const char* user_title = (const char*)arg1;
            const char* user_status = (const char*)arg2;
            void* user_cap_out = (void*)arg3;
            if (!user_title || !user_cap_out) { regs->eax = (uint32)-1; break; }
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int owner_pid = user_task_get_running_pid();
            if (owner_pid <= 0) { regs->eax = (uint32)-1; break; }

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

            int handle = user_gui_alloc_handle(owner_pid, 0);
            if (handle < 0) { regs->eax = (uint32)-1; break; }

            user_gui_t* e = &g_user_guis[handle];
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
            e->font_handle = vga_system_font_acquire();
            if (!e->title) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }

            int cap_win = wm_create_window(e->title, 100 + (handle - 1) * 24,
                                            60  + (handle - 1) * 24, 700, 480,
                                            e->status_left);
            if (cap_win < 0) {
                user_gui_free_entry(e);
                regs->eax = (uint32)-1;
                break;
            }
            e->win_id = cap_win;
            e->is_floating = 1;
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;
            wm_register_gui_client2(cap_win, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uintptr)handle);
            wm_register_gui_close_cb(cap_win, user_gui_close_cb);
            wm_invalidate_window(cap_win);

            uint32 rights = CAP_R_READ | CAP_R_WRITE | CAP_R_CLOSE;
            cap_t cap;
            if (cap_mint(&cap, e, CAP_OBJ_USER_GUI, rights) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            if (copyout(user_cap_out, &cap, sizeof(cap)) != 0) {
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_MINT_GUI: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            uint32 req_rights = (uint32)arg2;
            void* user_cap_out = (void*)arg3;
            if (!user_cap_out) { regs->eax = (uint32)-1; break; }

            user_gui_t* e = user_gui_get(handle);
            if (!e) { regs->eax = (uint32)-1; break; }

            uint32 allowed = CAP_R_READ | CAP_R_WRITE | CAP_R_CLOSE;
            uint32 rights = req_rights ? (req_rights & allowed) : allowed;
            if (rights == 0) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (cap_mint(&cap, e, CAP_OBJ_USER_GUI, rights) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            if (copyout(user_cap_out, &cap, sizeof(cap)) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_SET_TITLE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_set_title(handle=arg1, title_ptr=arg2)
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const char* user_title = (const char*)arg2;
            if (!user_title) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);

            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            char* new_title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            if (!new_title) { regs->eax = (uint32)-1; break; }
            if (e->title) free(e->title);
            e->title = new_title;
            if (e->is_floating) {
                wm_set_title_status(e->win_id, e->title, e->status_left, NULL);
            } else {
                tile_set_title_status(e->tile_idx, e->title, e->status_left, NULL);
                tile_invalidate_decorations(e->tile_idx);
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_ATTACH: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_attach(title_ptr=arg1, status_left_ptr=arg2)
            // Binds a GUI client to the tile backing the current ring3 task.
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int owner_pid = user_task_get_running_pid();
            if (owner_pid <= 0) { regs->eax = (uint32)-1; break; }

            int tile_idx = -1;
            if (g_user_task_term >= 0) tile_idx = tile_find_by_term(g_user_task_term);
            if (tile_idx < 0) { regs->eax = (uint32)-1; break; }

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
                if (copyin_cstr(status_tmp, sizeof(status_tmp), user_status) != 0) status_tmp[0] = '\0';
                trim_trailing_crlf(status_tmp);
            }

            int existing_self_handle = user_gui_find_self_handle_by_pid(owner_pid);
            if (existing_self_handle > 0) {
                user_gui_free_entry(&g_user_guis[existing_self_handle]);
            }

            int self_handle = user_gui_alloc_handle(owner_pid, 1);
            if (self_handle < 0) { regs->eax = (uint32)-1; break; }

            user_gui_t* e = &g_user_guis[self_handle];
            e->tile_idx = tile_idx;
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
			e->font_handle = vga_system_font_acquire();
            if (!e->title) { user_gui_free_entry(e); regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;

            tile_set_title_status(tile_idx, e->title, e->status_left, NULL);
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uintptr)self_handle);
            tile_register_gui_close_cb(tile_idx, user_gui_close_cb);
            tile_invalidate_decorations(tile_idx);
            tile_invalidate_gui(tile_idx);
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_ATTACH: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const char* user_title = (const char*)arg1;
            const char* user_status = (const char*)arg2;
            void* user_cap_out = (void*)arg3;
            if (!user_title || !user_cap_out) { regs->eax = (uint32)-1; break; }
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }
            int owner_pid = user_task_get_running_pid();
            if (owner_pid <= 0) { regs->eax = (uint32)-1; break; }

            int tile_idx = -1;
            if (g_user_task_term >= 0) tile_idx = tile_find_by_term(g_user_task_term);
            if (tile_idx < 0) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);
            if (!title_tmp[0]) strncpy(title_tmp, "User App", sizeof(title_tmp) - 1);
            title_tmp[sizeof(title_tmp) - 1] = '\0';

            char status_tmp[64];
            status_tmp[0] = '\0';
            if (user_status) {
                if (copyin_cstr(status_tmp, sizeof(status_tmp), user_status) != 0) status_tmp[0] = '\0';
                trim_trailing_crlf(status_tmp);
            }

            int existing_self_handle = user_gui_find_self_handle_by_pid(owner_pid);
            if (existing_self_handle > 0) {
                user_gui_free_entry(&g_user_guis[existing_self_handle]);
            }

            int self_handle = user_gui_alloc_handle(owner_pid, 1);
            if (self_handle < 0) { regs->eax = (uint32)-1; break; }

            user_gui_t* e = &g_user_guis[self_handle];
            e->tile_idx = tile_idx;
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
            e->font_handle = vga_system_font_acquire();
            if (!e->title) { user_gui_free_entry(e); regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;

            tile_set_title_status(tile_idx, e->title, e->status_left, NULL);
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uintptr)self_handle);
            tile_register_gui_close_cb(tile_idx, user_gui_close_cb);
            tile_invalidate_decorations(tile_idx);
            tile_invalidate_gui(tile_idx);

            uint32 rights = CAP_R_READ | CAP_R_WRITE | CAP_R_CLOSE;
            cap_t cap;
            if (cap_mint(&cap, e, CAP_OBJ_USER_GUI, rights) != 0) {
                regs->eax = (uint32)-1;
                break;
            }
            if (copyout(user_cap_out, &cap, sizeof(cap)) != 0) {
                regs->eax = (uint32)-1;
                break;
            }

            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_BEGIN: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_begin(handle)
            int handle = (int)arg1;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_CLEAR: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_clear(handle, rgb_ptr)
            int handle = (int)arg1;
            const void* user_rgb = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_rgb) { regs->eax = (uint32)-1; break; }
            typedef struct { uint8 r, g, b, _pad; } rgb_t;
            rgb_t rgb;
            if (copyin(&rgb, user_rgb, sizeof(rgb)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_CLEAR;
            c->r = rgb.r;
            c->g = rgb.g;
            c->b = rgb.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_FILL_RECT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_fill_rect(handle, rect_ptr)
            int handle = (int)arg1;
            const void* user_rect = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_rect) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y, w, h; uint8 r, g, b, _pad; } rect_t;
            rect_t rect;
            if (copyin(&rect, user_rect, sizeof(rect)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_FILL_RECT;
            c->x = (int)rect.x;
            c->y = (int)rect.y;
            c->w = (int)rect.w;
            c->h = (int)rect.h;
            c->r = rect.r;
            c->g = rect.g;
            c->b = rect.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_DRAW_TEXT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_draw_text(handle, textcmd_ptr)
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y; uint8 r, g, b, _pad; const char* text; } textcmd_t;
            textcmd_t t;
            if (copyin(&t, user_cmd, sizeof(t)) != 0) { regs->eax = (uint32)-1; break; }
            if (!t.text) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_TEXT;
            c->x = (int)t.x;
            c->y = (int)t.y;
            c->r = t.r;
            c->g = t.g;
            c->b = t.b;
            c->font_slot = 0;
            if (copyin_cstr(c->text, sizeof(c->text), t.text) != 0) {
                c->text[0] = '\0';
            }
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }

        case SYSCALL_GUI_DRAW_LINE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_draw_line(handle, line_ptr)
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x1, y1, x2, y2; uint8 r, g, b, _pad; } linecmd_t;
            linecmd_t l;
            if (copyin(&l, user_cmd, sizeof(l)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_LINE;
            c->x = (int)l.x1;
            c->y = (int)l.y1;
            c->x2 = (int)l.x2;
            c->y2 = (int)l.y2;
            c->r = l.r;
            c->g = l.g;
            c->b = l.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_DRAW_ICON: {
            /*
             * gui_draw_icon(handle, icon_cmd_ptr)
             *
             * Appends a USER_GUI_CMD_ICON to the command buffer.
             * The icon name (e.g. "file_c", "dir_empty") is copied into
             * the text[64] field; x,y give the draw position.
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE))
                { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd)
                { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y; const char* icon_name; } iconcmd_t;
            iconcmd_t ic;
            if (copyin(&ic, user_cmd, sizeof(ic)) != 0)
                { regs->eax = (uint32)-1; break; }
            if (!ic.icon_name)
                { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0])))
                { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_ICON;
            c->x = (int)ic.x;
            c->y = (int)ic.y;
            if (copyin_cstr(c->text, sizeof(c->text), ic.icon_name) != 0)
                c->text[0] = '\0';
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_OUTLINE_RECT: {
            /*
             * gui_outline_rect(handle, rect_ptr)
             *
             * Draws a 1-pixel outlined rectangle (no fill).
             * Uses the same rect_t layout as FILL_RECT.
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE))
                { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_rect = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_rect)
                { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y, w, h; uint8 r, g, b, _pad; } rect_t;
            rect_t rect;
            if (copyin(&rect, user_rect, sizeof(rect)) != 0)
                { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0])))
                { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_OUTLINE_RECT;
            c->x = (int)rect.x;
            c->y = (int)rect.y;
            c->w = (int)rect.w;
            c->h = (int)rect.h;
            c->r = rect.r;
            c->g = rect.g;
            c->b = rect.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_DRAW_CHAR: {
            /*
             * gui_draw_char(handle, charcmd_ptr)
             *
             * Draws a single character at pixel position (x,y) with the GUI's
             * current font. More efficient than gui_draw_text for single chars
             * because it avoids string copy overhead.
             * charcmd_t: { int32 x, y, ch; uint8 r, g, b, _pad; }
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE))
                { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd)
                { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y, ch; uint8 r, g, b, _pad; } charcmd_t;
            charcmd_t cc;
            if (copyin(&cc, user_cmd, sizeof(cc)) != 0)
                { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0])))
                { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_CHAR;
            c->x = (int)cc.x;
            c->y = (int)cc.y;
            c->w = (int)cc.ch;   /* reuse w field for character codepoint */
            c->r = cc.r;
            c->g = cc.g;
            c->b = cc.b;
            c->font_slot = 0;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }

        case SYSCALL_GUI_LOAD_FONT: {
            if (!syscall_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY | CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE))
                { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const char* user_path = (const char*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_path)
                { regs->eax = (uint32)-1; break; }

            char path[128];
            if (copyin_cstr(path, sizeof(path), user_path) != 0)
                { regs->eax = (uint32)-1; break; }
            if (!path[0])
                { regs->eax = (uint32)-1; break; }

            int new_font = vga_font_acquire_path(0, path);
            if (new_font <= 0)
                { regs->eax = (uint32)-1; break; }

            for (int i = 0; i < USER_GUI_EXTRA_FONTS; ++i) {
                if (e->extra_font_handles[i] == new_font) {
                    vga_font_release(new_font);
                    regs->eax = (uint32)(i + 1);
                    break;
                }
            }
            if ((int32)regs->eax > 0) break;

            int slot = -1;
            for (int i = 0; i < USER_GUI_EXTRA_FONTS; ++i) {
                if (e->extra_font_handles[i] <= 0) { slot = i; break; }
            }
            if (slot < 0) {
                vga_font_release(new_font);
                regs->eax = (uint32)-1;
                break;
            }

            e->extra_font_handles[slot] = new_font;
            regs->eax = (uint32)(slot + 1);
            break;
        }

        case SYSCALL_GUI_DRAW_TEXT_FONT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 font_id; int32 x, y; uint8 r, g, b, _pad; const char* text; } textcmd_font_t;
            textcmd_font_t t;
            if (copyin(&t, user_cmd, sizeof(t)) != 0) { regs->eax = (uint32)-1; break; }
            if (!t.text) { regs->eax = (uint32)-1; break; }
            if (t.font_id < 0 || t.font_id > USER_GUI_EXTRA_FONTS) { regs->eax = (uint32)-1; break; }
            if (t.font_id > 0 && user_gui_font_handle_from_slot(e, (int)t.font_id) <= 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }

            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_TEXT;
            c->font_slot = (int)t.font_id;
            c->x = (int)t.x;
            c->y = (int)t.y;
            c->r = t.r;
            c->g = t.g;
            c->b = t.b;
            if (copyin_cstr(c->text, sizeof(c->text), t.text) != 0) c->text[0] = '\0';
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }

        case SYSCALL_GUI_DRAW_CHAR_FONT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 font_id; int32 x, y, ch; uint8 r, g, b, _pad; } charcmd_font_t;
            charcmd_font_t cc;
            if (copyin(&cc, user_cmd, sizeof(cc)) != 0) { regs->eax = (uint32)-1; break; }
            if (cc.font_id < 0 || cc.font_id > USER_GUI_EXTRA_FONTS) { regs->eax = (uint32)-1; break; }
            if (cc.font_id > 0 && user_gui_font_handle_from_slot(e, (int)cc.font_id) <= 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }

            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_CHAR;
            c->font_slot = (int)cc.font_id;
            c->x = (int)cc.x;
            c->y = (int)cc.y;
            c->w = (int)cc.ch;
            c->r = cc.r;
            c->g = cc.g;
            c->b = cc.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_GET_FONT_METRICS: {
            /*
             * gui_get_font_metrics(handle, out_metrics_ptr)
             *
             * Returns the cell width and height (in pixels) of the GUI's current
             * font, enabling userland to perform accurate text layout.
             * metrics_t: { int32 char_w, char_h; }
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE))
                { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            void* user_out = (void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_out)
                { regs->eax = (uint32)-1; break; }
            typedef struct { int32 char_w, char_h; } font_metrics_t;
            font_metrics_t m;
            /*
             * For scalable fonts, width is the nominal max advance and height
             * is the active line step for the handle.
             */
            m.char_w = (int32)vga_font_advance_width(e->font_handle);
            m.char_h = (int32)vga_font_line_height(e->font_handle);
            if (m.char_w <= 0) m.char_w = 8;
            if (m.char_h <= 0) m.char_h = 8; /* fallback for built-in */
            if (copyout(user_out, &m, sizeof(m)) != 0)
                { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_PRESENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_present(handle)
            int handle = (int)arg1;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            if (e->is_floating) {
                wm_invalidate_window(e->win_id);
                /*
                 * While a ring-3 task owns the CPU, the main WM loop is paused.
                 * Floating-window invalidation alone only marks the window dirty;
                 * nothing repaints until some unrelated input path triggers a
                 * compositor frame.  Render immediately here so GUI clients that
                 * animate without input (audio progress bars, clocks, etc.) update
                 * continuously.
                 */
                tile_render_once();
            } else {
                tile_invalidate_gui(e->tile_idx);
                /* Render a single frame now so GUI updates appear without input events. */
                tile_render_once();
            }
            e->frame_pending = 0;
            regs->eax = 0;
            break;
        }

        case SYSCALL_GUI_GET_CONTENT_SIZE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_get_content_size(handle, out_size_ptr)
            int handle = (int)arg1;
            void* user_out = (void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_out) { regs->eax = (uint32)-1; break; }

            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (e->is_floating) {
                wm_get_content_rect(e->win_id, &cx, &cy, &cw, &ch);
            } else {
                tile_get_content_rect(e->tile_idx, &cx, &cy, &cw, &ch);
            }
            typedef struct { int32 w, h; } gui_size_t;
            gui_size_t s;
            s.w = (int32)cw;
            s.h = (int32)ch;
            if (copyout(user_out, &s, sizeof(s)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_SET_FONT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_set_font(handle, path_ptr). If path is NULL or empty, resets to the system default font.
            int handle = (int)arg1;
            const char* user_path = (const char*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }

            int new_font = vga_system_font_acquire();
            if (user_path) {
                char path[128];
                if (copyin_cstr(path, sizeof(path), user_path) == 0) {
                    trim_trailing_crlf(path);
                    if (path[0]) {
                        const char* cwd = "/";
                        if (g_user_task_active) cwd = vterm_get_cwd(g_user_task_term);
                        char abspath[128];
                        resolve_path(path, cwd, abspath, sizeof(abspath));
                        uint8 drive = g_current_drive;
                        int h = vga_font_acquire_path(drive, abspath);
                        if (h > 0) new_font = h;
                    }
                }
            }

            if (e->font_handle != new_font) {
                if (e->font_handle > 0) vga_font_release(e->font_handle);
                e->font_handle = new_font;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_SET_CONTINUOUS_REDRAW: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_set_continuous_redraw(handle, enabled)
            int handle = (int)arg1;
            int enabled = (int)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            if (e->is_floating) {
                wm_set_continuous_redraw(e->win_id, enabled ? 1 : 0);
            } else {
                tile_set_gui_continuous_redraw(e->tile_idx, enabled ? 1 : 0);
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_BLIT_RGB565: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_blit_rgb565(handle, blit_ptr)
            int handle = (int)arg1;
            const void* user_cmd = (const void*)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }

            typedef struct {
                int32 src_w;
                int32 src_h;
                const uint16* pixels;
                int32 dst_w;
                int32 dst_h;
            } gui_blit_rgb565_t;

            gui_blit_rgb565_t cmd;
            if (copyin(&cmd, user_cmd, sizeof(cmd)) != 0) { regs->eax = (uint32)-1; break; }
            if (!cmd.pixels || cmd.src_w <= 0 || cmd.src_h <= 0) { regs->eax = (uint32)-1; break; }

            const int max_w = 320;
            const int max_h = 200;
            if (cmd.src_w > max_w || cmd.src_h > max_h) { regs->eax = (uint32)-1; break; }

            size_t need = (size_t)cmd.src_w * (size_t)cmd.src_h * sizeof(uint16);
            if (need == 0 || need > (size_t)(max_w * max_h * 2)) { regs->eax = (uint32)-1; break; }

            if (!e->blit_buf || e->blit_w != cmd.src_w || e->blit_h != cmd.src_h) {
                if (e->blit_buf) { free(e->blit_buf); e->blit_buf = NULL; }
                e->blit_buf = (uint16*)malloc(need);
                if (!e->blit_buf) { regs->eax = (uint32)-1; break; }
                e->blit_w = cmd.src_w;
                e->blit_h = cmd.src_h;
            }

            if (copyin((uint8*)e->blit_buf, cmd.pixels, need) != 0) { regs->eax = (uint32)-1; break; }
            e->blit_dst_w = cmd.dst_w;
            e->blit_dst_h = cmd.dst_h;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_POLL_EVENT:
        case SYSCALL_GUI_WAIT_EVENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_poll_event(handle, out_event_ptr, out_size)
            // gui_wait_event(handle, out_event_ptr, out_size)
            int handle = (int)arg1;
            void* user_out = (void*)arg2;
            int out_sz = (int)arg3;
            user_gui_t* e = user_gui_get(handle);
            if (!e || !user_out || out_sz < (int)sizeof(user_gui_event_t)) { regs->eax = (uint32)-1; break; }

            // Back-end safety net: if a user GUI app queued draw commands but forgot
            // to call gui_present(), flush one frame before polling/waiting for input.
            user_gui_flush_pending_frame(e);

            user_gui_event_t ev;
            int have = 0;
            if (syscall_num == SYSCALL_GUI_WAIT_EVENT) {
                // Block until an event arrives.
                while (!(have = user_gui_pop_event(e, &ev))) {
                    if (g_user_interrupt) { regs->eax = (uint32)-1; goto gui_event_done; }
                    __asm__ __volatile__("sti");
                    __asm__ __volatile__("hlt");
                    __asm__ __volatile__("cli");
                }
            } else {
                have = user_gui_pop_event(e, &ev);
            }

            if (!have) { regs->eax = 0; break; }
            if (copyout(user_out, &ev, sizeof(ev)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 1;
        gui_event_done:
            break;
        }
        case SYSCALL_CAP_GUI_BEGIN: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_CLEAR: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_rgb = (const void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e) || !user_rgb) { regs->eax = (uint32)-1; break; }
            typedef struct { uint8 r, g, b, _pad; } rgb_t;
            rgb_t rgb;
            if (copyin(&rgb, user_rgb, sizeof(rgb)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_CLEAR;
            c->r = rgb.r;
            c->g = rgb.g;
            c->b = rgb.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_FILL_RECT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_rect = (const void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e) || !user_rect) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y, w, h; uint8 r, g, b, _pad; } rect_t;
            rect_t rect;
            if (copyin(&rect, user_rect, sizeof(rect)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_FILL_RECT;
            c->x = (int)rect.x;
            c->y = (int)rect.y;
            c->w = (int)rect.w;
            c->h = (int)rect.h;
            c->r = rect.r;
            c->g = rect.g;
            c->b = rect.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_DRAW_TEXT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_cmd = (const void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x, y; uint8 r, g, b, _pad; const char* text; } textcmd_t;
            textcmd_t t;
            if (copyin(&t, user_cmd, sizeof(t)) != 0) { regs->eax = (uint32)-1; break; }
            if (!t.text) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_TEXT;
            c->x = (int)t.x;
            c->y = (int)t.y;
            c->r = t.r;
            c->g = t.g;
            c->b = t.b;
            if (copyin_cstr(c->text, sizeof(c->text), t.text) != 0) {
                c->text[0] = '\0';
            }
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_DRAW_LINE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_cmd = (const void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }
            typedef struct { int32 x1, y1, x2, y2; uint8 r, g, b, _pad; } linecmd_t;
            linecmd_t l;
            if (copyin(&l, user_cmd, sizeof(l)) != 0) { regs->eax = (uint32)-1; break; }
            if (e->cmd_count >= (int)(sizeof(e->cmds) / sizeof(e->cmds[0]))) { regs->eax = (uint32)-1; break; }
            user_gui_cmd_t* c = &e->cmds[e->cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = USER_GUI_CMD_LINE;
            c->x = (int)l.x1;
            c->y = (int)l.y1;
            c->x2 = (int)l.x2;
            c->y2 = (int)l.y2;
            c->r = l.r;
            c->g = l.g;
            c->b = l.b;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_PRESENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            if (e->is_floating) {
                wm_invalidate_window(e->win_id);
            } else {
                tile_invalidate_gui(e->tile_idx);
                tile_render_once();
            }
            e->frame_pending = 0;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_GET_CONTENT_SIZE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            void* user_out = (void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_READ, NULL);
            if (!user_gui_active(e) || !user_out) { regs->eax = (uint32)-1; break; }

            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (e->is_floating) {
                wm_get_content_rect(e->win_id, &cx, &cy, &cw, &ch);
            } else {
                tile_get_content_rect(e->tile_idx, &cx, &cy, &cw, &ch);
            }
            typedef struct { int32 w, h; } gui_size_t;
            gui_size_t s;
            s.w = (int32)cw;
            s.h = (int32)ch;
            if (copyout(user_out, &s, sizeof(s)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_SET_TITLE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const char* user_title = (const char*)arg2;
            if (!user_title) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);

            char* new_title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            if (!new_title) { regs->eax = (uint32)-1; break; }
            if (e->title) free(e->title);
            e->title = new_title;
            if (e->is_floating) {
                wm_set_title_status(e->win_id, e->title, e->status_left, NULL);
            } else {
                tile_set_title_status(e->tile_idx, e->title, e->status_left, NULL);
                tile_invalidate_decorations(e->tile_idx);
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_SET_FONT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const char* user_path = (const char*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }

            int new_font = vga_system_font_acquire();
            if (user_path) {
                char path[128];
                if (copyin_cstr(path, sizeof(path), user_path) == 0) {
                    trim_trailing_crlf(path);
                    if (path[0]) {
                        const char* cwd = "/";
                        if (g_user_task_active) cwd = vterm_get_cwd(g_user_task_term);
                        char abspath[128];
                        resolve_path(path, cwd, abspath, sizeof(abspath));
                        uint8 drive = g_current_drive;
                        int h = vga_font_acquire_path(drive, abspath);
                        if (h > 0) new_font = h;
                    }
                }
            }

            if (e->font_handle != new_font) {
                if (e->font_handle > 0) vga_font_release(e->font_handle);
                e->font_handle = new_font;
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_SET_CONTINUOUS_REDRAW: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            int enabled = (int)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            if (e->is_floating) {
                wm_set_continuous_redraw(e->win_id, enabled ? 1 : 0);
            } else {
                tile_set_gui_continuous_redraw(e->tile_idx, enabled ? 1 : 0);
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_BLIT_RGB565: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            const void* user_cmd = (const void*)arg2;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!user_gui_active(e) || !user_cmd) { regs->eax = (uint32)-1; break; }

            typedef struct {
                int32 src_w;
                int32 src_h;
                const uint16* pixels;
                int32 dst_w;
                int32 dst_h;
            } gui_blit_rgb565_t;

            gui_blit_rgb565_t cmd;
            if (copyin(&cmd, user_cmd, sizeof(cmd)) != 0) { regs->eax = (uint32)-1; break; }
            if (!cmd.pixels || cmd.src_w <= 0 || cmd.src_h <= 0) { regs->eax = (uint32)-1; break; }

            const int max_w = 320;
            const int max_h = 200;
            if (cmd.src_w > max_w || cmd.src_h > max_h) { regs->eax = (uint32)-1; break; }

            size_t need = (size_t)cmd.src_w * (size_t)cmd.src_h * sizeof(uint16);
            if (need == 0 || need > (size_t)(max_w * max_h * 2)) { regs->eax = (uint32)-1; break; }

            if (!e->blit_buf || e->blit_w != cmd.src_w || e->blit_h != cmd.src_h) {
                if (e->blit_buf) { free(e->blit_buf); e->blit_buf = NULL; }
                e->blit_buf = (uint16*)malloc(need);
                if (!e->blit_buf) { regs->eax = (uint32)-1; break; }
                e->blit_w = cmd.src_w;
                e->blit_h = cmd.src_h;
            }

            if (copyin((uint8*)e->blit_buf, cmd.pixels, need) != 0) { regs->eax = (uint32)-1; break; }
            e->blit_dst_w = cmd.dst_w;
            e->blit_dst_h = cmd.dst_h;
            e->frame_pending = 1;
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_POLL_EVENT:
        case SYSCALL_CAP_GUI_WAIT_EVENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            void* user_out = (void*)arg2;
            int out_sz = (int)arg3;
            if (!user_out || out_sz < (int)sizeof(user_gui_event_t)) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_READ, NULL);
            if (!e) { regs->eax = (uint32)-1; break; }

            user_gui_flush_pending_frame(e);

            user_gui_event_t ev;
            int have = 0;
            if (syscall_num == SYSCALL_CAP_GUI_WAIT_EVENT) {
                while (!(have = user_gui_pop_event(e, &ev))) {
                    if (g_user_interrupt) { regs->eax = (uint32)-1; goto cap_gui_event_done; }
                    __asm__ __volatile__("sti");
                    __asm__ __volatile__("hlt");
                    __asm__ __volatile__("cli");
                }
            } else {
                have = user_gui_pop_event(e, &ev);
            }

            if (!have) { regs->eax = 0; break; }
            if (copyout(user_out, &ev, sizeof(ev)) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = 1;
        cap_gui_event_done:
            break;
        }
        case SYSCALL_CAP_GUI_CLOSE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }

            int handle = -1;
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_CLOSE, &handle);
            if (!e || handle <= 0) { regs->eax = (uint32)-1; break; }
            user_gui_free_entry(e);
            regs->eax = 0;
            break;
        }
        case SYSCALL_EXIT: {
            int exiting_pid = user_task_get_running_pid();
            if (exiting_pid > 0) {
                syscall_cleanup_user_resources_for_pid(exiting_pid);
            }
            user_task_notify_exit((int)arg1);
            g_user_interrupt = 0;

            /*
             * SECURITY-INVARIANT: GUI/FD/stream tables are currently shared
             * across user tasks. Global cleanup must only run when the last
             * live task exits.
             */
            if (!user_task_has_live_tasks()) {
                syscall_reset_user_guis();
                if (!g_user_fd_inherit_mode) {
                    syscall_reset_user_fds();
                }
                syscall_reset_user_streams();
                if (!g_user_fd_inherit_mode) {
                    syscall_reset_user_stdio_fds();
                }
            }
            regs->eax = 0;
            break;
        }
        case SYSCALL_GETKEY: {
            if (!syscall_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)kb_getchar_nonblocking();
            break;
        }
        case SYSCALL_GET_TICKS_MS: {
            /*
             * get_ticks_ms() -- return milliseconds elapsed since kernel boot.
             *
             * Computes ticks * 1000 / hz avoiding 32-bit overflow by splitting
             * into whole seconds and the fractional millisecond remainder.
             * Wraps at ~49.7 days (uint32 rollover), which is sufficient for
             * interactive userland timing.
             *
             * No capability required -- wall-clock time is not privileged.
             */
            uint32 ticks = sched_get_tick_count();
            uint32 hz    = sched_get_tick_hz();
            if (hz == 0) hz = 100;
            regs->eax = (ticks / hz) * 1000u + (ticks % hz) * 1000u / hz;
            break;
        }

        case SYSCALL_LSEEK: {
            /*
             * lseek(fd, offset, whence) -- reposition an open file descriptor.
             *
             * SECURITY-INVARIANT: fd is validated through user_fd_get() which
             * checks bounds and the 'used' flag; the file offset is clamped to
             * [0, ufd->size] by syscall_seek_fd() so no out-of-bounds kernel
             * reads can result from a malformed seek.
             *
             * args:  arg1 = int fd
             *        arg2 = int32 offset
             *        arg3 = int whence  (0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END)
             * returns: new file offset ≥ 0, or (uint32)-1 on error.
             */
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) {
                regs->eax = (uint32)-1;
                break;
            }
            int fd     = (int)arg1;
            int32 off  = (int32)arg2;
            int whence = (int)arg3;
            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd) { regs->eax = (uint32)-1; break; }
            int result = syscall_seek_fd(ufd, off, whence);
            regs->eax = (result < 0) ? (uint32)-1 : (uint32)result;
            break;
        }

        case SYSCALL_GUI_WARP_MOUSE: {
            /*
             * gui_warp_mouse(handle, x, y) -- move the physical cursor to
             * (x, y) relative to the window content area.
             *
             * SECURITY-INVARIANT: The caller must own the GUI handle and hold
             * CAP_DEV_INPUT so only the focused game process can move the
             * cursor.  Kernel-side clamping prevents out-of-screen warps.
             * The accumulated delta registers are zeroed after the warp to
             * suppress spurious motion events caused by the position jump.
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            int wx     = (int)arg2;   /* window-content-relative x */
            int wy     = (int)arg3;   /* window-content-relative y */
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (e->is_floating) {
                wm_get_content_rect(e->win_id, &cx, &cy, &cw, &ch);
            } else {
                tile_get_content_rect(e->tile_idx, &cx, &cy, &cw, &ch);
            }
            int abs_x = cx + wx;
            int abs_y = cy + wy;
            mouse_set_position(abs_x, abs_y);
            /* Zero accumulated deltas so this warp doesn't trigger a
             * spurious motion event on the next gui_poll_event call. */
            g_mouse_state.delta_x = 0;
            g_mouse_state.delta_y = 0;
            regs->eax = 0;
            break;
        }

        case SYSCALL_GUI_SET_CURSOR_VISIBLE: {
            /*
             * gui_set_cursor_visible(handle, visible) -- show or hide the
             * mouse cursor sprite.
             *
             * SECURITY-INVARIANT: Same capability gate as gui_warp_mouse.
             * The caller must own the GUI handle.
             */
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_DEV_INPUT, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            int handle = (int)arg1;
            int visible = (int)arg2;
            user_gui_t* e = user_gui_get(handle);
            if (!user_gui_active(e)) { regs->eax = (uint32)-1; break; }
            extern void gui_cursor_set_hidden(int hidden);
            gui_cursor_set_hidden(visible ? 0 : 1);
            regs->eax = 0;
            break;
        }

        /* ---- Audio (AC97) syscalls ---- */

        case SYSCALL_AUDIO_PROBE: {
            /*
             * Probe PCI bus for an AC97 audio controller.
             * CAPABILITY-REQUIRED: CAP_DEV_AUDIO.
             */
            if (!syscall_ctx_allow(CAP_DEV_AUDIO, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)ac97_probe();
            break;
        }

        case SYSCALL_AUDIO_INIT: {
            /*
             * Initialise the AC97 controller (DMA, codec, IRQ).
             * CAPABILITY-REQUIRED: CAP_DEV_AUDIO | CAP_ALLOC_MEMORY.
             */
            if (!syscall_ctx_allow(CAP_DEV_AUDIO | CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)ac97_init();
            break;
        }

        case SYSCALL_AUDIO_WRITE: {
            /*
             * Submit PCM data for playback.
             * CAPABILITY-REQUIRED: CAP_DEV_AUDIO.
             * SECURITY-INVARIANT: user buffer is validated via copyin.
             */
            if (!syscall_ctx_allow(CAP_DEV_AUDIO, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const void* user_buf = (const void*)arg1;
            int buf_size = (int)arg2;
            if (!user_buf || buf_size <= 0) { regs->eax = (uint32)-1; break; }
            if ((uint32)buf_size > AC97_DMA_BUF_SIZE) buf_size = (int)AC97_DMA_BUF_SIZE;

            uint8_t kbuf[AC97_DMA_BUF_SIZE];
            if (copyin(kbuf, user_buf, (size_t)buf_size) != 0) { regs->eax = (uint32)-1; break; }
            regs->eax = (uint32)ac97_write(kbuf, (size_t)buf_size);
            break;
        }

        case SYSCALL_AUDIO_STOP: {
            if (!syscall_ctx_allow(CAP_DEV_AUDIO, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            ac97_stop();
            regs->eax = 0;
            break;
        }

        case SYSCALL_AUDIO_IS_AVAILABLE: {
            /* No capability check -- informational query. */
            regs->eax = (uint32)ac97_is_available();
            break;
        }

        case SYSCALL_AUDIO_WRITE_BULK: {
            /*
             * Submit multiple 4 KB PCM chunks in one syscall, reducing
             * ring-3/ring-0 transitions by up to 8×.
             *
             * CAPABILITY-REQUIRED: CAP_DEV_AUDIO.
             * SECURITY-INVARIANT: each 4 KB chunk is validated via copyin.
             */
            if (!syscall_ctx_allow(CAP_DEV_AUDIO, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            const uint8_t* user_buf = (const uint8_t*)arg1;
            int total_size = (int)arg2;
            if (!user_buf || total_size <= 0) { regs->eax = (uint32)-1; break; }

            /*
             * Cap at 8 DMA buffers (32 KB) per syscall to bound time
             * spent with interrupts disabled.
             */
            int max_bytes = (int)(8u * AC97_DMA_BUF_SIZE);
            if (total_size > max_bytes) total_size = max_bytes;

            uint8_t kbuf[AC97_DMA_BUF_SIZE];
            int count = 0;
            int offset = 0;

            while (offset < total_size) {
                int chunk = total_size - offset;
                if (chunk > (int)AC97_DMA_BUF_SIZE) chunk = (int)AC97_DMA_BUF_SIZE;
                if (copyin(kbuf, user_buf + offset, (size_t)chunk) != 0) break;
                if (ac97_write(kbuf, (size_t)chunk) < 0) break;
                offset += chunk;
                count++;
            }

            regs->eax = (uint32)count;
            break;
        }

        default: {
            printf("%c[SYSCALL] Unknown syscall: %d\n", 255, 0, 0, syscall_num);
            regs->eax = (uint32)-1;
            break;
        }
    }

    (void)user_task_try_resume_from_syscall(regs);
    return regs->eax;
}

uint32 syscall_dispatch(regs_t* regs) {
    if (!regs) {
        return (uint32)-1;
    }

    return syscall_dispatch_core(regs,
                                 regs->eax,
                                 (uintptr)regs->ebx,
                                 (uintptr)regs->ecx,
                                 (uintptr)regs->edx,
                                 0,
                                 0);
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

