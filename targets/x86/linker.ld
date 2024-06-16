OUTPUT_FORMAT(elf32-i386)
ENTRY(start)

SECTIONS
{
    . = 1M;

    .boot :
    {
        KEEP(*(.asmheader1))
    }

    .text :
    {
        *(.text)
    }

    .data : 
    {
        *(.data) 
    }

    .bss  : 
    {
        *(.bss)  
    }
}
