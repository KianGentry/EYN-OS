# Build Requirements (Host Packages)

This repo builds EYN-OS from a typical Linux host using the `Makefile` targets `build` / `run`.

The build pipeline uses:
- Toolchain: `gcc`, `ld`, `objcopy`, `ar`, `make`, `nasm`
- ISO creation: `grub2-mkrescue` (or `grub-mkrescue`) + `xorriso`
- Image creation: `python3` (EYNFS image generator + population scripts)
- Emulation (optional): `qemu-system-i386` / `qemu-system-aarch64`

## Notes (important)

- **x86 kernel build is 32-bit (`-m32`)**. On many x86_64 hosts you must install 32-bit multilib support, otherwise `gcc -m32` will fail.
- `make build` creates `EYNOS.iso`, generates docs, and creates `eynfs.img`.
- `make run` / `make qemu-gdb` additionally require QEMU.
- AArch64 targets (`make aarch64*`) require an AArch64 cross toolchain *and* `qemu-system-aarch64` when using `AARCH64_PLATFORM=qemu-virt`.

---

## openSUSE Tumbleweed (zypper)

### Required for `make build` (x86)

```sh
sudo zypper in -y \
  make gcc binutils \
  nasm \
  python3 \
  xorriso \
  grub2-common grub2-i386-pc
```

### Required on x86_64 hosts for `gcc -m32`

```sh
sudo zypper in -y gcc-32bit glibc-devel-32bit
```

### Optional (run/debug)

```sh
sudo zypper in -y \
  qemu-x86 \
  gdb
```

### Optional (AArch64 bring-up under QEMU)

```sh
sudo zypper in -y \
  qemu-arm \
  cross-aarch64-binutils cross-aarch64-gcc15
```

openSUSE’s cross toolchain binaries are typically named with the `aarch64-suse-linux-*` triplet, so the Makefile’s auto-detect (which looks for `aarch64-none-elf-*`, `aarch64-elf-*`, or `aarch64-linux-gnu-*`) may not find them automatically.

If `make aarch64-*` fails with “No cross-compiler found”, locate the installed toolchain and pass it explicitly:

```sh
ls -1 /usr/bin/aarch64*gcc /usr/bin/aarch64*ld /usr/bin/aarch64*objcopy 2>/dev/null

make aarch64-full-qemu-run-gui \
  AARCH64_CC=aarch64-suse-linux-gcc \
  AARCH64_LD=aarch64-suse-linux-ld \
  AARCH64_OBJCOPY=aarch64-suse-linux-objcopy
```

(If your binaries have a different prefix, use the names printed by the `ls` command.)

### Optional (FAT32 image targets)

```sh
sudo zypper in -y dosfstools mtools
```

### Optional (REI/REIV conversion tooling in `devtools/png_to_rei.py`)

```sh
sudo zypper in -y ffmpeg python313-Pillow
```

(Use the `python3XY-Pillow` matching your installed Python; Tumbleweed often offers `python311-*`, `python312-*`, `python313-*`.)

---

## Fedora (dnf)

### Required for `make build` (x86)

```sh
sudo dnf install -y \
  make gcc binutils \
  nasm \
  python3 \
  grub2-tools xorriso
```

### Required on x86_64 hosts for `gcc -m32`

```sh
sudo dnf install -y gcc glibc-devel.i686 libgcc.i686
```

### Optional (run/debug)

```sh
sudo dnf install -y qemu-system-x86 gdb
```

### Optional (AArch64 bring-up)

```sh
sudo dnf install -y qemu-system-aarch64
# plus an AArch64 cross toolchain providing: aarch64-none-elf-gcc (preferred) or aarch64-linux-gnu-gcc
```

### Optional (FAT32 image targets)

```sh
sudo dnf install -y dosfstools mtools
```

---

## Debian / Ubuntu (apt)

### Required for `make build` (x86)

```sh
sudo apt update
sudo apt install -y \
  build-essential \
  nasm \
  python3 \
  grub-pc-bin xorriso
```

### Required on x86_64 hosts for `gcc -m32`

```sh
sudo apt install -y gcc-multilib
```

### Optional (run/debug)

```sh
sudo apt install -y qemu-system-x86 gdb
```

### Optional (AArch64 bring-up)

```sh
sudo apt install -y qemu-system-arm
sudo apt install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

### Optional (FAT32 image targets)

```sh
sudo apt install -y dosfstools mtools
```

---

## Arch Linux (pacman)

### Required for `make build` (x86)

```sh
sudo pacman -Syu --needed \
  base-devel \
  nasm \
  python \
  grub \
  xorriso
```

### Required on x86_64 hosts for `gcc -m32`

```sh
sudo pacman -S --needed gcc-multilib
```

### Optional (run/debug)

```sh
sudo pacman -S --needed qemu-system-x86 gdb
```

### Optional (AArch64 bring-up)

```sh
sudo pacman -S --needed qemu-system-aarch64 aarch64-linux-gnu-gcc aarch64-linux-gnu-binutils
```

### Optional (FAT32 image targets)

```sh
sudo pacman -S --needed dosfstools mtools
```

---

## Windows (MSYS2 / pacman)

If you build via MSYS2, the Windows guide in `docs/windows-build.md` recommends:

```sh
pacman -S mingw-w64-i686-binutils mingw-w64-i686-gcc nasm grub qemu python mtools dosfstools
```

---

## Quick sanity check (any distro)

These commands should exist for a full `make build`:

- `gcc`, `ld`, `objcopy`, `ar`, `make`, `nasm`
- `python3`
- `grub2-mkrescue` (or `grub-mkrescue`)
- `xorriso`

And if you want `make run`:

- `qemu-system-i386`
