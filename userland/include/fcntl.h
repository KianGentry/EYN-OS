#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Minimal flags for EYN-OS userland.
#define O_RDONLY 0

int open(const char* path, int flags, int mode);

#ifdef __cplusplus
}
#endif
