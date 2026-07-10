#include <cpu/fpu.h>

/*
 * Provide minimal FPU hooks so amd64 builds can progress while CR0/CR4 setup
 * is moved into arch-specific CPU init.
 */
void fpu_init(void) {
    __asm__ __volatile__("fninit");
}

void fpu_handle_nm(void) {
    __asm__ __volatile__("clts");
}
