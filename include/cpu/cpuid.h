#ifndef EYNOS_CPU_CPUID_H
#define EYNOS_CPU_CPUID_H

#include <misc/types.h>

typedef struct cpu_features_t {
    uint8 cpuid_supported;
    uint8 has_fpu;
    uint8 has_tsc;
    uint8 has_msr;
    uint8 has_pae;
    uint8 has_apic;
    uint8 has_fxsr;
    uint8 has_sse;
    uint8 has_sse2;
    uint8 has_cmov;
    uint8 stepping;
    uint8 model;
    uint8 family;
    char vendor[13];
} cpu_features_t;

const cpu_features_t* cpu_features_detect(void);
const cpu_features_t* cpu_features_get(void);
void cpu_features_log(void);

#endif