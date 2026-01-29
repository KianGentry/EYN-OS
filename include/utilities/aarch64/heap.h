#ifndef EYNOS_AARCH64_HEAP_H
#define EYNOS_AARCH64_HEAP_H

#include <misc/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the AArch64 bring-up heap.
 *
 * The heap is placed after the kernel image end symbol and capped to the DT RAM
 * region (when provided). This allocator is intended for bring-up/full-mode
 * port work, not a final VM-backed allocator.
 */
void aarch64_heap_init(uint64 ram_base, uint64 ram_size);

#ifdef __cplusplus
}
#endif

#endif
