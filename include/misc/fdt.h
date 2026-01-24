#ifndef EYNOS_MISC_FDT_H
#define EYNOS_MISC_FDT_H

#include <misc/types.h>

/*
 * Minimal Flattened Device Tree (FDT) helpers.
 *
 * This is intentionally tiny: enough for early bring-up on platforms like
 * Raspberry Pi 4 where firmware passes a DTB pointer to the kernel.
 */

#define FDT_MAGIC 0xD00DFEEDu

/*
 * Parse the DTB at dtb_ptr and attempt to read the first RAM region
 * from the /memory node's "reg" property.
 *
 * Returns 0 on success, -1 on failure.
 */
int fdt_parse_memory(uint64 dtb_ptr, uint64* out_base, uint64* out_size);

/*
 * Locate a GICv2-compatible interrupt controller node in the DTB and read the
 * distributor and CPU interface base addresses from its "reg" property.
 *
 * This supports common compatible strings like:
 * - "arm,cortex-a15-gic" (QEMU virt)
 * - "arm,gic-400" (some SBCs)
 *
 * Returns 0 on success, -1 on failure.
 */
int fdt_parse_gicv2(uint64 dtb_ptr, uint64* out_gicd_base, uint64* out_gicc_base);

/*
 * Locate the ARMv8 generic timer node ("arm,armv8-timer") and return the
 * GIC IRQ ID for the virtual timer interrupt (CNTV).
 *
 * The DT "interrupts" list uses the GIC binding triplets:
 *   <type number flags>
 * where type is 0=SPI, 1=PPI.
 *
 * Returns 0 on success, -1 on failure.
 */
int fdt_parse_armv8_timer_virtual_irq(uint64 dtb_ptr, uint32* out_irq_id);

#endif
