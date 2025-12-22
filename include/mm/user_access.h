#ifndef USER_ACCESS_H
#define USER_ACCESS_H

#include <types.h>
#include <stddef.h>

// Validate that a user pointer range is accessible.
// - write=0: range must be readable from user space
// - write=1: range must be writable from user space
// Returns 1 if OK, 0 if invalid.
int user_access_ok(const void* user_ptr, size_t len, int write);

// Copy from user memory into kernel memory.
// Returns 0 on success, -1 on invalid user pointer.
int copyin(void* dst, const void* user_src, size_t len);

// Copy from kernel memory into user memory.
// Returns 0 on success, -1 on invalid user pointer.
int copyout(void* user_dst, const void* src, size_t len);

#endif
