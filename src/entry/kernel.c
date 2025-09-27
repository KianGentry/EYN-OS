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

void* fat32_disk_img = 0;
multiboot_info_t *g_mbi = 0;

// Forward declaration for ATA driver functions
extern void ata_init_drives(void);

int kmain(uint32 magic, multiboot_info_t *mbi)
{
    // Validate multiboot information
    if (!mbi) {
        // Can't use printf yet, so just halt
        asm volatile("hlt");
        return -1;
    }
    
    g_mbi = mbi;
    if (mbi->flags & MULTIBOOT_INFO_MODS && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;
        if (mods) { // Add null check
            fat32_disk_img = (void*)mods[0].mod_start;
        }
    }

    // Full initialization - all services
    isr_install();
    clearScreen();
    
    printf("EYN-OS Release 14\n");
    printf("Type 'help' for a list of commands.\n\n");
    
    // Initialize ATA drives immediately
    ata_init_drives();

    // Initialize paging system (temporarily disabled for debugging)
    // init_paging();

    // Initialize predictive memory management system
    predictive_memory_init();
    
    // Initialize zero-copy file operations system
    zero_copy_init();
    
    // Initialize kernel API system
    eyn_kernel_api_init();
    
    // Initialize scheduler
    sched_init();
    
    // Initialize IRQs and PIT timer (enables IRQ0 dispatch)
    irq_init();
    
    // Initialize native execution system
    native_exec_init();
    
    // Launch tiling manager by default; it provides the graphical shell UI
    start_tiling_manager();

    // If tiling manager exits (e.g., user closes it), fall back to classic shell
    launch_shell(0);
    
    return 0;
}
