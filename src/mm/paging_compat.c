/* Compatibility wrappers for legacy paging symbols expected by older code. */

#include <utilities/paging.h>

#include <mm/vmm.h>
#include <isr.h>
#include <vga.h>

/* Legacy page fault handler signature used by ISR 14. */
void page_fault_handler(regs_t* r) {
    uint32 fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    // Extra diagnostics for ring3 faults (helps debug syscall/iret issues).
    if (r && ((r->cs & 3) == 3)) {
        printf("\n%c*** Page Fault (user) ***\n", 255, 0, 0);
        printf("Address: 0x%X  EIP: 0x%X  CS: 0x%X  SS: 0x%X  ESP: 0x%X\n",
               (unsigned)fault_addr, (unsigned)r->eip, (unsigned)r->cs, (unsigned)r->ss, (unsigned)r->useresp);
         printf("EAX: 0x%X  EBX: 0x%X  ECX: 0x%X  EDX: 0x%X\n",
             (unsigned)r->eax, (unsigned)r->ebx, (unsigned)r->ecx, (unsigned)r->edx);
         printf("ESI: 0x%X  EDI: 0x%X  EBP: 0x%X  EFLAGS: 0x%X\n",
             (unsigned)r->esi, (unsigned)r->edi, (unsigned)r->ebp, (unsigned)r->eflags);
        printf("Error: 0x%X\n", (unsigned)r->err_code);
    }
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
