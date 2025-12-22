; IRQ stubs 0..15 route to C dispatcher irq_dispatch_c(irq)

BITS 32

extern irq_dispatch_c
extern g_abort_to_shell
extern stack_space
extern ui_return_from_user_task

%macro IRQ_STUB 1
global irq%1
irq%1:
    pushad
    push dword %1
    call irq_dispatch_c
    add esp, 4

    ; If requested, abandon return-to-user and jump back into the shell.
    cmp dword [g_abort_to_shell], 0
    je .no_abort_%1
    mov dword [g_abort_to_shell], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_space
    sti
    call ui_return_from_user_task
.halt_%1:
    hlt
    jmp .halt_%1
.no_abort_%1:
    popad
    iretd
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
