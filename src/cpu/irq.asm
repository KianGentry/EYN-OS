; IRQ stubs 0..15 route to C dispatcher irq_dispatch_c(irq)

BITS 32

extern irq_dispatch_c

%macro IRQ_STUB 1
global irq%1
irq%1:
    pushad
    push dword %1
    call irq_dispatch_c
    add esp, 4
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
