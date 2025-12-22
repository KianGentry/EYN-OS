.global _start
.extern main
.extern _exit

# Minimal C runtime entry for EYN-OS ring3 programs.
# Calls main(), then _exit(main_return).
_start:
    call main

    # _exit(main_return)
    push %eax
    call _exit

.hang:
    jmp .hang
