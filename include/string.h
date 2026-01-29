#ifndef EYNOS_STRING_SHIM_H
#define EYNOS_STRING_SHIM_H

/*
 * Freestanding shim for sources that include <string.h>.
 *
 * The EYN-OS project provides its own libc-like routines in
 * include/utilities/string.h. The AArch64 build uses -ffreestanding and may not
 * provide system headers, so we route the standard include to the project API.
 */

#include <utilities/string.h>

#endif
