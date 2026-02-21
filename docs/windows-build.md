## Building EYN-OS on Windows 11 (PowerShell)

This guide explains how to use the `build.ps1` script to compile and run EYN-OS on Windows. The project is designed for a freestanding 32-bit i386 environment, so a cross-toolchain is required.

### 1. Required Tools

Install these components (either via MSYS2, Chocolatey, or Scoop):

| Component | Purpose | Suggested Source |
|-----------|---------|------------------|
| i686-elf-gcc / binutils | Cross compiler + linker | Build manually or MSYS2 packages |
| nasm | Assembler | MSYS2 / choco / scoop |
| grub-mkrescue (or grub2-mkrescue) | ISO creation (Multiboot) | MSYS2 `grub` package |
| qemu-system-i386 | Emulator for testing | MSYS2 / choco / scoop |
| python3 | For doc generation & FS scripts | MSYS2 / python.org / choco |
| mtools (optional) | FAT32 population | MSYS2 `mtools` |
| dosfstools (optional) | mkfs.fat / mkfs.vfat | MSYS2 `dosfstools` |

### 2. MSYS2 Setup (Recommended)

1. Download MSYS2 from https://www.msys2.org/ and install.
2. Open the MSYS2 MinGW32 shell.
3. Update packages:
   ```sh
   pacman -Syu
   # Restart shell if requested
   pacman -Syu
   ```
4. Install required packages:
   ```sh
   pacman -S mingw-w64-i686-binutils mingw-w64-i686-gcc nasm grub qemu python mtools dosfstools
   ```
5. Add the MinGW32 bin directory to your Windows PATH if using PowerShell outside MSYS2 (e.g. `C:\msys64\mingw32\bin`).

### 3. Cross Compiler (If Not Using MSYS2 Packages)

If you need to build an `i686-elf-gcc` toolchain manually:
1. Download source for binutils and gcc.
2. Configure binutils:
   ```sh
   ./configure --target=i686-elf --prefix=/opt/cross --with-sysroot --disable-nls --disable-werror
   make -j
   make install
   ```
3. Configure gcc (C only):
   ```sh
   ./configure --target=i686-elf --prefix=/opt/cross --disable-nls --enable-languages=c --without-headers
   make all-gcc -j
   make all-target-libgcc -j
   make install-gcc
   make install-target-libgcc
   ```
4. Add `/opt/cross/bin` to PATH.

### 4. Running the PowerShell Script

From a PowerShell prompt in the repository root:
```powershell
pwsh -File build.ps1 build      # Build kernel, ISO, filesystem image, docs
pwsh -File build.ps1 run        # Build + run in QEMU (9MB RAM)
pwsh -File build.ps1 qemu-debug # Build + run with debug flags & serial output
pwsh -File build.ps1 qemu-gdb   # Build + run waiting for GDB (:1234)
pwsh -File build.ps1 analyze    # Static analyzer pass (GCC -fanalyzer)
pwsh -File build.ps1 clean      # Remove artifacts
```

Additional filesystem/image targets:
```powershell
pwsh -File build.ps1 eynfsimg            # Create & populate eynfs.img
pwsh -File build.ps1 testimg             # Create testimg.img
pwsh -File build.ps1 test                # Run with testimg as second drive
pwsh -File build.ps1 fat32img            # Create empty FAT32 image (fat32.img)
pwsh -File build.ps1 fat32img-populate   # Populate FAT32 image (requires mtools)
pwsh -File build.ps1 runfat32            # Run with FAT32 as secondary drive
pwsh -File build.ps1 fsck_eynfs          # Run filesystem check (requires python3)
```

### 5. GDB Debugging

For `qemu-gdb` target the VM halts at start. Attach from a separate shell:
```sh
gdb -ex "target remote :1234" -ex "set architecture i386"
```

### 6. Notes / Differences vs Makefile

* EFI stripping step is skipped (loopback mount not straightforward on Windows without admin). ISO from grub-mkrescue is usable in QEMU.
* Image creation uses file length allocation instead of `dd`.
* Static analysis errors are suppressed from console (redirected); adapt as needed.
* Incremental rebuilds are not yet implemented; script recompiles all C units.

### 7. Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Missing multiboot header | Incorrect linker script path | Verify `src/boot/link.ld` exists |
| QEMU fails to start | Tool not in PATH | Ensure MSYS2 MinGW32 bin added to PATH |
| grub-mkrescue not found | GRUB not installed | `pacman -S grub` (MSYS2) |
| i686-elf-gcc not found | Cross toolchain missing | Install packages or build manually |
| Python scripts skipped | python3 missing | Install python3 and ensure on PATH |

### 8. Next Steps / Improvements

Planned optional enhancements:
* Incremental compile (timestamp comparison)
* Parallel compilation using `Start-Job`
* ANSI color toggle for non-Windows terminals
* Artifact hash summary

---
Last updated: $(Get-Date -Format 'yyyy-MM-dd')
