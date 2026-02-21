#ifndef TYPES_H
#define TYPES_H

typedef signed char int8;
typedef unsigned char uint8;

typedef signed short int16;
typedef unsigned short uint16;

typedef signed int int32;
typedef unsigned int uint32;

typedef signed long long int64;
typedef unsigned long long uint64;

typedef char* string; 

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

#endif
