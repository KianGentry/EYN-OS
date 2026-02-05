#include <misc/types.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/irq_router.h>
#include <cpu/aarch64/smp.h>

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);
void aarch64_irq_cpu_init(void);

// Shared tick counter for bring-up.
volatile uint32 g_aarch64_ticks = 0;

static gicv2_t g_gic;
static uint32 g_timer_irq_id = 0;

void aarch64_timer_on_tick(void);

#define AARCH64_IRQ_MAX_ID 1024u
static aarch64_irq_handler_t g_irq_handlers[AARCH64_IRQ_MAX_ID];
static void* g_irq_users[AARCH64_IRQ_MAX_ID];

static aarch64_tick_handler_t g_tick_handler;
static void* g_tick_user;

int aarch64_irq_register_handler(uint32 irq_id, aarch64_irq_handler_t handler, void* user) {
    if (irq_id >= AARCH64_IRQ_MAX_ID) return -1;
    g_irq_handlers[irq_id] = handler;
    g_irq_users[irq_id] = user;
    return 0;
}

void aarch64_irq_enable_id(uint32 irq_id) {
    gicv2_enable_irq(&g_gic, irq_id);
}

void aarch64_irq_disable_id(uint32 irq_id) {
    gicv2_disable_irq(&g_gic, irq_id);
}

void aarch64_irq_set_tick_handler(aarch64_tick_handler_t handler, void* user) {
    g_tick_handler = handler;
    g_tick_user = user;
}

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id) {
    g_gic.dist_base = gicd_base;
    g_gic.cpuif_base = gicc_base;
    g_timer_irq_id = timer_irq_id;

    for (uint32 i = 0; i < AARCH64_IRQ_MAX_ID; i++) {
        g_irq_handlers[i] = 0;
        g_irq_users[i] = 0;
    }
    g_tick_handler = 0;
    g_tick_user = 0;

    gicv2_dist_init(&g_gic);
    aarch64_irq_cpu_init();
}

void aarch64_irq_cpu_init(void) {
    gicv2_cpu_init(&g_gic);
    if (g_timer_irq_id != 0) {
        gicv2_enable_irq(&g_gic, g_timer_irq_id);
    }
}

void aarch64_irq_dispatch(void) {
    // Ack returns IAR; low bits include IRQ ID.
    uint32 iar = gicv2_ack_irq(&g_gic);
    uint32 irq_id = iar & 0x3FFu;

    // 1023 is spurious on GICv2.
    if (irq_id == 1023u) {
        return;
    }

    if (g_timer_irq_id != 0 && irq_id == g_timer_irq_id) {
        uint32 cpu = aarch64_cpu_id();
        if (cpu < AARCH64_MAX_CPUS) {
            aarch64_cpu_ticks[cpu]++;
        }
        g_aarch64_ticks++;
        aarch64_timer_on_tick();

        if (g_tick_handler) {
            g_tick_handler(g_tick_user);
        }
    }

    if (irq_id < AARCH64_IRQ_MAX_ID) {
        aarch64_irq_handler_t h = g_irq_handlers[irq_id];
        if (h) {
            h(irq_id, g_irq_users[irq_id]);
        }
    }

    gicv2_eoi_irq(&g_gic, iar);
}
