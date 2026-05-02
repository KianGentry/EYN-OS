#include <cpu/irq.h>
#include <idt.h>
#include <system.h>
#include <util.h>
#include <vga.h>
#include <tile_manager.h>
#include <watchdog.h>
#include <misc/sched.h>
#include <cpu/user_elf.h>

extern void poll_keyboard_for_ctrl_c();

// PIC ports
#define PIC1            0x20
#define PIC2            0xA0
#define PIC1_COMMAND    PIC1
#define PIC1_DATA       (PIC1+1)
#define PIC2_COMMAND    PIC2
#define PIC2_DATA       (PIC2+1)

#define PIC_EOI         0x20

// PIT ports
#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43

// store handlers
static irq_handler_t g_irq_handlers[16];

#if defined(EYNOS_ARCH_AMD64)
typedef struct irq_saved_frame_amd64_t {
    uint64 rax;
    uint64 rcx;
    uint64 rdx;
    uint64 rbx;
    uint64 rbp;
    uint64 rsi;
    uint64 rdi;
    uint64 r8;
    uint64 r9;
    uint64 r10;
    uint64 r11;
    uint64 r12;
    uint64 r13;
    uint64 r14;
    uint64 r15;
    uint64 rip;
    uint64 cs;
    uint64 rflags;
} irq_saved_frame_amd64_t;
#else
typedef struct irq_saved_frame_i386_t {
    uint32 edi;
    uint32 esi;
    uint32 ebp;
    uint32 esp;
    uint32 ebx;
    uint32 edx;
    uint32 ecx;
    uint32 eax;
    uint32 eip;
    uint32 cs;
    uint32 eflags;
} irq_saved_frame_i386_t;
#endif

static int irq_frame_from_user_mode(void* frame_ptr) {
    if (!frame_ptr) return 0;
#if defined(EYNOS_ARCH_AMD64)
    const irq_saved_frame_amd64_t* frame = (const irq_saved_frame_amd64_t*)frame_ptr;
    return ((uint16)frame->cs & 3u) == 3u;
#else
    const irq_saved_frame_i386_t* frame = (const irq_saved_frame_i386_t*)frame_ptr;
    return ((uint16)frame->cs & 3u) == 3u;
#endif
}

// assembly stubs to route to C handlers
extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

static void pic_remap(void) {
    // save masks
    uint8 a1 = inportb(PIC1_DATA);
    uint8 a2 = inportb(PIC2_DATA);

    // start initialization in cascade mode
    outportb(PIC1_COMMAND, 0x11);
    outportb(PIC2_COMMAND, 0x11);
    // set vector offsets 0x20 and 0x28
    outportb(PIC1_DATA, 0x20);
    outportb(PIC2_DATA, 0x28);
    // tell PICs about their cascade identity
    outportb(PIC1_DATA, 0x04);
    outportb(PIC2_DATA, 0x02);
    // set 8086/88 mode
    outportb(PIC1_DATA, 0x01);
    outportb(PIC2_DATA, 0x01);
    // restore masks
    outportb(PIC1_DATA, a1);
    outportb(PIC2_DATA, a2);
}

static void pit_init(uint32 hz) {
    // PIT input clock is 1193182 Hz
    uint32 divisor = 1193182 / (hz ? hz : 100);
    outportb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3, binary
    outportb(PIT_CHANNEL0, (uint8)(divisor & 0xFF));
    outportb(PIT_CHANNEL0, (uint8)((divisor >> 8) & 0xFF));
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) g_irq_handlers[i] = 0;

    pic_remap();

    // install IDT gates for IRQs 0..15 at 0x20..0x2F
    set_idt_gate(32, (uintptr)irq0);
    set_idt_gate(33, (uintptr)irq1);
    set_idt_gate(34, (uintptr)irq2);
    set_idt_gate(35, (uintptr)irq3);
    set_idt_gate(36, (uintptr)irq4);
    set_idt_gate(37, (uintptr)irq5);
    set_idt_gate(38, (uintptr)irq6);
    set_idt_gate(39, (uintptr)irq7);
    set_idt_gate(40, (uintptr)irq8);
    set_idt_gate(41, (uintptr)irq9);
    set_idt_gate(42, (uintptr)irq10);
    set_idt_gate(43, (uintptr)irq11);
    set_idt_gate(44, (uintptr)irq12);
    set_idt_gate(45, (uintptr)irq13);
    set_idt_gate(46, (uintptr)irq14);
    set_idt_gate(47, (uintptr)irq15);

    // unmask PIT on PIC (IRQ0)
    uint8 mask1 = inportb(PIC1_DATA);
    mask1 &= ~(1 << 0);
    outportb(PIC1_DATA, mask1);

    /*
     * 1000 Hz PIT: gives 1 ms tick granularity so usleep() can provide
     * accurate sub-20ms sleeps without spin-waiting.  At 50 Hz each
     * sched_sleep_us call was rounded up to the nearest 20 ms, causing
     * video frame pacing to overshoot by up to 19 ms per frame.
     *
     * Overhead: ~1 µs ISR × 1000/s = 1 ms/s CPU cost -- negligible.
     */
    pit_init(1000);
    sched_set_tick_hz(1000);
}

void register_interrupt_handler(int irq, irq_handler_t handler) {
    if (irq < 0 || irq >= 16) return;
    g_irq_handlers[irq] = handler;
}

void pic_send_eoi(int irq) {
    if (irq >= 8) {
        outportb(PIC2_COMMAND, PIC_EOI);
    }
    outportb(PIC1_COMMAND, PIC_EOI);
}

static void irq_dispatch_core(int irq_number, int send_eoi, void* frame_ptr) {
    if (irq_number < 0 || irq_number >= 16) {
        return;
    }

    int interrupted_user_mode = irq_frame_from_user_mode(frame_ptr);

    if (irq_number == 0 && g_user_task_active) {
        // Ring3 tasks can run for long periods without calling into the kernel.
        // While they run, IRQ0 still pumps input/render; count that as forward
        // progress so the watchdog doesn't fire.
        watchdog_kick("user-task");
        // While a ring3 task runs, the main tiler loop is blocked. We do a tiny
        // input+render pump here. Critical: do not consume scancodes from two
        // different readers (it corrupts input and can make UI appear frozen).
        if (tile_is_tiling_active()) {
            int any_key = 0;
            for (int i = 0; i < 4; ++i) {
                if (!tile_pump_input_once()) break;
                any_key = 1;
            }
            if (any_key) g_user_task_ui_dirty = 1;
        } else {
            // Non-tiling mode: we don't have the TUI key pump, so use the minimal poll.
            poll_keyboard_for_ctrl_c();
        }

        if (g_user_interrupt) {
            if (interrupted_user_mode) {
                g_user_interrupt = 0;
                user_task_notify_exit(-130);
                g_user_task_active = 0;
                g_user_task_term = -1;
                g_user_task_ui_dirty = 0;
                g_abort_to_shell = 1;
                printf("^C\n");
                if (send_eoi) {
                    pic_send_eoi(irq_number);
                }
                return;
            }
        }

        // Throttle renders (PIT is 50Hz). Keep repainting while a user task is active
        // so the desktop stays responsive even when the program emits no output.
        if (tile_is_tiling_active()) {
            static uint32 ui_div = 0;
            if (++ui_div >= 3) { // ~16 FPS max while user task active
                ui_div = 0;
                // Keep the compositor advancing even if the active task is silent.
                // Some UI state only becomes visible after an actual frame swap,
                // so waiting for stdout or menu activity makes the desktop look frozen.
                if (g_user_task_ui_dirty) {
                    g_user_task_ui_dirty = 0;
                }
                tile_render_once();
            }
        }

    }

    irq_handler_t h = g_irq_handlers[irq_number];
    if (h) {
        h();
    }

    if (irq_number == 0 && g_user_task_active && interrupted_user_mode && sched_mlfq_irq_preempt_enabled()) {
        (void)user_task_try_preempt_from_irq(frame_ptr);
    }

    if (send_eoi) {
        pic_send_eoi(irq_number);
    }
}

// common C-level IRQ dispatcher called from assembly stubs
void irq_dispatch_c(int irq_number, void* frame_ptr) {
    if (sched_det_is_enabled()) {
        if (sched_det_queue_irq(irq_number) == 0) {
            pic_send_eoi(irq_number);
            return;
        }
    }
    irq_dispatch_core(irq_number, 1, frame_ptr);
}

void irq_dispatch_deferred(int irq_number, void* frame_ptr) {
    irq_dispatch_core(irq_number, 0, frame_ptr);
}


