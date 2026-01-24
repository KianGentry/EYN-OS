#include <misc/types.h>
#include <cpu/aarch64/gicv2.h>

// Shared tick counter for bring-up.
volatile uint32 g_aarch64_ticks = 0;

// These match the hardcoded bring-up init in kernel_main for now.
static gicv2_t g_gic = { .dist_base = 0x08000000ull, .cpuif_base = 0x08010000ull };

void aarch64_timer_on_tick(void);

void aarch64_irq_dispatch(void) {
    // Ack returns IAR; low bits include IRQ ID.
    uint32 iar = gicv2_ack_irq(&g_gic);
    uint32 irq_id = iar & 0x3FFu;

    // 1023 is spurious on GICv2.
    if (irq_id == 1023u) {
        return;
    }

    if (irq_id == 27u) {
        g_aarch64_ticks++;
        aarch64_timer_on_tick();
    }

    gicv2_eoi_irq(&g_gic, iar);
}
