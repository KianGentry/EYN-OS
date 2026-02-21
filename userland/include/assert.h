#pragma once

// Minimal assert() for EYN-OS userland.
// If an assertion fails, terminate the process.

#ifdef __cplusplus
extern "C" {
#endif

void _exit(int code);

#ifdef __cplusplus
}
#endif

#define assert(expr) do { \
    if (!(expr)) { \
        _exit(1); \
    } \
} while (0)
