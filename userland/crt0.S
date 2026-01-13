.global _start
.extern main
.extern _exit

# Minimal C runtime entry for EYN-OS ring3 programs.
# Calls main(), then _exit(main_return).
_start:
    // Kernel provides a SysV-like initial stack:
    //   [esp+0]  argc
    //   [esp+4]  argv[0]
    //   ...
    //   [esp+4*argc] argv[argc-1]
    //   [esp+4*(argc+1)] NULL
    // (envp omitted for now)

    mov (%esp), %eax      // argc
    lea 4(%esp), %edx     // argv

    // Keep main()'s entry stack 16-byte aligned.
    and $-16, %esp
    sub $4, %esp
    push %edx
    push %eax
    call main

    # _exit(main_return)
    push %eax
    call _exit

.hang:
    jmp .hang
