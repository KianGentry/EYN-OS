# EYN-OS Virtual Memory Subsystem

## Architecture Overview

### Beginner's Guide: What is Paging?
Imagine memory as a giant book.
- **Physical Memory (RAM)** is the actual paper pages in the book.
- **Virtual Memory** is the page numbers in the Table of Contents.
- **Paging** is the system that maps the Table of Contents (Virtual) to the actual paper pages (Physical).

This allows programs to think they have a continuous block of memory (e.g., pages 1-100), even if the actual paper pages are scattered all over the book (e.g., page 1 is on physical sheet 50, page 2 is on sheet 12). It also lets us "swap" pages out to disk if we run out of RAM, like putting a page in a filing cabinet until it's needed again.

### Technical Implementation
```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        i386 PAGING MODEL (4KB pages)                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   31              22 21              12 11               0                  │
│  ┌─────────────────┬─────────────────┬─────────────────┐                    │
│  │   PDE Index     │   PTE Index     │   Page Offset   │  Virtual Address   │
│  │   (10 bits)     │   (10 bits)     │   (12 bits)     │                    │
│  └────────┬────────┴────────┬────────┴────────┬────────┘                    │
│           │                 │                 │                             │
│           ▼                 ▼                 ▼                             │
│  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐                │
│  │ Page Directory  │ │   Page Table    │ │  Physical Page  │                │
│  │  (1024 PDEs)    │→│  (1024 PTEs)    │→│    (4KB)        │                │
│  └────────┬────────┘ └─────────────────┘ └─────────────────┘                │
│           │                                                                 │
│           │  CR3 holds physical address of Page Directory                   │
│           │  CR0.PG (bit 31) enables paging                                 │
│           │                                                                 │
└───────────┴─────────────────────────────────────────────────────────────────┘
```

## PDE/PTE Bit Layout

### Beginner's Guide: Page Table Entries
Each entry in the page table is like a permission slip for a specific page of memory. It tells the CPU:
- **Present (P)**: Is this page actually in RAM right now? (If 0, the CPU pauses and asks the OS to fetch it).
- **Read/Write (R/W)**: Can we write to this page, or is it read-only?
- **User/Super (U/S)**: Can a normal user program touch this, or is it for the Kernel only?
- **Accessed/Dirty (A/D)**: Has this page been used or modified recently? (Used for deciding what to swap out).

### Bit Definitions
```
31              12 11  9  8   7   6   5   4   3   2   1   0
+------------------+-----+---+---+---+---+---+---+---+---+---+
|    Frame/PFN     | AVL | G | S | D | A |PCD|PWT|U/S|R/W| P |
+------------------+-----+---+---+---+---+---+---+---+---+---+

P   (0)  Present:       1=mapped, 0=triggers #PF
R/W (1)  Read/Write:    1=writable, 0=read-only
U/S (2)  User/Super:    1=ring3 ok, 0=kernel only
PWT (3)  Write-through: cache policy
PCD (4)  Cache disable: cache policy
A   (5)  Accessed:      set by CPU on access
D   (6)  Dirty:         set by CPU on write (PTE only)
S   (7)  Page Size:     1=4MB (PDE), 0=4KB via PT
G   (8)  Global:        survives CR3 reload (PTE only)
AVL (9-11) Available:   OS-defined (we use for COW, swap, demand)
```

## Virtual Address Space Layout

### Beginner's Guide: The Map
The virtual address space is divided into two main worlds:
1.  **Kernel Space (Top)**: Where the OS lives. It stays the same for every program.
2.  **User Space (Bottom)**: Where programs live. Each program gets its own private "User Space".

This separation ensures that a crashing program can't hurt the OS or other programs.

### Memory Map
```
┌──────────────────────────────────────────────────────────────┐
│ 0xFFFFFFFF ─┬─ Recursive PD mapping (self-reference trick)   │
│             │                                                │
│ 0xFF800000 ─┼─ Page tables via recursive mapping             │
│             │                                                │
│ 0xD0000000 ─┼─ Kernel heap                                   │
│             │                                                │
│ 0xC0000000 ─┼─ KERNEL_BASE ──────────────────────────────────│
│             │  Identity map of physical 0-16MB               │
│             │  (kernel code/data)                            │
├─────────────┼────────────────────────────────────────────────┤
│ 0xBFFFFFFF ─┤                                                │
│     ↓       │  User Stack (grows DOWN)                       │
│ 0xB0000000 ─┼─ USER_STACK_BASE                               │
├─────────────┼────────────────────────────────────────────────┤
│             │                                                │
│             │  Shared Memory / mmap region                   │
│ 0x80000000 ─┼─ USER_SHARED_BASE                              │
├─────────────┼────────────────────────────────────────────────┤
│     ↑       │                                                │
│             │  User Heap (grows UP via brk/sbrk)             │
│ 0x40000000 ─┼─ USER_HEAP_BASE                                │
├─────────────┼────────────────────────────────────────────────┤
│             │                                                │
│             │  User Code + Data                              │
│ 0x00400000 ─┼─ USER_CODE_BASE (ELF load address)             │
│             │                                                │
│ 0x00000000 ─┴─ NULL guard (unmapped → catches null deref)    │
└──────────────────────────────────────────────────────────────┘
```

## Core Data Structures

```c
/* Raw PDE/PTE are uint32 with embedded flags */
typedef uint32 pde_t;
typedef uint32 pte_t;

/* Page Directory: 1024 entries × 4 bytes = 4KB, page-aligned */
typedef struct page_directory {
    pde_t entries[1024];
} __attribute__((aligned(4096))) page_directory_t;

/* Page Table: 1024 entries × 4 bytes = 4KB, page-aligned */
typedef struct page_table {
    pte_t entries[1024];
} __attribute__((aligned(4096))) page_table_t;

/* Per-process address space descriptor */
typedef struct address_space {
    page_directory_t* pd;    /* Page directory (virtual) */
    uint32 pd_phys;          /* Physical address of PD */
    uint32 heap_break;       /* Current heap end (brk) */
    uint32 stack_bottom;     /* Stack limit (grows down) */
    uint32 refcount;         /* For shared AS (threads) */
    uint32 ws_pages;         /* Working set size estimate */
    uint32 fault_count;      /* Page faults this interval */
} address_space_t;

/* Physical frame allocator: bitmap, 1 bit per 4KB frame */
typedef struct frame_allocator {
    uint32 bitmap[1024];     /* Supports 128MB (32K frames) */
    uint32 total_frames;
    uint32 free_frames;
    uint32 search_hint;      /* Optimization: where to start searching */
} frame_allocator_t;
```

## Core API

### Initialization
```c
void vmm_init(uint32 total_ram_bytes);  /* Called from kmain */
void vmm_enable_paging(void);           /* Load CR3, set CR0.PG */
```

### Page Mapping
```c
/* Map virtual address to physical frame with given flags */
int map_page(address_space_t* as, uint32 va, uint32 pa, uint32 flags);

/* Unmap page, free frame, invalidate TLB */
int unmap_page(address_space_t* as, uint32 va);

/* Walk PD→PT hierarchy, optionally create missing tables */
pte_t* walk_page_tables(address_space_t* as, uint32 va, int create);

/* TLB management */
void invalidate_tlb_entry(uint32 va);
void invalidate_tlb_all(void);
```

### Address Space Management
```c
/* Create new user address space (copies kernel mappings) */
address_space_t* create_address_space(void);

/* Free all user pages/tables, destroy address space */
void destroy_address_space(address_space_t* as);

/* Clone for fork() — marks pages COW */
address_space_t* clone_address_space(address_space_t* src);

/* Switch CR3 to new address space */
void switch_address_space(address_space_t* as);
```

### Frame Allocator
```c
uint32 frame_alloc(void);                     /* Returns phys addr or 0 */
void frame_free(uint32 phys_addr);
uint32 frame_alloc_contiguous(uint32 count);  /* For DMA */
```

## Page Fault Handling

### Beginner's Guide: What is a Page Fault?
A "Page Fault" sounds bad, but it's actually a normal part of how the OS works. It happens when a program tries to access a memory page that isn't currently "Present" in RAM.

Think of it like a library:
1.  You look for a book on the shelf (Memory Access).
2.  The book isn't there (Page Fault!).
3.  You ask the librarian (The OS).
4.  The librarian goes to the basement (Disk/Swap), finds the book, and puts it on the shelf.
5.  You can now read the book (Program continues).

However, if you ask for a book that doesn't exist or you're not allowed to read, the librarian kicks you out (Segmentation Fault / Crash).

### Error Code Decoding (INT 14)

```c
#define PF_ERR_PRESENT  (1 << 0)  /* 0=not-present, 1=protection */
#define PF_ERR_WRITE    (1 << 1)  /* 0=read, 1=write */
#define PF_ERR_USER     (1 << 2)  /* 0=kernel, 1=user mode */
#define PF_ERR_RESERVED (1 << 3)  /* Reserved bit violation */
```

### Fault Resolution Flowchart

```
                    Page Fault (INT 14)
                           │
                           ▼
              ┌─────────────────────────┐
              │ Read CR2 (fault addr)   │
              │ Read error code         │
              └───────────┬─────────────┘
                          │
            ┌─────────────┴─────────────┐
            │                           │
    ┌───────▼───────┐          ┌────────▼────────┐
    │ Not Present   │          │ Protection      │
    │ (P bit = 0)   │          │ (P bit = 1)     │
    └───────┬───────┘          └────────┬────────┘
            │                           │
    ┌───────┴───────┐          ┌────────┴────────┐
    │               │          │                 │
┌───▼───┐     ┌─────▼─────┐   ┌▼─────┐     ┌─────▼─────┐
│DEMAND │     │ SWAPPED   │   │ COW  │     │ SEGFAULT  │
│(lazy) │     │ page      │   │fault │     │(no COW,   │
└───┬───┘     └─────┬─────┘   └──┬───┘     │ write to  │
    │               │            │         │ read-only)│
    ▼               ▼            ▼         └───────────┘
Allocate       Swap in      Copy page,
frame,         from disk,   map as RW,
zero it,       map frame    free old if
map page                    unshared
```

### Demand Paging Pseudocode

```c
void handle_demand_fault(address_space_t* as, uint32 va, pte_t* pte) {
    uint32 frame = frame_alloc();
    if (!frame) {
        frame = clock_evict_page();  /* Try page replacement */
        if (!frame) kill_process();  /* OOM */
    }
    
    memset((void*)(KERNEL_BASE + frame), 0, PAGE_SIZE);  /* Zero for security */
    
    /* Restore original permissions, clear DEMAND flag */
    uint32 flags = (*pte & (PTE_USER | PTE_RW)) | PTE_PRESENT;
    *pte = (frame & PTE_FRAME_MASK) | flags;
    
    invalidate_tlb_entry(va);
}
```

## Page Replacement — Clock Algorithm

### Concept
- Circular buffer of evictable pages
- Each entry: (frame, PTE*, VA, owning AS)
- Clock hand advances on allocation
- Second-chance: pages with A=1 get another rotation

### Algorithm

```
clock_evict_page():
    loops = 0
    while loops < CLOCK_SIZE × 2:
        entry = clock[hand]
        advance hand
        
        if entry.empty:
            continue
            
        pte = entry.pte_ptr
        if pte.ACCESSED:
            clear pte.ACCESSED     # Second chance
            continue
        
        # Victim found!
        if pte.DIRTY:
            swap_out(entry)        # Write to swap
        else:
            clear pte              # Just invalidate
        
        remove entry from clock
        return entry.frame
    
    return 0  # All accessed = thrashing
```

### Required Bookkeeping

```c
typedef struct clock_entry {
    uint32 frame;          /* Physical frame number */
    pte_t* pte_ptr;        /* Pointer to PTE */
    uint32 va;             /* For TLB invalidation */
    address_space_t* as;   /* Owning process */
} clock_entry_t;

typedef struct clock_state {
    clock_entry_t entries[CLOCK_SIZE];
    uint32 hand;           /* Current position */
    uint32 count;          /* Entries in use */
} clock_state_t;
```

## Copy-on-Write (COW)

### fork() Implementation

```c
address_space_t* clone_address_space(address_space_t* src) {
    dst = create_address_space();
    
    for each user PDE in src:
        copy page table to dst
        for each PTE:
            if (PTE.RW && PTE.PRESENT):
                # Mark as COW in BOTH source and destination
                PTE.RW = 0
                PTE.COW = 1
                src.PT[pti] = PTE  # Also update source!
            dst.PT[pti] = PTE
    
    invalidate_tlb_all();  # Source's PTEs changed
    return dst;
}
```

### COW Fault Handling

```c
int handle_cow_fault(address_space_t* as, uint32 va, pte_t* pte) {
    old_frame = *pte & PTE_FRAME_MASK;
    new_frame = frame_alloc();
    
    # Copy page contents
    memcpy(KERNEL_BASE + new_frame, KERNEL_BASE + old_frame, 4096);
    
    # Update PTE: new frame, writable, clear COW
    *pte = new_frame | (flags & ~PTE_COW) | PTE_RW | PTE_PRESENT;
    
    invalidate_tlb_entry(va);
    # Note: old_frame stays allocated if other processes share it
    return 0;
}
```

## Thrashing Avoidance

### Working Set Approximation

```c
void update_working_set(address_space_t* as) {
    ws_count = 0;
    for each user page in as:
        if (PTE.ACCESSED):
            ws_count++
            PTE.ACCESSED = 0  # Reset for next interval
    
    as->ws_pages = ws_count;
}
```

### Scheduling Rule

```c
/* Called periodically (e.g., every 100 ticks) */
void check_thrashing(void) {
    for each process:
        if (process->fault_count > THRASH_THRESHOLD):
            suspend_process(process);  /* Let others run */
            process->fault_count = 0;
}
```

## Boot Sequence

```c
void kernel_main(multiboot_info_t* mbi) {
    uint32 ram = detect_ram(mbi);       /* From multiboot memory map */
    
    vmm_init(ram);                      /* Step 1: Initialize frame allocator,
                                         *         create kernel PD,
                                         *         identity-map first 16MB */
    
    vmm_enable_paging();                /* Step 2: Load CR3, set CR0.PG
                                         *         NOW RUNNING WITH PAGING! */
    
    /* Step 3: Optional — protect kernel text as read-only */
    // paging_protect_kernel_text_ro();
    
    /* Continue kernel init... */
}
```

### Initial Memory Map (before enabling paging)

```
Physical Memory:
0x00000000 - 0x000FFFFF  Reserved (BIOS, VGA, etc.)
0x00100000 - kernel_end  Kernel code/data
kernel_end - RAM_TOP     Free frames

Virtual Memory (after vmm_init):
0x00000000 - 0x00FFFFFF  Identity mapped (16MB)
0xC0000000 - 0xC0FFFFFF  Same physical 16MB (kernel's preferred VA)
0xFFC00000 - 0xFFFFFFFF  Recursive mapping (PD→PT access)
```

### Enabling Paging

```c
void vmm_enable_paging(void) {
    /* Load page directory physical address */
    write_cr3(vmm_kernel_as.pd_phys);
    
    /* Enable paging: set CR0.PG (bit 31) */
    uint32 cr0 = read_cr0();
    cr0 |= 0x80000000;
    write_cr0(cr0);
    
    /* CPU now translating ALL addresses through page tables.
     * Thanks to identity mapping, current EIP is still valid.
     * Kernel should soon jump to high addresses (0xC0000000+). */
}
```

## Integration Points

### Files Created
- `include/mm/vmm.h` — Public API and data structures
- `src/mm/vmm.c` — Full implementation

### Makefile Integration
Add to `OBJS` in Makefile:
```makefile
OBJS += obj/vmm.o

obj/vmm.o: src/mm/vmm.c include/mm/vmm.h
	$(COMPILER) $(CFLAGS) src/mm/vmm.c -o obj/vmm.o
```

### Kernel Integration (kernel.c)
```c
#include <vmm.h>

int kmain(uint32 magic, multiboot_info_t *mbi) {
    uint32 ram = detect_available_memory();
    
    vmm_init(ram);
    vmm_enable_paging();
    
    /* Rest of kernel init... */
}
```

### Page Fault Handler (isr.c)
```c
#include <vmm.h>

/* In ISR 14 handler: */
void handle_isr14(regs_t* r) {
    uint32 fault_addr = read_cr2();
    vmm_page_fault_handler(r->err_code, fault_addr, r->eip);
}
```
