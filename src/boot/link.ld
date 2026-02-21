OUTPUT_FORMAT(elf32-i386)
ENTRY(start)

SECTIONS
{
    /* Kernel is loaded at 1MB by GRUB/Multiboot */
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

        /* Keep registered shell command metadata in read-only memory.
           help_tui and the shell iterate this region using these symbols. */
        . = ALIGN(4);
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

    .bss  : 
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __kernel_end = .;
}
