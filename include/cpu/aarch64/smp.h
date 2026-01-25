#ifndef EYNOS_CPU_AARCH64_SMP_H
#define EYNOS_CPU_AARCH64_SMP_H

#include <misc/types.h>

#define AARCH64_MAX_CPUS 4
#define AARCH64_CPU_STACK_SIZE 4096

void aarch64_smp_boot(uint64 dtb_ptr);
void aarch64_secondary_entry(uint64 cpu_id);

#endif
