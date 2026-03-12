#pragma once

/*
 * ABI-INVARIANT: jmp_buf layout for i386 (32-bit x86, SysV calling convention).
 *
 * setjmp() saves the current execution context into buf so that longjmp()
 * can restore it at a later point, returning a non-zero value from the
 * original setjmp() call site.
 *
 * Saved register layout (jmp_buf indices):
 *   0  ebx  -- callee-saved
 *   1  esi  -- callee-saved
 *   2  edi  -- callee-saved
 *   3  ebp  -- frame pointer
 *   4  esp  -- stack pointer at setjmp() call site
 *   5  eip  -- return address (top of stack at call site)
 *
 * Changing this layout without recompiling all translation units that
 * link setjmp.c breaks the ABI silently.  Treat as a stable interface.
 *
 * Security: setjmp/longjmp are used by DOOM's error-recovery path (I_Error
 * -> longjmp to the main loop).  They do not cross privilege boundaries and
 * have no kernel involvement.
 */
typedef unsigned int jmp_buf[6];

/*
 * setjmp() saves the current register context into buf.
 * Returns 0 on the direct call; returns val (coerced to 1 if 0) when
 * jumped to via longjmp().
 */
int setjmp(jmp_buf buf);

/*
 * longjmp() restores the context saved by setjmp() and causes setjmp() to
 * appear to return val.  If val == 0 it is silently promoted to 1 (POSIX).
 * This function does not return.
 */
void longjmp(jmp_buf buf, int val) __attribute__((noreturn));
