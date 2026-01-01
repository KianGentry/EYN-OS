#include <kb.h>
#include <isr.h>
#include <idt.h>
#include <util.h>
#include <shell.h>
#include <vga.h>
#include <multiboot.h>
#include <fat32.h>
#include <system.h>
#include <predictive_memory.h>
#include <zero_copy.h>
#include <paging.h>
#include <kernel_api.h>
#include <native_exec.h>
#include <sched.h>
#include <irq.h>
#include <tile_manager.h>
#include <serial.h>
#include <watchdog.h>
#include <help_tui.h>
#include <mm/vmm.h>
#include <partition.h>
#include <gdt.h>
#include <ata.h>
#include <ui_prefs.h>

void* fat32_disk_img = 0;
multiboot_info_t *g_mbi = 0;

int kmain(uint32 magic, multiboot_info_t *mbi)
{
    // Validate multiboot information
    if (!mbi) {
        // Can't use printf yet, so just halt
        asm volatile("hlt");
        return -1;
    }
    
    g_mbi = mbi;
    // Initialize serial early for logging (COM1 @ 115200). Safe even if absent.
    serial_init(SERIAL_COM1, 115200);

    // Install our own GDT (kernel/user segments) + TSS before setting up IDT gates.
    gdt_init();
    if (mbi->flags & MULTIBOOT_INFO_MODS && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;
        if (mods) { // Add null check
            fat32_disk_img = (void*)mods[0].mod_start;
        }
    }

    // Full initialization - all services
    isr_install();
    clearScreen();
    
    printf("EYN-OS Release 15\n");
    printf("Please wait for system services to start...\n\n");

    // Initialize VMM/paging as early as possible so any subsequent malloc()
    // picks a heap start after VMM boot allocations (page tables, etc.).
    // If malloc() runs before vmm_init(), the heap falls back to a low
    // address and can be overwritten by early_alloc(), corrupting heap
    // metadata and later causing copyout/page faults in user mode.
    uint32 ram = detect_available_memory();
    vmm_init(ram);
    vmm_enable_paging();
    
    printf("Starting ATA driver...");
    // Initialize ATA drives immediately
    ata_init_drives();
    printf("ATA driver started.\n");

    // Initialize partition system and auto-mount partitions
    vdrive_init();
    printf("Virtual drive system started.\n");
    
    // Scan drive 0 for partitions and auto-mount EYNFS partitions
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

    // Load and apply persisted UI preferences (theme + font) if present.
    // Safe to do here: VFS is usable after vdrive_init()+mount.
    ui_prefs_load_apply(0);
    printf("UI preferences loaded.\n");

    // Initialize predictive memory management system
    predictive_memory_init();
    printf("Predictive memory manager started.\n");
    
    // Initialize zero-copy file operations system
    zero_copy_init();
    printf("Zero-copy file system started.\n");
    
    // Initialize kernel API system
    eyn_kernel_api_init();
    printf("(Legacy) Kernel API system started.\n");
    
    // Initialize IRQs and PIT timer (sets up IDT/PIC and PIT IRQ0)
    // NOTE: irq_init() clears the IRQ handler table, so it must run
    // before any register_interrupt_handler() calls (including sched_init).
    irq_init();
    printf("IRQ system started.\n");

    // Initialize scheduler (registers IRQ0 tick handler)
    sched_init();
    printf("Scheduler started.\n");

    // Enable interrupts globally now that IDT/PIC/PIT are initialized and
    // the IRQ0 handler is registered. Without this, sched_get_tick_count()
    // never advances (breaking REIV playback timing and other tick-based code).
    __asm__ __volatile__("sti");
    // Initialize watchdog with a sensitive default (~250ms)
    uint32 hz = sched_get_tick_hz();
    uint32 to = (hz ? (hz/4) : 12); // ~0.25s
    watchdog_init(to);
    printf("Watchdog started.\n");
    
    // Initialize native execution system
    native_exec_init();
    printf("(Legacy) Native execution system started.\n");
    
    // Launch tiling manager by default; it provides the graphical shell UI
    start_tiling_manager();

    // Pre-initialize help UI state (build sorted command list) so the help UI
    // can be shown later without rebuilding and to avoid runtime recomputation
    // that may expose memory layout differences.
    extern void help_tui_init_state(void);
    help_tui_init_state();

    // If tiling manager exits (e.g., user closes it), fall back to classic shell
    launch_shell(0);
    
    return 0;
}
