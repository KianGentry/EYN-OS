#include <cpu/aarch64/gicv2.h>

// GICv2 Distributor registers
#define GICD_CTLR          0x000
#define GICD_ISENABLER(n)  (0x100 + ((n) * 4))
#define GICD_ICENABLER(n)  (0x180 + ((n) * 4))

// GICv2 CPU interface registers
#define GICC_CTLR          0x0000
#define GICC_PMR           0x0004
#define GICC_IAR           0x000C
#define GICC_EOIR          0x0010

static inline void mmio_write(uint64 addr, uint32 v) {
    *(volatile uint32*)addr = v;
}

static inline uint32 mmio_read(uint64 addr) {
    return *(volatile uint32*)addr;
}

void gicv2_init(gicv2_t* gic, uint64 dist_base, uint64 cpuif_base) {
    if (!gic) return;

    gic->dist_base = dist_base;
    gic->cpuif_base = cpuif_base;

    gicv2_dist_init(gic);
    gicv2_cpu_init(gic);
}

void gicv2_dist_init(gicv2_t* gic) {
    if (!gic) return;
    // Enable distributor.
    mmio_write(gic->dist_base + GICD_CTLR, 1);
}

void gicv2_cpu_init(gicv2_t* gic) {
    if (!gic) return;
    // Allow all priorities through.
    mmio_write(gic->cpuif_base + GICC_PMR, 0xFF);

    // Enable CPU interface.
    mmio_write(gic->cpuif_base + GICC_CTLR, 1);
}

void gicv2_enable_irq(gicv2_t* gic, uint32 irq_id) {
    if (!gic) return;

    uint32 reg = irq_id / 32;
    uint32 bit = irq_id % 32;
    mmio_write(gic->dist_base + GICD_ISENABLER(reg), (1u << bit));
}

void gicv2_disable_irq(gicv2_t* gic, uint32 irq_id) {
    if (!gic) return;

    uint32 reg = irq_id / 32;
    uint32 bit = irq_id % 32;
    mmio_write(gic->dist_base + GICD_ICENABLER(reg), (1u << bit));
}

uint32 gicv2_ack_irq(gicv2_t* gic) {
    return mmio_read(gic->cpuif_base + GICC_IAR);
}

void gicv2_eoi_irq(gicv2_t* gic, uint32 iar) {
    mmio_write(gic->cpuif_base + GICC_EOIR, iar);
}
