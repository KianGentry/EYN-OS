section .data
align 16
buf: resb 3
arr: resd 2
msg: db "Hello from LEA!", 0x0A

section .text
global _start
_start:
    ; write(stdout, msg, len)
    mov eax, 4          ; syscall: sys_write
    mov ebx, 1          ; fd = stdout
    lea ecx, [msg]      ; buffer address (also works: mov ecx, msg)
    mov edx, 16         ; length of string
    int 0x80

    ; exercise mem/immediate
    add [arr], 1        ; increment first dword of arr
    mov ebx, [buf]      ; load from reserved buffer (zero)

    ; exit(0)
    mov eax, 1          ; syscall: exit
    xor ebx, ebx
    int 0x80
