; Minimal amd64 IRQ stubs for Milestone A build bring-up.
; Full PIC/APIC dispatch integration is implemented in later milestones.

bits 64

section .text

%macro IRQ_STUB 1
global irq%1
irq%1:
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
