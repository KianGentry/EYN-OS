# GRUB EYNFS Module

This directory contains helper notes/scripts for integrating the EYNFS filesystem driver into a custom GRUB build.

## Files
- `../../grub-module-src/eynfs.c`: GRUB filesystem module implementation (read-only).
- `build_eynfs_module.sh`: convenience wrapper to copy/register/build the module inside a GRUB source tree.

## What This Enables
- GRUB can read `/boot/kernel.bin` and `/boot/grub/grub.cfg` directly from an EYNFS partition.
- Required groundwork for in-guest installer flow that writes EYNFS target disks and installs GRUB.

## Constraints (Current Module)
- Read-only module.
- Assumes EYNFS block size is 512 bytes.
- Supports partition-style layout (superblock at sector 0 of the partition) and legacy full-disk fallback (superblock at absolute LBA 2048).
