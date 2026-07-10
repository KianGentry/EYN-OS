; amd64 ISR exception stubs (0..31)
;
; ABI-INVARIANT: Exceptions that push an error code must discard that word
; before iretq. Returning without popping it makes iretq consume err_code as RIP.
;
; The C-side bridge consumes amd64_interrupt_frame_t generated in each stub.

bits 64

default rel

extern isr_amd64_dispatch_frame
extern g_abort_to_shell
extern g_user_task_active
extern isr_abort_stack_top
extern user_task_abort_continue

section .text

%macro PUSH_GPRS 0
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
%endmacro

%macro POP_GPRS 0
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
%endmacro

%macro ISR_NOERR 1
global isr%1
isr%1:
    PUSH_GPRS
    mov rbp, rsp
    sub rsp, 40
    mov qword [rsp + 0], %1
    mov qword [rsp + 8], 0
    mov rax, [rbp + 120]
    mov [rsp + 16], rax
    mov rax, [rbp + 128]
    mov [rsp + 24], rax
    mov rax, [rbp + 136]
    mov [rsp + 32], rax
    mov rdi, rsp
    call isr_amd64_dispatch_frame
    cmp dword [rel g_abort_to_shell], 0
    je .no_abort_%1
    mov dword [rel g_abort_to_shell], 0
    mov dword [rel g_user_task_active], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, isr_abort_stack_top
    sti
    call user_task_abort_continue
.halt_%1:
    hlt
    jmp .halt_%1
.no_abort_%1:
    add rsp, 40
    POP_GPRS
    iretq
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    PUSH_GPRS
    mov rbp, rsp
    sub rsp, 40
    mov qword [rsp + 0], %1
    mov rax, [rbp + 120]
    mov [rsp + 8], rax
    mov rax, [rbp + 128]
    mov [rsp + 16], rax
    mov rax, [rbp + 136]
    mov [rsp + 24], rax
    mov rax, [rbp + 144]
    mov [rsp + 32], rax
    mov rdi, rsp
    call isr_amd64_dispatch_frame
    cmp dword [rel g_abort_to_shell], 0
    je .no_abort_%1
    mov dword [rel g_abort_to_shell], 0
    mov dword [rel g_user_task_active], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, isr_abort_stack_top
    sti
    call user_task_abort_continue
.halt_%1:
    hlt
    jmp .halt_%1
.no_abort_%1:
    add rsp, 40
    POP_GPRS
    add rsp, 8
    iretq
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31

section .note.GNU-stack noalloc noexec nowrite progbits
