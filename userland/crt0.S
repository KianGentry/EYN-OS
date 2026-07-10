.global _start
.extern main
.extern _exit
.extern _eyn_libc_init

# Minimal C runtime entry for EYN-OS ring3 programs.
# Calls main(), then _exit(main_return).
_start:
    // Kernel provides a SysV-like initial stack:
    //   [esp+0]  argc
    //   [esp+4]  argv[0]
    //   ...
    //   [esp+4*argc] argv[argc-1]
    //   [esp+4*(argc+1)] NULL
    //   [esp+4*(argc+2)] envp[0]
    //   ...
    //   envp NULL terminator follows.

    mov (%esp), %esi      // argc
    lea 4(%esp), %edi     // argv
    lea 8(%esp,%esi,4), %ebx // envp

    // Keep main()'s entry stack 16-byte aligned.
    and $-16, %esp
    sub $4, %esp
    push %ebx
    push %edi
    push %esi
    call _eyn_libc_init
    add $12, %esp

    push %edi
    push %esi
    call main

    # _exit(main_return)
    push %eax
    call _exit

.hang:
    jmp .hang
