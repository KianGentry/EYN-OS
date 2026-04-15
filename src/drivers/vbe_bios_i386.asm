; i386 BIOS VBE thunk for runtime mode switching.
;
; int vbe_bios_int10_set_mode_i386(uint16 mode)
;   - Requests VBE mode set via INT 10h AX=4F02 with LFB bit (BX|=0x4000)
;   - Returns BIOS AX status (0x004F on success) in EAX

bits 32

global vbe_bios_int10_set_mode_i386

%define RM_SCRATCH      0x7000
%define RM_SAVE_ESP     (RM_SCRATCH + 0)
%define RM_SAVE_CR0     (RM_SCRATCH + 4)
%define RM_RET_AX       (RM_SCRATCH + 8)
%define RM_MODE_WORD    (RM_SCRATCH + 10)
%define RM_PM_PTR       (RM_SCRATCH + 12)  ; 32-bit offset + 16-bit selector

%define RM_STUB_ADDR    0x7100
%define RM_STUB_SEG     (RM_STUB_ADDR >> 4)
%define RM_STUB_OFF     (RM_STUB_ADDR & 0x000F)
%define RM_STACK_TOP    0x7B00

section .text

vbe_bios_int10_set_mode_i386:
    pushfd
    pushad
    cli

    mov eax, esp
    mov [RM_SAVE_ESP], eax
    mov eax, cr0
    mov [RM_SAVE_CR0], eax

    mov eax, [esp + 40]
    mov [RM_MODE_WORD], ax
    mov word [RM_RET_AX], 0xFFFF

    mov dword [RM_PM_PTR], pm_return
    mov word [RM_PM_PTR + 4], 0x0008

    ; Copy the real-mode stub to low memory where CS:IP addressing is safe.
    mov esi, rm_stub_start
    mov edi, RM_STUB_ADDR
    mov ecx, rm_stub_end - rm_stub_start
    cld
    rep movsb

    ; Disable paging + protected mode, then far jump (16:16) into copied stub.
    mov eax, [RM_SAVE_CR0]
    and eax, 0x7FFFFFFE
    mov cr0, eax
    db 0xEA
    dw RM_STUB_OFF
    dw RM_STUB_SEG

bits 16
rm_stub_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, RM_STACK_TOP

    mov bx, [RM_MODE_WORD]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    mov [RM_RET_AX], ax

    ; Re-enable protected mode and far jump back to 32-bit kernel code.
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp dword far [RM_PM_PTR]

rm_stub_end:

bits 32
pm_return:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Restore original CR0 (re-enables paging if it was on).
    mov eax, [RM_SAVE_CR0]
    mov cr0, eax
    jmp short .paging_restored

.paging_restored:
    mov esp, [RM_SAVE_ESP]

    popad
    popfd

    xor eax, eax
    mov ax, [RM_RET_AX]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
