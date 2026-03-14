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
2. Partition + EYNFS format via `INSTALLER_PREPARE_DRIVE`
3. Recursive copy from `RAM:/` to target drive
4. Write `/boot/grub/grub.cfg`
5. Embed GRUB BIOS bootloader to disk

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

Installer ramdisk build now prunes dev-only payload by default:
- Excludes `testdir/code` from RAM media staging.
- Toggle with environment variable:
  - `EYN_INSTALLER_RAMDISK_PRUNE=1` (default): prune dev-only content
  - `EYN_INSTALLER_RAMDISK_PRUNE=0`: include full `testdir/`
