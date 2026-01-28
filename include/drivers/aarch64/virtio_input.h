#ifndef EYNOS_DRIVERS_AARCH64_VIRTIO_INPUT_H
#define EYNOS_DRIVERS_AARCH64_VIRTIO_INPUT_H

#include <misc/types.h>

/*
 * Minimal virtio-input keyboard (virtio-mmio transport).
 *
 * Intended for early AArch64 bring-up on QEMU virt with a graphical display.
 * Provides a simple non-blocking ASCII input stream (best-effort mapping).
 */

int virtio_input_init(uint64 dtb_ptr);
int virtio_input_ready(void);
uint64 virtio_input_base(void);

/* Returns 0 on success and writes one ASCII byte to out_c. Nonzero if no data. */
int virtio_input_getc_nonblock(char* out_c);

#endif
