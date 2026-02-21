; hello_world.asm
section .text
global _start

_start:
    mov eax, 1          ; syscall: WRITE (EYN-OS)
    mov ebx, 1          ; fd = stdout
    lea ecx, [hello_str]
    mov edx, 14         ; len("Hello, world!\n")
    int 0x80

    mov eax, 2          ; syscall: EXIT (EYN-OS)
    xor ebx, ebx
    int 0x80

section .data
hello_str:
    db "Hello, world!", 0x0A