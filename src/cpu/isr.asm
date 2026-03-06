; 32-bit ISR exception stubs (0..31)
; Build a regs_t compatible stack frame (see include/cpu/isr.h) and
; dispatch to C handler isr_dispatch(regs_t* r).

bits 32

extern isr_dispatch
extern g_abort_to_shell
extern g_user_task_active
extern stack_space
extern isr_abort_stack_top
extern ui_return_from_user_task

section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0        ; err_code
    push dword %1       ; int_no
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1       ; int_no (CPU already pushed err_code)
    jmp isr_common_stub
%endmacro

isr_common_stub:
    pusha

    ; pass &regs_t (starts at saved EDI)
    mov eax, esp
    push eax
    call isr_dispatch
    add esp, 4

    ; If requested, abandon return-to-context and jump back into the shell/UI.
    cmp dword [g_abort_to_shell], 0
    je .no_abort
    mov dword [g_abort_to_shell], 0
    mov dword [g_user_task_active], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, isr_abort_stack_top
    sti
    call ui_return_from_user_task
.halt:
    hlt
    jmp .halt
.no_abort:

    popa

    ; discard int_no and err_code (err_code is CPU-pushed for ISR_ERR,
    ; synthetic 0 for ISR_NOERR)
    add esp, 8

    iretd

; Exceptions without error code
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

; Exceptions with error code
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15

ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR 30
ISR_NOERR 31
