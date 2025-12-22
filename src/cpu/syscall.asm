; 32 bit ISR stub for syscall interrupt 0x80

bits 32

global syscall_entry
extern syscall_dispatch
extern g_abort_to_shell
extern g_user_task_active
extern stack_space
extern ui_return_from_user_task

section .text
syscall_entry:
    ; Save segments
    push ds
    push es
    push fs
    push gs

    ; Save general registers
    pusha

    ; Push synthetic error code and int number (0 for errcode, 0x80 int)
    push dword 0            ; err_code
    push dword 0x80         ; int_no

    ; Saved EDI is at [esp + 8]
    mov eax, esp
    add eax, 8 ; eax = &saved EDI (start of regs_t)
    push eax
    call syscall_dispatch
    add esp, 4

    ; If requested, abandon return-to-user and jump back into the shell.
    cmp dword [g_abort_to_shell], 0
    je .no_abort
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

    mov [esp + 36], eax

    ; Pop our synthetic fields
    add esp, 8 ; discard int_no, err_code

    ; Restore registers and segments
    popa
    pop gs
    pop fs
    pop es
    pop ds

    iretd

