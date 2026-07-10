; 32 bit ISR stub for syscall interrupt 0x80

bits 32

global syscall_entry
extern syscall_dispatch
extern g_abort_to_shell
extern g_user_task_active
extern g_user_segdom_ds
extern g_user_segdom_gs
extern stack_space
extern stack_bottom
extern isr_abort_stack_top
extern isr_abort_stack_bottom
extern user_task_abort_continue

section .text
syscall_entry:
    ; Save user GS *before* clobbering segment registers.
    ; Linux i386 TLS uses GS (set via set_thread_area), and it must survive syscalls.
    push gs

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
    ; Stack layout (low→high after pusha):
    ;   [esp+0..31]  pusha frame (EDI..EAX); [esp+28]=EAX=syscall#
    ;   [esp+32]     int_no (0x80)
    ;   [esp+36]     err_code (0)
    ;   [esp+40]     user_gs  (our push gs above)
    ;   [esp+44+]    CPU iret frame: EIP, CS, EFLAGS, user_ESP, SS
    push dword 0            ; err_code
    push dword 0x80         ; int_no
    pusha

    ; Stack overflow tripwire: if the kernel C call stack underflowed (ESP below
    ; stack_bottom), bail out to a known-good stack.
    cmp esp, stack_bottom
    jae .stack_ok
    mov dword [g_abort_to_shell], 1
    jmp .do_abort
.stack_ok:
    ; Preserve original syscall number (saved EAX in pusha frame) across C call.
    mov edx, [esp + 28]
    push edx

    ; pass &regs_t (starts at saved EDI)
    lea eax, [esp + 4]
    push eax
    call syscall_dispatch
    add esp, 4
    pop edx

    ; SYSCALL_EXIT (eax=2) must never return to user mode.
    cmp edx, 2
    je .do_abort

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
    mov esp, isr_abort_stack_top
    sti
    call user_task_abort_continue
.halt:
    hlt
    jmp .halt
.no_abort:

    ; Stash return value into saved EAX within the pusha frame.
    mov [esp + 28], eax

    ; Restore registers
    popa

    ; Discard int_no and err_code; stack now: [esp+0]=user_gs, [esp+4+]=iret frame
    add esp, 8

    ; Restore user DS/ES/FS from global (flat user data segment).
    ; GS is restored from the value saved on entry, preserving TLS.
    push eax
    mov ax, [g_user_segdom_ds]
    test ax, ax
    jnz .has_ds
    mov ax, 0x23
.has_ds:
    mov ds, ax
    mov es, ax
    mov fs, ax
    pop eax

    ; Discard saved user GS from entry frame and restore GS from the tracked
    ; selector. This lets set_thread_area updates persist across syscalls.
    add esp, 4
    push eax
    mov ax, [g_user_segdom_gs]
    test ax, ax
    jnz .has_gs
    mov ax, [g_user_segdom_ds]
.has_gs:
    mov gs, ax
    pop eax

    iretd


