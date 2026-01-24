#include <misc/types.h>
#include <cpu/aarch64/gicv2.h>

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);

// Shared tick counter for bring-up.
volatile uint32 g_aarch64_ticks = 0;

static gicv2_t g_gic;
static uint32 g_timer_irq_id = 0;

void aarch64_timer_on_tick(void);

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id) {
    g_gic.dist_base = gicd_base;
    g_gic.cpuif_base = gicc_base;
    g_timer_irq_id = timer_irq_id;

    gicv2_init(&g_gic, gicd_base, gicc_base);
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
        g_aarch64_ticks++;
        aarch64_timer_on_tick();
    }

    gicv2_eoi_irq(&g_gic, iar);
}
