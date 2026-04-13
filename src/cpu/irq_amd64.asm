; amd64 IRQ stubs 0..15 route to C dispatcher irq_dispatch_c(irq)

bits 64

default rel

extern irq_dispatch_c
extern g_abort_to_shell
extern ui_return_from_user_task
extern isr_abort_stack_top

section .text

%macro PUSH_GPRS 0
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
%endmacro

%macro IRQ_STUB 1
global irq%1
irq%1:
    PUSH_GPRS
    mov edi, %1
    call irq_dispatch_c
    cmp dword [rel g_abort_to_shell], 0
    je .no_abort_%1
    mov dword [rel g_abort_to_shell], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, isr_abort_stack_top
    sti
    call ui_return_from_user_task
.halt_%1:
    hlt
    jmp .halt_%1
.no_abort_%1:
    POP_GPRS
    iretq
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

section .note.GNU-stack noalloc noexec nowrite progbits
