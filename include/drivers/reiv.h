#ifndef REIV_H
#define REIV_H

#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>

// REIV magic number ('REV\0')
#define REIV_MAGIC 0x52455600

// REIV versions
#define REIV_VERSION_V1 1
#define REIV_VERSION_V2 2
#define REIV_VERSION_V3 3
#define REIV_VERSION REIV_VERSION_V3

// Pixel formats
#define REIV_PIXFMT_RGB565LE 1

// Flags
#define REIV_FLAG_LOOP_DEFAULT 0x01
#define REIV_FLAG_LOOP_LOCKED  0x02
#define REIV_FLAG_RESERVED     0xFC

// Maximum dimensions
#define REIV_MAX_WIDTH 640
#define REIV_MAX_HEIGHT 480

// On-disk header for REIV container.
//
// Version 1:
//   Frames are stored as fixed-size RGB565LE images starting at frames_offset.
//   frame i lives at: frames_offset + i * (width*height*2)
//
// Version 2:
//   frames_offset points to a frame index table of `frame_count` entries.
//   Data begins immediately after the index.
typedef struct {
    uint32_t magic;         // REIV_MAGIC
    uint16_t width;
    uint16_t height;
    uint8_t pixfmt;         // REIV_PIXFMT_*
    uint8_t flags;          // REIV_FLAG_*
    uint16_t version;       // REIV_VERSION
    uint32_t frame_count;   // number of frames
    uint32_t fps_num;       // frames per second = fps_num / fps_den
    uint32_t fps_den;
    uint32_t frames_offset; // byte offset to frame 0
} reiv_header_t;

// v2 frame index entry.
// `offset` is relative to the start of the frame data region (immediately after the index).
// `size` is the byte length of the stored frame payload.
// If `flags & REIV_FRAME_FLAG_RLE565`, the payload is PackBits-style RLE over RGB565 pixels.
typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
} reiv_frame_entry_t;

#define REIV_FRAME_FLAG_RLE565 0x00000001u
#define REIV_FRAME_FLAG_RLE8   0x00000002u
#define REIV_FRAME_FLAG_DELTA_XOR_PREV 0x00000004u

int reiv_read_header(const uint8_t* data, size_t size, reiv_header_t* header);
int reiv_validate_header(const reiv_header_t* header);
uint32_t reiv_frame_size_bytes(const reiv_header_t* header);
uint32_t reiv_frame_offset_bytes(const reiv_header_t* header, uint32_t frame_index);

uint32_t reiv_index_size_bytes(const reiv_header_t* header);
uint32_t reiv_data_offset_bytes(const reiv_header_t* header);

#endif // REIV_H
