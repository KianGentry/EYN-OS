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

start:
        cli ; clears interrupts 

        mov esp, stack_space
        push ebx
        push eax

        call kmain ; continue kernel.c kmain func
        ;call Shutdown
        hlt

section .bss
resb 8192
stack_space:

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
