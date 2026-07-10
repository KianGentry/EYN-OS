MBOOT_HEADER_MAGIC equ 0x1BADB002
MBOOT_PAGE_ALIGN equ 1 << 0
MBOOT_MEM_INFO equ 1 << 1
MBOOT_GRAPH_MODE equ 1 << 2
MBOOT_HEADER_FLAGS equ MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO | MBOOT_GRAPH_MODE
MBOOT_CHECKSUM equ -(MBOOT_HEADER_MAGIC + MBOOT_HEADER_FLAGS)

bits 32 ; funny downgrade

section .text
        align 32
        dd MBOOT_HEADER_MAGIC
        dd MBOOT_HEADER_FLAGS
        dd MBOOT_CHECKSUM

        dd 0
        dd 0
        dd 0
        dd 0
        dd 0 ; skip over load of flags

        dd 0 ; graphical mode
        dd 640 ; width of screen
        dd 480 ; height of screen
        dd 32 ; bit depth

global start
extern kmain ; kernel.c

global Shutdown  ; Export Shutdown function
global stack_space
global stack_bottom
global isr_abort_stack_top
global isr_abort_stack_bottom

start:
        cli ; clears interrupts 

        mov esp, stack_space
        and esp, 0xFFFFFFF0
        sub esp, 8
        push ebx
        push eax

        call kmain ; continue kernel.c kmain func
        ;call Shutdown
        hlt

section .bss
; Main kernel C call stack. Shell, script executor, and all normal kernel code
; run on this stack. It must NOT be touched by interrupt/abort handlers so that
; kernel_longjmp() can safely return into it after a user-task exit.
stack_bottom:
resb 32768
stack_space:

; Dedicated ISR/abort-handler stack (TSS.esp0 = isr_abort_stack_top).
; Syscall, exception, and IRQ abort-to-shell paths use this stack exclusively
; so they never clobber the main C call stack that shell script commands run on.
; 8 KB is sufficient for a single non-nested ISR invocation chain.
;
; ABI-INVARIANT: isr_abort_stack_top is the value stored in TSS.esp0 for all
; ring-3 user tasks. Changing the size requires no other adjustments, but
; decreasing below ~4 KB risks overflow under deep syscall call chains.
isr_abort_stack_bottom:
resb 8192
isr_abort_stack_top:

Shutdown: ; actually a reboot but i dont dare rename it
    mov ax, 0x1000
    mov ax, ss
    mov sp, 0xf000
    mov ax, 0x5307
    mov bx, 0x0001
    mov cx, 0x0003
    int 0x15

WaitForEnter:
    mov ah, 0
    int 0x16
    cmp al, 0x0D
    jne WaitForEnter
    ret
