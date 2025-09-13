#include <irq.h>
#include <idt.h>
#include <system.h>

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
    set_idt_gate(32, (uint32)irq0);
    set_idt_gate(33, (uint32)irq1);
    set_idt_gate(34, (uint32)irq2);
    set_idt_gate(35, (uint32)irq3);
    set_idt_gate(36, (uint32)irq4);
    set_idt_gate(37, (uint32)irq5);
    set_idt_gate(38, (uint32)irq6);
    set_idt_gate(39, (uint32)irq7);
    set_idt_gate(40, (uint32)irq8);
    set_idt_gate(41, (uint32)irq9);
    set_idt_gate(42, (uint32)irq10);
    set_idt_gate(43, (uint32)irq11);
    set_idt_gate(44, (uint32)irq12);
    set_idt_gate(45, (uint32)irq13);
    set_idt_gate(46, (uint32)irq14);
    set_idt_gate(47, (uint32)irq15);

    // unmask PIT on PIC (IRQ0)
    uint8 mask1 = inportb(PIC1_DATA);
    mask1 &= ~(1 << 0);
    outportb(PIC1_DATA, mask1);

    pit_init(100); // 100 Hz ticks
    extern void sched_set_tick_hz(uint32);
    sched_set_tick_hz(100);
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

// common C-level IRQ dispatcher called from assembly stubs
void irq_dispatch_c(int irq_number) {
    if (irq_number < 0 || irq_number >= 16) {
        return;
    }
    irq_handler_t h = g_irq_handlers[irq_number];
    if (h) {
        h();
    }
    pic_send_eoi(irq_number);
}


