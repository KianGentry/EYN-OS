#include <kb.h>
#include <isr.h>
#include <idt.h>
#include <util.h>
#include <shell.h>
#include <vga.h>
#include <vga_text.h>
#include <multiboot.h>
#include <fat32.h>
#include <system.h>
#include <predictive_memory.h>
#include <zero_copy.h>
#include <paging.h>
#include <kernel_api.h>
#include <native_exec.h>
#include <misc/sched.h>
#include <irq.h>
#include <tile_manager.h>
#include <serial.h>
#include <watchdog.h>
#include <help_tui.h>
#include <fs/vfs.h>
#include <misc/tui.h>
#include <mm/vmm.h>
#include <partition.h>
#include <gdt.h>
#include <ata.h>
#include <ui_prefs.h>
#include <cpu/fpu.h>
#include <arch.h>
#include <capabilities.h>
#include <crashlog.h>
#include <network/netstack.h>

void* fat32_disk_img = 0;
multiboot_info_t *g_mbi = 0;

/* OS and kernel version information */
const char* g_os_version = "EYN-OS Release 15";
const char* g_kernel_version = "v1.0";

static inline void* kernel_u32_to_ptr(uint32 address) {
    return (void*)(uintptr)address;
}

#if defined(EYNOS_ARCH_AMD64)
extern uint64 pd_table3[512];

static uint32 align_down_2m(uint32 value) {
    return value & 0xFFE00000u;
}

static uint32 align_up_2m(uint32 value) {
    return (value + 0x001FFFFFu) & 0xFFE00000u;
}

static void amd64_patch_bootstrap_framebuffer_mapping(multiboot_info_t* mbi) {
    if (!mbi || !mbi->framebuffer_addr) return;

    uint32 fb_addr = (uint32)mbi->framebuffer_addr;
    if (fb_addr < 0xC0000000u) return;

    uint32 fb_size = 0;
    if (mbi->framebuffer_pitch && mbi->framebuffer_height) {
        fb_size = mbi->framebuffer_pitch * mbi->framebuffer_height;
    }
    if (fb_size == 0) {
        fb_size = 2u * 1024u * 1024u;
    }

    uint32 start = align_down_2m(fb_addr);
    uint32 end = align_up_2m(fb_addr + fb_size);

    for (uint32 va = start; va < end; va += 0x00200000u) {
        uint32 index = (va - 0xC0000000u) >> 21;
        if (index >= 512u) break;
        pd_table3[index] = ((uint64)va & 0xFFFFFFFFFFE00000ull) | 0x83ull;
    }

    uintptr cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3));

    printf("[boot] amd64: patched bootstrap framebuffer mapping at 0x%X (%u bytes)\n",
           (unsigned)fb_addr,
           (unsigned)fb_size);
}
#endif

static char boot_ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)('a' + (c - 'A'));
    return c;
}

static int boot_starts_with_icase(const char* s, const char* pfx) {
    if (!s || !pfx) return 0;
    while (*pfx) {
        if (boot_ascii_tolower(*s++) != boot_ascii_tolower(*pfx++)) return 0;
    }
    return 1;
}

static const char* boot_skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t' || *s == '\r')) ++s;
    return s;
}

static void boot_trim_trailing_ws(char* s) {
    if (!s) return;
    int len = 0;
    while (s[len]) ++len;
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[len - 1] = '\0';
        --len;
    }
}

static int boot_parse_bool(const char* s, int* out_value) {
    s = boot_skip_ws(s);
    if (!s || !*s) return -1;

    if (boot_starts_with_icase(s, "true") || boot_starts_with_icase(s, "yes") || boot_starts_with_icase(s, "on")) {
        if (out_value) *out_value = 1;
        return 0;
    }
    if (boot_starts_with_icase(s, "false") || boot_starts_with_icase(s, "no") || boot_starts_with_icase(s, "off")) {
        if (out_value) *out_value = 0;
        return 0;
    }

    if (*s >= '0' && *s <= '9') {
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            ++s;
        }
        if (out_value) *out_value = (v != 0);
        return 0;
    }

    return -1;
}

static int boot_cfg_line_requests_text_mode(const char* line) {
    if (!line) return 0;

    const char* s = boot_skip_ws(line);
    if (!*s || *s == '#') return 0;

    const char* eq = s;
    while (*eq && *eq != '=' && *eq != ':' && *eq != ' ' && *eq != '\t') ++eq;
    if (*eq != '=' && *eq != ':') return 0;

    int key_len = (int)(eq - s);
    if (key_len != 4) return 0;
    if (!(boot_ascii_tolower(s[0]) == 't' && boot_ascii_tolower(s[1]) == 'e' && boot_ascii_tolower(s[2]) == 'x' && boot_ascii_tolower(s[3]) == 't')) {
        return 0;
    }

    int value = 0;
    if (boot_parse_bool(eq + 1, &value) != 0) return 0;
    return value ? 1 : 0;
}

static int boot_cfg_requests_text_mode(uint8 drive) {
    char buf[512];
    int n = vfs_read_file(drive, "/config/boot.cfg", buf, (int)sizeof(buf) - 1);
    if (n < 0) {
        n = vfs_read_file(drive, "/boot.cfg", buf, (int)sizeof(buf) - 1);
        if (n < 0) return 0;
    }
    buf[n] = '\0';

    char* line = buf;
    while (line && *line) {
        char* next = line;
        while (*next && *next != '\n') ++next;
        if (*next == '\n') {
            *next = '\0';
            ++next;
        }
        boot_trim_trailing_ws(line);
        if (boot_cfg_line_requests_text_mode(line)) return 1;
        line = next;
    }

    return 0;
}

static int boot_escape_requested(void) {
    for (;;) {
        int key = tui_read_key();
        if (!key) return 0;
        if (key == 27 || key == -27) return 1;
    }
}

int kmain(uint32 magic, multiboot_info_t *mbi)
{
    // Validate multiboot information
    if (!mbi) {
        // Can't use printf yet, so just halt
        arch_halt_forever();
        return -1;
    }
    
    g_mbi = mbi;
    // Initialize serial early for logging (COM1 @ 115200). Safe even if absent.
    serial_init(SERIAL_COM1, 115200);

    if (mbi->flags & MULTIBOOT_INFO_MODS) {
        printf("[boot] multiboot modules: %u\n", (unsigned)mbi->mods_count);
        if (mbi->mods_count > 0 && mbi->mods_addr) {
            multiboot_module_t* mods = (multiboot_module_t*)kernel_u32_to_ptr((uint32)mbi->mods_addr);
            printf("[boot] mod0: start=0x%X end=0x%X cmdline=%s\n",
                   (unsigned)mods[0].mod_start,
                   (unsigned)mods[0].mod_end,
                   mods[0].cmdline ? (const char*)kernel_u32_to_ptr((uint32)mods[0].cmdline) : "(null)");
        }
    } else {
        printf("[boot] multiboot modules: none (flags=0x%X)\n", (unsigned)mbi->flags);
    }

    vga_log_boot_capabilities();

#if defined(EYNOS_ARCH_AMD64)
    amd64_patch_bootstrap_framebuffer_mapping(mbi);
#endif

    // Install kernel/user code-data descriptors plus TSS before IDT setup.
    gdt_init();
    if (mbi->flags & MULTIBOOT_INFO_MODS && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)kernel_u32_to_ptr((uint32)mbi->mods_addr);
        if (mods) { // Add null check
            fat32_disk_img = kernel_u32_to_ptr((uint32)mods[0].mod_start);
        }
    }

    // Full initialization - all services
    isr_install();

    // Enable x87 FPU early so ring3 programs can use float/double.
    fpu_init();
    clearScreen();
    
    printf("EYN-OS Release 15\n");
    printf("Please wait for system services to start.\n\n");

    // Initialize VMM/paging as early as possible so any subsequent malloc()
    // picks a heap start after VMM boot allocations (page tables, etc.).
    // If malloc() runs before vmm_init(), the heap falls back to a low
    // address and can be overwritten by early_alloc(), corrupting heap
    // metadata and later causing copyout/page faults in user mode.
    uint32 ram = detect_available_memory();
        printf("Detected RAM: %u KB\n", (unsigned)(ram / 1024u));
    printf("Starting memory manager...\n");
    vmm_init(ram);
        printf("Frames: %u total, %u free\n",
            (unsigned)vmm_get_total_frames(), (unsigned)vmm_get_free_frames());
        printf("Enabling paging...");
#if defined(EYNOS_ARCH_AMD64)
    vmm_mark_paging_enabled();
    printf("Paging already enabled by bootloader (AMD64)...");
#else
    vmm_enable_paging();
#endif
    printf("Done.\n");

    // Initialize capability secret/registry before any user-facing objects.
    cap_init();
    
    printf("Starting ATA driver...");
    // Initialize ATA drives immediately
    ata_init_drives();
    printf("Done.\n");

    // Initialize partition system and auto-mount partitions
    printf("Starting virtual drive system...");
    vdrive_init();
    printf("Done.\n");
    
    // Scan drive 0 for partitions and auto-mount EYNFS partitions
    printf("Scanning drive 0 for partitions...\n");
    {
        disk_info_t disk;
        if (partition_read_table(0, &disk) == 0 && disk.partition_count > 0) {
            // Auto-mount first EYNFS partition as drive 0
            for (int i = 0; i < disk.partition_count; i++) {
                if (disk.partitions[i].type == PART_TYPE_EYNFS) {
                    vdrive_mount(0, i, 0);  // Mount partition i from drive 0 as virtual drive 0
                    printf("Mounted EYNFS partition %d on drive 0.\n", i);
                    break;
                }
            }
            // Auto-initialize swap partition if present
            for (int i = 0; i < disk.partition_count; i++) {
                if (disk.partitions[i].type == PART_TYPE_EYNOS_SWAP) {
                    swap_partition_init(0, i);
                    printf("Mounted swap partition %d on drive 0.\n", i);
                    break;
                }
            }
        }
    }

    // Initialize crash log after VFS is available.
    crashlog_init(0);

    // Load and apply persisted UI preferences (theme + font) if present.
    // Safe to do here: VFS is usable after vdrive_init()+mount.
    printf("Loading UI preferences...");
    ui_prefs_load_apply(0);
    printf("Done.\n");

    int boot_text_mode = 0;

#if !CONFIG_GUI_ENABLED && !CONFIG_TTY_ENABLED
    printf("Fatal: both GUI and TTY are disabled in this build.\n");
    arch_halt_forever();
#elif !CONFIG_GUI_ENABLED
    // TTY-only build: force text mode.
    boot_text_mode = 1;
#elif !CONFIG_TTY_ENABLED
    // GUI-only build: force GUI mode.
    boot_text_mode = 0;
#else
    // When both are enabled, allow boot configuration or ESC to request text mode.
    boot_text_mode = boot_cfg_requests_text_mode(0) || boot_escape_requested();
#endif

    g_boot_text_mode = boot_text_mode;
    if (boot_text_mode) {
#if CONFIG_TTY_ENABLED
        // If a framebuffer was provided by the bootloader, prefer using the
        // framebuffer-backed console (drawText). Only initialize the legacy
        // VGA text driver when no framebuffer is present.
        if (!(g_mbi && g_mbi->framebuffer_addr)) {
            vga_text_init();
        }
#endif
        printf("Boot mode: text CLI\n");
    }

    // Initialize predictive memory management system
    printf("Starting predictive memory manager...");
    predictive_memory_init();
    printf("Done.\n");
    
    // Initialize zero-copy file operations system
    printf("Starting zero-copy file system...");
    zero_copy_init();
    printf("Done.\n");
    
    // Initialize kernel API system
    printf("Starting (Legacy) kernel API system...");
    eyn_kernel_api_init();
    printf("Done.\n");
    
    // Initialize IRQs and PIT timer (sets up IDT/PIC and PIT IRQ0)
    // NOTE: irq_init() clears the IRQ handler table, so it must run
    // before any register_interrupt_handler() calls (including sched_init).
    printf("Starting IRQ system...");
    irq_init();
    printf("Done.\n");

    // Initialize scheduler (registers IRQ0 tick handler)
    printf("Starting scheduler...");
    sched_init();
    printf("Done.\n");

    // Enable interrupts globally now that IDT/PIC/PIT are initialized and
    // the IRQ0 handler is registered. Without this, sched_get_tick_count()
    // never advances (breaking REIV playback timing and other tick-based code).
    __asm__ __volatile__("sti");

    printf("Starting network stack...");
    {
        int net_rc = net_init_e1000_default();
        if (net_rc == 0) {
            printf("Done.\n");
        } else {
            /*
             * Installer and userland may still proceed using local package cache.
             * Keep boot non-fatal on platforms without a usable NIC.
             */
            printf("Skipped (rc=%d).\n", net_rc);
        }
    }

    // Initialize watchdog with a sensitive default (~250ms)
    printf("Starting watchdog...");
    uint32 hz = sched_get_tick_hz();
    uint32 to = (hz ? (hz/4) : 12); // ~0.25s
    watchdog_init(to);
    printf("Done.\n");
    
    // Initialize native execution system
    printf("Starting (Legacy) native execution system...");
    native_exec_init();
    printf("Done.\n");
    
    if (boot_text_mode) {
        clearScreen();
        launch_shell(0);
    } else {
#if CONFIG_GUI_ENABLED
        // Launch interactive UI path.
        printf("Starting Tiling Manager...");
        start_tiling_manager();
#if CONFIG_TTY_ENABLED
        // If the tiling manager exits, fall back to the classic shell only when TTY is available.
        launch_shell(0);
#else
        printf("Warning: GUI exited; TTY is disabled in this build.\n");
        arch_halt_forever();
#endif
#else
        // GUI is disabled, so the only valid path is the shell.
        printf("Warning: GUI is disabled; starting shell.\n");
        launch_shell(0);
#endif
    }
    
    return 0;
}
