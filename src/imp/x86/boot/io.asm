bits 64
section .text

align 4
dd 0x1BADB002              ; magic
dd 0x00                    ; flags
dd - (0x1BADB002 + 0x00)   ; checksum. m+f+c should be zero

global start1
global keyboard_handler
global read_port
global write_port
global load_idt

extern kernel_main         ; this is defined in the C file
extern keyboard_handler_main

read_port:
    mov edx, [esp + 4]
    in al, dx
    ret

write_port:
    mov edx, [esp + 4]
    mov al, [esp + 4 + 4]
    out dx, al
    ret

load_idt:
    mov edx, [esp + 4]
    lidt [edx]

    ret

load_idt_fail:
    mov al, "F"
    jmp error

keyboard_handler:
    call keyboard_handler_main
    iretd

start1:
    cli                     ; block interrupts
    mov esp, stack_space
    call setup_idt          ; setup the IDT before enabling interrupts
    call load_idt_wrapper   ; load IDT and enable interrupts if ready
    call kernel_main
    hlt                     ; halt the CPU

load_idt_wrapper:
    call load_idt           ; load the IDT
    call check_idt_setup    ; add a function to check IDT setup (for debugging)
    sti                     ; enable interrupts
    ret

setup_idt:
    mov rax, keyboard_handler
    mov word [idt + 0x21*16 + 0], ax                     ; offset low
    mov word [idt + 0x21*16 + 4], 0x08                    ; selector (code segment)
    mov byte [idt + 0x21*16 + 6], 0                       ; zero
    mov byte [idt + 0x21*16 + 7], 0x8E                    ; type_attr (present, DPL=0, 32-bit interrupt gate)
    shr rax, 16
    mov word [idt + 0x21*16 + 8], ax                      ; offset mid
    shr rax, 16
    mov dword [idt + 0x21*16 + 10], eax                   ; offset high
    mov dword [idt + 0x21*16 + 14], 0                     ; zero

    ; loads idt pointer (below)
    mov qword [idtr + 2], idt      ; base address of IDT
    mov word [idtr], 256*16-1      ; limit (size of IDT)
    ret

check_idt_setup:
    ; debugging shit (i hate assembly)
    mov edx, 0xE9       ; port for debug output (example, Bochs VGA display)
    mov rax, [idtr + 2] ; base address of IDT
    out dx, eax         ; send lower 32 bits of rax
    shr rax, 32
    out dx, eax         ; send upper 32 bits of rax
    mov ax, [idtr]      ; limit (size of IDT)
    out dx, ax
    ret
error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte [0xb800a], al
    hlt

section .bss
resb 256*16 ; IDT size
idt:
resb 10 ; IDTR size
idtr:
resb 4096*2 ; 8KB for stack
stack_space:
