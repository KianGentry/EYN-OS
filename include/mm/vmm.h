/*
 * vmm.h - Virtual Memory Manager for EYN-OS
 * 
 * i386 paging with 4KB pages, demand paging, COW, swap integration,
 * and working-set tracking. Designed for 3–9 MB physical RAM.
 */

#ifndef VMM_H
#define VMM_H

#include <types.h>
#include <stddef.h>

/*
 * TLB invalidation notes (80386 compatibility):
 * - INVLPG is a 486+ instruction. A strict 80386 build must not emit it.
 * - When INVLPG is unavailable, the only architecturally-correct invalidation
 *   mechanism is a full TLB flush via CR3 reload.
 */
#ifndef CONFIG_CPU_HAS_INVLPG
#define CONFIG_CPU_HAS_INVLPG 0
#endif

/*
 * i386 PAGING MODEL - BIT DEFINITIONS
 *
 *  31              12 11  9  8   7   6   5   4   3   2   1   0
 * +---++---+---+---+---+---+---+---+---+---+
 * |    Frame/PFN     | AVL | G | S | D | A |PCD|PWT|U/S|R/W| P |
 * +---++---+---+---+---+---+---+---+---+---+
 *
 * P      (0) - Present: 1=mapped, 0=not present (triggers #PF)
 * R/W    (1) - Read/Write: 1=writable, 0=read-only
 * U/S    (2) - User/Supervisor: 1=ring3 accessible, 0=kernel only
 * PWT    (3) - Write-through caching
 * PCD    (4) - Cache disable
 * A      (5) - Accessed: set by CPU on read/write
 * D      (6) - Dirty: set by CPU on write (PTE only)
 * S/PAT  (7) - Page size (PDE): 1=4MB page, 0=4KB pages via PT
 * G      (8) - Global: don't flush from TLB on CR3 switch (PTE only)
 * AVL  (9-11)- Available for OS use (we use for COW, swap, etc.)
 *
 * CR3 holds physical address of the page directory (bits 31:12) plus PCD/PWT
 * bits. CR0.PG (bit 31) enables paging when set.
 */

/* Page/frame sizes and counts */
#define PAGE_SIZE           4096
#define PAGE_SHIFT          12
#define PAGE_MASK           (~(PAGE_SIZE - 1))
#define ENTRIES_PER_TABLE   1024
#define ENTRIES_PER_DIR     1024

/* Extract page directory index from virtual address (bits 31:22) */
#define PDE_INDEX(va)       (((va) >> 22) & 0x3FF)
/* Extract page table index from virtual address (bits 21:12) */
#define PTE_INDEX(va)       (((va) >> 12) & 0x3FF)
/* Extract offset within page (bits 11:0) */
#define PAGE_OFFSET(va)     ((va) & 0xFFF)

/* Build address from indices */
#define VA_FROM_INDICES(pdi, pti) (((pdi) << 22) | ((pti) << 12))

// PDE/PTE BIT FLAGS
#define PTE_PRESENT         (1 << 0)
#define PTE_RW              (1 << 1)
#define PTE_USER            (1 << 2)
#define PTE_PWT             (1 << 3)
#define PTE_PCD             (1 << 4)
#define PTE_ACCESSED        (1 << 5)
#define PTE_DIRTY           (1 << 6)
#define PTE_PAT             (1 << 7)  /* PTE: PAT index bit; PDE: page size */
#define PTE_GLOBAL          (1 << 8)

/* OS-defined AVL bits (9-11) */
#define PTE_COW             (1 << 9)  /* Copy-on-write: shared read-only */
#define PTE_SWAPPED         (1 << 10) /* Page is in swap, frame field = swap slot */
#define PTE_DEMAND          (1 << 11) /* Demand-zero: allocate on first access */

/* PDE: 4MB page bit */
#define PDE_4MB             (1 << 7)

/* Mask to extract physical frame address from PDE/PTE */
#define PTE_FRAME_MASK      0xFFFFF000

/*
 * VIRTUAL ADDRESS SPACE LAYOUT - HIGH-HALF KERNEL (3GB split)
 *
 *  0x00000000 – 0x3FFFFFFF  User code/data          (1 GB)
 *  0x40000000 – 0x7FFFFFFF  User heap (grows up)    (1 GB)
 *  0x80000000 – 0xAFFFFFFF  Shared memory regions   (768 MB)
 *  0xB0000000 – 0xBFFFFFFF  User stack (grows down) (256 MB)
 *  0xC0000000 – 0xFFFFFFFF  Kernel space            (1 GB)
 *      0xC0000000 – identity map of low 4–16 MB (kernel code/data)
 *      0xD0000000 – kernel heap
 *      0xFF800000 – recursive page directory mapping
 *      0xFFC00000 – page tables (via recursive mapping)
 */

/* User space boundaries */
#define USER_BASE           0x00000000
#define USER_CODE_BASE      0x00400000  /* Leave first 4MB unmapped (null guard) */
#define USER_CODE_END       0x40000000
#define USER_HEAP_BASE      0x40000000
#define USER_HEAP_END       0x80000000
#define USER_SHARED_BASE    0x80000000
#define USER_SHARED_END     0xB0000000
#define USER_STACK_TOP      0xC0000000  /* Stack grows down from here */
#define USER_STACK_BASE     0xB0000000

/* Kernel space boundaries (high-half) */
#define KERNEL_BASE         0xC0000000
#define KERNEL_IDENTITY_END 0xD0000000  /* First 256MB identity-mapped */
#define KERNEL_HEAP_BASE    0xD0000000
#define KERNEL_HEAP_END     0xFF800000

/* Recursive mapping trick: map PD entry 1023 to itself
 * This makes page tables appear at known virtual addresses */
#define RECURSIVE_PD_INDEX  1023
#define RECURSIVE_PD_VA     0xFFFFF000  /* VA of page directory itself */
#define RECURSIVE_PT_BASE   0xFFC00000  /* VA of page tables array */

/* Get virtual address of page table for a given PDE index */
#define PT_VA(pdi)          (RECURSIVE_PT_BASE + ((pdi) << PAGE_SHIFT))

// DATA STRUCTURES

/* Raw PDE/PTE are just uint32 with bitfields */
typedef uint32 pde_t;
typedef uint32 pte_t;

/* Page directory: 1024 PDEs, 4KB aligned */
typedef struct page_directory {
    pde_t entries[ENTRIES_PER_DIR];
} __attribute__((aligned(PAGE_SIZE))) page_directory_t;

/* Page table: 1024 PTEs, 4KB aligned */
typedef struct page_table {
    pte_t entries[ENTRIES_PER_TABLE];
} __attribute__((aligned(PAGE_SIZE))) page_table_t;

/*
 * PHYSICAL FRAME ALLOCATOR
 * Simple bitmap allocator - one bit per 4KB frame. With a 128MB cap we track
 * 32768 frames (1024 uint32 words) for a 4KB bitmap footprint.
 */

#define MAX_PHYSICAL_MB     128
#define MAX_FRAMES          (MAX_PHYSICAL_MB * 1024 * 1024 / PAGE_SIZE)
#define FRAME_BITMAP_WORDS  (MAX_FRAMES / 32)

/* Frame allocator state (defined in vmm.c) */
typedef struct frame_allocator {
    uint32 bitmap[FRAME_BITMAP_WORDS]; /* 1 = used, 0 = free */
    uint32 total_frames;               /* Actual frames based on detected RAM */
    uint32 free_frames;                /* Count of free frames */
    uint32 search_hint;                /* Bitmap word to start searching from */
} frame_allocator_t;

/*
 * ADDRESS SPACE DESCRIPTOR
 * Per-process virtual memory state. Kernel has a single global address space;
 * each user process gets its own with shared kernel mappings.
 */

typedef struct address_space {
    page_directory_t* pd;           /* Page directory (virtual address) */
    uint32 pd_phys;                 /* Physical address of page directory */
    uint32 heap_break;              /* Current end of heap */
    uint32 stack_bottom;            /* Current bottom of stack (grows down) */
    uint32 refcount;                /* For shared address spaces (threads) */
    
    /* Working-set tracking for thrashing avoidance */
    uint32 ws_pages;                /* Estimated working set size (pages) */
    uint32 fault_count;             /* Page faults since last reset */
    uint32 last_fault_tick;         /* Timestamp of last fault */
} address_space_t;

/*
 * PAGE FAULT ERROR CODE (pushed by CPU on #PF, INT 14)
 *
 *  Bit 0 (P): 0 = non-present page, 1 = protection violation
 *  Bit 1 (W): 0 = read access, 1 = write access
 *  Bit 2 (U): 0 = supervisor mode, 1 = user mode
 *  Bit 3 (R): 1 = reserved bit violation
 *  Bit 4 (I): 1 = instruction fetch (NX violation, not on i386)
 */

#define PF_ERR_PRESENT      (1 << 0)
#define PF_ERR_WRITE        (1 << 1)
#define PF_ERR_USER         (1 << 2)
#define PF_ERR_RESERVED     (1 << 3)
#define PF_ERR_IFETCH       (1 << 4)

/*
 * SWAP SUBSYSTEM
 * Slot-based swap; each slot holds one 4KB page. The backing device can be a
 * disk partition or file.
 */

#define SWAP_SLOT_NONE      0xFFFFFFFF
#define MAX_SWAP_SLOTS      4096  /* 16MB swap space */

typedef struct swap_state {
    uint32 bitmap[MAX_SWAP_SLOTS / 32]; /* 1 = used, 0 = free */
    uint32 total_slots;
    uint32 free_slots;
} swap_state_t;

/*
 * PAGE REPLACEMENT - CLOCK ALGORITHM
 * Circular buffer of (physical frame, owning PTE pointer) pairs. The hand
 * advances on allocation; pages with A=1 get a second chance.
 */

typedef struct clock_entry {
    uint32 frame;       /* Physical frame number */
    pte_t* pte_ptr;     /* Pointer to PTE (for clearing A bit, evicting) */
    uint32 va;          /* Virtual address (for TLB invalidation) */
    address_space_t* as;/* Owning address space */
} clock_entry_t;

#define CLOCK_SIZE          1024  /* Track up to 1024 evictable pages */

typedef struct clock_state {
    clock_entry_t entries[CLOCK_SIZE];
    uint32 hand;        /* Current position */
    uint32 count;       /* Number of entries in use */
} clock_state_t;

// CORE VMM API

/* Initialization */
void vmm_init(uint32 total_ram_bytes);

/*
 * Returns the current end of the VMM's boot-time bump allocator (physical
 * address, page-aligned). This is used by legacy subsystems (like util.c's
 * malloc) to place their heap after early page tables.
 */
uint32 vmm_get_boot_alloc_end(void);
void vmm_enable_paging(void);

/* Frame allocator */
uint32 frame_alloc(void);                    /* Returns physical address or 0 */
void frame_free(uint32 phys_addr);

/* Reserve a physical address range (page-aligned internally) so the frame
 * allocator will never hand these frames out. Useful for the legacy heap
 * which lives in a fixed physical region.
 */
void vmm_reserve_phys_range(uint32 phys_start, uint32 phys_end);
uint32 frame_alloc_contiguous(uint32 count); /* For DMA buffers */

/* Page mapping */
int vmm_map_page(address_space_t* as, uint32 va, uint32 pa, uint32 flags);
int vmm_unmap_page(address_space_t* as, uint32 va);
pte_t* vmm_walk_page_tables(address_space_t* as, uint32 va, int create);
void invalidate_tlb_entry(uint32 va);
void invalidate_tlb_all(void);

/* Preferred invalidation API (used by the 386/486+ compat layer). */
void vm_invalidate_page(void* addr);
void vm_invalidate_range(void* start, size_t len);

/*
 * Strict-80386 performance helper:
 * When INVLPG is unavailable, vm_invalidate_* falls back to a full CR3 reload.
 * Some teardown paths unmap many pages in a loop; deferring lets us collapse
 * those into a single CR3 reload at the end of the batch.
 */
void vm_tlb_defer_begin(void);
void vm_tlb_defer_end(void);

/* Address space management */
address_space_t* create_address_space(void);
void destroy_address_space(address_space_t* as);
address_space_t* clone_address_space(address_space_t* src); /* For fork() */
void switch_address_space(address_space_t* as);

/* User memory operations */
int vmm_brk(address_space_t* as, uint32 new_break); /* sbrk() backend */
int vmm_mmap(address_space_t* as, uint32 va, uint32 size, uint32 flags);
int vmm_munmap(address_space_t* as, uint32 va, uint32 size);

/* Page fault handling (called from ISR 14) */
void vmm_page_fault_handler(uint32 error_code, uint32 fault_addr, uint32 eip);

/* Swap operations */
uint32 swap_out_page(pte_t* pte, uint32 va, address_space_t* as);
int swap_in_page(address_space_t* as, uint32 va, uint32 swap_slot);

/* Page replacement */
uint32 clock_evict_page(void);  /* Returns frame to reuse, or 0 if none */
void clock_add_page(uint32 frame, pte_t* pte, uint32 va, address_space_t* as);
void clock_remove_page(uint32 frame);

/* Copy-on-write support */
int vmm_handle_cow_fault(address_space_t* as, uint32 va, pte_t* pte);
void vmm_mark_region_cow(address_space_t* as, uint32 start, uint32 end);

/* Working-set and thrashing avoidance */
void vmm_update_working_set(address_space_t* as);
int vmm_should_throttle(address_space_t* as); /* Returns 1 if thrashing */

/* Query functions */
uint32 vmm_get_free_frames(void);
uint32 vmm_get_total_frames(void);
int vmm_is_page_present(address_space_t* as, uint32 va);
int vmm_is_page_writable(address_space_t* as, uint32 va);
uint32 vmm_virt_to_phys(address_space_t* as, uint32 va);

/* Kernel helpers */
void* vmm_kmalloc_page(void);              /* Allocate one kernel page */
void vmm_kfree_page(void* ptr);
void* vmm_kmalloc_aligned(uint32 size);    /* Page-aligned kernel allocation */

/* Current address space (for quick access in fault handler) */
extern address_space_t* vmm_current_as;
extern address_space_t vmm_kernel_as;

#endif /* VMM_H */
