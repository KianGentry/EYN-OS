ENTRY(_start)

SECTIONS
{
  /* User-space base address expected by the kernel loader */
  . = 0x00400000;

  .text : ALIGN(0x1000)
  {
    *(.text*)
  }

  .rodata : ALIGN(0x1000)
  {
    *(.rodata*)
    *(.rdata*)
  }

  .data : ALIGN(0x1000)
  {
    *(.data*)
    *(.sdata*)
    *(.got*)
  }

  .bss : ALIGN(0x1000)
  {
    *(COMMON)
    *(.bss*)
    *(.sbss*)
  }

  /* Keep the image simple for the minimal kernel ELF loader. */
  /DISCARD/ :
  {
    *(.note.gnu.property)
    *(.note*)
    *(.comment)
    *(.eh_frame*)
    *(.eh_frame_hdr*)
  }
}
