; GDT/TSS helpers + ring-3 entry

BITS 32

global gdt_flush
global tss_flush
global enter_user_mode

section .text

; void gdt_flush(uint32 gdt_ptr_addr)
; Loads GDTR and reloads segment registers to our selectors.
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush_cs
.flush_cs:
    ret

; void tss_flush(uint16 tss_selector)
; Loads task register.
tss_flush:
    mov ax, [esp + 4]
    ltr ax
    ret

; void enter_user_mode(uint32 entry, uint32 user_stack_top)
; Sets user segments and irets to CPL=3.
enter_user_mode:
    mov eax, [esp + 4]    ; entry
    mov edx, [esp + 8]    ; user_stack_top

    mov cx, 0x23
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push dword 0x23       ; SS
    push edx              ; ESP

    pushfd
    pop ecx
    or ecx, 0x200         ; IF=1
    push ecx              ; EFLAGS

    push dword 0x1B       ; CS
    push eax              ; EIP

    iretd
