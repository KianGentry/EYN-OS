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

/*
 * Parse CPU nodes under /cpus and extract MPIDR values (from "reg").
 * This is used for SMP bring-up (PSCI CPU_ON).
 *
 * Returns 0 on success (count > 0), -1 on failure.
 */
int fdt_parse_cpus_mpidr(uint64 dtb_ptr, uint64* out_mpidrs, uint32 max_cpus, uint32* out_count);

/*
 * Parse the PSCI node and determine the conduit method.
 * Sets *out_use_hvc to 1 for "hvc", 0 for "smc".
 * Returns 0 on success, -1 on failure.
 */
int fdt_parse_psci_method(uint64 dtb_ptr, uint32* out_use_hvc);

#endif
