OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

SECTIONS
{
    /*
     * ABI-INVARIANT: Keep the initial load address aligned with the current
     * BIOS+GRUB bootstrap expectations during early amd64 bring-up.
     */
    . = 0x100000;

    __kernel_start = .;
    .text :
    {
        __kernel_text_start = .;
        *(.text)
        __kernel_text_end = .;
    }

    .rodata :
    {
        __kernel_rodata_start = .;
        *(.rodata*)

        . = ALIGN(8);
        __start_shellcmds = .;
        KEEP(*(.shellcmds))
        __stop_shellcmds = .;

        __kernel_rodata_end = .;
    }

    .data :
    {
        *(.data)
        *(.data.*)
    }

    .bss :
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __kernel_end = .;
}
