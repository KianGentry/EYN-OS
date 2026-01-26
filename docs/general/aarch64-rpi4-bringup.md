# AArch64 Raspberry Pi 4 Bring-up

This document describes the minimal bring-up image used to validate that EYN-OS can boot on a Raspberry Pi 4 in AArch64 mode and print debug output over UART.

## What this is

- A standalone `kernel8.img` that prints a boot banner and the DTB pointer.
- It does not include the full i386 kernel, GRUB/Multiboot, paging, VFS, shell, etc.

## Build

The Makefile provides a dedicated bring-up target:

- `make aarch64`

This target requires an AArch64 cross toolchain. The Makefile attempts to auto-detect common GCC prefixes, but you may need to install packages or override the tool names.

Examples of toolchains:

- Bare-metal: `aarch64-none-elf-gcc` + `aarch64-none-elf-ld`
- Linux cross: `aarch64-linux-gnu-gcc` + `aarch64-linux-gnu-ld`

You can override the detected tools explicitly:

- `make aarch64 AARCH64_CC=aarch64-elf-gcc AARCH64_LD=aarch64-elf-ld AARCH64_OBJCOPY=aarch64-elf-objcopy`

Output:

- `tmp_aarch64_user/boot/kernel8.img`

## Test on a development machine (QEMU)

Raspberry Pi hardware is not identical to QEMU, but the QEMU `virt` machine is the fastest way to iterate on AArch64 bring-up.

Targets:

- `make aarch64-qemu` (build with QEMU platform settings)
- `make aarch64-qemu-run` (build + run under `qemu-system-aarch64` with serial output)
- `make aarch64-qemu-run-gui` (build + run with a visible window and simple framebuffer)

The run target uses:

- `qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a57 -smp 4 -nographic -kernel tmp_aarch64_user/boot/kernel8.elf`

The GUI run target uses:

- `qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a57 -smp 4 -device ramfb -display gtk -serial stdio -kernel tmp_aarch64_user/boot/kernel8.elf`

Note: the QEMU build uses a different linker script/load address than the Raspberry Pi firmware build.

For QEMU, the build also embeds a generated `virt.dtb` into the ELF so the FDT parser can be exercised even when QEMU does not provide a DTB pointer in `x0`.

## Boot on Raspberry Pi 4

1) Create or reuse a Raspberry Pi boot partition (FAT32).
2) Copy `kernel8.img` to the boot partition.
   - Either rename it to `kernel8.img` or set `kernel8=...` in `config.txt`.
3) Add/update `config.txt` with:

```ini
arm_64bit=1
enable_uart=1
core_freq=250
# kernel=kernel8.img   # usually not required; firmware defaults to kernel8.img in 64-bit mode
```

4) Connect a 3.3V UART adapter to the Pi UART pins and open a serial console at `115200 8N1`.

Expected output includes:

- `EYN-OS AArch64 bring-up`
- `DTB @ 0x...`
- `RAM base 0x... size 0x...`

When interrupts are enabled (QEMU bring-up), you should also see periodic timer tick output:

- `tick 0x...`

When running with the GUI target, the same early text is rendered to a simple framebuffer (in addition to serial output).

The bring-up also attempts to discover the GICv2 base addresses from the DTB (instead of hardcoding them). On QEMU you should see:

- `GICD @ 0x... GICC @ 0x...`

The bring-up also extracts the ARMv8 virtual timer IRQ (CNTV) from the DTB and prints it:

- `CNTV IRQ 0x...`

If the DTB uses `interrupts-extended` instead of `interrupts` for the timer node, the bring-up will still resolve the IRQ ID (GIC binding with 3 interrupt cells).

If the kernel hits a synchronous exception during bring-up, it will print basic fault registers over UART:

- `AArch64 SYNC EXCEPTION`
- `ESR_EL1 0x... FAR_EL1 0x...`
- `ELR_EL1 0x... SPSR_EL1 0x...`

SMP bring-up on QEMU uses PSCI and should print one line per secondary core, for example:

- `CPU_ON 0x... rc=0x0`
- `CPU 0x... online`

Each core also enables its own virtual timer; per-CPU ticks are maintained internally (not yet printed).

If the DTB does not expose all CPUs, the QEMU bring-up falls back to a synthetic MPIDR list to start cores 1..3. The PSCI conduit is selected from the DTB `method` property (HVC or SMC); QEMU typically uses HVC.

## Source layout

- Entry: `src/entry/aarch64/start.S` and `src/entry/aarch64/kernel.c`
- Linker script: `src/boot/aarch64-rpi4.ld`
- UART: `src/drivers/aarch64/uart_pl011.c`
