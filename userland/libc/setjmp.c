/*
 * i386 setjmp / longjmp implementation for EYN-OS userland.
 *
 * ABI-INVARIANT: See setjmp.h for the jmp_buf layout.
 *
 * Both functions use __attribute__((naked)) to suppress compiler-generated
 * prologue/epilogue.  This is required so that the esp and ebp captured by
 * setjmp() are exactly those of the caller's stack frame, not of setjmp()'s
 * own frame.
 *
 * Calling convention (SysV i386 cdecl):
 *   On entry to setjmp:  [esp+0] = return address, [esp+4] = buf pointer
 *   On entry to longjmp: [esp+0] = return address, [esp+4] = buf, [esp+8] = val
 */

#include <setjmp.h>

/* setjmp(jmp_buf buf)
 *
 * Saves callee-saved registers + esp + return address into buf[0..5].
 * Returns 0 to the caller.
 */
__attribute__((naked))
int setjmp(jmp_buf buf __attribute__((unused))) {
    __asm__ __volatile__(
        "movl  4(%esp), %eax\n"   /* eax = buf pointer (first arg on stack) */
        "movl  %ebx, 0(%eax)\n"  /* buf[0] = ebx */
        "movl  %esi, 4(%eax)\n"  /* buf[1] = esi */
        "movl  %edi, 8(%eax)\n"  /* buf[2] = edi */
        "movl  %ebp, 12(%eax)\n" /* buf[3] = ebp */
        "movl  %esp, 16(%eax)\n" /* buf[4] = esp (caller's stack pointer) */
        "movl  (%esp), %ecx\n"   /* ecx = return address at top of stack   */
        "movl  %ecx, 20(%eax)\n" /* buf[5] = eip (return address)          */
        "xorl  %eax, %eax\n"     /* return 0 */
        "ret\n"
    );
}

/* longjmp(jmp_buf buf, int val)
 *
 * Restores esp, ebp, and callee-saved registers from buf, then causes
 * the matching setjmp() call to return val (minimum 1).
 */
__attribute__((naked, noreturn))
void longjmp(jmp_buf buf __attribute__((unused)), int val __attribute__((unused))) {
    __asm__ __volatile__(
        "movl  4(%esp), %edx\n"  /* edx = buf pointer */
        "movl  8(%esp), %eax\n"  /* eax = val */
        /* Coerce val == 0 → 1 (POSIX requirement). */
        "testl %eax, %eax\n"
        "jnz   1f\n"
        "movl  $1, %eax\n"
        "1:\n"
        /* Restore callee-saved registers. */
        "movl  0(%edx), %ebx\n"
        "movl  4(%edx), %esi\n"
        "movl  8(%edx), %edi\n"
        "movl  12(%edx), %ebp\n"
        /* Restore stack pointer and overwrite the return address so that
         * ret jumps into the setjmp() caller with eax = val. */
        "movl  16(%edx), %esp\n"
        "movl  20(%edx), %ecx\n"
        "movl  %ecx, (%esp)\n"   /* overwrite return address on restored stack */
        "ret\n"
    );
}
