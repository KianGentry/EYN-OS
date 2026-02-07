#include <misc/types.h>
#include <misc/fdt.h>
#include <arch.h>
#include <cpu/aarch64/gicv2.h>
#include <cpu/aarch64/timer.h>
#include <cpu/aarch64/smp.h>
#include <drivers/aarch64/fb_simple.h>
#include <drivers/aarch64/virtio_input.h>
#include <drivers/aarch64/virtio_blk.h>
#include <utilities/aarch64/heap.h>

#include <graphics/gfx.h>
#include <shell.h>

#include <ata.h>

/* Full-mode bring-up entry: keeps the existing AArch64 interrupt/timer/SMP code
 * but drops into an interactive serial shell instead of the tick-only loop.
 */

void aarch64_irq_init(uint64 gicd_base, uint64 gicc_base, uint32 timer_irq_id);
void aarch64_irq_cpu_init(void);

void uart_pl011_init_115200(void);
void uart_pl011_write(const char* s);
void uart_pl011_write_hex64(uint64 v);

void aarch64_shell_set_meminfo(uint64 ram_base, uint64 ram_size);
void aarch64_multiboot_compat_init_from_fb(void);

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

static void aarch64_disable_alignment_check(void) {
    /*
     * QEMU (and some firmware defaults) may enable strict alignment checking at EL1.
     * The existing codebase contains structs with byte fields up front (e.g. ATA
     * drive_info_t), which makes subsequent fields naturally unaligned. GCC may
     * still emit wider loads/stores that would fault if SCTLR_EL1.A is set.
     */
    uint64 sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1ull << 1); /* A (alignment check enable) */
    asm volatile("msr sctlr_el1, %0\n\tisb" :: "r"(sctlr) : "memory");
}

void kernel_main(uint64 dtb_ptr) {
    uart_pl011_init_115200();

    aarch64_enable_fp_simd();
    aarch64_disable_alignment_check();

    uart_pl011_write("\nEYN-OS AArch64 full bring-up\n");

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    if (dtb_ptr == 0) {
        extern const uint8 _binary_virt_dtb_start[];
        dtb_ptr = (uint64)(const void*)_binary_virt_dtb_start;
    }
#endif

    uart_pl011_write("DTB @ ");
    uart_pl011_write_hex64(dtb_ptr);
    uart_pl011_write("\n");

#if defined(AARCH64_PLATFORM_QEMU_VIRT)
    uint64 fwcfg_base_early = 0;
    if (fdt_parse_qemu_fw_cfg_mmio(dtb_ptr, &fwcfg_base_early) == 0) {
        uart_pl011_write("FW_CFG @ ");
        uart_pl011_write_hex64(fwcfg_base_early);
        uart_pl011_write("\n");
    } else {
        uart_pl011_write("FW_CFG not found in DTB\n");
    }
#endif

    if (fb_simple_init(dtb_ptr) == 0) {
        uart_pl011_write("FB init ok\n");

        uint64 fb_base = 0;
        uint32 fb_w = 0, fb_h = 0, fb_stride = 0, fb_bpp = 0;
        const char* fb_fmt = 0;
        if (fb_simple_get_info(&fb_base, &fb_w, &fb_h, &fb_stride, &fb_bpp, &fb_fmt) == 0) {
            uart_pl011_write("FB base ");
            uart_pl011_write_hex64(fb_base);
            uart_pl011_write(" w ");
            uart_pl011_write_hex64((uint64)fb_w);
            uart_pl011_write(" h ");
            uart_pl011_write_hex64((uint64)fb_h);
            uart_pl011_write(" stride ");
            uart_pl011_write_hex64((uint64)fb_stride);
            uart_pl011_write(" bpp ");
            uart_pl011_write_hex64((uint64)fb_bpp);
            uart_pl011_write(" fmt ");
            uart_pl011_write(fb_fmt ? fb_fmt : "(null)");
            uart_pl011_write("\n");
        } else {
            uart_pl011_write("FB info query failed\n");
        }

        /* Make the GUI window visibly change immediately (helps debug ramfb refresh). */
        fb_simple_clear_rgb(255, 0, 255);
        fb_simple_write("EYN-OS AArch64 full bring-up\n");
        fb_simple_flush();

        // Provide multiboot-style framebuffer information for shared VGA/GUI code.
        aarch64_multiboot_compat_init_from_fb();

        /* Enable the arch-neutral gfx facade for the interactive shell. */
        gfx_init_default();
    } else {
        uart_pl011_write("FB init failed, code ");
        uart_pl011_write_hex64((uint64)fb_simple_last_error());
        uart_pl011_write("\n");
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

    /* Optional: virtio-blk disk (enables VFS/EYNFS/FAT32 on QEMU 'virt'). */
    int brc = virtio_blk_init(dtb_ptr);
    if (brc == 0) {
        uart_pl011_write("virtio-blk ready\n");
        if (fb_simple_ready()) {
            fb_simple_write("virtio-blk ready\n");
        }

        /* Wire the legacy ATA sector API to virtio-blk. */
        ata_init_drives();
    } else {
        uart_pl011_write("virtio-blk not ready\n");
        if (fb_simple_ready()) {
            fb_simple_write("virtio-blk not ready\n");
        }
    }

    if (fb_simple_ready()) {
        fb_simple_write("DTB @ ");
        fb_write_hex64(dtb_ptr);
        fb_simple_putc('\n');
        fb_simple_flush();
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

        /* Enable dynamic allocations for future subsystem porting. */
        aarch64_heap_init(ram_base, ram_size);

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

    /* Drop into the shared shell core. */
    launch_shell(0);

    /* If shell ever returns, halt. */
    for (;;) asm volatile("wfi" ::: "memory");
}
