#ifndef VIEW_BACKEND_PROTOCOL_H
#define VIEW_BACKEND_PROTOCOL_H

#include <stdint.h>

/*
 * Backend-to-view frame transport protocol (phase 1).
 * Backends write this header followed by raw pixel data.
 */
#define VIEW_FRAME_MAGIC 0x31524656u /* 'VFR1' little-endian */
#define VIEW_FRAME_FMT_RGB565 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t reserved;
    uint32_t data_bytes;
} view_frame_header_t;

#endif
