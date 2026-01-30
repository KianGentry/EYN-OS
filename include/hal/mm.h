#ifndef EYNOS_HAL_MM_H
#define EYNOS_HAL_MM_H

#include <misc/types.h>

/*
 * HAL Memory Management
 *
 * Purpose:
 *  Provide the minimal primitives needed by shared subsystems that need to
 *  interact with paging/mapping.
 *
 * Contract:
 *  - These are kernel-only primitives.
 *  - Address arguments are 64-bit to cover both i386 (uses low bits) and AArch64.
 */

#define HAL_PAGE_SIZE 4096u

/* Generic mapping flags (best-effort; backend maps to HW page table bits). */
#define HAL_MM_READ     (1u << 0)
#define HAL_MM_WRITE    (1u << 1)
#define HAL_MM_EXEC     (1u << 2)
#define HAL_MM_USER     (1u << 3)
#define HAL_MM_DEVICE   (1u << 4)
#define HAL_MM_NOCACHE  (1u << 5)

/* Allocate/free a single page (HAL_PAGE_SIZE). Returns NULL on failure. */
void* hal_page_alloc(void);
void hal_page_free(void* page);

/* Map/unmap a single page. Returns 0 on success, negative on error. */
int hal_map_page(uint64 virt_addr, uint64 phys_addr, uint32 flags);
int hal_unmap_page(uint64 virt_addr);

#endif
