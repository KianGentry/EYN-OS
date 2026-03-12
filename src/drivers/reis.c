/*
 * REIS audio format parser — kernel side.
 *
 * Handles header validation and optional PackBits RLE decompression.
 * The decompression reuses the same PackBits algorithm as REI and REIV.
 *
 * Memory ownership: reis_parse_audio() allocates the sample buffer via
 * kmalloc().  The caller must call reis_free_audio() to release it.
 */

#include <drivers/reis.h>
#include <util.h>
#include <stdint.h>
#include <stddef.h>

/* ---- Freestanding helpers ---- */

static void reis_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

static void reis_memset(void* dst, uint8_t val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; ++i) d[i] = val;
}

/* ---- PackBits RLE decoder (shared codec with REI/REIV) ---- */

static int rle_decode_packbits(const uint8_t* in, size_t in_len,
                               uint8_t* out, size_t out_len,
                               int pixel_size) {
    size_t ip = 0;
    size_t op = 0;
    if (!in || !out || pixel_size <= 0) return -1;

    while (ip < in_len && op < out_len) {
        int8_t n = (int8_t)in[ip++];
        if (n >= 0) {
            /* Literal run: n + 1 units. */
            size_t count = (size_t)n + 1u;
            size_t bytes = count * (size_t)pixel_size;
            if (ip + bytes > in_len) return -1;
            if (op + bytes > out_len) return -1;
            reis_memcpy(out + op, in + ip, bytes);
            ip += bytes;
            op += bytes;
        } else if (n != -128) {
            /* Replicate run: 1 - n copies of next unit. */
            size_t count = (size_t)(1 - n);
            size_t bytes = count * (size_t)pixel_size;
            if (ip + (size_t)pixel_size > in_len) return -1;
            if (op + bytes > out_len) return -1;
            const uint8_t* px = in + ip;
            ip += (size_t)pixel_size;
            for (size_t i = 0; i < count; ++i) {
                reis_memcpy(out + op, px, (size_t)pixel_size);
                op += (size_t)pixel_size;
            }
        }
        /* n == -128 is a no-op (padding byte). */
    }
    return (op == out_len) ? 0 : -1;
}

/* ---- Public API ---- */

int reis_read_header(const uint8_t* data, size_t size, reis_header_t* out) {
    if (!data || !out) return -1;
    if (size < sizeof(reis_header_t)) return -1;

    reis_memcpy(out, data, sizeof(reis_header_t));
    return reis_validate_header(out);
}

int reis_validate_header(const reis_header_t* header) {
    if (!header) return -1;
    if (header->magic != REIS_MAGIC) return -1;
    if (header->version != REIS_VERSION) return -1;
    if (header->channels != 1 && header->channels != 2) return -1;
    if (header->bits != 8 && header->bits != 16) return -1;
    if (header->sample_rate == 0 || header->sample_rate > REIS_MAX_SAMPLE_RATE) return -1;
    if (header->frame_count == 0 || header->frame_count > REIS_MAX_FRAME_COUNT) return -1;
    if (header->data_offset < sizeof(reis_header_t)) return -1;
    if (header->data_size == 0) return -1;
    if ((header->flags & REIS_COMP_MASK) > REIS_COMP_RLE) return -1;
    return 0;
}

int reis_parse_audio(const uint8_t* data, size_t size, reis_audio_t* audio) {
    if (!data || !audio) return -1;

    reis_header_t hdr;
    if (reis_read_header(data, size, &hdr) != 0) return -1;

    /* Validate that the payload fits within the provided data. */
    if ((size_t)hdr.data_offset + (size_t)hdr.data_size > size) return -1;

    uint32_t pcm_bytes = reis_pcm_size(&hdr);
    if (pcm_bytes == 0) return -1;

    audio->samples = (uint8_t*)malloc(pcm_bytes);
    if (!audio->samples) return -1;
    audio->samples_size = pcm_bytes;
    audio->header = hdr;

    const uint8_t* payload = data + hdr.data_offset;
    uint8_t comp = (uint8_t)(hdr.flags & REIS_COMP_MASK);

    if (comp == REIS_COMP_NONE) {
        if (hdr.data_size < pcm_bytes) {
            free(audio->samples);
            audio->samples = NULL;
            return -1;
        }
        reis_memcpy(audio->samples, payload, pcm_bytes);
    } else if (comp == REIS_COMP_RLE) {
        int unit = (int)reis_frame_bytes(&hdr);
        if (rle_decode_packbits(payload, hdr.data_size,
                                audio->samples, pcm_bytes, unit) != 0) {
            free(audio->samples);
            audio->samples = NULL;
            return -1;
        }
    } else {
        free(audio->samples);
        audio->samples = NULL;
        return -1;
    }

    return 0;
}

void reis_free_audio(reis_audio_t* audio) {
    if (!audio) return;
    if (audio->samples) {
        free(audio->samples);
        audio->samples = NULL;
    }
    audio->samples_size = 0;
    reis_memset(&audio->header, 0, sizeof(audio->header));
}
