#include <cpu/cpuid.h>

#include <string.h>
#include <vga.h>

static cpu_features_t g_cpu_features;
static int g_cpu_features_ready;

static int cpu_has_cpuid_instruction(void) {
    uint32 original_flags = 0;
    uint32 modified_flags = 0;
    uint32 after_flags = 0;

    __asm__ __volatile__("pushfl\n\tpopl %0" : "=r"(original_flags));
    modified_flags = original_flags ^ (1u << 21);
    __asm__ __volatile__("pushl %0\n\tpopfl" :: "r"(modified_flags) : "cc", "memory");
    __asm__ __volatile__("pushfl\n\tpopl %0" : "=r"(after_flags));
    __asm__ __volatile__("pushl %0\n\tpopfl" :: "r"(original_flags) : "cc", "memory");

    return ((original_flags ^ after_flags) & (1u << 21)) != 0;
}

static void cpu_cpuid(uint32 leaf, uint32 subleaf, uint32* eax, uint32* ebx, uint32* ecx, uint32* edx) {
    uint32 a = leaf;
    uint32 b = 0;
    uint32 c = subleaf;
    uint32 d = 0;

    __asm__ __volatile__(
        "cpuid"
        : "+a"(a), "=b"(b), "+c"(c), "=d"(d)
        :
        : "cc", "memory");

    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

const cpu_features_t* cpu_features_detect(void) {
    if (g_cpu_features_ready) {
        return &g_cpu_features;
    }

    memset(&g_cpu_features, 0, sizeof(g_cpu_features));
    memcpy(g_cpu_features.vendor, "unknown", 7);
    g_cpu_features.vendor[7] = '\0';

    if (!cpu_has_cpuid_instruction()) {
        g_cpu_features_ready = 1;
        return &g_cpu_features;
    }

    uint32 eax = 0;
    uint32 ebx = 0;
    uint32 ecx = 0;
    uint32 edx = 0;

    cpu_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_features.cpuid_supported = 1;
    memcpy(&g_cpu_features.vendor[0], &ebx, sizeof(ebx));
    memcpy(&g_cpu_features.vendor[4], &edx, sizeof(edx));
    memcpy(&g_cpu_features.vendor[8], &ecx, sizeof(ecx));
    g_cpu_features.vendor[12] = '\0';

    cpu_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_features.stepping = (uint8)(eax & 0x0Fu);
    g_cpu_features.model = (uint8)((eax >> 4) & 0x0Fu);
    g_cpu_features.family = (uint8)((eax >> 8) & 0x0Fu);
    if (((eax >> 8) & 0x0Fu) == 0x0Fu) {
        g_cpu_features.family = (uint8)(((eax >> 8) & 0x0Fu) + ((eax >> 20) & 0xFFu));
    }
    if (((eax >> 8) & 0x0Fu) == 0x06u || ((eax >> 8) & 0x0Fu) == 0x0Fu) {
        g_cpu_features.model = (uint8)(((eax >> 4) & 0x0Fu) | (((eax >> 16) & 0x0Fu) << 4));
    }

    g_cpu_features.has_fpu = (edx & (1u << 0)) ? 1 : 0;
    g_cpu_features.has_tsc = (edx & (1u << 4)) ? 1 : 0;
    g_cpu_features.has_msr = (edx & (1u << 5)) ? 1 : 0;
    g_cpu_features.has_pae = (edx & (1u << 6)) ? 1 : 0;
    g_cpu_features.has_apic = (edx & (1u << 9)) ? 1 : 0;
    g_cpu_features.has_cmov = (edx & (1u << 15)) ? 1 : 0;
    g_cpu_features.has_fxsr = (edx & (1u << 24)) ? 1 : 0;
    g_cpu_features.has_sse = (edx & (1u << 25)) ? 1 : 0;
    g_cpu_features.has_sse2 = (edx & (1u << 26)) ? 1 : 0;

    g_cpu_features_ready = 1;
    return &g_cpu_features;
}

const cpu_features_t* cpu_features_get(void) {
    return cpu_features_detect();
}

void cpu_features_log(void) {
    const cpu_features_t* features = cpu_features_detect();

    printf("[cpu] vendor=%s family=%u model=%u stepping=%u cpuid=%u fpu=%u fxsr=%u sse=%u sse2=%u tsc=%u msr=%u pae=%u apic=%u cmov=%u\n",
           features->vendor,
           (unsigned)features->family,
           (unsigned)features->model,
           (unsigned)features->stepping,
           (unsigned)features->cpuid_supported,
           (unsigned)features->has_fpu,
           (unsigned)features->has_fxsr,
           (unsigned)features->has_sse,
           (unsigned)features->has_sse2,
           (unsigned)features->has_tsc,
           (unsigned)features->has_msr,
           (unsigned)features->has_pae,
           (unsigned)features->has_apic,
           (unsigned)features->has_cmov);
}