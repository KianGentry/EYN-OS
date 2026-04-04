#ifndef TYPES_H
#define TYPES_H

#include <misc/arch_config.h>

typedef signed char int8;
typedef unsigned char uint8;

typedef signed short int16;
typedef unsigned short uint16;

typedef signed int int32;
typedef unsigned int uint32;

typedef signed long long int64;
typedef unsigned long long uint64;

#if EYNOS_POINTER_BITS == 64
typedef uint64 uintptr;
typedef int64 intptr;
#else
typedef uint32 uintptr;
typedef int32 intptr;
#endif

typedef char* string; 

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define EYNOS_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
#define EYNOS_STATIC_ASSERT(cond, msg) typedef char eynos_static_assertion__##__LINE__[(cond) ? 1 : -1]
#endif

EYNOS_STATIC_ASSERT((sizeof(void*) * 8u) == EYNOS_POINTER_BITS,
					"EYNOS_POINTER_BITS must match compiler pointer width");
EYNOS_STATIC_ASSERT(sizeof(uint8) == 1u, "uint8 must be 1 byte");
EYNOS_STATIC_ASSERT(sizeof(uint16) == 2u, "uint16 must be 2 bytes");
EYNOS_STATIC_ASSERT(sizeof(uint32) == 4u, "uint32 must be 4 bytes");
EYNOS_STATIC_ASSERT(sizeof(uint64) == 8u, "uint64 must be 8 bytes");

/*
 * Cache-alignment helper for hot kernel data.
 *
 * Why: Small i386-era caches are sensitive to split-line loads/stores; aligning
 *      frequently-touched objects/arrays can reduce line splits and conflict
 *      misses in tight loops.
 * Invariant: Alignment is a power-of-two and safe for all i386 targets.
 * Breakage if changed: May change the layout/size of annotated globals/structs
 *                      and can impact memory usage on low-RAM targets.
 */
#if defined(__GNUC__)
#define CACHE_ALIGNED_32 __attribute__((aligned(32)))
#else
#define CACHE_ALIGNED_32
#endif

static inline uint16 low_16(uint32 address) { return (uint16)(address & 0xFFFF); }
static inline uint16 high_16(uint32 address) { return (uint16)((address >> 16) & 0xFFFF); }
static inline uintptr ptr_to_uintptr(const void* pointer) { return (uintptr)pointer; }
static inline void* uintptr_to_ptr(uintptr address) { return (void*)address; }

#endif
