#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>
#include <misc/types.h>

/*
 * 386-optimized slab allocator (page-backed, power-of-two size classes).
 *
 * Design goals:
 * - 4KB slab pages
 * - O(1) alloc/free fast paths (single pointer pop/push)
 * - No division/modulo in the hot path
 * - Deterministic behavior; no recursion
 */

/* Enable freed-memory poisoning for debug builds. */
#ifndef CONFIG_SLAB_POISON
#define CONFIG_SLAB_POISON 1
#endif

/* Optional tracking counters (alloc/free per class). */
#ifndef CONFIG_SLAB_TRACKING
#define CONFIG_SLAB_TRACKING 0
#endif

void slab_init(void);

/*
 * Allocate from slab if size is supported, else returns NULL.
 * Caller is expected to fall back to the general heap allocator.
 */
void* slab_alloc(size_t size);

/*
 * Free to slab if pointer is owned by a slab page.
 * Returns 1 if freed, 0 if the pointer is not slab-owned.
 */
int slab_free(void* ptr);

/* Return non-zero if ptr belongs to a slab page. */
int slab_owns(const void* ptr);

#endif /* SLAB_H */
