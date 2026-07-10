# GRUB EYNFS Module

This document describes the EYNFS filesystem module added for custom GRUB builds.

## Goal
Allow GRUB to load files directly from EYNFS so installed systems can keep:
- `/boot/kernel.bin`
- `/boot/grub/grub.cfg`
on an EYNFS partition.

## Source
- Module source: `grub-module-src/eynfs.c`
- Build helper: `devtools/grub-module/build_eynfs_module.sh`

## Implementation Notes
- Module name: `eynfs`
- Read-only filesystem driver implementing GRUB `fs` hooks (`dir/open/read/close`)
- Recognizes EYNFS magic `0x45594E46` and block size `512`
- Directory model:
  - first 4 bytes of each directory block = next directory block
  - entries follow as packed EYNFS dir entries
- File model:
  - first 4 bytes of each file data block = next file data block
  - payload bytes start at offset 4 (508 bytes per block)

## Superblock Location
The module checks in this order:
1. Sector `0` (partition-style EYNFS, expected for installed disks)
2. Sector `2048` (legacy full-disk fallback)

## Build Flow
From GRUB source tree:
1. Copy module source to `grub-core/fs/eynfs.c`
2. Register module in `grub-core/Makefile.core.def`
3. Reconfigure and build GRUB

The helper script automates those steps.

## Security / Robustness
- Read-only parser to minimize mutation risk in bootloader context
- Rejects non-matching magic or unsupported block size
- Returns standard GRUB filesystem errors on invalid traversal

## Next Installer Integration Steps
1. Create installer ISO payload that includes EYNFS-targeted `/boot/grub/grub.cfg`
2. Installer formats target partition as EYNFS and copies OS payload
3. Installer writes custom GRUB stage files/MBR using built GRUB artifacts with `eynfs` module
