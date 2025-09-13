; Hello World for EYN-OS custom assembler/runtime
; Uses int 0x80 syscalls:
;  - eax=1 (write), ebx=1 (stdout), ecx=buffer, edx=len
;  - eax=2 (exit), ebx=code

section .text
global _start

_start:
    ; Write syscall (syscall 1)
    ; EAX = 1 (write)
    ; EBX = 1 (stdout)
    ; ECX = message address
    ; EDX = message length
    
    mov eax, 1          ; syscall number for write
    mov ebx, 1          ; file descriptor (stdout)
    mov ecx, message    ; message to write
    mov edx, 18         ; message length (including newline)
    int 0x80            ; make syscall
    
    ; Exit syscall (syscall 2)
    ; EAX = 2 (exit)
    ; EBX = 0 (exit code)
    
    mov eax, 2          ; syscall number for exit
    mov ebx, 0          ; exit code 0
    int 0x80            ; make syscall

section .data
message: db "Hello, World!", 0x0A  ; "Hello, World!" + newline 