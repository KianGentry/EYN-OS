#ifndef USER_ELF_H
#define USER_ELF_H

#include <types.h>

// Minimal ELF32 loader that maps PT_LOAD segments into user space and enters ring3.
// Returns 0 on success (does not return to caller if user mode is entered), -1 on failure.
int user_elf_run(uint8 drive, const char* abspath);

// Run a ring3 ELF32 program with argv. argv strings are copied onto the user stack.
// argc may be 0; argv may be NULL.
int user_elf_run_argv(uint8 drive, const char* abspath, int argc, const char* const* argv);

#endif
