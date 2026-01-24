#include <misc/types.h>
#include <misc/fdt.h>
#include <arch.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/timer.h>

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
    // For QEMU virt with GICv2, the default memory map is:
    //   GICD @ 0x08000000, GICC @ 0x08010000
    // For Pi4 this will be DT-driven later; for now we keep it minimal.
    gicv2_t gic;
    gicv2_init(&gic, 0x08000000ull, 0x08010000ull);

    // Virtual timer interrupt is PPI 27.
    const uint32 CNTV_IRQ = 27;
    gicv2_enable_irq(&gic, CNTV_IRQ);
    aarch64_timer_init_tick_hz(100);

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
