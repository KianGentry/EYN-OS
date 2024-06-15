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
}
