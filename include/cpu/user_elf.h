#ifndef USER_ELF_H
#define USER_ELF_H

#include <types.h>

// Minimal ELF32 loader that maps PT_LOAD segments into user space and enters ring3.
// Returns 0 on success (does not return to caller if user mode is entered), -1 on failure.
int user_elf_run(uint8 drive, const char* abspath);

#endif
