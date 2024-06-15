section .asmheader1
header_start:
    dd 0xe85250d6 ; multiboot 2 specification
    dd 0 ; i386 protected
    dd header_end - header_start ; length
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    dw 0 ; no more shit to run
    dw 0
    dd 8
header_end:
