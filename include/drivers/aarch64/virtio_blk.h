#pragma once

#include <misc/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes virtio-blk (virtio-mmio) using the device tree to find the device.
// Returns 0 on success, negative on failure.
int virtio_blk_init(uint64 dtb_ptr);

// 512-byte LBA sector I/O. Returns 0 on success, negative on failure.
int virtio_blk_read_sector(uint32 lba, void* buf512);
int virtio_blk_write_sector(uint32 lba, const void* buf512);

#ifdef __cplusplus
}
#endif
