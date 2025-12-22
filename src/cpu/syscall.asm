; 32 bit ISR stub for syscall interrupt 0x80

bits 32

global syscall_entry
extern syscall_dispatch
extern g_abort_to_shell
extern g_user_task_active
extern stack_space
extern stack_bottom
extern ui_return_from_user_task

section .text
syscall_entry:
    ; Ensure kernel data segments while running the handler.
    ; Preserve EAX: user passes syscall number in EAX.
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop eax

    ; Build a regs_t compatible frame (see include/cpu/isr.h)
    ; Layout from &EDI: edi..eax, int_no, err_code, then CPU iret frame.
    push dword 0            ; err_code
    push dword 0x80         ; int_no
    pusha

    ; Stack overflow tripwire: if the kernel stack underflowed, bail out
    ; to the shell immediately on a known-good stack.
    cmp esp, stack_bottom
    jae .stack_ok
    mov dword [g_abort_to_shell], 1
    jmp .do_abort
.stack_ok:

    ; pass &regs_t (starts at saved EDI)
    mov eax, esp
    push eax
    call syscall_dispatch
    add esp, 4

    ; If requested, abandon return-to-user and jump back into the shell.
    cmp dword [g_abort_to_shell], 0
    je .no_abort
.do_abort:
    mov dword [g_abort_to_shell], 0
    mov dword [g_user_task_active], 0
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_space
    sti
    call ui_return_from_user_task
.halt:
    hlt
    jmp .halt
.no_abort:

    ; Stash return value into saved EAX within the pusha frame.
    mov [esp + 28], eax

    ; Restore registers
    popa

    ; Discard int_no and err_code
    add esp, 8

    ; Restore data segments appropriate for the return privilege level.
    ; After the add above, the iret frame starts at [esp]: eip, cs, eflags, useresp, ss
    ; Preserve EAX: holds syscall return value.
    push eax
    mov ax, [esp + 8]
    test ax, 3
    jz .ret_kernel
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    jmp .seg_done
.ret_kernel:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.seg_done:
    pop eax
    iretd

