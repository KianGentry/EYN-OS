; amd64 syscall entry for int 0x80 compatibility during Milestone A.
;
; ABI-INVARIANT: Preserve all interrupted GPRs and only replace return RAX.
; This keeps legacy userland expectations while the full amd64 user ABI is
; still being brought up.

bits 64

default rel

global syscall_entry
extern syscall_dispatch_amd64

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

    ; Legacy int 0x80 convention: eax=sysno, ebx/ecx/edx=args1..3.
    ; Pass two extra args (saved rsi/rdi) for forward compatibility.
    mov rdi, [rsp + 112] ; saved rax (syscall number)
    mov rsi, [rsp + 88]  ; saved rbx
    mov rdx, [rsp + 104] ; saved rcx
    mov rcx, [rsp + 96]  ; saved rdx
    mov r8,  [rsp + 72]  ; saved rsi
    mov r9,  [rsp + 64]  ; saved rdi
    call syscall_dispatch_amd64

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
