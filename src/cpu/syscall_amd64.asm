; amd64 syscall entry for int 0x80 compatibility during Milestone A.
;
; ABI-INVARIANT: Preserve all interrupted GPRs and only replace return RAX.
; This keeps legacy userland expectations while the full amd64 user ABI is
; still being brought up.

bits 64

default rel

global syscall_entry
extern syscall_dispatch_amd64_frame

section .text
syscall_entry:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rbp, rsp
    sub rsp, 72

    ; Legacy int 0x80 convention: eax=sysno, ebx/ecx/edx=args1..3.
    ; We also pass saved RSI/RDI and return frame metadata.
    mov rax, [rbp + 112]
    mov [rsp + 0], rax
    mov rax, [rbp + 88]
    mov [rsp + 8], rax
    mov rax, [rbp + 104]
    mov [rsp + 16], rax
    mov rax, [rbp + 96]
    mov [rsp + 24], rax
    mov rax, [rbp + 72]
    mov [rsp + 32], rax
    mov rax, [rbp + 64]
    mov [rsp + 40], rax
    mov rax, [rbp + 120]
    mov [rsp + 48], rax
    mov rax, [rbp + 128]
    mov [rsp + 56], rax
    mov rax, [rbp + 136]
    mov [rsp + 64], rax

    mov rdi, rsp
    call syscall_dispatch_amd64_frame

    add rsp, 72

    ; Place syscall return value into saved RAX slot.
    mov [rsp + 112], rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
