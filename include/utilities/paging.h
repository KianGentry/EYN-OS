#ifndef PAGING_H
#define PAGING_H
#include <isr.h>

// Legacy ISR14 page-fault entrypoint used by src/mm/paging_compat.c.
void page_fault_handler(regs_t* r);


/* Compatibility shim over the new VMM. */

#include <mm/vmm.h>

/* Legacy constants mapped to new layout. */
#define KERNEL_VIRTUAL_BASE KERNEL_BASE

/* Legacy guard function prototypes (implemented in paging_compat.c) */
void paging_install_null_guard(void);
void paging_protect_kernel_text_ro(void);

/* Legacy map/unmap signatures mapped to kernel address space. */
static inline void map_page(uint32 virtual_addr, uint32 physical_addr, int user, int rw) {
    uint32 flags = (user ? PTE_USER : 0) | (rw ? PTE_RW : 0);
    vmm_map_page(&vmm_kernel_as, virtual_addr, physical_addr, flags);
}

static inline void unmap_page(uint32 virtual_addr) {
    vmm_unmap_page(&vmm_kernel_as, virtual_addr);
}

#endif /* PAGING_H */
