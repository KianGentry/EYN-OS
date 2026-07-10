; i386 BIOS VBE thunks for runtime graphics mode control.
;
; uint32 vbe_bios_int10_set_mode_i386(uint16 mode)
;   - INT 10h AX=4F02, BX=mode|0x4000 (LFB)
;   - returns BIOS AX status in EAX (0x004F = success)
;
; uint32 vbe_bios_int10_get_mode_info_i386(uint16 mode, void* out_block)
;   - INT 10h AX=4F01, CX=mode, ES:DI -> temporary low-memory buffer
;   - copies 256-byte mode info block into out_block after returning to PM
;   - returns BIOS AX status in EAX (0x004F = success)

bits 32

global vbe_bios_int10_set_mode_i386
global vbe_bios_int10_get_mode_info_i386

%define RM_SCRATCH            0x7000
%define RM_SAVE_ESP           (RM_SCRATCH + 0)
%define RM_SAVE_CR0           (RM_SCRATCH + 4)
%define RM_RET_AX             (RM_SCRATCH + 8)
%define RM_MODE_WORD          (RM_SCRATCH + 10)
%define RM_OUT_PTR            (RM_SCRATCH + 12)
%define RM_PM_PTR             (RM_SCRATCH + 16)  ; dword offset + word selector

%define RM_STUB_SET_ADDR      0x7100
%define RM_STUB_SET_SEG       (RM_STUB_SET_ADDR >> 4)
%define RM_STUB_SET_OFF       (RM_STUB_SET_ADDR & 0x000F)

%define RM_STUB_GET_ADDR      0x7180
%define RM_STUB_GET_SEG       (RM_STUB_GET_ADDR >> 4)
%define RM_STUB_GET_OFF       (RM_STUB_GET_ADDR & 0x000F)

%define RM_MODE_INFO_BUF      0x7200
%define RM_MODE_INFO_SEG      (RM_MODE_INFO_BUF >> 4)
%define RM_MODE_INFO_OFF      (RM_MODE_INFO_BUF & 0x000F)
%define RM_MODE_INFO_SIZE     256

%define RM_STACK_TOP          0x7B00

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

    mov dword [RM_PM_PTR], pm_return_set
    mov word [RM_PM_PTR + 4], 0x0008

    ; Copy real-mode stub to low memory.
    mov esi, rm_stub_set_start
    mov edi, RM_STUB_SET_ADDR
    mov ecx, rm_stub_set_end - rm_stub_set_start
    cld
    rep movsb

    ; Disable paging+protected mode and jump to copied real-mode stub.
    mov eax, [RM_SAVE_CR0]
    and eax, 0x7FFFFFFE
    mov cr0, eax
    db 0xEA
    dw RM_STUB_SET_OFF
    dw RM_STUB_SET_SEG

bits 16
rm_stub_set_start:
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

    ; Re-enter protected mode and far jump back to kernel code.
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp dword far [RM_PM_PTR]
rm_stub_set_end:

bits 32
pm_return_set:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Restore original CR0 (re-enables paging if it was set).
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

vbe_bios_int10_get_mode_info_i386:
    pushfd
    pushad
    cli

    mov eax, esp
    mov [RM_SAVE_ESP], eax
    mov eax, cr0
    mov [RM_SAVE_CR0], eax

    mov eax, [esp + 40]
    mov [RM_MODE_WORD], ax
    mov eax, [esp + 44]
    mov [RM_OUT_PTR], eax
    mov word [RM_RET_AX], 0xFFFF

    mov dword [RM_PM_PTR], pm_return_get
    mov word [RM_PM_PTR + 4], 0x0008

    ; Copy real-mode stub to low memory.
    mov esi, rm_stub_get_start
    mov edi, RM_STUB_GET_ADDR
    mov ecx, rm_stub_get_end - rm_stub_get_start
    cld
    rep movsb

    ; Disable paging+protected mode and jump to copied real-mode stub.
    mov eax, [RM_SAVE_CR0]
    and eax, 0x7FFFFFFE
    mov cr0, eax
    db 0xEA
    dw RM_STUB_GET_OFF
    dw RM_STUB_GET_SEG

bits 16
rm_stub_get_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, RM_STACK_TOP

    mov cx, [RM_MODE_WORD]
    mov ax, RM_MODE_INFO_SEG
    mov es, ax
    mov di, RM_MODE_INFO_OFF
    mov ax, 0x4F01
    int 0x10
    mov [RM_RET_AX], ax

    ; Re-enter protected mode and far jump back to kernel code.
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp dword far [RM_PM_PTR]
rm_stub_get_end:

bits 32
pm_return_get:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Restore original CR0 (re-enables paging if it was set).
    mov eax, [RM_SAVE_CR0]
    mov cr0, eax
    jmp short .paging_restored_get

.paging_restored_get:
    mov edi, [RM_OUT_PTR]
    test edi, edi
    jz .skip_copy

    mov esi, RM_MODE_INFO_BUF
    mov ecx, RM_MODE_INFO_SIZE / 4
    cld
    rep movsd

.skip_copy:
    mov esp, [RM_SAVE_ESP]

    popad
    popfd

    xor eax, eax
    mov ax, [RM_RET_AX]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
