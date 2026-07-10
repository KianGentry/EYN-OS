#include <cpu/fpu.h>
#include <cpu/cpuid.h>

static inline uint32 read_cr0(void) {
    uint32 cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void write_cr0(uint32 cr0) {
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static inline uint32 read_cr4(void) {
    uint32 cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void write_cr4(uint32 cr4) {
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void fpu_init(void) {
    const cpu_features_t* features = cpu_features_detect();

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

    if (features && features->has_fxsr) {
        uint32 cr4 = read_cr4();
        cr4 |= (1u << 9);  // OSFXSR
        if (features->has_sse) {
            cr4 |= (1u << 10); // OSXMMEXCPT
        }
        write_cr4(cr4);
    }

    // Initialize x87 state.
    __asm__ __volatile__("fninit");
}

void fpu_handle_nm(void) {
    // Clear TS so the faulting task can execute x87 instructions.
    __asm__ __volatile__("clts");
}
