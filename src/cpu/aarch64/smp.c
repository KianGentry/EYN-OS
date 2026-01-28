#include <misc/types.h>
#include <misc/fdt.h>
#include <cpu/aarch64/psci.h>
#include <cpu/aarch64/smp.h>
#include <cpu/aarch64/timer.h>
#include <arch.h>

void uart_pl011_write(const char* s);
void uart_pl011_write_hex64(uint64 v);

uint8 aarch64_cpu_stacks[AARCH64_MAX_CPUS][AARCH64_CPU_STACK_SIZE] __attribute__((aligned(16)));
volatile uint32 aarch64_cpu_online[AARCH64_MAX_CPUS];
volatile uint32 aarch64_cpu_ticks[AARCH64_MAX_CPUS];

static inline uint64 read_mpidr_el1(void) {
    uint64 v;
    asm volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

uint32 aarch64_cpu_id(void) {
    return (uint32)(read_mpidr_el1() & 0xFFu);
}

static char* append_str(char* p, char* end, const char* s) {
    if (!s) s = "(null)";
    while (p < end && *s) {
        *p++ = *s++;
    }
    return p;
}

static char* append_hex64(char* p, char* end, uint64 v) {
    static const char* hex = "0123456789ABCDEF";
    if (end - p < 18) {
        return p;
    }
    /* p may be odd-aligned; keep these as byte stores (no STRH). */
    *p++ = '0';
    asm volatile("" ::: "memory");
    *p++ = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        *p++ = hex[(v >> (uint64)i) & 0xFULL];
    }
    return p;
}

void aarch64_smp_boot(uint64 dtb_ptr) {
    uint64 mpidrs[AARCH64_MAX_CPUS];
    uint32 cpu_count = 0;
    uint32 use_hvc = 0;

    if (fdt_parse_psci_method(dtb_ptr, &use_hvc) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        use_hvc = 1;
        uart_pl011_write("FDT parse failed (psci method), using HVC\n");
#else
        uart_pl011_write("FDT parse failed (psci method)\n");
#endif
    }
    psci_set_conduit(use_hvc);

    if (fdt_parse_cpus_mpidr(dtb_ptr, mpidrs, AARCH64_MAX_CPUS, &cpu_count) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        uart_pl011_write("FDT parse failed (cpus), using QEMU fallback\n");
        cpu_count = AARCH64_MAX_CPUS;
        for (uint32 i = 0; i < cpu_count; i++) {
            mpidrs[i] = (uint64)i;
        }
#else
        uart_pl011_write("FDT parse failed (cpus)\n");
        return;
#endif
    }

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    if (cpu_count <= 1) {
        uart_pl011_write("DTB reported only one CPU; using QEMU fallback list\n");
        cpu_count = AARCH64_MAX_CPUS;
        for (uint32 i = 0; i < cpu_count; i++) {
            mpidrs[i] = (uint64)i;
        }
    }
#endif

    uint64 primary = read_mpidr_el1() & 0xFFFFFFu;

    {
        char line[64] __attribute__((aligned(16)));
        char* p = line;
        char* end = line + sizeof(line) - 1;
        p = append_str(p, end, "CPUs: ");
        p = append_hex64(p, end, (uint64)cpu_count);
        p = append_str(p, end, "\n");
        *p = '\0';
        uart_pl011_write(line);
    }

    for (uint32 i = 0; i < cpu_count; i++) {
        uint64 mpidr = mpidrs[i] & 0xFFFFFFu;
        if (mpidr == primary) {
            continue;
        }

        extern void aarch64_secondary_start(void);
        uint64 entry = (uint64)(const void*)aarch64_secondary_start;
        int rc = psci_cpu_on(mpidrs[i], entry, 0);

        {
            char line[96] __attribute__((aligned(16)));
            char* p = line;
            char* end = line + sizeof(line) - 1;
            p = append_str(p, end, "CPU_ON ");
            p = append_hex64(p, end, mpidrs[i]);
            p = append_str(p, end, " rc=");
            p = append_hex64(p, end, (uint64)(uint32)rc);
            p = append_str(p, end, "\n");
            *p = '\0';
            uart_pl011_write(line);
        }
    }
}

void aarch64_secondary_entry(uint64 cpu_id) {
    if (cpu_id < AARCH64_MAX_CPUS) {
        aarch64_cpu_online[cpu_id] = 1;
    }

    {
        char line[80] __attribute__((aligned(16)));
        char* p = line;
        char* end = line + sizeof(line) - 1;
        p = append_str(p, end, "CPU ");
        p = append_hex64(p, end, cpu_id);
        p = append_str(p, end, " online\n");
        *p = '\0';
        uart_pl011_write(line);
    }

    /* Enable per-CPU timer IRQs on secondary cores. */
    extern void aarch64_irq_cpu_init(void);
    aarch64_irq_cpu_init();
    aarch64_timer_init_tick_hz(100);
    arch_enable_interrupts();

    for (;;) {
        asm volatile("wfi" ::: "memory");
    }
}
