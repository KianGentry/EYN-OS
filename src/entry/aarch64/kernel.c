#include <misc/types.h>
#include <misc/fdt.h>
#include <arch.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/timer.h>
#include <cpu/aarch64/smp.h>
#include <drivers/aarch64/fb_simple.h>

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);
void aarch64_irq_cpu_init(void);

void uart_pl011_init_115200(void);
void uart_pl011_write(const char* s);
void uart_pl011_write_hex64(uint64 v);

static void uart_write_hex64(uint64 v) {
    uart_pl011_write_hex64(v);
}

static void fb_write_hex64(uint64 v) {
    static const char* hex = "0123456789ABCDEF";
    fb_simple_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> (uint64)i) & 0xFULL];
        fb_simple_putc(c);
    }
}

static void aarch64_enable_fp_simd(void) {
    /* Allow FP/SIMD at EL1 (prevents traps on Q-register spills). */
    uint64 cpacr;
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ull << 20); /* FPEN = 0b11 */
    asm volatile("msr cpacr_el1, %0\n\tisb" :: "r"(cpacr) : "memory");
}

void kernel_main(uint64 dtb_ptr) {
    uart_pl011_init_115200();

    aarch64_enable_fp_simd();

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

    /* Initialize framebuffer after DTB pointer is finalized. */
    if (fb_simple_init(dtb_ptr) == 0) {
        fb_simple_write("EYN-OS AArch64 bring-up\n");

        uint64 fb_base = 0;
        uint32 fb_w = 0, fb_h = 0, fb_stride = 0, fb_bpp = 0;
        const char* fb_fmt = 0;
        if (fb_simple_get_info(&fb_base, &fb_w, &fb_h, &fb_stride, &fb_bpp, &fb_fmt) == 0) {
            uart_pl011_write("FB base ");
            uart_write_hex64(fb_base);
            uart_pl011_write(" w ");
            uart_write_hex64((uint64)fb_w);
            uart_pl011_write(" h ");
            uart_write_hex64((uint64)fb_h);
            uart_pl011_write(" stride ");
            uart_write_hex64((uint64)fb_stride);
            uart_pl011_write(" bpp ");
            uart_write_hex64((uint64)fb_bpp);
            uart_pl011_write(" fmt ");
            uart_pl011_write(fb_fmt ? fb_fmt : "(null)");
            uart_pl011_write("\n");
        }
    } else {
        uart_pl011_write("FB init failed, code ");
        uart_write_hex64((uint64)fb_simple_last_error());
        uart_pl011_write("\n");
    }

    uart_pl011_write("DTB @ ");
    uart_write_hex64(dtb_ptr);
    uart_pl011_write("\n");

    if (fb_simple_ready()) {
        fb_simple_write("DTB @ ");
        fb_write_hex64(dtb_ptr);
        fb_simple_putc('\n');
    }

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    uint64 fwcfg_base = 0;
    if (fdt_parse_qemu_fw_cfg_mmio(dtb_ptr, &fwcfg_base) == 0) {
        uart_pl011_write("FW_CFG @ ");
        uart_write_hex64(fwcfg_base);
        uart_pl011_write("\n");
        if (fb_simple_ready()) {
            fb_simple_write("FW_CFG @ ");
            fb_write_hex64(fwcfg_base);
            fb_simple_putc('\n');
        }
    }
#endif

    uint64 ram_base = 0;
    uint64 ram_size = 0;
    if (fdt_parse_memory(dtb_ptr, &ram_base, &ram_size) == 0) {
        uart_pl011_write("RAM base ");
        uart_write_hex64(ram_base);
        uart_pl011_write(" size ");
        uart_write_hex64(ram_size);
        uart_pl011_write("\n");

        if (fb_simple_ready()) {
            fb_simple_write("RAM base ");
            fb_write_hex64(ram_base);
            fb_simple_write(" size ");
            fb_write_hex64(ram_size);
            fb_simple_putc('\n');
        }
    } else {
        uart_pl011_write("FDT parse failed (memory)\n");
        if (fb_simple_ready()) {
            fb_simple_write("FDT parse failed (memory)\n");
        }
    }

    uart_pl011_write("Continuing.\n");
    if (fb_simple_ready()) {
        fb_simple_write("Continuing.\n");
    }

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
    aarch64_irq_cpu_init();

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
