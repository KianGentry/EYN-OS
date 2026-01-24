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

#endif
