#ifndef PAGING_H
#define PAGING_H

#include <types.h>
#include <isr.h>

// Page size (4KB)
#define PAGE_SIZE 4096
#define PAGE_ENTRIES 1024

// Page directory and table structures
typedef struct {
    uint32 present : 1;      // Page present in memory
    uint32 rw : 1;          // Read/write permission
    uint32 user : 1;        // User/supervisor privilege
    uint32 accessed : 1;    // Page accessed
    uint32 dirty : 1;       // Page written to
    uint32 unused : 7;      // Unused bits
    uint32 frame : 20;      // Frame address (shifted right 12 bits)
} __attribute__((packed)) page_t;

typedef struct {
    page_t pages[PAGE_ENTRIES];
} page_table_t;

typedef struct {
    page_table_t* tables[PAGE_ENTRIES];
    uint32 physical_addr[PAGE_ENTRIES];
} page_directory_t;

// Page fault error codes
#define PF_PRESENT    0x1
#define PF_RW         0x2
#define PF_USER       0x4
#define PF_RESERVED   0x8
#define PF_INSTRUCTION 0x10

// Memory regions
#define KERNEL_VIRTUAL_BASE 0xC0000000  // 3GB
#define KERNEL_PHYSICAL_BASE 0x100000   // 1MB
#define USER_STACK_BASE 0xB0000000      // 2.75GB
#define USER_HEAP_BASE 0x80000000       // 2GB
#define USER_CODE_BASE 0x40000000       // 1GB

// Function prototypes
void init_paging(void);
void switch_page_directory(page_directory_t* new_dir);
page_t* get_page(uint32 address, int make, page_directory_t* dir);
void page_fault_handler(regs_t* r);
void map_page(uint32 virtual_addr, uint32 physical_addr, int user, int rw);
void unmap_page(uint32 virtual_addr);
void* kmalloc_a(uint32 size);  // Allocate aligned memory
void* kmalloc_p(uint32 size, uint32* physical);  // Allocate with physical address
void* kmalloc_ap(uint32 size, uint32* physical); // Allocate aligned with physical address
void kfree(void* ptr);

// Process memory management
page_directory_t* create_process_page_directory(void);
void destroy_process_page_directory(page_directory_t* dir);
void copy_page_directory(page_directory_t* dest, page_directory_t* src);
void switch_to_process(page_directory_t* dir);

// Memory protection
void protect_page(uint32 virtual_addr);
void unprotect_page(uint32 virtual_addr);
int is_page_present(uint32 virtual_addr);
int is_page_user(uint32 virtual_addr);
int is_page_writable(uint32 virtual_addr);

// Optional guards (safe no-ops until paging enabled)
void paging_install_null_guard(void);
void paging_protect_kernel_text_ro(void);

// Global page directory
extern page_directory_t* current_directory;

#endif
