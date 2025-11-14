.section .data
msg:
    .ascii "Hello from ELF32 on EYN-OS!\n"
len = . - msg

.section .text
.global _start
_start:
    movl $1, %eax    # sys_write (EYN-OS native syscall numbering)
    movl $1, %ebx    # stdout
    movl $msg, %ecx
    movl $len, %edx
    int $0x80

    movl $2, %eax    # sys_exit (EYN-OS native syscall numbering)
    xorl %ebx, %ebx
    int $0x80
