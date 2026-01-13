#pragma once

// Minimal stdarg for EYN-OS userland (i386 cdecl).
// This is intentionally implemented without compiler builtins so that
// chibicc can parse it.

typedef char* va_list;

#define _EYN_VA_ALIGN(sz) (((sz) + (int)sizeof(int) - 1) & ~((int)sizeof(int) - 1))

#define va_start(ap, last) ((ap) = (va_list)((char*)&(last) + _EYN_VA_ALIGN(sizeof(last))))
#define va_end(ap) ((void)0)
#define va_copy(dst, src) ((dst) = (src))

#define va_arg(ap, type) \
	(*(type*)(((ap) += _EYN_VA_ALIGN(sizeof(type))) - _EYN_VA_ALIGN(sizeof(type))))
