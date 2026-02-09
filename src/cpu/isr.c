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

#include <sched.h>

#include <tile_manager.h>
#include <terminals.h>
#include <mm/user_access.h>
#include <fs/vfs.h>
#include <fs_commands.h>
#include <eynfs.h>
#include <context.h>
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

    // ISR 7: #NM (Device Not Available). Used for lazy x87 enabling (CR0.TS).
    // Even if we don't use lazy switching yet, handling this avoids spurious
    // fatal errors if firmware/boot code left TS set.
    if (regs->int_no == 7) {
        extern void fpu_handle_nm(void);
        fpu_handle_nm();
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
    char text[64];
} user_gui_cmd_t;

enum {
    USER_GUI_CMD_CLEAR = 1,
    USER_GUI_CMD_FILL_RECT = 2,
    USER_GUI_CMD_TEXT = 3,
    USER_GUI_CMD_LINE = 4,
};

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
        if (g_user_fds[i].used) {
            cap_revoke_object(&g_user_fds[i], CAP_OBJ_USER_FD);
        }
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

    // Current font handle for this GUI context (0 = built-in fallback).
    int font_handle;

    // Immediate-mode command list. Draw callback replays this list.
    int cmd_count;
    user_gui_cmd_t cmds[48];

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

// handle 0 is reserved for the current user task's existing tile (g_user_task_term).
#define USER_GUI_MAX 8
static user_gui_t g_user_guis[USER_GUI_MAX];
static char* g_user_self_title = NULL;
static int g_user_self_tile_idx = -1;

static void user_gui_free_entry(user_gui_t* e) {
    if (!e) return;

    cap_revoke_object(e, CAP_OBJ_USER_GUI);

	if (e->font_handle > 0) {
		vga_font_release(e->font_handle);
		e->font_handle = 0;
	}
    if (e->blit_buf) {
        free(e->blit_buf);
        e->blit_buf = NULL;
    }
    e->blit_w = 0;
    e->blit_h = 0;
    e->blit_dst_w = 0;
    e->blit_dst_h = 0;
    if (e->tile_idx >= 0) {
        tile_unregister_gui_client(e->tile_idx);
        tile_close(e->tile_idx);
    }
    if (e->title) free(e->title);
    if (e->status_left) free(e->status_left);
    e->used = 0;
    e->tile_idx = -1;
    e->title = NULL;
    e->status_left = NULL;

    e->cmd_count = 0;
    e->ev_head = 0;
    e->ev_tail = 0;
}

static int user_gui_alloc_handle(void) {
    // start at 1; handle 0 reserved for "self"
    for (int i = 1; i < USER_GUI_MAX; ++i) {
        if (!g_user_guis[i].used) {
            g_user_guis[i].used = 1;
            g_user_guis[i].tile_idx = -1;
            g_user_guis[i].title = NULL;
            g_user_guis[i].status_left = NULL;
            g_user_guis[i].blit_buf = NULL;
            g_user_guis[i].blit_w = 0;
            g_user_guis[i].blit_h = 0;
            g_user_guis[i].blit_dst_w = 0;
            g_user_guis[i].blit_dst_h = 0;
            g_user_guis[i].cmd_count = 0;
            g_user_guis[i].ev_head = 0;
            g_user_guis[i].ev_tail = 0;
            return i;
        }
    }
    return -1;
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
    int handle = (int)(uint32)userdata;
    user_gui_t* e = user_gui_get(handle);
    if (!e || e->tile_idx != tile_idx) return;
    if (content_w <= 0 || content_h <= 0) return;

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
            for (int j = 0; c->text[j] && j < (int)sizeof(c->text); ++j) {
                drawCharAt_font(e->font_handle, x + j * 8, y, (int)(unsigned char)c->text[j], c->r, c->g, c->b);
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
    }
}

static void user_gui_key_cb(int tile_idx, int key, void* userdata) {
    int handle = (int)(uint32)userdata;
    user_gui_t* e = user_gui_get(handle);
    if (!e || e->tile_idx != tile_idx) return;
    user_gui_event_t ev;
    ev.type = USER_GUI_EVENT_KEY;
    ev.a = (int32)key;
    ev.b = 0;
    ev.c = 0;
    ev.d = 0;
    user_gui_push_event(e, &ev);
}

static void user_gui_mouse_cb(int tile_idx, const mouse_event_t* me, void* userdata) {
    if (!me) return;
    int handle = (int)(uint32)userdata;
    user_gui_t* e = user_gui_get(handle);
    if (!e || e->tile_idx != tile_idx) return;

    int cx = 0, cy = 0, cw = 0, ch = 0;
    tile_get_content_rect(tile_idx, &cx, &cy, &cw, &ch);
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
    if (handle < 0 || handle >= USER_GUI_MAX) return NULL;
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

    // Reset handle 0 "attached" GUI state too.
    // Important: unregister the GUI client so the tile returns to normal vterm rendering.
    if (g_user_guis[0].used) {
        int tile_idx0 = g_user_guis[0].tile_idx;
        if (tile_is_tiling_active() && tile_idx0 >= 0) {
            tile_unregister_gui_client(tile_idx0);
        }
        cap_revoke_object(&g_user_guis[0], CAP_OBJ_USER_GUI);
        if (g_user_guis[0].font_handle > 0) {
            vga_font_release(g_user_guis[0].font_handle);
            g_user_guis[0].font_handle = 0;
        }
        if (g_user_guis[0].blit_buf) {
            free(g_user_guis[0].blit_buf);
            g_user_guis[0].blit_buf = NULL;
        }
        g_user_guis[0].blit_w = 0;
        g_user_guis[0].blit_h = 0;
        g_user_guis[0].blit_dst_w = 0;
        g_user_guis[0].blit_dst_h = 0;
        if (g_user_guis[0].title) { free(g_user_guis[0].title); g_user_guis[0].title = NULL; }
        if (g_user_guis[0].status_left) { free(g_user_guis[0].status_left); g_user_guis[0].status_left = NULL; }
        g_user_guis[0].used = 0;
        g_user_guis[0].tile_idx = -1;
        g_user_guis[0].cmd_count = 0;
        g_user_guis[0].ev_head = 0;
        g_user_guis[0].ev_tail = 0;
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

static int user_fd_index_from_ptr(const user_fd_t* ptr) {
    if (!ptr) return -1;
    uint32 base = (uint32)(uint32)&g_user_fds[0];
    uint32 end = (uint32)(uint32)&g_user_fds[USER_FD_MAX];
    uint32 p = (uint32)(uint32)ptr;
    if (p < base || p >= end) return -1;
    uint32 off = p - base;
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
    user_fd_t* ufd = (user_fd_t*)(uint32)cap->obj;
    int idx = user_fd_index_from_ptr(ufd);
    if (idx < 0) return NULL;
    if (!g_user_fds[idx].used) return NULL;
    if (!cap_validate(cap, ufd, CAP_OBJ_USER_FD, required_rights)) return NULL;
    if (out_fd) *out_fd = idx;
    return ufd;
}

static int user_gui_index_from_ptr(const user_gui_t* ptr) {
    if (!ptr) return -1;
    uint32 base = (uint32)(uint32)&g_user_guis[0];
    uint32 end = (uint32)(uint32)&g_user_guis[USER_GUI_MAX];
    uint32 p = (uint32)(uint32)ptr;
    if (p < base || p >= end) return -1;
    uint32 off = p - base;
    if (off % sizeof(user_gui_t) != 0) return -1;
    int idx = (int)(off / sizeof(user_gui_t));
    if (idx < 0 || idx >= USER_GUI_MAX) return -1;
    if (&g_user_guis[idx] != ptr) return -1;
    return idx;
}

static user_gui_t* user_gui_from_cap(const cap_t* cap, uint32 required_rights, int* out_handle) {
    if (!cap) return NULL;
    user_gui_t* e = (user_gui_t*)(uint32)cap->obj;
    int idx = user_gui_index_from_ptr(e);
    if (idx < 0) return NULL;
    if (!g_user_guis[idx].used) return NULL;
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

// When the tiling manager is active, user-mode programs should print into the
// focused virtual terminal so output is visible in the graphical shell.
static void syscall_console_write(const char* buf, int len) {
    if (!buf || len <= 0) return;

    // For user-task output, explicitly control the vterm color so it does not
    // inherit shell capture/redirect colors. Programs can change it by writing
    // the byte sequence: 0xFF, r, g, b.
    extern volatile int g_user_task_color_r;
    extern volatile int g_user_task_color_g;
    extern volatile int g_user_task_color_b;
    extern volatile uint8 g_user_task_color_state;
    extern volatile uint8 g_user_task_color_bytes[3];
    extern int shell_redirect_color_r;
    extern int shell_redirect_color_g;
    extern int shell_redirect_color_b;

    int prev_r = shell_redirect_color_r;
    int prev_g = shell_redirect_color_g;
    int prev_b = shell_redirect_color_b;

    // Start with current user-task color (default white).
    shell_redirect_color_r = (int)g_user_task_color_r;
    shell_redirect_color_g = (int)g_user_task_color_g;
    shell_redirect_color_b = (int)g_user_task_color_b;
    if (tile_is_tiling_active()) {
        int term = tile_get_focused();
        if (g_user_task_active) {
            term = g_user_task_term;
        }
        if (term < 0) term = 0;
        for (int i = 0; i < len; ++i) {
            uint8 ch = (uint8)buf[i];

            if (g_user_task_color_state == 0) {
                if (ch == 0xFF) {
                    g_user_task_color_state = 1;
                    continue;
                }
            } else {
                // Collect r,g,b across bytes (sequence may be split across writes).
                int idx = (int)g_user_task_color_state - 1;
                if (idx >= 0 && idx < 3) {
                    g_user_task_color_bytes[idx] = ch;
                }
                g_user_task_color_state++;
                if (g_user_task_color_state == 4) {
                    g_user_task_color_r = (int)g_user_task_color_bytes[0];
                    g_user_task_color_g = (int)g_user_task_color_bytes[1];
                    g_user_task_color_b = (int)g_user_task_color_bytes[2];
                    shell_redirect_color_r = (int)g_user_task_color_r;
                    shell_redirect_color_g = (int)g_user_task_color_g;
                    shell_redirect_color_b = (int)g_user_task_color_b;
                    g_user_task_color_state = 0;
                }
                continue;
            }

            vterm_write_char(term, (char)ch);
        }
        if (g_user_task_active) {
            g_user_task_ui_dirty = 1;
        }
        shell_redirect_color_r = prev_r;
        shell_redirect_color_g = prev_g;
        shell_redirect_color_b = prev_b;
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

    shell_redirect_color_r = prev_r;
    shell_redirect_color_g = prev_g;
    shell_redirect_color_b = prev_b;
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
#define GUI_HANDLE_SYSCALLS_DISABLED 1
#endif

static int syscall_read_file(user_fd_t* ufd, char* user_dst, int maxlen) {
    if (!ufd || !user_dst || maxlen <= 0) return 0;
    if (ufd->is_dir) return -1;

    if (ufd->offset == 0) {
        if (SYSCALL_DEBUG) {
            printf("[SYSCALL:READ file] drive=%d path='%s' size=%d max=%d\n",
                   (int)ufd->drive, ufd->path, (int)ufd->size, maxlen);
        }
    }

    char tmp[256];
    int remaining = maxlen;
    int total = 0;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)sizeof(tmp)) chunk = (int)sizeof(tmp);
        int n = vfs_read_file_at(ufd->drive, ufd->path, tmp, chunk, ufd->offset);
        if (n < 0) return -1;
        if (n == 0) {
            if (total == 0 && SYSCALL_DEBUG) {
                printf("[SYSCALL:READ file] EOF at off=%d (size=%d)\n",
                       (int)ufd->offset, (int)ufd->size);
            }
            break;
        }
        if (copyout(user_dst + total, tmp, (size_t)n) != 0) return -1;
        ufd->offset += (uint32)n;
        total += n;
        remaining -= n;
        if (n < chunk) break;
    }

    return total;
}

static int syscall_write_file_from_fd(user_fd_t* ufd, const void* user_src, int len) {
    if (!ufd || !user_src || len < 0) return -1;
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
    if (!ufd || ufd->is_dir) return -1;

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
    return (int)ufd->offset;
}

// C dispatcher called by the assembly stub. Returns value in EAX to user.
uint32 syscall_dispatch(regs_t* regs) {
    uint32 syscall_num = regs->eax;
    uint32 arg1 = regs->ebx;
    uint32 arg2 = regs->ecx;
    uint32 arg3 = regs->edx;

    switch (syscall_num) {
        case SYSCALL_WRITE: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) {
                regs->eax = (uint32)-1;
                break;
            }
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
                if (tile_is_tiling_active() && g_user_task_term >= 0) {
                    vterm_stdin_consume(g_user_task_term);
                }
            stdin_done:
                break;
            }

            // file read
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
            user_fd_t* ufd = user_fd_get(fd);
            if (!ufd || ufd->is_dir) { regs->eax = (uint32)-1; break; }

            int n = syscall_read_file(ufd, (char*)arg2, maxlen);
            regs->eax = (n < 0) ? (uint32)-1 : (uint32)n;
            break;
        }
        case SYSCALL_OPEN: {
            if (!syscall_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) { regs->eax = (uint32)-1; break; }
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
            cap_revoke_object(&g_user_fds[fd], CAP_OBJ_USER_FD);
            g_user_fds[fd].used = 0;
            regs->eax = 0;
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

            int n = syscall_read_file(ufd, user_buf, maxlen);
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
            cap_revoke_object(ufd, CAP_OBJ_USER_FD);
            ufd->used = 0;
            regs->eax = 0;
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

            int written = syscall_write_file_from_fd(ufd, user_buf, len);
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
            if (len == 0) { regs->eax = 0; break; }

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

            extern uint8 g_current_drive;
            uint8 drive = g_current_drive;

            if (SYSCALL_WRITEFILE_DEBUG) {
                printf("[SYSCALL:WRITEFILE] req='%s' cwd='%s' => '%s' len=%d drive=%d\n",
                       path, cwd ? cwd : "(null)", abspath, len, (int)drive);
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
			e->font_handle = vga_system_font_acquire();
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
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uint32)handle);
            tile_invalidate_decorations(tile_idx);
            tile_invalidate_gui(tile_idx);
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

            user_gui_t* e = &g_user_guis[handle];
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
            e->font_handle = vga_system_font_acquire();
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
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uint32)handle);
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

            if (handle == 0) {
                int tile_idx = -1;
                if (g_user_task_term >= 0) tile_idx = tile_find_by_term(g_user_task_term);
                if (tile_idx < 0) tile_idx = tile_get_focused();
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
        case SYSCALL_GUI_ATTACH: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_attach(title_ptr=arg1, status_left_ptr=arg2)
            // Binds a GUI client to the tile backing the current ring3 task.
            if (!tile_is_tiling_active() || !g_user_task_active) { regs->eax = (uint32)-1; break; }

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

            // Use handle 0 slot for "current tile" GUI state.
            user_gui_t* e = &g_user_guis[0];
            // If we previously attached, clean up previous strings; do not close the tile.
            if (e->used) {
                if (e->title) free(e->title);
                if (e->status_left) free(e->status_left);
                e->title = NULL;
                e->status_left = NULL;
				if (e->font_handle > 0) {
					vga_font_release(e->font_handle);
					e->font_handle = 0;
				}
                if (e->blit_buf) {
                    free(e->blit_buf);
                    e->blit_buf = NULL;
                }
                e->blit_w = 0;
                e->blit_h = 0;
                e->blit_dst_w = 0;
                e->blit_dst_h = 0;
            }

            e->used = 1;
            e->tile_idx = tile_idx;
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
			e->font_handle = vga_system_font_acquire();
            if (!e->title) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;

            tile_set_title_status(tile_idx, e->title, e->status_left, NULL);
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uint32)0);
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

            user_gui_t* e = &g_user_guis[0];
            if (e->used) {
                if (e->title) free(e->title);
                if (e->status_left) free(e->status_left);
                e->title = NULL;
                e->status_left = NULL;
                if (e->font_handle > 0) {
                    vga_font_release(e->font_handle);
                    e->font_handle = 0;
                }
                if (e->blit_buf) {
                    free(e->blit_buf);
                    e->blit_buf = NULL;
                }
                e->blit_w = 0;
                e->blit_h = 0;
                e->blit_dst_w = 0;
                e->blit_dst_h = 0;
            }

            e->used = 1;
            e->tile_idx = tile_idx;
            e->title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            e->status_left = status_tmp[0] ? kstrdup_bounded(status_tmp, sizeof(status_tmp) - 1) : NULL;
            e->font_handle = vga_system_font_acquire();
            if (!e->title) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
            e->ev_head = 0;
            e->ev_tail = 0;

            tile_set_title_status(tile_idx, e->title, e->status_left, NULL);
            tile_register_gui_client2(tile_idx, user_gui_draw_cb, user_gui_key_cb, user_gui_mouse_cb, (void*)(uint32)0);
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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
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
            if (!e || e->tile_idx < 0 || !user_rgb) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_rect) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }
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
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_PRESENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_present(handle)
            int handle = (int)arg1;
            user_gui_t* e = user_gui_get(handle);
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            tile_invalidate_gui(e->tile_idx);
            // If the main tiler loop is blocked by a running ring3 task, render
            // a single frame now so GUI updates appear without input events.
            tile_render_once();
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
            if (!e || e->tile_idx < 0 || !user_out) { regs->eax = (uint32)-1; break; }

            int cx = 0, cy = 0, cw = 0, ch = 0;
            tile_get_content_rect(e->tile_idx, &cx, &cy, &cw, &ch);
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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }

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
                        extern uint8 g_current_drive;
                        uint8 drive = g_current_drive;
                        int h = vga_font_acquire_hex(drive, abspath);
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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            tile_set_gui_continuous_redraw(e->tile_idx, enabled ? 1 : 0);
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }

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
            regs->eax = 0;
            break;
        }
        case SYSCALL_GUI_POLL_EVENT:
        case SYSCALL_GUI_WAIT_EVENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            if (GUI_HANDLE_SYSCALLS_DISABLED) { regs->eax = (uint32)-1; break; }
            // gui_poll_event(handle, out_event_ptr, out_size)
            // gui_wait_event(handle, out_event_ptr, out_size)
            int handle = (int)arg1;
            void* user_out = (void*)arg2;
            int out_sz = (int)arg3;
            user_gui_t* e = user_gui_get(handle);
            if (!e || !user_out || out_sz < (int)sizeof(user_gui_event_t)) { regs->eax = (uint32)-1; break; }

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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            e->cmd_count = 0;
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
            if (!e || e->tile_idx < 0 || !user_rgb) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_rect) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }
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
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_PRESENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, NULL);
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            tile_invalidate_gui(e->tile_idx);
            tile_render_once();
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
            if (!e || e->tile_idx < 0 || !user_out) { regs->eax = (uint32)-1; break; }

            int cx = 0, cy = 0, cw = 0, ch = 0;
            tile_get_content_rect(e->tile_idx, &cx, &cy, &cw, &ch);
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

            int handle = -1;
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_WRITE, &handle);
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }

            char title_tmp[96];
            if (copyin_cstr(title_tmp, sizeof(title_tmp), user_title) != 0) { regs->eax = (uint32)-1; break; }
            trim_trailing_crlf(title_tmp);

            if (handle == 0) {
                int tile_idx = -1;
                if (g_user_task_term >= 0) tile_idx = tile_find_by_term(g_user_task_term);
                if (tile_idx < 0) tile_idx = tile_get_focused();
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

            char* new_title = kstrdup_bounded(title_tmp, sizeof(title_tmp) - 1);
            if (!new_title) { regs->eax = (uint32)-1; break; }
            if (e->title) free(e->title);
            e->title = new_title;
            tile_set_title_status(e->tile_idx, e->title, e->status_left, NULL);
            tile_invalidate_decorations(e->tile_idx);
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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }

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
                        extern uint8 g_current_drive;
                        uint8 drive = g_current_drive;
                        int h = vga_font_acquire_hex(drive, abspath);
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
            if (!e || e->tile_idx < 0) { regs->eax = (uint32)-1; break; }
            tile_set_gui_continuous_redraw(e->tile_idx, enabled ? 1 : 0);
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
            if (!e || e->tile_idx < 0 || !user_cmd) { regs->eax = (uint32)-1; break; }

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
            regs->eax = 0;
            break;
        }
        case SYSCALL_CAP_GUI_POLL_EVENT:
        case SYSCALL_CAP_GUI_WAIT_EVENT: {
            if (!syscall_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) { regs->eax = (uint32)-1; break; }
            const void* user_cap_ptr = (const void*)arg1;
            void* user_out = (void*)arg2;
            int out_sz = (int)arg3;
            if (!user_out || out_sz < (int)sizeof(user_gui_event_t)) { regs->eax = (uint32)-1; break; }

            cap_t cap;
            if (syscall_cap_copyin(user_cap_ptr, &cap) != 0) { regs->eax = (uint32)-1; break; }
            user_gui_t* e = user_gui_from_cap(&cap, CAP_R_READ, NULL);
            if (!e) { regs->eax = (uint32)-1; break; }

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

