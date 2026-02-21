/* Compatibility wrappers for legacy paging symbols expected by older code. */

#include <utilities/paging.h>

#include <mm/vmm.h>
#include <isr.h>
#include <vga.h>
#include <mm/user_access.h>
#include <serial.h>
#include <system.h>

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
    uint32 fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    // Always emit a minimal serial line first in case VGA output is unavailable.
    serial_puts_unsafe("[PF] addr=");
    serial_put_hex32(fault_addr);
    serial_puts_unsafe(" eip=");
    serial_put_hex32(r ? r->eip : 0);
    serial_puts_unsafe(" err=");
    serial_put_hex32(r ? r->err_code : 0);
    serial_puts_unsafe("\n");

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

        // If the faulting code uses a frame pointer, we can often recover caller context.
        // On i386 cdecl with a typical prologue:
        //   [ebp+0] saved ebp
        //   [ebp+4] return address
        //   [ebp+8] first arg
        uint32 saved_ebp = 0;
        uint32 ret_eip = 0;
        uint32 arg0 = 0;
        int ok_saved = copyin(&saved_ebp, (const void*)(uint32)r->ebp, sizeof(saved_ebp)) == 0;
        int ok_ret = copyin(&ret_eip, (const void*)((uint32)r->ebp + 4u), sizeof(ret_eip)) == 0;
        int ok_arg = copyin(&arg0, (const void*)((uint32)r->ebp + 8u), sizeof(arg0)) == 0;

        if (ok_saved || ok_ret || ok_arg) {
            printf("Frame: ");
            if (ok_saved) printf("saved_ebp=0x%X ", (unsigned)saved_ebp);
            if (ok_ret) printf("ret=0x%X ", (unsigned)ret_eip);
            if (ok_arg) printf("arg0=0x%X", (unsigned)arg0);
            printf("\n");
        }
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
