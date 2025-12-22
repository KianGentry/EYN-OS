/*
 * vmm.c — Virtual Memory Manager Implementation for EYN-OS
 *
 * Complete i386 paging subsystem with:
 * - 4KB page support (4MB pages optional)
 * - Demand paging and lazy allocation
 * - Copy-on-write for fork()
 * - Clock page replacement algorithm
 * - Swap integration
 * - Working-set tracking for thrashing avoidance
 */

#include "vmm.h"
#include <types.h>
#include <string.h>
#include <vga.h>
#include <panic.h>
#include <multiboot.h>

/* External multiboot info for framebuffer mapping */
extern multiboot_info_t *g_mbi;

/* Linker symbols for kernel section boundaries */
extern uint32 __kernel_start;
extern uint32 __kernel_end;
extern uint32 __kernel_text_start;
extern uint32 __kernel_text_end;

// GLOBAL STATE

/* Frame allocator instance */
static frame_allocator_t g_frame_alloc;

/* Swap state */
static swap_state_t g_swap;

/* Clock page replacement state */
static clock_state_t g_clock;

/* Kernel address space (always mapped, shared by all processes) */
address_space_t vmm_kernel_as;

/* Currently active address space */
address_space_t* vmm_current_as = &vmm_kernel_as;

/* Early boot: need physical allocation before paging is on */
static uint32 early_heap_ptr = 0;
static int paging_enabled = 0;

// LOW-LEVEL CR REGISTER ACCESS

static inline uint32 read_cr0(void) {
    uint32 val;
    asm volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint32 val) {
    asm volatile("mov %0, %%cr0" :: "r"(val));
}

static inline uint32 read_cr2(void) {
    uint32 val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint32 read_cr3(void) {
    uint32 val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint32 val) {
    asm volatile("mov %0, %%cr3" :: "r"(val));
}

// TLB MANAGEMENT

void invalidate_tlb_entry(uint32 va) {
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void invalidate_tlb_all(void) {
    /* Reloading CR3 flushes all non-global TLB entries */
    write_cr3(read_cr3());
}

/*
 * FRAME ALLOCATOR — BITMAP-BASED
 * Each bit represents one 4KB frame. We scan for zeros to find free frames.
 * search_hint accelerates sequential allocations by remembering where we last
 * found a free frame.
 */

/* Mark a frame as used */
static inline void frame_set(uint32 frame_num) {
    g_frame_alloc.bitmap[frame_num / 32] |= (1 << (frame_num % 32));
}

/* Mark a frame as free */
static inline void frame_clear(uint32 frame_num) {
    g_frame_alloc.bitmap[frame_num / 32] &= ~(1 << (frame_num % 32));
}

/* Test if a frame is used */
static inline int frame_test(uint32 frame_num) {
    return (g_frame_alloc.bitmap[frame_num / 32] & (1 << (frame_num % 32))) != 0;
}

/* Initialize frame allocator based on detected RAM */
static void frame_alloc_init(uint32 total_ram_bytes) {
    memset(&g_frame_alloc, 0, sizeof(g_frame_alloc));
    
    /* Calculate usable frames (cap at MAX_FRAMES) */
    uint32 detected_frames = total_ram_bytes / PAGE_SIZE;
    g_frame_alloc.total_frames = (detected_frames > MAX_FRAMES) ? MAX_FRAMES : detected_frames;
    g_frame_alloc.free_frames = g_frame_alloc.total_frames;
    g_frame_alloc.search_hint = 0;
    
    /* Reserve first 1MB for BIOS/legacy (256 frames) — these are never allocatable */
    for (uint32 i = 0; i < 256; i++) {
        frame_set(i);
        g_frame_alloc.free_frames--;
    }
    
    /* Reserve frames occupied by kernel (1MB to kernel_end) */
    uint32 kernel_end_frame = ((uint32)&__kernel_end) / PAGE_SIZE + 1;
    for (uint32 i = 256; i <= kernel_end_frame && i < g_frame_alloc.total_frames; i++) {
        frame_set(i);
        g_frame_alloc.free_frames--;
    }
    
    /* Start searching after kernel */
    g_frame_alloc.search_hint = kernel_end_frame / 32;
}

/* Allocate a single physical frame, returns physical address or 0 on failure */
uint32 frame_alloc(void) {
    if (g_frame_alloc.free_frames == 0) {
        /* Try page replacement before giving up */
        uint32 evicted = clock_evict_page();
        if (evicted == 0) {
            return 0;  /* Out of memory */
        }
        return evicted;
    }
    
    /* Search starting from hint for faster sequential allocations */
    uint32 start_word = g_frame_alloc.search_hint;
    uint32 total_words = g_frame_alloc.total_frames / 32;
    
    for (uint32 i = 0; i < total_words; i++) {
        uint32 word_idx = (start_word + i) % total_words;
        
        /* Skip fully-allocated words */
        if (g_frame_alloc.bitmap[word_idx] == 0xFFFFFFFF) {
            continue;
        }
        
        /* Find first zero bit in this word */
        uint32 word = g_frame_alloc.bitmap[word_idx];
        for (uint32 bit = 0; bit < 32; bit++) {
            if ((word & (1 << bit)) == 0) {
                uint32 frame_num = word_idx * 32 + bit;
                if (frame_num >= g_frame_alloc.total_frames) {
                    continue;  /* Beyond detected RAM */
                }
                
                frame_set(frame_num);
                g_frame_alloc.free_frames--;
                g_frame_alloc.search_hint = word_idx;
                
                return frame_num * PAGE_SIZE;
            }
        }
    }
    
    return 0;  /* No free frames found */
}

/* Free a physical frame */
void frame_free(uint32 phys_addr) {
    uint32 frame_num = phys_addr / PAGE_SIZE;
    
    /* Sanity checks: don't free reserved low memory or beyond RAM */
    if (frame_num < 256 || frame_num >= g_frame_alloc.total_frames) {
        return;
    }
    
    if (frame_test(frame_num)) {
        frame_clear(frame_num);
        g_frame_alloc.free_frames++;
        
        /* Update hint to free region for faster reallocation */
        if (frame_num / 32 < g_frame_alloc.search_hint) {
            g_frame_alloc.search_hint = frame_num / 32;
        }
    }
}

void vmm_reserve_phys_range(uint32 phys_start, uint32 phys_end) {
    if (phys_end <= phys_start) {
        return;
    }

    uint32 start = phys_start & PAGE_MASK;
    uint32 end = (phys_end + PAGE_SIZE - 1) & PAGE_MASK;

    for (uint32 pa = start; pa < end; pa += PAGE_SIZE) {
        uint32 frame_num = pa / PAGE_SIZE;
        if (frame_num >= g_frame_alloc.total_frames) {
            break;
        }
        if (!frame_test(frame_num)) {
            frame_set(frame_num);
            if (g_frame_alloc.free_frames) {
                g_frame_alloc.free_frames--;
            }
        }
    }
}

/* Allocate contiguous frames for DMA (returns start physical address) */
uint32 frame_alloc_contiguous(uint32 count) {
    if (count == 0 || count > g_frame_alloc.free_frames) {
        return 0;
    }
    
    uint32 run_start = 0;
    uint32 run_length = 0;
    
    /* Linear scan for contiguous region */
    for (uint32 i = 256; i < g_frame_alloc.total_frames; i++) {
        if (!frame_test(i)) {
            if (run_length == 0) {
                run_start = i;
            }
            run_length++;
            if (run_length == count) {
                /* Found enough contiguous frames */
                for (uint32 j = run_start; j < run_start + count; j++) {
                    frame_set(j);
                    g_frame_alloc.free_frames--;
                }
                return run_start * PAGE_SIZE;
            }
        } else {
            run_length = 0;
        }
    }
    
    return 0;  /* No contiguous region found */
}

uint32 vmm_get_free_frames(void) {
    return g_frame_alloc.free_frames;
}

uint32 vmm_get_total_frames(void) {
    return g_frame_alloc.total_frames;
}

/*
 * EARLY BOOT ALLOCATION
 * Before paging is enabled, use a bump allocator after the kernel to carve
 * page directories/tables and mark the backing frames as busy.
 */

static void* early_alloc(uint32 size, uint32 alignment) {
    /* Align up */
    early_heap_ptr = (early_heap_ptr + alignment - 1) & ~(alignment - 1);
    
    void* ptr = (void*)early_heap_ptr;
    early_heap_ptr += size;
    
    /* Mark allocated frames as used */
    uint32 start_frame = ((uint32)ptr) / PAGE_SIZE;
    uint32 end_frame = (early_heap_ptr - 1) / PAGE_SIZE;
    for (uint32 f = start_frame; f <= end_frame; f++) {
        if (f < g_frame_alloc.total_frames) {
            if (!frame_test(f)) {
                frame_set(f);
                g_frame_alloc.free_frames--;
            }
        }
    }
    
    return ptr;
}

uint32 vmm_get_boot_alloc_end(void) {
    if (early_heap_ptr == 0) return 0;
    return (early_heap_ptr + PAGE_SIZE - 1) & PAGE_MASK;
}

/*
 * PAGE TABLE WALKING AND MANIPULATION
 * walk_page_tables traverses the PD→PT hierarchy, optionally creating tables,
 * and returns the PTE for a given virtual address.
 */

pte_t* vmm_walk_page_tables(address_space_t* as, uint32 va, int create) {
    if (!as || !as->pd) {
        return 0;
    }
    
    uint32 pdi = PDE_INDEX(va);
    uint32 pti = PTE_INDEX(va);
    
    pde_t* pde = &as->pd->entries[pdi];
    
    /* Check if page table exists */
    if (!(*pde & PTE_PRESENT)) {
        if (!create) {
            return 0;  /* Table doesn't exist and we shouldn't create it */
        }
        
        /* Allocate a new page table */
        uint32 pt_phys;
        page_table_t* pt;
        
        if (paging_enabled) {
            pt_phys = frame_alloc();
            if (pt_phys == 0) {
                return 0;  /* Out of memory */
            }
            /* Map the new page table temporarily to access it */
            pt = (page_table_t*)(KERNEL_BASE + pt_phys);
        } else {
            /* Before paging: allocate from early heap */
            pt = (page_table_t*)early_alloc(sizeof(page_table_t), PAGE_SIZE);
            pt_phys = (uint32)pt;
        }
        
        memset(pt, 0, sizeof(page_table_t));
        
        /* Set up PDE to point to new page table */
        *pde = pt_phys | PTE_PRESENT | PTE_RW | PTE_USER;
    }
    
    /* Get page table address from PDE */
    uint32 pt_phys = *pde & PTE_FRAME_MASK;
    page_table_t* pt;
    
    if (paging_enabled) {
        /* With recursive mapping, page tables are at known VAs */
        pt = (page_table_t*)PT_VA(pdi);
    } else {
        /* Before paging: physical = virtual */
        pt = (page_table_t*)pt_phys;
    }
    
    return &pt->entries[pti];
}

// PAGE MAPPING/UNMAPPING

int vmm_map_page(address_space_t* as, uint32 va, uint32 pa, uint32 flags) {
    pte_t* pte = vmm_walk_page_tables(as, va, 1);
    if (!pte) {
        return -1;  /* Couldn't create page table */
    }

    /* For user mappings, the PDE must also be user-accessible.
     * If the page table already existed as supervisor-only, upgrade it. */
    if (flags & PTE_USER) {
        pde_t* pde = &as->pd->entries[PDE_INDEX(va)];
        *pde |= PTE_USER;
    }
    
    /* If page already mapped, unmap first (avoids frame leak) */
    if (*pte & PTE_PRESENT) {
        /* Could be replacing mapping — don't free the old frame 
         * if caller is remapping same address */
    }
    
    *pte = (pa & PTE_FRAME_MASK) | flags | PTE_PRESENT;
    
    /* Add to clock for potential eviction (user pages only) */
    if (flags & PTE_USER) {
        clock_add_page(pa, pte, va, as);
    }
    
    invalidate_tlb_entry(va);
    return 0;
}

int vmm_unmap_page(address_space_t* as, uint32 va) {
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
        return -1;  /* Page not mapped */
    }
    
    uint32 frame = *pte & PTE_FRAME_MASK;
    
    /* Remove from clock replacement tracking */
    clock_remove_page(frame);
    
    /* Free the physical frame unless it's a shared/COW page */
    if (!(*pte & PTE_COW)) {
        frame_free(frame);
    }
    
    *pte = 0;
    invalidate_tlb_entry(va);
    
    return 0;
}

// ADDRESS SPACE MANAGEMENT

address_space_t* create_address_space(void) {
    address_space_t* as = (address_space_t*)vmm_kmalloc_page();
    if (!as) {
        return 0;
    }
    memset(as, 0, sizeof(address_space_t));
    
    /* Allocate page directory */
    uint32 pd_phys = frame_alloc();
    if (pd_phys == 0) {
        vmm_kfree_page(as);
        return 0;
    }
    
    as->pd = (page_directory_t*)(KERNEL_BASE + pd_phys);
    as->pd_phys = pd_phys;
    memset(as->pd, 0, sizeof(page_directory_t));
    
    /* Copy kernel mappings (entries 768-1023 for addresses >= 0xC0000000) */
    for (int i = 768; i < 1024; i++) {
        as->pd->entries[i] = vmm_kernel_as.pd->entries[i];
    }
    
    /* Set up recursive mapping: PD entry 1023 points to PD itself */
    as->pd->entries[RECURSIVE_PD_INDEX] = pd_phys | PTE_PRESENT | PTE_RW;
    
    /* Initialize user space boundaries */
    as->heap_break = USER_HEAP_BASE;
    as->stack_bottom = USER_STACK_TOP - PAGE_SIZE;  /* Initial 4KB stack */
    as->refcount = 1;
    
    return as;
}

void destroy_address_space(address_space_t* as) {
    if (!as || as == &vmm_kernel_as) {
        return;  /* Don't destroy kernel AS */
    }
    
    as->refcount--;
    if (as->refcount > 0) {
        return;  /* Still referenced by other threads */
    }
    
    /* Free all user-space page tables and frames */
    for (int pdi = 0; pdi < 768; pdi++) {  /* User space = entries 0-767 */
        pde_t pde = as->pd->entries[pdi];
        if (!(pde & PTE_PRESENT)) {
            continue;
        }
        
        /* Walk page table and free frames */
        page_table_t* pt = (page_table_t*)(KERNEL_BASE + (pde & PTE_FRAME_MASK));
        for (int pti = 0; pti < ENTRIES_PER_TABLE; pti++) {
            pte_t pte = pt->entries[pti];
            if (pte & PTE_PRESENT) {
                if (!(pte & PTE_COW)) {
                    frame_free(pte & PTE_FRAME_MASK);
                }
            } else if (pte & PTE_SWAPPED) {
                /* Free swap slot */
                uint32 slot = (pte >> 12) & 0xFFFFF;
                if (slot < MAX_SWAP_SLOTS) {
                    g_swap.bitmap[slot / 32] &= ~(1 << (slot % 32));
                    g_swap.free_slots++;
                }
            }
        }
        
        /* Free the page table itself */
        frame_free(pde & PTE_FRAME_MASK);
    }
    
    /* Free page directory */
    frame_free(as->pd_phys);
    
    /* Free address space struct */
    vmm_kfree_page(as);
}

address_space_t* clone_address_space(address_space_t* src) {
    if (!src) {
        return 0;
    }
    
    address_space_t* dst = create_address_space();
    if (!dst) {
        return 0;
    }
    
    /* Copy metadata */
    dst->heap_break = src->heap_break;
    dst->stack_bottom = src->stack_bottom;
    
    /* Mark all user pages as COW in both source and destination */
    for (int pdi = 0; pdi < 768; pdi++) {
        pde_t src_pde = src->pd->entries[pdi];
        if (!(src_pde & PTE_PRESENT)) {
            continue;
        }
        
        /* Create corresponding page table in destination */
        page_table_t* src_pt = (page_table_t*)(KERNEL_BASE + (src_pde & PTE_FRAME_MASK));
        
        /* Allocate destination page table */
        uint32 dst_pt_phys = frame_alloc();
        if (dst_pt_phys == 0) {
            destroy_address_space(dst);
            return 0;
        }
        
        page_table_t* dst_pt = (page_table_t*)(KERNEL_BASE + dst_pt_phys);
        dst->pd->entries[pdi] = dst_pt_phys | (src_pde & 0xFFF);
        
        /* Copy PTEs, marking writable pages as COW */
        for (int pti = 0; pti < ENTRIES_PER_TABLE; pti++) {
            pte_t pte = src_pt->entries[pti];
            if (!(pte & PTE_PRESENT)) {
                dst_pt->entries[pti] = pte;  /* Copy non-present PTEs as-is */
                continue;
            }
            
            if (pte & PTE_RW) {
                /* Writable page: mark as COW and read-only in both */
                pte = (pte & ~PTE_RW) | PTE_COW;
                src_pt->entries[pti] = pte;
            }
            
            dst_pt->entries[pti] = pte;
        }
    }
    
    /* Flush TLB since we modified source's PTEs */
    invalidate_tlb_all();
    
    return dst;
}

void switch_address_space(address_space_t* as) {
    if (!as || as == vmm_current_as) {
        return;
    }
    
    vmm_current_as = as;
    write_cr3(as->pd_phys);
}

/*
 * PAGE FAULT HANDLER
 * Decodes the fault cause: demand paging/swap-in for non-present pages,
 * copy-on-write resolution for write faults, or process termination on
 * unrecoverable access.
 */

void vmm_page_fault_handler(uint32 error_code, uint32 fault_addr, uint32 eip) {
    address_space_t* as = vmm_current_as;
    
    /* Update fault statistics for working-set tracking */
    as->fault_count++;
    /* as->last_fault_tick = get_ticks(); */
    
    int is_present = error_code & PF_ERR_PRESENT;
    int is_write = error_code & PF_ERR_WRITE;
    int is_user = error_code & PF_ERR_USER;
    
    pte_t* pte = vmm_walk_page_tables(as, fault_addr, 0);
    
    /* CASE 1: Page not present, but has special flags */
    if (!is_present && pte) {
        pte_t entry = *pte;
        
        /* Demand-zero page: allocate and zero on first access */
        if (entry & PTE_DEMAND) {
            uint32 frame = frame_alloc();
            if (frame == 0) {
                goto segfault;
            }
            
            /* Zero the new page for security */
            memset((void*)(KERNEL_BASE + frame), 0, PAGE_SIZE);
            
            /* Map with original permissions (restore RW if it was set) */
            uint32 flags = (entry & 0xFFF) & ~PTE_DEMAND;
            flags |= PTE_PRESENT;
            *pte = (frame & PTE_FRAME_MASK) | flags;
            
            invalidate_tlb_entry(fault_addr & PAGE_MASK);
            return;
        }
        
        /* Swapped page: bring back from swap */
        if (entry & PTE_SWAPPED) {
            uint32 swap_slot = (entry >> 12) & 0xFFFFF;
            if (swap_in_page(as, fault_addr, swap_slot) == 0) {
                return;
            }
            goto segfault;
        }
    }
    
    /* CASE 2: Protection violation on write — possible COW */
    if (is_present && is_write && pte) {
        pte_t entry = *pte;
        
        if (entry & PTE_COW) {
            if (vmm_handle_cow_fault(as, fault_addr, pte) == 0) {
                return;
            }
            goto segfault;
        }
    }
    
    /* CASE 3: Access in valid region but page not yet allocated
     * Check if address falls within heap or stack growth area */
    if (!is_present) {
        /* Stack growth: address between stack_bottom and USER_STACK_TOP */
        if (fault_addr >= as->stack_bottom - PAGE_SIZE && 
            fault_addr < USER_STACK_TOP) {
            /* Grow stack down */
            uint32 new_page = fault_addr & PAGE_MASK;
            uint32 frame = frame_alloc();
            if (frame == 0) {
                goto segfault;
            }
            
            memset((void*)(KERNEL_BASE + frame), 0, PAGE_SIZE);
            vmm_map_page(as, new_page, frame, PTE_USER | PTE_RW);
            
            if (new_page < as->stack_bottom) {
                as->stack_bottom = new_page;
            }
            return;
        }
        
        /* Heap access within brk: should have been mapped via brk() */
        if (fault_addr >= USER_HEAP_BASE && fault_addr < as->heap_break) {
            /* Lazy heap page — allocate now */
            uint32 page_va = fault_addr & PAGE_MASK;
            uint32 frame = frame_alloc();
            if (frame == 0) {
                goto segfault;
            }
            
            memset((void*)(KERNEL_BASE + frame), 0, PAGE_SIZE);
            vmm_map_page(as, page_va, frame, PTE_USER | PTE_RW);
            return;
        }
    }
    
segfault:
    /* Unrecoverable fault — print diagnostic and halt/kill process */
    printf("\n%c*** Page Fault ***\n", 255, 0, 0);
    printf("Address: 0x%08X  EIP: 0x%08X\n", fault_addr, eip);
    printf("Error: %s %s %s\n",
           is_present ? "protection" : "not-present",
           is_write ? "write" : "read",
           is_user ? "user" : "kernel");
    
    if (is_user) {
        /* User mode fault: kill the process (when we have processes) */
        printf("Segmentation fault in user process\n");
        /* For now, halt */
        asm volatile("cli; hlt");
    } else {
        /* Kernel mode fault: panic */
        printf("Kernel panic: page fault in kernel mode\n");
        asm volatile("cli; hlt");
    }
}

// COPY-ON-WRITE HANDLING

int vmm_handle_cow_fault(address_space_t* as, uint32 va, pte_t* pte) {
    pte_t entry = *pte;
    uint32 old_frame = entry & PTE_FRAME_MASK;
    
    /* Allocate new frame for private copy */
    uint32 new_frame = frame_alloc();
    if (new_frame == 0) {
        return -1;
    }
    
    /* Copy page contents */
    memcpy((void*)(KERNEL_BASE + new_frame), 
           (void*)(KERNEL_BASE + old_frame), 
           PAGE_SIZE);
    
    /* Update PTE: new frame, writable, clear COW flag */
    *pte = (new_frame & PTE_FRAME_MASK) | 
           ((entry & 0xFFF) & ~PTE_COW) | 
           PTE_RW | PTE_PRESENT;
    
    /* Add new page to clock, remove old from tracking for this AS */
    clock_add_page(new_frame, pte, va, as);
    
    invalidate_tlb_entry(va);
    
    /* Note: old_frame is not freed — other processes may still reference it.
     * A proper implementation would reference-count shared frames. */
    
    return 0;
}

void vmm_mark_region_cow(address_space_t* as, uint32 start, uint32 end) {
    for (uint32 va = start & PAGE_MASK; va < end; va += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(as, va, 0);
        if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_RW)) {
            *pte = (*pte & ~PTE_RW) | PTE_COW;
        }
    }
    invalidate_tlb_all();
}

/*
 * SWAP SUBSYSTEM
 * Tracks swap slots and delegates paging I/O to a swap partition when
 * available; otherwise falls back to stubbed-in memory placeholders.
 */

static uint32 swap_alloc_slot(void) {
    if (g_swap.free_slots == 0) {
        return SWAP_SLOT_NONE;
    }
    
    for (uint32 i = 0; i < MAX_SWAP_SLOTS / 32; i++) {
        if (g_swap.bitmap[i] != 0xFFFFFFFF) {
            for (uint32 j = 0; j < 32; j++) {
                if (!(g_swap.bitmap[i] & (1 << j))) {
                    uint32 slot = i * 32 + j;
                    g_swap.bitmap[i] |= (1 << j);
                    g_swap.free_slots--;
                    return slot;
                }
            }
        }
    }
    return SWAP_SLOT_NONE;
}

static void swap_free_slot(uint32 slot) {
    if (slot >= MAX_SWAP_SLOTS) return;
    g_swap.bitmap[slot / 32] &= ~(1 << (slot % 32));
    g_swap.free_slots++;
}

/* Import partition swap functions if available */
extern int swap_partition_write_page(uint32 page_num, const void *buffer);
extern int swap_partition_read_page(uint32 page_num, void *buffer);
extern void* swap_partition_get_info(void);

/* Write page to swap device (partition or memory placeholder) */
static int swap_write_page(uint32 slot, void* page_data) {
    /* Try partition-based swap first */
    if (swap_partition_get_info()) {
        return swap_partition_write_page(slot, page_data);
    }
    /* Fallback: no swap partition, operation fails silently */
    (void)slot;
    (void)page_data;
    return 0;
}

/* Read page from swap device */
static int swap_read_page(uint32 slot, void* page_data) {
    /* Try partition-based swap first */
    if (swap_partition_get_info()) {
        return swap_partition_read_page(slot, page_data);
    }
    /* Fallback: zero the page as placeholder */
    (void)slot;
    memset(page_data, 0, PAGE_SIZE);
    return 0;
}

uint32 swap_out_page(pte_t* pte, uint32 va, address_space_t* as) {
    (void)as;
    
    if (!pte || !(*pte & PTE_PRESENT)) {
        return SWAP_SLOT_NONE;
    }
    
    uint32 frame = *pte & PTE_FRAME_MASK;
    uint32 slot = swap_alloc_slot();
    
    if (slot == SWAP_SLOT_NONE) {
        return SWAP_SLOT_NONE;  /* Swap full */
    }
    
    /* Write page to swap */
    if (swap_write_page(slot, (void*)(KERNEL_BASE + frame)) != 0) {
        swap_free_slot(slot);
        return SWAP_SLOT_NONE;
    }
    
    /* Update PTE: not present, store swap slot in frame bits */
    *pte = (slot << 12) | PTE_SWAPPED | (*pte & (PTE_USER | PTE_RW));
    
    /* Free the physical frame */
    frame_free(frame);
    clock_remove_page(frame);
    
    invalidate_tlb_entry(va);
    
    return slot;
}

int swap_in_page(address_space_t* as, uint32 va, uint32 swap_slot) {
    if (swap_slot >= MAX_SWAP_SLOTS) {
        return -1;
    }
    
    /* Allocate a fresh frame */
    uint32 frame = frame_alloc();
    if (frame == 0) {
        return -1;
    }
    
    /* Read from swap */
    if (swap_read_page(swap_slot, (void*)(KERNEL_BASE + frame)) != 0) {
        frame_free(frame);
        return -1;
    }
    
    /* Get PTE and restore mapping */
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    if (!pte) {
        frame_free(frame);
        return -1;
    }
    
    /* Restore original flags, clear swap flag */
    uint32 flags = (*pte & (PTE_USER | PTE_RW)) | PTE_PRESENT;
    *pte = (frame & PTE_FRAME_MASK) | flags;
    
    /* Free the swap slot */
    swap_free_slot(swap_slot);
    
    /* Add to clock for future eviction */
    clock_add_page(frame, pte, va, as);
    
    invalidate_tlb_entry(va);
    
    return 0;
}

/*
 * CLOCK PAGE REPLACEMENT ALGORITHM
 * Circular buffer of (frame, PTE*, VA, AS) tuples. Second-chance eviction:
 * clear Accessed on first pass, evict on next pass if still unused.
 */

void clock_add_page(uint32 frame, pte_t* pte, uint32 va, address_space_t* as) {
    if (g_clock.count >= CLOCK_SIZE) {
        /* Clock buffer full — would need to evict first, but this shouldn't
         * happen if CLOCK_SIZE matches typical resident set */
        return;
    }
    
    /* Find empty slot (frame == 0 indicates unused) */
    for (uint32 i = 0; i < CLOCK_SIZE; i++) {
        if (g_clock.entries[i].frame == 0) {
            g_clock.entries[i].frame = frame;
            g_clock.entries[i].pte_ptr = pte;
            g_clock.entries[i].va = va;
            g_clock.entries[i].as = as;
            g_clock.count++;
            return;
        }
    }
}

void clock_remove_page(uint32 frame) {
    for (uint32 i = 0; i < CLOCK_SIZE; i++) {
        if (g_clock.entries[i].frame == frame) {
            g_clock.entries[i].frame = 0;
            g_clock.entries[i].pte_ptr = 0;
            g_clock.entries[i].va = 0;
            g_clock.entries[i].as = 0;
            g_clock.count--;
            return;
        }
    }
}

uint32 clock_evict_page(void) {
    if (g_clock.count == 0) {
        return 0;  /* Nothing to evict */
    }
    
    uint32 start = g_clock.hand;
    uint32 loops = 0;
    
    while (loops < CLOCK_SIZE * 2) {  /* Max 2 full rotations */
        clock_entry_t* entry = &g_clock.entries[g_clock.hand];
        
        /* Advance hand for next iteration */
        g_clock.hand = (g_clock.hand + 1) % CLOCK_SIZE;
        loops++;
        
        if (entry->frame == 0) {
            continue;  /* Empty slot */
        }
        
        pte_t* pte = entry->pte_ptr;
        if (!pte || !(*pte & PTE_PRESENT)) {
            /* Stale entry — remove it */
            clock_remove_page(entry->frame);
            continue;
        }
        
        /* Check accessed bit */
        if (*pte & PTE_ACCESSED) {
            /* Second chance: clear accessed bit, move on */
            *pte &= ~PTE_ACCESSED;
            continue;
        }
        
        /* Victim found! Swap out if dirty, then reclaim frame */
        uint32 frame = entry->frame;
        
        if (*pte & PTE_DIRTY) {
            /* Page was modified — must write to swap */
            swap_out_page(pte, entry->va, entry->as);
        } else {
            /* Clean page — just invalidate */
            *pte = 0;
            invalidate_tlb_entry(entry->va);
        }
        
        /* Clear clock entry */
        entry->frame = 0;
        entry->pte_ptr = 0;
        g_clock.count--;
        
        return frame;
    }
    
    /* Full rotation with no victim — all pages recently accessed (thrashing!) */
    return 0;
}

/*
 * WORKING SET TRACKING & THRASHING AVOIDANCE
 * Simple heuristic: if faults cross a threshold within a window, treat the
 * process as thrashing so the scheduler can throttle it.
 */

#define FAULT_WINDOW_TICKS  100   /* ~1 second at 100Hz */
#define THRASH_THRESHOLD    50    /* >50 faults/second = thrashing */

void vmm_update_working_set(address_space_t* as) {
    /* Count pages with Accessed bit set — rough working set size */
    uint32 ws_count = 0;
    
    for (int pdi = 0; pdi < 768; pdi++) {
        if (!(as->pd->entries[pdi] & PTE_PRESENT)) {
            continue;
        }
        
        page_table_t* pt = (page_table_t*)PT_VA(pdi);
        for (int pti = 0; pti < ENTRIES_PER_TABLE; pti++) {
            pte_t pte = pt->entries[pti];
            if ((pte & PTE_PRESENT) && (pte & PTE_ACCESSED)) {
                ws_count++;
                /* Clear accessed bit for next interval */
                pt->entries[pti] &= ~PTE_ACCESSED;
            }
        }
    }
    
    as->ws_pages = ws_count;
}

int vmm_should_throttle(address_space_t* as) {
    /* Simple threshold: more than THRASH_THRESHOLD faults = thrashing */
    if (as->fault_count > THRASH_THRESHOLD) {
        as->fault_count = 0;  /* Reset for next window */
        return 1;
    }
    return 0;
}

// USER MEMORY OPERATIONS (sbrk, mmap)

int vmm_brk(address_space_t* as, uint32 new_break) {
    if (new_break < USER_HEAP_BASE || new_break >= USER_HEAP_END) {
        return -1;  /* Invalid break address */
    }
    
    if (new_break > as->heap_break) {
        /* Growing heap: mark new pages as demand-zero (lazy allocation) */
        for (uint32 va = as->heap_break; va < new_break; va += PAGE_SIZE) {
            pte_t* pte = vmm_walk_page_tables(as, va & PAGE_MASK, 1);
            if (!pte) {
                return -1;
            }
            /* Mark as demand-zero: not present, but DEMAND flag set */
            *pte = PTE_DEMAND | PTE_USER | PTE_RW;
        }
    } else if (new_break < as->heap_break) {
        /* Shrinking heap: unmap and free pages */
        for (uint32 va = new_break; va < as->heap_break; va += PAGE_SIZE) {
            vmm_unmap_page(as, va & PAGE_MASK);
        }
    }
    
    as->heap_break = new_break;
    return 0;
}

int vmm_mmap(address_space_t* as, uint32 va, uint32 size, uint32 flags) {
    /* Simple fixed-address mmap in shared region */
    if (va < USER_SHARED_BASE || va >= USER_SHARED_END) {
        return -1;
    }
    
    uint32 end = va + size;
    for (uint32 addr = va & PAGE_MASK; addr < end; addr += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(as, addr, 1);
        if (!pte) {
            return -1;
        }
        /* Mark as demand-zero with specified flags */
        *pte = PTE_DEMAND | (flags & (PTE_USER | PTE_RW));
    }
    
    return 0;
}

int vmm_munmap(address_space_t* as, uint32 va, uint32 size) {
    uint32 end = va + size;
    for (uint32 addr = va & PAGE_MASK; addr < end; addr += PAGE_SIZE) {
        vmm_unmap_page(as, addr);
    }
    return 0;
}

// KERNEL MEMORY HELPERS

void* vmm_kmalloc_page(void) {
    uint32 frame = frame_alloc();
    if (frame == 0) {
        return 0;
    }
    
    /* Kernel pages are identity-mapped at KERNEL_BASE + phys */
    return (void*)(KERNEL_BASE + frame);
}

void vmm_kfree_page(void* ptr) {
    if (!ptr) return;
    
    uint32 va = (uint32)ptr;
    if (va < KERNEL_BASE) return;
    
    uint32 phys = va - KERNEL_BASE;
    frame_free(phys);
}

void* vmm_kmalloc_aligned(uint32 size) {
    uint32 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32 phys = frame_alloc_contiguous(pages);
    if (phys == 0) {
        return 0;
    }
    return (void*)(KERNEL_BASE + phys);
}

// QUERY FUNCTIONS

int vmm_is_page_present(address_space_t* as, uint32 va) {
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    return pte && (*pte & PTE_PRESENT);
}

int vmm_is_page_writable(address_space_t* as, uint32 va) {
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    return pte && (*pte & PTE_PRESENT) && (*pte & PTE_RW);
}

uint32 vmm_virt_to_phys(address_space_t* as, uint32 va) {
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
        return 0;
    }
    return (*pte & PTE_FRAME_MASK) | PAGE_OFFSET(va);
}

/*
 * BOOT SEQUENCE INITIALIZATION
 * Called early from kmain():
 * 1. Initialize frame allocator based on detected RAM
 * 2. Create kernel page directory with identity mapping
 * 3. Set up recursive mapping for easy PT access
 * 4. Load CR3 and enable paging
 * 5. Optionally protect kernel .text as read-only
 */

void vmm_init(uint32 total_ram_bytes) {
    /* Initialize swap state */
    memset(&g_swap, 0, sizeof(g_swap));
    g_swap.total_slots = MAX_SWAP_SLOTS;
    g_swap.free_slots = MAX_SWAP_SLOTS;
    
    /* Initialize clock replacement state */
    memset(&g_clock, 0, sizeof(g_clock));
    
    /* Set up frame allocator */
    frame_alloc_init(total_ram_bytes);
    
    /* Set early heap pointer to right after kernel */
    early_heap_ptr = ((uint32)&__kernel_end + PAGE_SIZE) & PAGE_MASK;
    uint32 boot_alloc_start = early_heap_ptr;
    
    /* Allocate kernel page directory */
    page_directory_t* kernel_pd = (page_directory_t*)early_alloc(sizeof(page_directory_t), PAGE_SIZE);
    memset(kernel_pd, 0, sizeof(page_directory_t));
    
    vmm_kernel_as.pd = kernel_pd;
    vmm_kernel_as.pd_phys = (uint32)kernel_pd;  /* Before paging: virt == phys */
    vmm_kernel_as.refcount = 1;
    
    /*
     * IDENTITY MAP: Map first 16MB at both 0x00000000 and 0xC0000000
     * This allows kernel to run at high addresses while still accessing
     * low memory (BIOS, VGA, etc.) during transition.
     */
    
    /* Create page tables for first 16MB identity mapped */
    for (uint32 pdi = 0; pdi < 4; pdi++) {  /* 4 PDEs = 16MB */
        page_table_t* pt = (page_table_t*)early_alloc(sizeof(page_table_t), PAGE_SIZE);
        memset(pt, 0, sizeof(page_table_t));
        
        /* Fill page table: map VA to same PA (identity) */
        for (uint32 pti = 0; pti < ENTRIES_PER_TABLE; pti++) {
            uint32 pa = (pdi * ENTRIES_PER_TABLE + pti) * PAGE_SIZE;
            pt->entries[pti] = pa | PTE_PRESENT | PTE_RW;
        }
        
        /* Low mapping (0x00000000+) */
        kernel_pd->entries[pdi] = (uint32)pt | PTE_PRESENT | PTE_RW;
        
        /* High mapping (0xC0000000+) — kernel's preferred addresses */
        kernel_pd->entries[768 + pdi] = (uint32)pt | PTE_PRESENT | PTE_RW;
    }
    
    /*
     * Map framebuffer region (MMIO) if available
     * The framebuffer is typically at a high address (e.g., 0xFD000000)
     * We identity-map the region containing it.
     */
    if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO)) {
        uint32 fb_addr = (uint32)g_mbi->framebuffer_addr;
        uint32 fb_size = g_mbi->framebuffer_pitch * g_mbi->framebuffer_height;
        if (fb_size == 0) fb_size = 4 * 1024 * 1024;  /* Default 4MB if unknown */
        
        /* Round fb_addr down to 4MB boundary for simpler mapping */
        uint32 fb_start = fb_addr & ~0x3FFFFF;  /* Align to 4MB */
        uint32 fb_end = (fb_addr + fb_size + 0x3FFFFF) & ~0x3FFFFF;
        
        /* Map each 4MB region (create page table for each PDE) */
        for (uint32 addr = fb_start; addr < fb_end; addr += 4 * 1024 * 1024) {
            uint32 pdi = addr >> 22;  /* PDE index */
            
            /* Skip if already mapped (shouldn't happen for high addresses) */
            if (kernel_pd->entries[pdi] & PTE_PRESENT) continue;
            
            /* Allocate page table for this region */
            page_table_t* pt = (page_table_t*)early_alloc(sizeof(page_table_t), PAGE_SIZE);
            memset(pt, 0, sizeof(page_table_t));
            
            /* Fill page table: identity map */
            for (uint32 pti = 0; pti < ENTRIES_PER_TABLE; pti++) {
                uint32 pa = (pdi * ENTRIES_PER_TABLE + pti) * PAGE_SIZE;
                /* Mark as present, RW, and uncacheable for MMIO */
                pt->entries[pti] = pa | PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;
            }
            
            kernel_pd->entries[pdi] = (uint32)pt | PTE_PRESENT | PTE_RW;
        }
    }
    
    /* Set up recursive mapping: PD[1023] points to PD itself */
    kernel_pd->entries[RECURSIVE_PD_INDEX] = (uint32)kernel_pd | PTE_PRESENT | PTE_RW;
    
    /* Set null page as not-present (null pointer guard) */
    page_table_t* pt0 = (page_table_t*)(kernel_pd->entries[0] & PTE_FRAME_MASK);
    pt0->entries[0] = 0;  /* First page not present — dereferencing NULL faults */

    /* IMPORTANT: Reserve all boot-time early_alloc() memory in the frame bitmap.
     * frame_alloc_init() only reserves up through __kernel_end; without this,
     * frame_alloc() can hand out frames that overlap page tables or the heap,
     * leading to silent corruption (e.g., broken user PTEs and failing copyout).
     */
    uint32 boot_alloc_end = (early_heap_ptr + PAGE_SIZE - 1) & PAGE_MASK;
    for (uint32 pa = boot_alloc_start; pa < boot_alloc_end; pa += PAGE_SIZE) {
        uint32 frame_num = pa / PAGE_SIZE;
        if (frame_num >= g_frame_alloc.total_frames) {
            break;
        }
        if (!frame_test(frame_num)) {
            frame_set(frame_num);
            if (g_frame_alloc.free_frames) {
                g_frame_alloc.free_frames--;
            }
        }
    }
    
    printf("VMM: Initialized with %d MB RAM, %d free frames\n",
           total_ram_bytes / (1024 * 1024),
           g_frame_alloc.free_frames);
}

void vmm_enable_paging(void) {
    /* Load page directory physical address into CR3 */
    write_cr3(vmm_kernel_as.pd_phys);
    
    /* Enable paging by setting CR0.PG (bit 31) */
    uint32 cr0 = read_cr0();
    cr0 |= 0x80000000;  /* Set PG bit */
    write_cr0(cr0);
    
    paging_enabled = 1;
    
    /* Now executing with paging enabled!
     * Thanks to identity mapping, current EIP is still valid. */
    
    printf("VMM: Paging enabled\n");
}
