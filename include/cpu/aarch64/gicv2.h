#ifndef EYNOS_CPU_AARCH64_GICV2_H
#define EYNOS_CPU_AARCH64_GICV2_H

#include <misc/types.h>

/*
 * Minimal ARM GICv2 driver (sufficient for QEMU virt,gic-version=2 bring-up).
 *
 * Only supports:
 * - distributor enable
 * - CPU interface enable
 * - enable IRQ IDs (including PPIs)
 * - ack/eoi
 */

typedef struct {
    uint64 dist_base;
    uint64 cpuif_base;
} gicv2_t;

void gicv2_init(gicv2_t* gic, uint64 dist_base, uint64 cpuif_base);
void gicv2_dist_init(gicv2_t* gic);
void gicv2_cpu_init(gicv2_t* gic);
void gicv2_enable_irq(gicv2_t* gic, uint32 irq_id);
void gicv2_disable_irq(gicv2_t* gic, uint32 irq_id);
uint32 gicv2_ack_irq(gicv2_t* gic);
void gicv2_eoi_irq(gicv2_t* gic, uint32 iar);

#endif
