#ifndef EYNOS_MISC_ARCH_CONFIG_H
#define EYNOS_MISC_ARCH_CONFIG_H

/*
 * ABI-INVARIANT: Exactly one architecture target macro must be active.
 *
 * Why: Build and runtime code select architecture-sensitive layouts (register
 *      frames, linker scripts, and pointer-width assumptions) from these flags.
 * Breakage if changed: Enabling both or neither produces undefined structure
 *                      layouts and invalid boot artifacts.
 */
#if !defined(EYNOS_ARCH_I386) && !defined(EYNOS_ARCH_AMD64)
#if defined(__x86_64__) || defined(_M_X64)
#define EYNOS_ARCH_AMD64 1
#else
#define EYNOS_ARCH_I386 1
#endif
#endif

#if defined(EYNOS_ARCH_I386) && defined(EYNOS_ARCH_AMD64)
#error "EYNOS_ARCH_I386 and EYNOS_ARCH_AMD64 cannot both be defined"
#endif

#if defined(EYNOS_ARCH_AMD64)
#define EYNOS_POINTER_BITS 64
#else
#define EYNOS_POINTER_BITS 32
#endif

#endif
