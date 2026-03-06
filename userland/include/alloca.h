/*
 * alloca.h compatibility stub for EYN-OS.
 *
 * alloca() is provided by GCC as a built-in (__builtin_alloca); declaring
 * the fallback prototype here satisfies header includes in DOOM source files
 * that explicitly #include <alloca.h>.  The actual allocation uses the
 * stack and requires no libc support.
 */
#pragma once
#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#ifdef __GNUC__
#define alloca(size) __builtin_alloca(size)
#elif defined(__chibicc__)
/*
 * chibicc registers alloca() as a built-in symbol internally (parse.c:
 * declare_builtin_functions). No macro is needed; bare alloca() works.
 */
#else
/* For non-GCC compilers, provide a non-inline fallback (rarely needed). */
void* alloca(size_t size);
#endif

#endif /* _ALLOCA_H */
