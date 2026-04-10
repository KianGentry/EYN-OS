# Installer Runtime Foundation

This document captures the kernel/runtime changes that enable the in-guest installer workflow.

## `RAM:/` Read-Only Drive

A reserved VFS drive ID (`VFS_DRIVE_RAM = 0xFE`) is now exposed as `RAM:/`.

Behavior:
- Backed by a multiboot module loaded by GRUB.
- The module selection prefers one whose multiboot command line contains `ramdisk` (case-insensitive).
- If no tagged module exists, the first module is used.
- EYNFS is mounted read-only from the in-memory image.
- Supports:
  - `vfs_detect`
  - `vfs_stat`
  - `vfs_get_file_size`
  - `vfs_read_file`
  - `vfs_read_file_at`
  - `vfs_listdir`
- Mutating operations (`write/mkdir/rmdir/unlink`) are rejected for `RAM:/`.

Path support in syscall layer:
- `RAM:/path` is accepted in `open` and `chdir`.
- Numeric explicit drives `N:/path` are also parsed in `open` and `chdir`.

## Installer Disk Syscalls

New syscall numbers:
- `133`: `INSTALLER_PREPARE_DRIVE`
- `134`: `INSTALLER_FORMAT_EYNFS_PARTITION`
- `135`: `INSTALLER_WRITE_SECTOR`
- `136`: `INSTALLER_GET_PARTITIONS`

Capabilities required:
- `INSTALLER_PREPARE_DRIVE`: `CAP_DEV_DISK | CAP_WRITE_FS`
- `INSTALLER_FORMAT_EYNFS_PARTITION`: `CAP_DEV_DISK | CAP_WRITE_FS`
- `INSTALLER_WRITE_SECTOR`: `CAP_DEV_DISK | CAP_WRITE_FS`
- `INSTALLER_GET_PARTITIONS`: `CAP_DEV_DISK | CAP_READ_FS`

Summary:
- `INSTALLER_PREPARE_DRIVE(logical_drive)`:
  - Creates a single bootable EYNFS partition starting at LBA 2048.
  - Uses the remainder of the disk.
  - Formats that partition as EYNFS.
- `INSTALLER_FORMAT_EYNFS_PARTITION(logical_drive, partition_num)`:
  - Formats an existing partition (1..4) as EYNFS.
- `INSTALLER_WRITE_SECTOR(logical_drive, lba, sector512)`:
  - Writes one raw 512-byte sector.
  - Intended for installer boot code / MBR staging.
- `INSTALLER_GET_PARTITIONS(logical_drive, out)`:
  - Returns a compact partition table payload for installer UI display.

## Userland ABI

Userland wrappers and structs were added in:
- `userland/include/eynos_syscall.h`

New types:
- `eyn_installer_partition_t`
- `eyn_installer_partitions_t`

New wrappers:
- `eyn_sys_installer_prepare_drive`
- `eyn_sys_installer_format_eynfs_partition`
- `eyn_sys_installer_write_sector`
- `eyn_sys_installer_get_partitions`

## Boot-Media Integration

`make build` now generates installer boot media with a RAM module:
- Builds userland installer binary: `testdir/binaries/installer`
- Builds userland install binary: `testdir/binaries/install`
- Builds RAM EYNFS image: `tmp_user/boot/installer_ramdisk.img`
- Adds GRUB module line in ISO config:
  - `module /boot/installer_ramdisk.img ramdisk`

The shell auto-launches installer on boot when this file exists in RAM media:
- `RAM:/binaries/installer`

Auto-start policy:
- RAM installer auto-start is now fallback-only.
- If any EYNFS disk already contains `/binaries/installer`, boot stays on disk
  flow and RAM installer is not auto-launched.

## Installer Program

Source:
- `testdir/code/installer_uelf.c`

Current flow:
1. GUI drive selection
2. GUI install-options screen (menuconfig-like toggles):
  - Select exact base packages from `base.manifest`
  - Toggle icon payload (`/icons`, `/icons16`)
  - Toggle font payload and choose per-font files from `RAM:/fonts`
  - Toggle optional compiler payload (`/binaries/chibicc` from `RAM:/programs/chibicc`)
3. Partition + EYNFS format via `INSTALLER_PREPARE_DRIVE`
4. Run package workflow (requires `RAM:/installer/base.manifest`):
  - Seed `/binaries/install`, `/binaries/extract`, `/binaries/installer`, and `/boot/kernel.bin`
  - Seed required system content from RAM media:
    - `/etc/resolv.conf`
    - `/config/*`
    - `/.view/*`
    - Optional by installer selection: `/icons/*`, `/icons16/*`, `/fonts/*`, `/binaries/chibicc`
   - Seed cache files from RAM media:
     - `/cache/index.json`
     - `/cache/base.pkg`
     - `/cache/base.manifest`
    - Optional per-package fallback cache: `RAM:/installer/pkg/<package>.pkg`
   - Install packages listed in manifest using the in-process install engine
     (same resolver/package logic as `install`, without per-package spawn/wait)
    - Installer runs package installs in auto-confirm mode for destination prompts
      (non-interactive install path)
5. Write `/boot/grub/grub.cfg`
6. Embed GRUB BIOS bootloader to disk

Installer UX progress reporting:
- During install phases, the GUI now repaints continuously from the worker path
  (no "frozen" look while copy/write is in progress).
- A progress bar is shown during package install and bootloader stages.
- Live status text below the bar shows the current file/directory/action.

Note:
- Installer bootloader install now writes:
  - MBR boot code from `RAM:/installer/grub/boot.img`
  - Embedded GRUB core image from `RAM:/installer/grub/core.img` to LBA 1..N
- Installer also writes `RAM:/boot/kernel.bin` as a raw contiguous blob at
  fixed LBA (`1024`), and the embedded GRUB config boots it via blocklist
  (`multiboot (hd0)1024+<sectors>`).
- The partition table is rebuilt from installer partition metadata so MBR boot
  code installation does not erase partition entries.
- `core.img` is generated during host build using `grub-mkimage` and
  packed into installer RAM media.

## RAM Media Size

Installer ramdisk build now always stages a minimal package-installation medium:
- `RAM:/binaries/install`
- `RAM:/binaries/extract`
- `RAM:/binaries/installer`
- `RAM:/etc/resolv.conf`
- `RAM:/config/*`
- `RAM:/.view/*`
- `RAM:/icons/*`
- `RAM:/icons16/*`
- `RAM:/fonts/*` (optional)
- `RAM:/programs/chibicc` (optional)
- `RAM:/installer/index.json`
- `RAM:/installer/base.manifest`
- `RAM:/installer/base.pkg`
- `RAM:/installer/pkg/<package>.pkg`
- `RAM:/boot/kernel.bin`

Installer ramdisk image sizing is now dynamic:
- The builder sizes EYNFS and swap partitions from staged installer content,
  instead of always creating a fixed 28MB module.
- Default low-RAM profile is small swap and conservative EYNFS headroom.

Size tuning environment variables:
- `EYN_INSTALLER_RAMDISK_HEADROOM` (default `0.10`): metadata/expansion
  multiplier used when picking EYNFS partition size.
- `EYN_INSTALLER_RAMDISK_MIN_EYNFS_SECTORS` (default `6144`, ~3MB): lower
  bound for EYNFS partition size.
- `EYN_INSTALLER_RAMDISK_SWAP_MB` (default `0`): swap partition size in MB.
- `EYN_INSTALLER_RAMDISK_PART1_START_SECTOR` (default `1`): partition start
  LBA in the installer ramdisk image (reduces module file bytes vs 2048).

Build integration note:
- Installer ramdisk build explicitly disables `copy_testdir_to_eynfs.py`
  repo-level auto-injection of `/fonts` and `/include` (`EYNFS_COPY_FONTS=0`,
  `EYNFS_COPY_HEADERS=0`) to keep module size bounded.

## Host Build Menu Configuration

EYN-OS host builds now support a Linux-style text configuration screen:

- `make menuconfig`

This writes `.eynosconfig` at the repository root. The Makefile auto-loads
that file and applies options to both kernel build and installer ramdisk build.

Current keys:
- `ARCH`: target architecture (`i386` or `amd64`)

Note:
- Legacy menuconfig installer payload toggles may still appear in some local
  trees, but they are ignored by the current package-manifest-only installer
  media pipeline.

Package workflow media (default and only mode):
- Carries a minimal runtime package medium:
  - `RAM:/binaries/install`
  - `RAM:/binaries/extract`
  - `RAM:/binaries/installer`
  - `RAM:/installer/index.json`
  - `RAM:/installer/base.manifest`
  - `RAM:/installer/base.pkg`
  - `RAM:/installer/pkg/<package>.pkg`
  - `RAM:/boot/kernel.bin`
- `install` first seeds `/cache/pkg/<package>.pkg` directly from
  `RAM:/installer/pkg` for each requested package.
- In installer builds, package extraction uses the embedded extractor path
  (no per-package child spawn), preventing scheduler stalls between packages.
- When `RAM:/installer/pkg` is present (installer media), `install` does not
  unpack `base.pkg`; it uses per-package media cache and then network fetch.
- `base.pkg` remains a compatibility fallback only for environments that do not
  provide `RAM:/installer/pkg`.

