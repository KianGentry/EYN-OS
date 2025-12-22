.global _start
.extern main

; Minimal C runtime entry for EYN-OS ring3 programs.
; Kernel enters at ELF e_entry with a valid user stack.
_start:
    call main

    ; exit(main_return)
    mov %eax, %ebx
    mov $2, %eax
    int $0x80

.hang:
    jmp .hang
