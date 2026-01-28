#include <misc/types.h>
#include <misc/fdt.h>
#include <arch.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/timer.h>
#include <cpu/aarch64/smp.h>
#include <drivers/aarch64/fb_simple.h>
#include <drivers/aarch64/virtio_input.h>

/* Full-mode bring-up entry: keeps the existing AArch64 interrupt/timer/SMP code
 * but drops into an interactive serial shell instead of the tick-only loop.
 */

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);
void aarch64_irq_cpu_init(void);

void uart_pl011_init_115200(void);
void uart_pl011_write(const char* s);
void uart_pl011_write_hex64(uint64 v);

void aarch64_bringup_shell_set_meminfo(uint64 ram_base, uint64 ram_size);
void aarch64_shell_set_meminfo(uint64 ram_base, uint64 ram_size);
void aarch64_bringup_shell_run(void);

static void fb_write_hex64(uint64 v) {
    static const char* hex = "0123456789ABCDEF";
    fb_simple_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> (uint64)i) & 0xFULL];
        fb_simple_putc(c);
    }
}

static void aarch64_enable_fp_simd(void) {
    /*
     * Enable FP/SIMD at EL1.
     * Without this, GCC may emit Q-register spills (e.g. in printf prologues)
     * which trap as a synchronous exception.
     */
    uint64 cpacr;
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ull << 20); /* FPEN = 0b11 */
    asm volatile("msr cpacr_el1, %0\n\tisb" :: "r"(cpacr) : "memory");
}

void kernel_main(uint64 dtb_ptr) {
    uart_pl011_init_115200();

    aarch64_enable_fp_simd();

    uart_pl011_write("\nEYN-OS AArch64 full bring-up\n");

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    if (dtb_ptr == 0) {
        extern const uint8 _binary_virt_dtb_start[];
        dtb_ptr = (uint64)(const void*)_binary_virt_dtb_start;
    }
#endif

    if (fb_simple_init(dtb_ptr) == 0) {
        fb_simple_write("EYN-OS AArch64 full bring-up\n");
    }

    /* Optional: virtio-input keyboard (for QEMU GUI window typing). */
    int vrc = virtio_input_init(dtb_ptr);
    if (vrc == 0) {
        uart_pl011_write("virtio-input @ ");
        uart_pl011_write_hex64(virtio_input_base());
        uart_pl011_write("\n");

        if (fb_simple_ready()) {
            fb_simple_write("virtio-input @ ");
            fb_write_hex64(virtio_input_base());
            fb_simple_putc('\n');
        }
    } else {
        uart_pl011_write("virtio-input not ready\n");
        if (fb_simple_ready()) {
            fb_simple_write("virtio-input not ready\n");
        }
    }

    uart_pl011_write("DTB @ ");
    uart_pl011_write_hex64(dtb_ptr);
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
        uart_pl011_write_hex64(fwcfg_base);
        uart_pl011_write("\n");
    }
#endif

    uint64 ram_base = 0;
    uint64 ram_size = 0;
    if (fdt_parse_memory(dtb_ptr, &ram_base, &ram_size) == 0) {
        uart_pl011_write("RAM base ");
        uart_pl011_write_hex64(ram_base);
        uart_pl011_write(" size ");
        uart_pl011_write_hex64(ram_size);
        uart_pl011_write("\n");
        aarch64_bringup_shell_set_meminfo(ram_base, ram_size);
        aarch64_shell_set_meminfo(ram_base, ram_size);
    } else {
        uart_pl011_write("FDT parse failed (memory)\n");
    }

    // --- Interrupts + tick bring-up ---

    uint64 gicd_base = 0;
    uint64 gicc_base = 0;
    if (fdt_parse_gicv2(dtb_ptr, &gicd_base, &gicc_base) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        gicd_base = 0x08000000ull;
        gicc_base = 0x08010000ull;
#else
        uart_pl011_write("FDT parse failed (gic)\n");
        for (;;) asm volatile("wfi" ::: "memory");
#endif
    }

    uint32 cntv_irq_id = 0;
    if (fdt_parse_armv8_timer_virtual_irq(dtb_ptr, &cntv_irq_id) != 0) {
#if defined(AARCH64_PLATFORM_QEMU_VIRT)
        cntv_irq_id = 27;
#else
        uart_pl011_write("FDT parse failed (armv8-timer)\n");
        for (;;) asm volatile("wfi" ::: "memory");
#endif
    }

    aarch64_timer_init_tick_hz(100);
    aarch64_irq_init(gicd_base, gicc_base, cntv_irq_id);
    aarch64_irq_cpu_init();

    aarch64_smp_boot(dtb_ptr);
    arch_enable_interrupts();

    aarch64_bringup_shell_run();

    /* If shell ever returns, halt. */
    for (;;) asm volatile("wfi" ::: "memory");
}
