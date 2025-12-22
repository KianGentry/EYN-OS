.intel_syntax noprefix

.section .text
.global _start
_start:
    mov eax, 1          # syscall: write
    mov ebx, 1          # fd=stdout
    lea ecx, [fizz]
    mov edx, 5          # len("Fizz\n")
    int 0x80

    mov eax, 1          # syscall: write
    mov ebx, 1          # fd=stdout
    lea ecx, [buzz]
    mov edx, 5          # len("Buzz\n")
    int 0x80

    jmp _start

.section .rodata
fizz:
    .ascii "Fizz\n"

buzz:
    .ascii "Buzz\n"
