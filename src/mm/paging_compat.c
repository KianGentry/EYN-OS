/* Compatibility wrappers for legacy paging symbols expected by older code. */

#include <mm/vmm.h>
#include <isr.h>

/* Legacy page fault handler signature used by ISR 14. */
void page_fault_handler(regs_t* r) {
    uint32 fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
    vmm_page_fault_handler(r->err_code, fault_addr, r->eip);
}

/* Legacy guard: ensure page 0 is unmapped. */
void paging_install_null_guard(void) {
    vmm_unmap_page(&vmm_kernel_as, 0x0);
    invalidate_tlb_entry(0x0);
}

/* Legacy guard: mark kernel .text (and .rodata) read-only. */
void paging_protect_kernel_text_ro(void) {
    extern uint32 __kernel_text_start;
    extern uint32 __kernel_text_end;
    extern uint32 __kernel_rodata_start;
    extern uint32 __kernel_rodata_end;

    uint32 start = ((uint32)&__kernel_text_start) & ~(PAGE_SIZE - 1);
    uint32 end   = ((uint32)&__kernel_text_end);

    for (uint32 va = start; va < end; va += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            *pte &= ~PTE_RW;
        }
    }

    start = ((uint32)&__kernel_rodata_start) & ~(PAGE_SIZE - 1);
    end   = ((uint32)&__kernel_rodata_end);

    for (uint32 va = start; va < end; va += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            *pte &= ~PTE_RW;
        }
    }

    invalidate_tlb_all();
}
