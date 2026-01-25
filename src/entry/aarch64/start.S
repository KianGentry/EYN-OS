// AArch64 Raspberry Pi 4 firmware entry
// Firmware loads kernel8.img at 0x80000 and enters at _start with:
//   x0 = physical address of the device tree blob (DTB)

    .section .text._start, "ax"
    .global _start
    .type _start, %function

_start:
    // Set up a temporary stack.
    adrp x1, __stack_top
    add  x1, x1, :lo12:__stack_top
    mov  sp, x1

    // Preserve DTB pointer for C.
    mov  x19, x0

    // Zero BSS: [__bss_start, __bss_end)
    adrp x0, __bss_start
    add  x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add  x1, x1, :lo12:__bss_end

    // x0 = current, x1 = end
1:
    cmp  x0, x1
    b.hs 2f
    str  xzr, [x0], #8
    b    1b
2:

    // Install exception vectors for EL1.
    adrp x0, aarch64_vectors
    add  x0, x0, :lo12:aarch64_vectors
    msr  vbar_el1, x0
    isb

    // Call kernel_main(dtb_ptr)
    mov  x0, x19
    bl   kernel_main

    // If kernel_main returns, halt.
3:
    wfi
    b 3b

// Secondary CPU entry point (PSCI CPU_ON target).
// x0 is undefined; we read MPIDR_EL1 to select per-CPU stack.
    .global aarch64_secondary_start
    .type aarch64_secondary_start, %function
aarch64_secondary_start:
    // Install exception vectors for EL1.
    adrp x0, aarch64_vectors
    add  x0, x0, :lo12:aarch64_vectors
    msr  vbar_el1, x0
    isb

    // cpu_id = MPIDR_EL1[7:0]
    mrs  x0, mpidr_el1
    and  x0, x0, #0xFF

    // sp = &aarch64_cpu_stacks[cpu_id][AARCH64_CPU_STACK_SIZE]
    adrp x1, aarch64_cpu_stacks
    add  x1, x1, :lo12:aarch64_cpu_stacks
    mov  x2, #4096
    mul  x3, x0, x2
    add  x1, x1, x3
    add  x1, x1, x2
    mov  sp, x1

    // Call C secondary entry.
    bl   aarch64_secondary_entry

4:
    wfi
    b 4b

    .size _start, . - _start
