#ifndef CPU_FPU_H
#define CPU_FPU_H

#include <misc/types.h>

// Minimal x87 FPU support for i386 ring3 programs.
//
// Current model: EYN-OS runs one user task at a time (no preemptive user task
// scheduling yet), so we only need to ensure the FPU is enabled and that the
// #NM (Device Not Available) exception can be serviced.

// Enable x87 and initialize FPU state.
void fpu_init(void);

// Handle ISR 7 (#NM / Device Not Available). Typically clears CR0.TS.
void fpu_handle_nm(void);

#endif
