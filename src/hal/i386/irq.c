#include <hal/irq.h>

#include <cpu/irq.h>
#include <system.h>

// PIC data ports
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

static hal_irq_handler_t g_handlers[16];
static void* g_users[16];

static void pic_set_mask(uint32 irq_line, int masked) {
    if (irq_line >= 16) return;

    uint16 port = (irq_line < 8) ? PIC1_DATA : PIC2_DATA;
    uint8 bit = (uint8)(irq_line & 7u);
    uint8 mask = inportb(port);
    if (masked) mask |= (uint8)(1u << bit);
    else mask &= (uint8)~(1u << bit);
    outportb(port, mask);
}

#define DECL_TRAMP(n) \
    static void irq_tramp_##n(void) { \
        if (g_handlers[n]) { \
            g_handlers[n]((uint32)(n), g_users[n]); \
        } \
    }

DECL_TRAMP(0)
DECL_TRAMP(1)
DECL_TRAMP(2)
DECL_TRAMP(3)
DECL_TRAMP(4)
DECL_TRAMP(5)
DECL_TRAMP(6)
DECL_TRAMP(7)
DECL_TRAMP(8)
DECL_TRAMP(9)
DECL_TRAMP(10)
DECL_TRAMP(11)
DECL_TRAMP(12)
DECL_TRAMP(13)
DECL_TRAMP(14)
DECL_TRAMP(15)

static irq_handler_t tramp_for(uint32 irq_id) {
    switch (irq_id) {
        case 0: return irq_tramp_0;
        case 1: return irq_tramp_1;
        case 2: return irq_tramp_2;
        case 3: return irq_tramp_3;
        case 4: return irq_tramp_4;
        case 5: return irq_tramp_5;
        case 6: return irq_tramp_6;
        case 7: return irq_tramp_7;
        case 8: return irq_tramp_8;
        case 9: return irq_tramp_9;
        case 10: return irq_tramp_10;
        case 11: return irq_tramp_11;
        case 12: return irq_tramp_12;
        case 13: return irq_tramp_13;
        case 14: return irq_tramp_14;
        case 15: return irq_tramp_15;
        default: return 0;
    }
}

int hal_irq_register(uint32 irq_id, hal_irq_handler_t handler, void* user) {
    if (irq_id >= 16) return -1;

    g_handlers[irq_id] = handler;
    g_users[irq_id] = user;

    irq_handler_t tramp = tramp_for(irq_id);
    if (!tramp) return -1;
    register_interrupt_handler((int)irq_id, tramp);
    return 0;
}

void hal_irq_enable(uint32 irq_id) {
    pic_set_mask(irq_id, 0);
}

void hal_irq_disable(uint32 irq_id) {
    pic_set_mask(irq_id, 1);
}
