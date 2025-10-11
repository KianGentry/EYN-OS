#include <paging.h>
#include <util.h>
#include <vga.h>
#include <isr.h>
#include <types.h>
#include <string.h>

// Symbols exported by linker script for section bounds
extern uint32 __kernel_text_start;
extern uint32 __kernel_text_end;
extern uint32 __kernel_rodata_start;
extern uint32 __kernel_rodata_end;
extern uint32 __kernel_start;
extern uint32 __kernel_end;

// Global page directory
page_directory_t* current_directory = 0;

// Frame bitmap for tracking used/free physical frames
#define FRAME_BITMAP_SIZE 32768  // 128MB / 4KB = 32768 frames
static uint32 frame_bitmap[FRAME_BITMAP_SIZE / 32];

// Set a bit in the frame bitmap
static void set_frame(uint32 frame_addr) {
    uint32 frame = frame_addr / PAGE_SIZE;
    uint32 idx = frame / 32;
    uint32 off = frame % 32;
    frame_bitmap[idx] |= (1 << off);
}

// Clear a bit in the frame bitmap
static void clear_frame(uint32 frame_addr) {
    uint32 frame = frame_addr / PAGE_SIZE;
    uint32 idx = frame / 32;
    uint32 off = frame % 32;
    frame_bitmap[idx] &= ~(1 << off);
}

// Test if a frame is set
static uint32 test_frame(uint32 frame_addr) {
    uint32 frame = frame_addr / PAGE_SIZE;
    uint32 idx = frame / 32;
    uint32 off = frame % 32;
    return (frame_bitmap[idx] & (1 << off));
}

// Find the first free frame
static uint32 first_free_frame() {
    for (uint32 i = 0; i < FRAME_BITMAP_SIZE / 32; i++) {
        if (frame_bitmap[i] != 0xFFFFFFFF) {
            for (uint32 j = 0; j < 32; j++) {
                uint32 bit = 1 << j;
                if (!(frame_bitmap[i] & bit)) {
                    return i * 32 + j;
                }
            }
        }
    }
    return (uint32)-1; // No free frames
}

// Allocate a frame
static void alloc_frame(page_t* page, int is_kernel, int is_writeable) {
    if (page->frame != 0) {
        return; // Frame already allocated
    } else {
        uint32 idx = first_free_frame();
        if (idx == (uint32)-1) {
            // PANIC! no free frames
            printf("No free frames!\n");
            return;
        }
        set_frame(idx * PAGE_SIZE);
        page->present = 1;
        page->rw = (is_writeable) ? 1 : 0;
        page->user = (is_kernel) ? 0 : 1;
        page->frame = idx;
    }
}

// Free a frame
static void free_frame(page_t* page) {
    uint32 frame;
    if (!(frame = page->frame)) {
        return; // Frame not allocated
    } else {
        clear_frame(frame * PAGE_SIZE);
        page->frame = 0;
        page->present = 0;
    }
}

// Get page from address
page_t* get_page(uint32 address, int make, page_directory_t* dir) {
    // Turn the address into an index
    address /= PAGE_SIZE;
    uint32 table_idx = address / PAGE_ENTRIES;
    uint32 page_idx = address % PAGE_ENTRIES;

    // Find the page table containing this address
    if (dir->tables[table_idx]) {
        return &dir->tables[table_idx]->pages[page_idx];
    } else if (make) {
        // Create the page table
        uint32 tmp;
        dir->tables[table_idx] = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &tmp);
        memset(dir->tables[table_idx], 0, PAGE_SIZE);
        dir->physical_addr[table_idx] = tmp | 0x7; // PRESENT, RW, US
        return &dir->tables[table_idx]->pages[page_idx];
    } else {
        return 0;
    }
}

// Page fault handler - called from ISR 14
void page_fault_handler(regs_t* r) {
    uint32 fault_addr;
    asm volatile("mov %%cr2, %0" : "=r" (fault_addr));

    // The error code gives us details about what happened
    int present = !(r->err_code & PF_PRESENT);
    int rw = r->err_code & PF_RW;
    int us = r->err_code & PF_USER;
    int reserved = r->err_code & PF_RESERVED;

    // Output an error message
    printf("Page fault! ( ");
    if (present) printf("present ");
    if (rw) printf("read-only ");
    if (us) printf("user-mode ");
    if (reserved) printf("reserved ");
    printf(") at 0x%X\n", fault_addr);

    // If the fault was caused by a reserved bit being set, we need to panic
    if (reserved) {
        printf("Reserved bit was set, panic!\n");
        return;
    }

    // If the fault was caused by a page not being present, we need to allocate it
    if (present) {
        // Get the page
        page_t* page = get_page(fault_addr, 1, current_directory);
        if (page) {
            alloc_frame(page, (r->cs & 3) == 0, 1);
            return;
        }
    }

    // If we get here, the fault was caused by a non-present page
    printf("Page fault at 0x%X\n", fault_addr);
}

// Initialize paging
void init_paging() {
    // Clear the frame bitmap
    memset(frame_bitmap, 0, sizeof(frame_bitmap));

    // Create a page directory
    uint32 phys;
    current_directory = (page_directory_t*)kmalloc_ap(sizeof(page_directory_t), &phys);
    memset(current_directory, 0, sizeof(page_directory_t));
    current_directory->physical_addr[0] = (uint32)current_directory | 0x7; // PRESENT, RW, US

    // Identity map the first 4MB (RW for now)
    for (int i = 0; i < 1024; i++) {
        alloc_frame(get_page(i * PAGE_SIZE, 1, current_directory), 1, 1);
    }

    // Page fault handler is already registered as ISR 14
    // We'll modify the ISR system to call our handler

    // Switch to our page directory
    switch_page_directory(current_directory);

    printf("Paging initialized\n");
}

// Switch page directory
void switch_page_directory(page_directory_t* new_dir) {
    current_directory = new_dir;
    asm volatile("mov %0, %%cr3":: "r"(new_dir->physical_addr));
    uint32 cr0;
    asm volatile("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000; // Enable paging!
    asm volatile("mov %0, %%cr0":: "r"(cr0));
}

// Map a page
void map_page(uint32 virtual_addr, uint32 physical_addr, int user, int rw) {
    page_t* page = get_page(virtual_addr, 1, current_directory);
    if (page) {
        page->present = 1;
        page->rw = rw;
        page->user = user;
        page->frame = physical_addr / PAGE_SIZE;
    }
}

// Unmap a page
void unmap_page(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    if (page) {
        free_frame(page);
    }
    // Flush the TLB
    asm volatile("invlpg (%0)" : : "r" (virtual_addr) : "memory");
}

// Kernel memory allocation with physical address
void* kmalloc_p(uint32 size, uint32* physical) {
    if (size == 0) return 0;

    // Round up to page size
    uint32 pages = (size / PAGE_SIZE) + 1;
    uint32 frame = first_free_frame();
    if (frame == (uint32)-1) return 0; // No free frames

    // Mark frames as used
    for (uint32 i = 0; i < pages; i++) {
        set_frame((frame + i) * PAGE_SIZE);
    }

    // Map the frames
    uint32 virtual_addr = KERNEL_VIRTUAL_BASE + (frame * PAGE_SIZE);
    for (uint32 i = 0; i < pages; i++) {
        map_page(virtual_addr + (i * PAGE_SIZE), (frame + i) * PAGE_SIZE, 0, 1);
    }

    if (physical) *physical = frame * PAGE_SIZE;
    return (void*)virtual_addr;
}

// Kernel memory allocation aligned
void* kmalloc_a(uint32 size) {
    return kmalloc_p(size, 0);
}

// Kernel memory allocation aligned with physical address
void* kmalloc_ap(uint32 size, uint32* physical) {
    return kmalloc_p(size, physical);
}

// Free kernel memory
void kfree(void* ptr) {
    if (!ptr) return;

    uint32 virtual_addr = (uint32)ptr;
    uint32 frame = (virtual_addr - KERNEL_VIRTUAL_BASE) / PAGE_SIZE;
    
    // Clear frames
    clear_frame(frame * PAGE_SIZE);
    
    // Unmap pages
    unmap_page(virtual_addr);
}

// Create a new page directory for a process
page_directory_t* create_process_page_directory() {
    uint32 phys;
    page_directory_t* dir = (page_directory_t*)kmalloc_ap(sizeof(page_directory_t), &phys);
    memset(dir, 0, sizeof(page_directory_t));
    
    // Copy kernel mappings
    for (int i = 0; i < 1024; i++) {
        if (current_directory->tables[i]) {
            dir->tables[i] = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &phys);
            memcpy(dir->tables[i], current_directory->tables[i], sizeof(page_table_t));
            dir->physical_addr[i] = phys | 0x7;
        }
    }
    
    return dir;
}

// Destroy a process page directory
void destroy_process_page_directory(page_directory_t* dir) {
    if (!dir) return;
    
    // Free user space pages
    for (int i = 0; i < 1024; i++) {
        if (dir->tables[i] && i >= 256) { // User space starts at 1GB
            for (int j = 0; j < PAGE_ENTRIES; j++) {
                if (dir->tables[i]->pages[j].present) {
                    free_frame(&dir->tables[i]->pages[j]);
                }
            }
            kfree(dir->tables[i]);
        }
    }
    
    kfree(dir);
}

// Copy a page directory
void copy_page_directory(page_directory_t* dest, page_directory_t* src) {
    if (!dest || !src) return;
    
    // Copy kernel mappings
    for (int i = 0; i < 1024; i++) {
        if (src->tables[i] && i < 256) { // Kernel space
            uint32 phys;
            dest->tables[i] = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &phys);
            memcpy(dest->tables[i], src->tables[i], sizeof(page_table_t));
            dest->physical_addr[i] = phys | 0x7;
        }
    }
}

// Switch to a process
void switch_to_process(page_directory_t* dir) {
    switch_page_directory(dir);
}

// Memory protection functions
void protect_page(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    if (page) {
        page->rw = 0;
    }
}

void unprotect_page(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    if (page) {
        page->rw = 1;
    }
}

int is_page_present(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    return page ? page->present : 0;
}

int is_page_user(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    return page ? page->user : 0;
}

int is_page_writable(uint32 virtual_addr) {
    page_t* page = get_page(virtual_addr, 0, current_directory);
    return page ? page->rw : 0;
}

// Optional guards: can be called after switch_page_directory when paging is enabled.
void paging_install_null_guard(void) {
    if (!current_directory) return;
    // Ensure page 0 is not present
    page_t* p0 = get_page(0x0, 1, current_directory);
    if (p0) { p0->present = 0; p0->rw = 0; p0->user = 0; p0->frame = 0; }
    asm volatile("invlpg (0)" ::: "memory");
}

void paging_protect_kernel_text_ro(void) {
    if (!current_directory) return;
    uint32 start = (uint32)&__kernel_text_start;
    uint32 end   = (uint32)&__kernel_text_end;
    for (uint32 va = start & ~(PAGE_SIZE-1); va < end; va += PAGE_SIZE) {
        page_t* pg = get_page(va, 0, current_directory);
        if (pg && pg->present) { pg->rw = 0; }
    }
    // Also make rodata read-only
    start = (uint32)&__kernel_rodata_start;
    end   = (uint32)&__kernel_rodata_end;
    for (uint32 va = start & ~(PAGE_SIZE-1); va < end; va += PAGE_SIZE) {
        page_t* pg = get_page(va, 0, current_directory);
        if (pg && pg->present) { pg->rw = 0; }
    }
    // TLB shootdown: flush all (cr3 reload)
    uint32 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3));
}
