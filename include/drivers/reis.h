/*
 * REIS -- REI Sound format (part of the REI multimedia family).
 *
 * REIS is a simple audio container designed for EYN-OS.  It stores
 * uncompressed or RLE-compressed PCM audio in a compact on-disk format that
 * can be streamed with minimal memory.  The format shares PackBits-style RLE
 * with REI/REIV for implementation reuse.
 *
 * On-disk layout
 * ==============
 *   Offset  Size   Field
 *   ------  ----   -----
 *     0       4    magic       -- 0x52454953 ('REIS')
 *     4       2    version     -- format version (currently 1)
 *     6       1    channels    -- 1 = mono, 2 = stereo
 *     7       1    bits        -- bits per sample (8 or 16)
 *     8       4    sample_rate -- samples per second (e.g. 22050, 44100)
 *    12       4    frame_count -- total sample frames (one frame = one sample per channel)
 *    16       4    data_offset -- byte offset from file start to PCM/compressed payload
 *    20       4    data_size   -- byte length of the payload region
 *    24       4    flags       -- bit 0 = RLE compression (PackBits, 1-byte or 2-byte unit)
 *    28       4    reserved    -- must be 0
 *                               (32 bytes total header)
 *
 * PCM encoding
 * ============
 *   8-bit:  unsigned, 128 = silence
 *   16-bit: signed little-endian, 0 = silence
 *
 * Samples are interleaved: L0 R0 L1 R1 … for stereo.
 * The "sample_size" (bytes per interleaved frame) is channels × (bits / 8).
 *
 * RLE compression (flags & REIS_COMP_RLE)
 * =======================================
 *   PackBits using `sample_size` as the pixel/unit size, identical to
 *   the REI/REIV RLE codec.  Encodes/decodes whole interleaved frames.
 *
 * FS-INVARIANT: The on-disk header is exactly 32 bytes.  Changing the layout
 *               or redefining field semantics is an ABI break.
 */
#ifndef REIS_H
#define REIS_H

#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>

/*
 * ABI-INVARIANT: REIS magic number ('R','E','I','S' → 0x52454953 LE).
 *
 * Endianness: stored as a 32-bit little-endian word.
 * Breakage if changed: all existing .reis files become unrecognisable.
 */
#define REIS_MAGIC 0x52454953u

/*
 * ABI-INVARIANT: Current format version.  Only version 1 is defined.
 * Readers must reject versions they do not understand.
 */
#define REIS_VERSION 1u

/*
 * Compression flags (stored in header.flags, low nibble).
 *
 * REIS_COMP_NONE (0): raw interleaved PCM follows immediately.
 * REIS_COMP_RLE  (1): PackBits-style RLE over whole sample frames.
 *
 * Breakage if redefined: existing compressed files decode incorrectly.
 */
#define REIS_COMP_NONE 0x0u
#define REIS_COMP_RLE  0x1u
#define REIS_COMP_MASK 0x0Fu

/*
 * SECURITY-INVARIANT: Maximum sample rate accepted by the kernel parser.
 *
 * Why: prevents pathological DMA rates that could starve the system.
 * Breakage if raised: higher CPU/DMA load, potential watchdog trips.
 */
#define REIS_MAX_SAMPLE_RATE 48000u

/*
 * SECURITY-INVARIANT: Maximum duration in sample frames the kernel will
 * accept for a single REIS file.  At 48 kHz stereo 16-bit this caps memory
 * at ~180 MB of raw PCM -- well above practical use on EYN-OS.  The field is
 * primarily a sanity check against corrupt headers.
 */
#define REIS_MAX_FRAME_COUNT (48000u * 60u * 30u)  /* 30 minutes at 48 kHz */

/*
 * On-disk header (32 bytes, little-endian, packed).
 *
 * FS-INVARIANT: sizeof(reis_header_t) == 32.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* REIS_MAGIC                                */
    uint16_t version;      /* REIS_VERSION                              */
    uint8_t  channels;     /* 1 = mono, 2 = stereo                     */
    uint8_t  bits;         /* 8 or 16 bits per sample                  */
    uint32_t sample_rate;  /* samples per second (e.g. 22050, 44100)   */
    uint32_t frame_count;  /* total interleaved sample frames           */
    uint32_t data_offset;  /* byte offset to payload from file start    */
    uint32_t data_size;    /* byte length of payload                    */
    uint32_t flags;        /* bit 0 = REIS_COMP_RLE                    */
    uint32_t reserved;     /* must be 0                                 */
} reis_header_t;

_Static_assert(sizeof(reis_header_t) == 32, "REIS header must be exactly 32 bytes");

/*
 * In-memory decoded audio buffer.
 *
 * After parsing and optional RLE decompression, `samples` points to raw
 * interleaved PCM (signed-16 or unsigned-8).
 */
typedef struct {
    reis_header_t header;
    uint8_t*      samples;      /* decoded PCM data   */
    size_t        samples_size; /* byte length of PCM */
} reis_audio_t;

/* ---- API ---- */

/*
 * Parse and validate a REIS header from raw bytes.
 * Returns 0 on success, -1 on invalid/unsupported header.
 */
int reis_read_header(const uint8_t* data, size_t size, reis_header_t* out);

/*
 * Validate an already-populated header struct.
 * Returns 0 if valid, -1 otherwise.
 */
int reis_validate_header(const reis_header_t* header);

/*
 * Parse a complete REIS file (header + payload) in memory.
 * Allocates `audio->samples` via slab/kmalloc.  Caller must call
 * reis_free_audio() when done.
 * Returns 0 on success, -1 on error.
 */
int reis_parse_audio(const uint8_t* data, size_t size, reis_audio_t* audio);

/*
 * Free the sample buffer inside a reis_audio_t.
 */
void reis_free_audio(reis_audio_t* audio);

/*
 * Compute raw PCM byte size from a validated header.
 */
static inline uint32_t reis_pcm_size(const reis_header_t* h) {
    return (uint32_t)h->frame_count * (uint32_t)h->channels * ((uint32_t)h->bits / 8u);
}

/*
 * Return the byte size of one interleaved sample frame.
 */
static inline uint32_t reis_frame_bytes(const reis_header_t* h) {
    return (uint32_t)h->channels * ((uint32_t)h->bits / 8u);
}

#endif /* REIS_H */
