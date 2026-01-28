#ifndef EYNOS_CPU_AARCH64_PSCI_H
#define EYNOS_CPU_AARCH64_PSCI_H

#include <misc/types.h>

/* PSCI v0.2 function IDs (SMC64). */
#define PSCI_CPU_ON 0x84000003u
#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u

/*
 * Bring up a secondary CPU using PSCI CPU_ON.
 *
 * Returns 0 on success, or a negative PSCI error code.
 */
int psci_cpu_on(uint64 target_mpidr, uint64 entry_point, uint64 context_id);

/*
 * Power off / reset the system via PSCI.
 *
 * These functions do not return.
 */
void psci_system_off(void);
void psci_system_reset(void);

/*
 * Select PSCI conduit: use_hvc=1 for HVC, 0 for SMC.
 */
void psci_set_conduit(uint32 use_hvc);

#endif
