#include <cpu/fpu.h>

static inline uint32 read_cr0(void) {
    uint32 cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void write_cr0(uint32 cr0) {
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void fpu_init(void) {
    // CR0 bits (relevant):
    //  - EM (bit 2): if set, x87 instructions raise #UD.
    //  - TS (bit 3): if set, x87/SSE instructions raise #NM until CLTS.
    //  - MP (bit 1): controls WAIT/FWAIT behavior with TS.
    //  - NE (bit 5): enable native x87 exceptions (#MF) instead of IRQ13.

    uint32 cr0 = read_cr0();
    cr0 &= ~(1u << 2); // EM=0
    cr0 &= ~(1u << 3); // TS=0
    cr0 |=  (1u << 1); // MP=1
    cr0 |=  (1u << 5); // NE=1
    write_cr0(cr0);

    // Initialize x87 state.
    __asm__ __volatile__("fninit");
}

void fpu_handle_nm(void) {
    // Clear TS so the faulting task can execute x87 instructions.
    __asm__ __volatile__("clts");
}
