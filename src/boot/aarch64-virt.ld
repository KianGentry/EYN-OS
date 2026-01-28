/*
 * AArch64 QEMU 'virt' linker script
 *
 * QEMU virt machine RAM typically starts at 0x40000000.
 * A conventional load address for bare-metal kernels is 0x40080000.
 */

ENTRY(_start)

SECTIONS
{
  . = 0x40080000;

  __kernel_start = .;

  .text : ALIGN(16)
  {
    KEEP(*(.text._start))
    *(.text .text.*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata .rodata.*)

    /* Keep registered shell command metadata in read-only memory.
       The shell iterates this region using these symbols. */
    . = ALIGN(8);
    __start_shellcmds = .;
    KEEP(*(.shellcmds))
    __stop_shellcmds = .;
  }

  .data : ALIGN(16)
  {
    *(.data .data.*)
  }

  .bss : ALIGN(16)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Simple static stack for early bring-up */
  . = ALIGN(16);
  __stack_bottom = .;
  . = . + 0x10000; /* 64 KiB */
  __stack_top = .;

  __kernel_end = .;
}
