#ifndef EYNOS_HAL_BLOCK_H
#define EYNOS_HAL_BLOCK_H

#include <misc/types.h>

/*
 * HAL Block Devices
 *
 * Purpose:
 *  Provide a uniform block I/O surface so the VFS and filesystem drivers can
 *  be transport-independent (ATA, virtio-blk, NVMe, etc.).
 *
 * Contract:
 *  - Sector size is assumed to be 512 bytes for now.
 *  - Returns 0 on success, negative on error.
 */

#define HAL_BLOCK_SECTOR_SIZE 512u

/* Read/write one or more 512-byte sectors. */
int hal_block_read(uint32 dev, uint64 lba, void* buf, uint32 sector_count);
int hal_block_write(uint32 dev, uint64 lba, const void* buf, uint32 sector_count);

/* Flush any write-back caches (safe no-op if unsupported). */
int hal_block_flush(uint32 dev);

#endif
