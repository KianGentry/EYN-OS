#include <misc/types.h>
#include <misc/fdt.h>
#include <arch.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/timer.h>
#include <cpu/aarch64/smp.h>

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);

void uart_pl011_init_115200(void);
void uart_pl011_write(const char* s);

static void uart_write_hex64(uint64 v) {
    static const char* hex = "0123456789ABCDEF";
    uart_pl011_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> (uint64)i) & 0xFULL];
        // tiny inline send to avoid needing stdlib
        extern void uart_pl011_putc(char c);
        uart_pl011_putc(c);
    }
}

void kernel_main(uint64 dtb_ptr) {
    uart_pl011_init_115200();

    uart_pl011_write("\nEYN-OS AArch64 bring-up\n");

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    /*
     * QEMU 'virt' bare-metal ELF boot does not reliably provide a DTB pointer in x0.
     * For local development we embed a generated DTB blob into the kernel ELF and
     * fall back to it when dtb_ptr is 0.
     */
    if (dtb_ptr == 0) {
        extern const uint8 _binary_virt_dtb_start[];
        dtb_ptr = (uint64)(const void*)_binary_virt_dtb_start;
    }
#endif

    uart_pl011_write("DTB @ ");
    uart_write_hex64(dtb_ptr);
    uart_pl011_write("\n");

    uint64 ram_base = 0;
    uint64 ram_size = 0;
    if (fdt_parse_memory(dtb_ptr, &ram_base, &ram_size) == 0) {
        uart_pl011_write("RAM base ");
        uart_write_hex64(ram_base);
        uart_pl011_write(" size ");
        uart_write_hex64(ram_size);
        uart_pl011_write("\n");
    } else {
        uart_pl011_write("FDT parse failed (memory)\n");
    }

    uart_pl011_write("Halting.\n");

    // --- Interrupts + tick bring-up (QEMU virt,gic-version=2 and Pi4 later) ---

    uint64 gicd_base = 0;
    uint64 gicc_base = 0;
    if (fdt_parse_gicv2(dtb_ptr, &gicd_base, &gicc_base) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        /* QEMU virt fallback defaults (still keep DT-based path as the primary). */
        gicd_base = 0x08000000ull;
        gicc_base = 0x08010000ull;
#else
        uart_pl011_write("FDT parse failed (gic)\n");
        for (;;) {
            asm volatile("wfi" ::: "memory");
        }
#endif
    }

    uart_pl011_write("GICD @ ");
    uart_write_hex64(gicd_base);
    uart_pl011_write(" GICC @ ");
    uart_write_hex64(gicc_base);
    uart_pl011_write("\n");

    uint32 cntv_irq_id = 0;
    if (fdt_parse_armv8_timer_virtual_irq(dtb_ptr, &cntv_irq_id) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        /* QEMU virt typically uses PPI 11 => IRQ ID 27 for CNTV. */
        cntv_irq_id = 27;
#else
        uart_pl011_write("FDT parse failed (armv8-timer)\n");
        for (;;) {
            asm volatile("wfi" ::: "memory");
        }
#endif
    }

    uart_pl011_write("CNTV IRQ ");
    uart_write_hex64((uint64)cntv_irq_id);
    uart_pl011_write("\n");

    aarch64_timer_init_tick_hz(100);
    aarch64_irq_init(gicd_base, gicc_base, cntv_irq_id);

    // Bring up secondary CPUs (QEMU virt uses PSCI).
    aarch64_smp_boot(dtb_ptr);

    volatile uint32 ticks = 0;
    arch_enable_interrupts();

    for (;;) {
        asm volatile("wfi" ::: "memory");
        // Tick counter is incremented in IRQ handler; we sample it here.
        extern volatile uint32 g_aarch64_ticks;
        if (g_aarch64_ticks != ticks) {
            ticks = g_aarch64_ticks;
            if ((ticks % 100) == 0) {
                uart_pl011_write("tick ");
                uart_write_hex64((uint64)ticks);
                uart_pl011_write("\n");
            }
        }
    }
}
