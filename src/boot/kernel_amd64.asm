MBOOT_HEADER_MAGIC equ 0x1BADB002
MBOOT_PAGE_ALIGN equ 1 << 0
MBOOT_MEM_INFO equ 1 << 1
MBOOT_GRAPH_MODE equ 1 << 2
MBOOT_HEADER_FLAGS equ MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO | MBOOT_GRAPH_MODE
MBOOT_CHECKSUM equ -(MBOOT_HEADER_MAGIC + MBOOT_HEADER_FLAGS)

bits 32

section .text
        align 32
        dd MBOOT_HEADER_MAGIC
        dd MBOOT_HEADER_FLAGS
        dd MBOOT_CHECKSUM

        dd 0
        dd 0
        dd 0
        dd 0
        dd 0

        dd 0
        dd 640
        dd 480
        dd 32

global start
extern kmain

global stack_space
global stack_bottom
global isr_abort_stack_top
global isr_abort_stack_bottom
global Shutdown
global pd_table3

start:
        cli

        mov [mb_magic], eax
        mov [mb_info], ebx

        mov esp, stack_space
        and esp, 0xFFFFFFF0

        mov edi, pml4_table
        mov ecx, (4096 * 6) / 4
        xor eax, eax
        rep stosd

        mov eax, pdpt_table
        or eax, 0x7
        mov [pml4_table + 0], eax
        mov dword [pml4_table + 4], 0

        mov eax, pd_table0
        or eax, 0x7
        mov [pdpt_table + 0], eax
        mov dword [pdpt_table + 4], 0

        mov eax, pd_table1
        or eax, 0x7
        mov [pdpt_table + 8], eax
        mov dword [pdpt_table + 12], 0

        mov eax, pd_table2
        or eax, 0x7
        mov [pdpt_table + 16], eax
        mov dword [pdpt_table + 20], 0

        mov eax, pd_table3
        or eax, 0x3
        mov [pdpt_table + 24], eax
        mov dword [pdpt_table + 28], 0

        mov ebx, 0
.map_pd_loop:
        mov edi, pd_table0
        mov edx, ebx
        shl edx, 12
        add edi, edx
        cmp ebx, 3
        jne .map_pd_identity_base
        mov eax, 0x83
        jmp .map_pd_base_ready
.map_pd_identity_base:
        mov eax, ebx
        shl eax, 30
        ; SECURITY-INVARIANT: Keep 0xC0000000+ alias supervisor-only (ebx==3),
        ; but mark lower identity map user-accessible during amd64 bring-up so
        ; ring3 ELFs can execute before full amd64 VMM page-table ownership lands.
        or eax, 0x87
.map_pd_base_ready:
        mov ecx, 512
.map_2m_pages:
        mov [edi + 0], eax
        mov dword [edi + 4], 0
        add eax, 0x200000
        add edi, 8
        loop .map_2m_pages

        inc ebx
        cmp ebx, 4
        jne .map_pd_loop

        lgdt [gdt64_ptr]

        mov eax, cr4
        or eax, (1 << 5)
        mov cr4, eax

        mov ecx, 0xC0000080
        rdmsr
        or eax, (1 << 8)
        wrmsr

        mov eax, pml4_table
        mov cr3, eax

        mov eax, cr0
        or eax, 0x80000001
        mov cr0, eax

        jmp 0x08:long_mode_start

bits 64
long_mode_start:
        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax

        ; Enable x87/SSE before calling C code compiled for amd64.
        ; GCC can emit XMM instructions in prologues/memcpy paths very early.
        mov rax, cr0
        and rax, ~(1 << 2)     ; CR0.EM = 0
        or rax, (1 << 1)       ; CR0.MP = 1
        mov cr0, rax

        mov rax, cr4
        or rax, (1 << 9) | (1 << 10) ; OSFXSR | OSXMMEXCPT
        mov cr4, rax

        mov rsp, stack_space
        and rsp, -16
        ; ABI-INVARIANT: SysV amd64 requires caller RSP to be 16-byte aligned
        ; before call so the callee sees RSP%16==8 after the return address push.
        ; Violating this causes aligned XMM stack stores (movaps) to fault.

        mov edi, dword [rel mb_magic]
        mov esi, dword [rel mb_info]

        xor ebp, ebp
        call kmain

.halt:
        hlt
        jmp .halt

Shutdown:
        cli
.shutdown_halt:
        hlt
        jmp .shutdown_halt

section .data
align 16
gdt64:
        dq 0x0000000000000000
        dq 0x00AF9A000000FFFF
        dq 0x00AF92000000FFFF
gdt64_end:

gdt64_ptr:
        dw gdt64_end - gdt64 - 1
        dq gdt64

section .bss
alignb 4096
pml4_table:
        resq 512
pdpt_table:
        resq 512
pd_table0:
        resq 512
pd_table1:
        resq 512
pd_table2:
        resq 512
pd_table3:
        resq 512

mb_magic:
        resd 1
mb_info:
        resd 1

alignb 16
stack_bottom:
        resb 32768
stack_space:

alignb 16
isr_abort_stack_bottom:
        resb 8192
isr_abort_stack_top: