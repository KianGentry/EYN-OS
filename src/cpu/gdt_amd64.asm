; amd64 GDT/TSS helpers + ring-3 entry

BITS 64

default rel

global gdt_flush
global tss_flush
global enter_user_mode
global enter_user_mode_segdom

section .text

; void gdt_flush(uintptr gdt_ptr_addr)
; Loads GDTR, reloads data segments, and refreshes CS via far return.
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push qword 0x08
    lea rax, [rel .flush_cs]
    push rax
    retfq
.flush_cs:
    ret

; void tss_flush(uint16 tss_selector)
; Loads the amd64 task register (TR).
tss_flush:
    mov ax, di
    ltr ax
    ret

; void enter_user_mode(uint32 entry, uint32 user_stack_top)
; Enter CPL3 at RIP=entry, RSP=user_stack_top using flat user selectors.
enter_user_mode:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x23
    mov eax, esi
    push rax

    pushfq
    pop rax
    or eax, 0x200
    push rax

    push qword 0x1B
    mov eax, edi
    push rax

    iretq

; void enter_user_mode_segdom(uint32 entry, uint32 user_stack_top,
;                             uint16 user_cs, uint16 user_ds)
; amd64 keeps this compatibility entrypoint for existing callers.
enter_user_mode_segdom:
    mov ax, cx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    movzx r8, cx
    push r8
    mov eax, esi
    push rax

    pushfq
    pop rax
    or eax, 0x200
    push rax

    movzx r8, dx
    push r8
    mov eax, edi
    push rax

    iretq

section .note.GNU-stack noalloc noexec nowrite progbits