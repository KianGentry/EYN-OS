/* Compatibility wrappers for legacy paging symbols expected by older code. */

#include <utilities/paging.h>

#include <mm/vmm.h>
#include <isr.h>
#include <vga.h>
#include <mm/user_access.h>
#include <serial.h>
#include <system.h>
#include <panic.h>

static void serial_putc_raw(char c) {
    uint16 port = SERIAL_COM1;
    int timeout = 100000;
    while (timeout--) {
        if (inportb(SERIAL_LINE_STATUS(port)) & SERIAL_THRE) break;
    }
    outportb(SERIAL_DATA_PORT(port), (uint8)c);
}

static void serial_puts_unsafe(const char* s) {
    if (!s) return;
    while (*s) {
        serial_putc_raw(*s++);
    }
}

static void serial_put_hex32(uint32 v) {
    static const char* hex = "0123456789ABCDEF";
    serial_puts_unsafe("0x");
    for (int i = 28; i >= 0; i -= 4) {
        char c = hex[(v >> (unsigned)i) & 0xFu];
        serial_putc_raw(c);
    }
}

/* Legacy page fault handler signature used by ISR 14. */
void page_fault_handler(regs_t* r) {
    uintptr fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    if (!r) {
        PANICF("PAGE FAULT (unknown regs): addr=0x%08X", (unsigned)fault_addr);
    }

    /*
     * SECURITY-INVARIANT: A page fault taken in CPL0 is a kernel bug.
     *
     * Why: Retrying a faulting kernel instruction can re-enter #PF paths and
     * drive secondary faults (e.g. during swap/IO/accounting), leaving the
     * system in an increasingly corrupted state.
     *
     * Invariant: Kernel-mode #PF triggers an immediate panic with a minimal
     * diagnostic payload. User-mode faults continue through the VMM handler.
     */
    if (r && ((r->cs & 3u) != 3u)) {
        // Emit a minimal serial line first in case VGA output is unavailable.
        serial_puts_unsafe("[PF] addr=");
        serial_put_hex32((uint32)fault_addr);
        serial_puts_unsafe(" eip=");
        serial_put_hex32(r->eip);
        serial_puts_unsafe(" err=");
        serial_put_hex32(r->err_code);
        serial_puts_unsafe("\n");

        PANICF("PAGE FAULT (kernel): addr=0x%08X eip=0x%08X err=0x%08X cs=0x%08X esp=0x%08X",
               (unsigned)fault_addr,
               (unsigned)r->eip,
               (unsigned)r->err_code,
               (unsigned)r->cs,
               (unsigned)r->esp);
    }
    vmm_page_fault_handler(r->err_code, (uint32)fault_addr, r->eip);
}

/* Legacy guard: ensure page 0 is unmapped. */
void paging_install_null_guard(void) {
    vmm_unmap_page(&vmm_kernel_as, 0x0);
    vm_invalidate_page((void*)0x0);
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

    vm_invalidate_range((void*)start, (size_t)(end - start));

    start = ((uint32)&__kernel_rodata_start) & ~(PAGE_SIZE - 1);
    end   = ((uint32)&__kernel_rodata_end);

    for (uint32 va = start; va < end; va += PAGE_SIZE) {
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            *pte &= ~PTE_RW;
        }
    }

    vm_invalidate_range((void*)start, (size_t)(end - start));
}
