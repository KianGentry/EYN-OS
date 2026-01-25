#include <cpu/aarch64/psci.h>

static uint32 g_psci_use_hvc = 0;

void psci_set_conduit(uint32 use_hvc) {
    g_psci_use_hvc = (use_hvc != 0) ? 1u : 0u;
}

static inline uint64 smc_call(uint64 x0, uint64 x1, uint64 x2, uint64 x3) {
    register uint64 r0 asm("x0") = x0;
    register uint64 r1 asm("x1") = x1;
    register uint64 r2 asm("x2") = x2;
    register uint64 r3 asm("x3") = x3;

    if (g_psci_use_hvc) {
        asm volatile("hvc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "x4", "x5", "x6", "x7", "memory");
    } else {
        asm volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "x4", "x5", "x6", "x7", "memory");
    }
    return r0;
}

int psci_cpu_on(uint64 target_mpidr, uint64 entry_point, uint64 context_id) {
    uint64 ret = smc_call(PSCI_CPU_ON, target_mpidr, entry_point, context_id);
    return (int)ret;
}
