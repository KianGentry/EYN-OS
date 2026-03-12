#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Open an REI image, REIV video, REIS audio, or WAV audio viewer.", "view /images/picture.rei");

#define REI_MAGIC 0x52454900u
#define REI_DEPTH_MONO 1u
#define REI_DEPTH_RGB 3u
#define REI_DEPTH_RGBA 4u
#define REI_COMP_NONE 0x0u
#define REI_COMP_RLE  0x1u
#define REI_COMP_MASK 0x0Fu

#define REIV_MAGIC 0x52455600u
#define REIV_VERSION_V1 1u
#define REIV_VERSION_V2 2u
#define REIV_VERSION_V3 3u
#define REIV_PIXFMT_RGB565LE 1u
#define REIV_FLAG_LOOP_DEFAULT 0x01u

#define REIV_FRAME_FLAG_RLE565 0x00000001u
#define REIV_FRAME_FLAG_RLE8 0x00000002u
#define REIV_FRAME_FLAG_DELTA_XOR_PREV 0x00000004u

/* ---- REIS (audio) constants ---- */
#define REIS_MAGIC       0x52454953u
#define REIS_VERSION     1u
#define REIS_COMP_NONE   0x0u
#define REIS_COMP_RLE    0x1u
#define REIS_COMP_MASK   0x0Fu
#define REIS_HEADER_SIZE 32u

/* ---- WAV constants ---- */
#define WAV_RIFF_MAGIC   0x46464952u  /* 'RIFF' LE */
#define WAV_WAVE_MAGIC   0x45564157u  /* 'WAVE' LE */
#define WAV_FMT_MAGIC    0x20746D66u  /* 'fmt ' LE */
#define WAV_DATA_MAGIC   0x61746164u  /* 'data' LE */

/* AC97 output format: 48 kHz stereo 16-bit signed LE */
#define AC97_RATE     48000u
#define AC97_CHANNELS 2u
#define AC97_BITS     16u
#define AC97_DMA_BUF  4096u

#define VIEW_STATUS_H 18
#define VIEW_BG_R 30
#define VIEW_BG_G 30
#define VIEW_BG_B 30

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t reserved1;
    uint16_t reserved2;
} rei_header_t;

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t pixfmt;
    uint8_t flags;
    uint16_t version;
    uint32_t frame_count;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t frames_offset;
} reiv_header_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
} reiv_frame_entry_t;

typedef struct {
    int loaded;
    int width;
    int height;
    uint16_t* pixels;
} rei_image_t;

typedef struct {
    int is_video;
    int width;
    int height;

    int fd;
    char path[256];
    reiv_header_t header;
    reiv_frame_entry_t* index;
    uint16_t* frame;
    uint16_t* prev_frame;
    uint8_t* payload_buf;
    size_t payload_cap;
    uint8_t* delta_buf;
    size_t delta_cap;

    uint32_t frame_size_bytes;
    uint32_t next_frame;
    uint32_t stream_payload_off;
    uint32_t stream_data_base;
    int playing;
    unsigned frame_delay_us;
} reiv_stream_t;

/* ---- REIS on-disk header ---- */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  channels;
    uint8_t  bits;
    uint32_t sample_rate;
    uint32_t frame_count;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t flags;
    uint32_t reserved;
} reis_header_t;

/* ---- Audio playback state ---- */

/*
 * Read-ahead buffer size: how many source bytes we read from disk in one
 * batch.  Larger = fewer disk syscalls = less stutter, but more stack.
 *
 * 32 768 bytes at 44100 Hz stereo 16-bit = ~186 ms of source audio per
 * disk read.  At 48000 Hz stereo 16-bit = exactly 8 full AC97 DMA buffers
 * (8 × 4096 = 32 768).  Chosen to fill roughly half the AC97 ring in one
 * disk read, which gives enough headroom to hide I/O latency.
 *
 * This lives on the stack inside audio_pump, so keep it below ~48 KB to
 * stay within the 64 KB userland stack.
 */
#define AUDIO_READAHEAD 32768u

typedef struct {
    int is_audio;       /* 1 if this is an audio file */
    int playing;
    int audio_inited;   /* 1 after AC97 init succeeds */
    int volume_percent; /* software gain applied before AC97 write */
    uint32_t play_started_ms;
    uint32_t played_ms;

    /*
     * Source PCM — two modes:
     *   Buffered: pcm != NULL, entire decoded audio in memory.
     *   Streaming: pcm == NULL, src_fd >= 0, frames read from disk
     *              on demand sequentially (no lseek in the hot loop).
     * Streaming is used for uncompressed REIS and WAV to avoid
     * multi-megabyte heap allocations in the 1 MB userland heap.
     */
    uint8_t*  pcm;           /* buffered PCM (NULL when streaming) */
    size_t    pcm_size;      /* byte size of pcm buffer */

    /* Streaming fields (used when pcm == NULL) */
    int       src_fd;        /* open file descriptor; -1 if not streaming */
    uint32_t  src_data_offset; /* byte offset of first PCM frame in file */
    uint32_t  src_frame_count; /* total source frames available */

    /* Resampled 48 kHz stereo s16le for AC97 */
    int16_t*  out_buf;       /* pre-resampled buffer (NULL — not used) */
    size_t    out_frames;    /* total output frames at AC97_RATE */
    size_t    out_size;      /* byte size of out_buf */

    /* Source format info */
    uint32_t  src_rate;
    uint8_t   src_channels;
    uint8_t   src_bits;

    /* Current playback cursor (in resampled output frames at AC97_RATE) */
    size_t    cursor;

    /* Duration tracking */
    uint32_t  duration_ms;

    /*
     * Read-ahead buffer for streaming path.
     *
     * We read AUDIO_READAHEAD bytes from disk in one batch, hold them here,
     * and feed the AC97 ring directly from this buffer.  This amortizes
     * disk-read syscall overhead across many DMA buffer fills and keeps
     * the ring fed even when individual VFS reads are slow.
     *
     * ra_buf   — raw source bytes
     * ra_len   — bytes currently valid in ra_buf
     * ra_pos   — read cursor within ra_buf (bytes consumed so far)
     * ra_error — Bresenham fractional error carried between refills
     */
    uint8_t  ra_buf[AUDIO_READAHEAD];
    size_t   ra_len;   /* valid bytes in ra_buf */
    size_t   ra_pos;   /* bytes consumed from ra_buf */
    uint32_t ra_error; /* Bresenham error (persists across refills) */
} audio_state_t;

typedef struct {
    int handle;
    int running;

    int content_w;
    int content_h;
    int viewport_w;
    int viewport_h;

    /*
     * Blit dimensions: capped at the kernel gui_blit_rgb565 hard limit of
     * 320×200 pixels.  viewport_w/h may be larger (the full content area);
     * blit_w/blit_h are the actual framebuffer dimensions handed to the
     * kernel.  render_framebuffer maps blit coords → viewport coords →
     * source image coords so that zoom/pan still operates in the full
     * viewport space.  The kernel upscales blit_w×blit_h → viewport_w×viewport_h
     * via the dst_w/dst_h fields of gui_blit_rgb565_t.
     */
    int blit_w;
    int blit_h;

    uint16_t* viewbuf;
    size_t viewbuf_cap;

    int zoom_permille;
    int origin_x;
    int origin_y;

    int prev_left_down;
    int dragging;
    int drag_last_x;
    int drag_last_y;

    int64_t frame_counter;
    uint32_t audio_ui_last_redraw_ms;
    int audio_ui_dirty;

    rei_image_t image;
    reiv_stream_t video;
    audio_state_t audio;
} app_t;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static int clampi(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int str_ends_with(const char* s, const char* suffix) {
    size_t a = s ? strlen(s) : 0;
    size_t b = suffix ? strlen(suffix) : 0;
    if (a < b) return 0;
    return strcmp(s + a - b, suffix) == 0;
}

static void str_copy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_append(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0 || !src) return;
    int n = (int)strlen(dst);
    int i = 0;
    if (n >= cap - 1) return;
    while (src[i] && (n + i) < cap - 1) {
        dst[n + i] = src[i];
        ++i;
    }
    dst[n + i] = '\0';
}

static void str_append_uint(char* dst, int cap, uint32_t value) {
    char tmp[16];
    int pos = 0;
    if (value == 0u) {
        str_append(dst, cap, "0");
        return;
    }
    while (value > 0u && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (pos > 0) {
        char c[2];
        c[0] = tmp[--pos];
        c[1] = '\0';
        str_append(dst, cap, c);
    }
}

static int read_exact_fd(int fd, void* out_buf, size_t len) {
    uint8_t* out = (uint8_t*)out_buf;
    size_t total = 0;
    while (total < len) {
        ssize_t got = read(fd, out + total, len - total);
        if (got <= 0) return -1;
        total += (size_t)got;
    }
    return 0;
}

/*
 * Read the first 4 bytes of a file as a little-endian uint32 to identify the
 * format magic, then close.  Returns 0 if the file cannot be read.
 */
static uint32_t probe_file_magic(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    uint8_t buf[4];
    ssize_t n = read(fd, buf, 4);
    close(fd);
    if (n != 4) return 0;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/*
 * WAV shares the RIFF magic with other RIFF variants.  Confirm the sub-format
 * is WAVE by checking bytes 8-11.
 */
static int probe_is_wav(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    uint8_t buf[12];
    ssize_t n = read(fd, buf, 12);
    close(fd);
    if (n < 12) return 0;
    uint32_t riff = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                  | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
    uint32_t wave = (uint32_t)buf[8] | ((uint32_t)buf[9]<<8)
                  | ((uint32_t)buf[10]<<16) | ((uint32_t)buf[11]<<24);
    return (riff == WAV_RIFF_MAGIC && wave == WAV_WAVE_MAGIC) ? 1 : 0;
}

static int skip_fd_bytes(int fd, size_t len) {
    uint8_t scratch[256];
    size_t left = len;
    while (left > 0) {
        size_t want = left > sizeof(scratch) ? sizeof(scratch) : left;
        ssize_t got = read(fd, scratch, want);
        if (got <= 0) return -1;
        left -= (size_t)got;
    }
    return 0;
}

static int rle_decode_packbits(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, int pixel_size) {
    size_t ip = 0;
    size_t op = 0;
    if (!in || !out || pixel_size <= 0) return -1;

    while (ip < in_len && op < out_len) {
        int8_t n = (int8_t)in[ip++];
        if (n >= 0) {
            size_t count = (size_t)n + 1u;
            size_t bytes = count * (size_t)pixel_size;
            if (ip + bytes > in_len) return -1;
            if (op + bytes > out_len) return -1;
            memcpy(out + op, in + ip, bytes);
            ip += bytes;
            op += bytes;
        } else if (n != -128) {
            size_t count = (size_t)(1 - n);
            size_t bytes = count * (size_t)pixel_size;
            if (ip + (size_t)pixel_size > in_len) return -1;
            if (op + bytes > out_len) return -1;
            const uint8_t* px = in + ip;
            ip += (size_t)pixel_size;
            for (size_t i = 0; i < count; ++i) {
                memcpy(out + op, px, (size_t)pixel_size);
                op += (size_t)pixel_size;
            }
        }
    }
    return (op == out_len) ? 0 : -1;
}

static void rei_free(rei_image_t* image) {
    if (!image) return;
    if (image->pixels) free(image->pixels);
    image->pixels = NULL;
    image->loaded = 0;
    image->width = 0;
    image->height = 0;
}

static int rei_load_file(const char* path, rei_image_t* out_image) {
    int fd;
    uint8_t* file_buf = NULL;
    uint8_t* pixel_bytes = NULL;
    uint16_t* rgb = NULL;
    size_t total = 0;
    size_t cap = 64u * 1024u;

    if (!path || !out_image) return -1;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    file_buf = (uint8_t*)malloc(cap);
    if (!file_buf) {
        close(fd);
        return -1;
    }

    for (;;) {
        if (total == cap) {
            size_t next_cap = cap * 2u;
            uint8_t* grown;
            if (next_cap > (4u * 1024u * 1024u)) {
                free(file_buf);
                close(fd);
                return -1;
            }
            grown = (uint8_t*)realloc(file_buf, next_cap);
            if (!grown) {
                free(file_buf);
                close(fd);
                return -1;
            }
            file_buf = grown;
            cap = next_cap;
        }
        ssize_t got = read(fd, file_buf + total, cap - total);
        if (got < 0) {
            free(file_buf);
            close(fd);
            return -1;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(fd);

    if (total < sizeof(rei_header_t)) {
        free(file_buf);
        return -1;
    }

    rei_header_t hdr;
    memcpy(&hdr, file_buf, sizeof(hdr));
    if (hdr.magic != REI_MAGIC || hdr.width == 0u || hdr.height == 0u) {
        free(file_buf);
        return -1;
    }
    if (hdr.depth != REI_DEPTH_MONO && hdr.depth != REI_DEPTH_RGB && hdr.depth != REI_DEPTH_RGBA) {
        free(file_buf);
        return -1;
    }

    size_t px_count = (size_t)hdr.width * (size_t)hdr.height;
    size_t in_expected = px_count * (size_t)hdr.depth;
    if (px_count == 0 || in_expected == 0) {
        free(file_buf);
        return -1;
    }

    pixel_bytes = (uint8_t*)malloc(in_expected);
    if (!pixel_bytes) {
        free(file_buf);
        return -1;
    }

    uint8_t comp = hdr.reserved1 & REI_COMP_MASK;
    const uint8_t* in = file_buf + sizeof(rei_header_t);
    size_t in_len = total - sizeof(rei_header_t);

    if (comp == REI_COMP_NONE) {
        if (in_len < in_expected) {
            free(pixel_bytes);
            free(file_buf);
            return -1;
        }
        memcpy(pixel_bytes, in, in_expected);
    } else if (comp == REI_COMP_RLE) {
        if (rle_decode_packbits(in, in_len, pixel_bytes, in_expected, (int)hdr.depth) != 0) {
            free(pixel_bytes);
            free(file_buf);
            return -1;
        }
    } else {
        free(pixel_bytes);
        free(file_buf);
        return -1;
    }

    rgb = (uint16_t*)malloc(px_count * sizeof(uint16_t));
    if (!rgb) {
        free(pixel_bytes);
        free(file_buf);
        return -1;
    }

    for (size_t i = 0; i < px_count; ++i) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        size_t off = i * (size_t)hdr.depth;
        if (hdr.depth == REI_DEPTH_MONO) {
            r = pixel_bytes[off];
            g = pixel_bytes[off];
            b = pixel_bytes[off];
        } else {
            r = pixel_bytes[off + 0u];
            g = pixel_bytes[off + 1u];
            b = pixel_bytes[off + 2u];
        }
        rgb[i] = rgb565(r, g, b);
    }

    free(pixel_bytes);
    free(file_buf);

    out_image->loaded = 1;
    out_image->width = (int)hdr.width;
    out_image->height = (int)hdr.height;
    out_image->pixels = rgb;
    return 0;
}

static void reiv_close(reiv_stream_t* video) {
    if (!video) return;
    if (video->fd >= 0) close(video->fd);
    if (video->index) free(video->index);
    if (video->frame) free(video->frame);
    if (video->prev_frame) free(video->prev_frame);
    if (video->payload_buf) free(video->payload_buf);
    if (video->delta_buf) free(video->delta_buf);
    memset(video, 0, sizeof(*video));
    video->fd = -1;
}

static int reiv_ensure_payload_buf(reiv_stream_t* video, size_t bytes) {
    if (video->payload_cap >= bytes) return 0;
    size_t next = video->payload_cap ? video->payload_cap : 4096u;
    while (next < bytes) next *= 2u;
    uint8_t* grown = (uint8_t*)realloc(video->payload_buf, next);
    if (!grown) return -1;
    video->payload_buf = grown;
    video->payload_cap = next;
    return 0;
}

static int reiv_ensure_delta_buf(reiv_stream_t* video, size_t bytes) {
    if (video->delta_cap >= bytes) return 0;
    size_t next = video->delta_cap ? video->delta_cap : 4096u;
    while (next < bytes) next *= 2u;
    uint8_t* grown = (uint8_t*)realloc(video->delta_buf, next);
    if (!grown) return -1;
    video->delta_buf = grown;
    video->delta_cap = next;
    return 0;
}

static int reiv_open_and_prepare_stream(reiv_stream_t* video, const char* path) {
    if (!video || !path) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    reiv_header_t hdr;
    if (read_exact_fd(fd, &hdr, sizeof(hdr)) != 0) {
        close(fd);
        return -1;
    }

    if (hdr.magic != REIV_MAGIC) {
        close(fd);
        return -1;
    }
    if (hdr.version != REIV_VERSION_V1 && hdr.version != REIV_VERSION_V2 && hdr.version != REIV_VERSION_V3) {
        close(fd);
        return -1;
    }
    if (hdr.width == 0u || hdr.height == 0u || hdr.width > 640u || hdr.height > 480u) {
        close(fd);
        return -1;
    }
    if (hdr.pixfmt != REIV_PIXFMT_RGB565LE) {
        close(fd);
        return -1;
    }
    if (hdr.frame_count == 0u) {
        close(fd);
        return -1;
    }

    uint32_t frame_bytes = (uint32_t)hdr.width * (uint32_t)hdr.height * 2u;
    if (frame_bytes == 0u) {
        close(fd);
        return -1;
    }

    if (hdr.frames_offset < sizeof(hdr)) {
        close(fd);
        return -1;
    }
    if (hdr.frames_offset > sizeof(hdr)) {
        if (skip_fd_bytes(fd, (size_t)(hdr.frames_offset - (uint32_t)sizeof(hdr))) != 0) {
            close(fd);
            return -1;
        }
    }

    video->fd = fd;
    video->header = hdr;
    video->frame_size_bytes = frame_bytes;
    video->next_frame = 0u;
    video->stream_payload_off = 0u;
    video->stream_data_base = 0u;

    if (hdr.version == REIV_VERSION_V1) {
        video->stream_data_base = hdr.frames_offset;
        return 0;
    }

    size_t index_bytes = (size_t)hdr.frame_count * sizeof(reiv_frame_entry_t);
    if (index_bytes == 0u || index_bytes > (size_t)(8u * 1024u * 1024u)) {
        return -1;
    }

    if (!video->index) {
        video->index = (reiv_frame_entry_t*)malloc(index_bytes);
        if (!video->index) return -1;
    }

    if (read_exact_fd(fd, video->index, index_bytes) != 0) {
        return -1;
    }

    video->stream_data_base = hdr.frames_offset + (uint32_t)index_bytes;
    return 0;
}

static int reiv_rewind(reiv_stream_t* video) {
    if (!video) return -1;
    if (!video->path[0]) return -1;

    if (video->fd >= 0) {
        close(video->fd);
        video->fd = -1;
    }

    video->next_frame = 0u;
    video->stream_payload_off = 0u;

    if (reiv_open_and_prepare_stream(video, video->path) != 0) {
        return -1;
    }

    memset(video->prev_frame, 0, (size_t)video->frame_size_bytes);
    return 0;
}

static int reiv_open(const char* path, reiv_stream_t* out_video) {
    if (!path || !out_video) return -1;

    memset(out_video, 0, sizeof(*out_video));
    out_video->fd = -1;

    size_t n = strlen(path);
    if (n >= sizeof(out_video->path)) return -1;
    memcpy(out_video->path, path, n + 1u);

    if (reiv_open_and_prepare_stream(out_video, path) != 0) {
        reiv_close(out_video);
        return -1;
    }

    out_video->width = (int)out_video->header.width;
    out_video->height = (int)out_video->header.height;
    out_video->is_video = 1;

    out_video->frame = (uint16_t*)malloc((size_t)out_video->frame_size_bytes);
    out_video->prev_frame = (uint16_t*)malloc((size_t)out_video->frame_size_bytes);
    if (!out_video->frame || !out_video->prev_frame) {
        reiv_close(out_video);
        return -1;
    }
    memset(out_video->frame, 0, (size_t)out_video->frame_size_bytes);
    memset(out_video->prev_frame, 0, (size_t)out_video->frame_size_bytes);

    uint32_t fps_num = out_video->header.fps_num ? out_video->header.fps_num : 30u;
    uint32_t fps_den = out_video->header.fps_den ? out_video->header.fps_den : 1u;
    uint32_t frame_us = (uint32_t)((1000000ull * (uint64_t)fps_den) / (uint64_t)fps_num);
    if (frame_us < 1000u) frame_us = 1000u;
    if (frame_us > 200000u) frame_us = 200000u;
    out_video->frame_delay_us = frame_us;
    out_video->playing = 1;
    return 0;
}

static int reiv_decode_next(reiv_stream_t* video) {
    if (!video || video->fd < 0) return -1;

    if (video->next_frame >= video->header.frame_count) {
        if ((video->header.flags & REIV_FLAG_LOOP_DEFAULT) == 0u) return 1;
        if (reiv_rewind(video) != 0) return -1;
    }

    if (video->header.version == REIV_VERSION_V1) {
        if (read_exact_fd(video->fd, video->frame, (size_t)video->frame_size_bytes) != 0) return -1;
        memcpy(video->prev_frame, video->frame, (size_t)video->frame_size_bytes);
        video->next_frame += 1u;
        video->stream_payload_off += video->frame_size_bytes;
        return 0;
    }

    const reiv_frame_entry_t* entry = &video->index[video->next_frame];
    if (entry->offset < video->stream_payload_off) {
        return -1;
    }

    uint32_t gap = entry->offset - video->stream_payload_off;
    if (gap > 0u) {
        if (skip_fd_bytes(video->fd, (size_t)gap) != 0) return -1;
        video->stream_payload_off += gap;
    }

    if (reiv_ensure_payload_buf(video, (size_t)entry->size) != 0) return -1;
    if (read_exact_fd(video->fd, video->payload_buf, (size_t)entry->size) != 0) return -1;
    video->stream_payload_off += entry->size;

    uint8_t* frame_bytes = (uint8_t*)video->frame;
    size_t frame_bytes_len = (size_t)video->frame_size_bytes;

    if (entry->flags & REIV_FRAME_FLAG_DELTA_XOR_PREV) {
        uint8_t* delta = NULL;
        if (entry->flags & REIV_FRAME_FLAG_RLE8) {
            if (reiv_ensure_delta_buf(video, frame_bytes_len) != 0) return -1;
            if (rle_decode_packbits(video->payload_buf, (size_t)entry->size, video->delta_buf, frame_bytes_len, 1) != 0) {
                return -1;
            }
            delta = video->delta_buf;
        } else {
            if ((size_t)entry->size != frame_bytes_len) return -1;
            delta = video->payload_buf;
        }

        const uint8_t* prev = (const uint8_t*)video->prev_frame;
        for (size_t i = 0; i < frame_bytes_len; ++i) {
            frame_bytes[i] = prev[i] ^ delta[i];
        }
    } else if (entry->flags & REIV_FRAME_FLAG_RLE565) {
        if (rle_decode_packbits(video->payload_buf, (size_t)entry->size, frame_bytes, frame_bytes_len, 2) != 0) {
            return -1;
        }
    } else {
        if ((size_t)entry->size != frame_bytes_len) return -1;
        memcpy(frame_bytes, video->payload_buf, frame_bytes_len);
    }

    memcpy(video->prev_frame, video->frame, frame_bytes_len);
    video->next_frame += 1u;
    return 0;
}

/* ==== Audio support: REIS and WAV loading + resampling ==== */

static void audio_free(audio_state_t* a) {
    if (!a) return;
    if (a->pcm) free(a->pcm);
    if (a->out_buf) free(a->out_buf);
    if (a->src_fd >= 0) close(a->src_fd);
    memset(a, 0, sizeof(*a));
    a->src_fd = -1;
}

static uint32_t audio_now_ms(void) {
    return (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
}

static uint32_t audio_playback_ms(const audio_state_t* a) {
    if (!a) return 0;

    /*
     * Cursor-based elapsed time: tracks how much audio has been delivered
     * to the AC97 ring.  This is more accurate than wall-clock timing
     * because it pauses naturally when the CPU cannot keep up with disk
     * I/O, rather than advancing past the actual playback position.
     */
    uint32_t pos_ms = (uint32_t)((uint64_t)a->cursor * 1000u / AC97_RATE);
    if (a->duration_ms > 0 && pos_ms > a->duration_ms)
        pos_ms = a->duration_ms;
    return pos_ms;
}

static int16_t audio_apply_volume(int16_t sample, int volume_percent) {
    long scaled;

    if (volume_percent <= 0) return 0;
    if (volume_percent == 100) return sample;

    scaled = ((long)sample * (long)volume_percent) / 100L;
    if (scaled > 32767L) return 32767;
    if (scaled < -32768L) return -32768;
    return (int16_t)scaled;
}

/*
 * Resample arbitrary-format PCM to 48 kHz stereo s16le for AC97 output.
 * Nearest-neighbour resampling (good enough for a simple player).
 */
static int audio_resample_to_ac97(audio_state_t* a) {
    if (!a) return -1;
    if (a->src_rate == 0 || a->src_channels == 0 || a->src_bits == 0) return -1;

    uint32_t src_frame_bytes = (uint32_t)a->src_channels * ((uint32_t)a->src_bits / 8u);
    if (src_frame_bytes == 0) return -1;

    /*
     * Determine total source frames: streaming uses src_frame_count directly;
     * buffered uses the pcm buffer size.
     */
    uint32_t src_frames;
    if (a->src_fd >= 0 && !a->pcm) {
        src_frames = a->src_frame_count;
    } else {
        if (!a->pcm || a->pcm_size == 0) return -1;
        src_frames = (uint32_t)(a->pcm_size / src_frame_bytes);
    }
    if (src_frames == 0) return -1;

    /* Calculate output frame count for progress/duration tracking only. */
    uint64_t out_frames64 = ((uint64_t)src_frames * AC97_RATE + a->src_rate - 1u) / a->src_rate;
    if (out_frames64 == 0) return -1;
    a->out_frames = (size_t)out_frames64;
    a->out_size   = a->out_frames * AC97_CHANNELS * sizeof(int16_t);

    /* out_buf is never pre-allocated; pump resamples into a stack buffer. */
    a->out_buf = NULL;

    a->duration_ms = (uint32_t)((uint64_t)a->out_frames * 1000u / AC97_RATE);

    /* Diagnostic: show computed audio parameters */
    printf("audio: %u Hz %u-bit %uch -> %u output frames, duration %u ms\n",
           (unsigned)a->src_rate, (unsigned)a->src_bits, (unsigned)a->src_channels,
           (unsigned)a->out_frames, (unsigned)a->duration_ms);

    return 0;
}

/*
 * Load a REIS audio file.
 */
static int audio_load_reis(const char* path, audio_state_t* a) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    reis_header_t hdr;
    if (read_exact_fd(fd, &hdr, sizeof(hdr)) != 0) { close(fd); return -1; }
    if (hdr.magic != REIS_MAGIC || hdr.version != REIS_VERSION) { close(fd); return -1; }
    if (hdr.channels != 1 && hdr.channels != 2) { close(fd); return -1; }
    if (hdr.bits != 8 && hdr.bits != 16) { close(fd); return -1; }
    if (hdr.sample_rate == 0 || hdr.frame_count == 0) { close(fd); return -1; }
    if (hdr.data_size == 0) { close(fd); return -1; }

    a->src_rate = hdr.sample_rate;
    a->src_channels = hdr.channels;
    a->src_bits = hdr.bits;
    a->src_frame_count = hdr.frame_count;
    a->is_audio = 1;
    a->playing = 0;
    a->volume_percent = 100;
    a->play_started_ms = 0;
    a->played_ms = 0;
    a->cursor = 0;
    a->ra_len = 0;
    a->ra_pos = 0;
    a->ra_error = 0;

    uint8_t comp = (uint8_t)(hdr.flags & REIS_COMP_MASK);
    uint32_t data_offset = (hdr.data_offset >= (uint32_t)sizeof(hdr))
                           ? hdr.data_offset : (uint32_t)sizeof(hdr);

    if (comp == REIS_COMP_NONE) {
        /*
         * Streaming path: keep the file open and read source frames
         * sequentially in audio_pump.  Position the fd at data_offset
         * so the first read gets the first PCM frame.
         */
        if (data_offset > (uint32_t)sizeof(hdr)) {
            if (skip_fd_bytes(fd, (size_t)(data_offset
                              - (uint32_t)sizeof(hdr))) != 0) {
                close(fd); return -1;
            }
        }
        a->src_fd = fd;          /* transfer ownership; audio_free closes it */
        a->src_data_offset = data_offset;
        a->pcm = NULL;
        a->pcm_size = 0;
    } else if (comp == REIS_COMP_RLE) {
        /*
         * Buffered path: decompress RLE payload into memory.
         * Limited to files whose payload fits in a reasonable heap budget;
         * very large RLE files will fail the malloc and return -1 here.
         */
        uint32_t pcm_bytes = hdr.frame_count * (uint32_t)hdr.channels
                           * ((uint32_t)hdr.bits / 8u);
        if (hdr.data_offset > sizeof(hdr)) {
            if (skip_fd_bytes(fd, (size_t)(hdr.data_offset
                              - (uint32_t)sizeof(hdr))) != 0) {
                close(fd); return -1;
            }
        }
        uint8_t* payload = (uint8_t*)malloc(hdr.data_size);
        if (!payload) { close(fd); return -1; }
        if (read_exact_fd(fd, payload, hdr.data_size) != 0) {
            free(payload); close(fd); return -1;
        }
        close(fd);
        a->pcm = (uint8_t*)malloc(pcm_bytes);
        if (!a->pcm) { free(payload); return -1; }
        int unit = (int)((uint32_t)hdr.channels * ((uint32_t)hdr.bits / 8u));
        if (rle_decode_packbits(payload, hdr.data_size, a->pcm,
                                pcm_bytes, unit) != 0) {
            free(a->pcm); a->pcm = NULL; free(payload); return -1;
        }
        a->pcm_size = pcm_bytes;
        free(payload);
        a->src_fd = -1;
    } else {
        close(fd); return -1;
    }

    return audio_resample_to_ac97(a);
}

/*
 * Load a WAV audio file (PCM format only).
 * Scans the RIFF chunk headers to locate the 'fmt ' and 'data' chunks,
 * records the data offset for streaming, and keeps the file descriptor
 * open for on-demand reads in audio_pump.  No PCM bytes are loaded into
 * the heap here, so arbitrarily large WAV files work within the 1 MB
 * userland heap budget.
 */
static int audio_load_wav(const char* path, audio_state_t* a) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    /* Read RIFF header: 'RIFF' <size> 'WAVE' */
    uint32_t riff_id, riff_size, wave_id;
    if (read_exact_fd(fd, &riff_id, 4) != 0)   { close(fd); return -1; }
    if (read_exact_fd(fd, &riff_size, 4) != 0)  { close(fd); return -1; }
    if (read_exact_fd(fd, &wave_id, 4) != 0)    { close(fd); return -1; }
    if (riff_id != WAV_RIFF_MAGIC || wave_id != WAV_WAVE_MAGIC) { close(fd); return -1; }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    int got_fmt = 0;
    uint32_t data_size = 0;
    long data_file_offset = 0;  /* byte offset of first PCM sample in file */
    int got_data = 0;

    /* Walk chunks locating 'fmt ' and 'data' without loading PCM. */
    for (;;) {
        uint32_t chunk_id, chunk_size;
        if (read_exact_fd(fd, &chunk_id, 4) != 0) break;
        if (read_exact_fd(fd, &chunk_size, 4) != 0) break;

        if (chunk_id == WAV_FMT_MAGIC) {
            if (chunk_size < 16) { close(fd); return -1; }
            uint32_t byte_rate;
            uint16_t block_align;
            if (read_exact_fd(fd, &audio_format, 2) != 0)    { close(fd); return -1; }
            if (read_exact_fd(fd, &channels, 2) != 0)        { close(fd); return -1; }
            if (read_exact_fd(fd, &sample_rate, 4) != 0)     { close(fd); return -1; }
            if (read_exact_fd(fd, &byte_rate, 4) != 0)       { close(fd); return -1; }
            if (read_exact_fd(fd, &block_align, 2) != 0)     { close(fd); return -1; }
            if (read_exact_fd(fd, &bits_per_sample, 2) != 0) { close(fd); return -1; }
            if (chunk_size > 16) {
                if (skip_fd_bytes(fd, chunk_size - 16) != 0) { close(fd); return -1; }
            }
            got_fmt = 1;
        } else if (chunk_id == WAV_DATA_MAGIC && got_fmt) {
            if (chunk_size == 0) { close(fd); return -1; }
            /* Record position immediately after the 'data' chunk header. */
            data_file_offset = lseek(fd, 0, SEEK_CUR);
            if (data_file_offset < 0) { close(fd); return -1; }
            data_size = chunk_size;
            got_data = 1;
            break;
        } else {
            if (chunk_size > 0) {
                if (skip_fd_bytes(fd, chunk_size) != 0) break;
            }
        }
    }

    if (!got_fmt || !got_data || data_size == 0) { close(fd); return -1; }
    if (audio_format != 1) { close(fd); return -1; }  /* PCM only */
    if (channels < 1 || channels > 2) { close(fd); return -1; }
    if (bits_per_sample != 8 && bits_per_sample != 16) { close(fd); return -1; }

    uint32_t frame_bytes = (uint32_t)channels * ((uint32_t)bits_per_sample / 8u);
    if (frame_bytes == 0) { close(fd); return -1; }

    /* Streaming path: keep fd open for audio_pump lseek reads. */
    a->src_fd = fd;
    a->src_data_offset = (uint32_t)data_file_offset;
    a->src_frame_count = data_size / frame_bytes;
    a->pcm = NULL;
    a->pcm_size = 0;
    a->src_rate = sample_rate;
    a->src_channels = (uint8_t)channels;
    a->src_bits = (uint8_t)bits_per_sample;
    a->is_audio = 1;
    a->playing = 0;
    a->volume_percent = 100;
    a->play_started_ms = 0;
    a->played_ms = 0;
    a->cursor = 0;
    a->ra_len = 0;
    a->ra_pos = 0;
    a->ra_error = 0;

    return audio_resample_to_ac97(a);
}

/*
 * Submit the next chunk of resampled audio to the AC97 driver.
 * Returns 0 if more data remains, 1 if playback finished, -1 on error.
 */
static int audio_pump(audio_state_t* a) {
    if (!a || !a->is_audio || !a->playing) return -1;
    if (!a->pcm && a->src_fd < 0) return -1;

    uint32_t src_frame_bytes = (uint32_t)a->src_channels * ((uint32_t)a->src_bits / 8u);
    if (src_frame_bytes == 0) return -1;

    uint32_t src_frames = a->pcm ? (uint32_t)(a->pcm_size / src_frame_bytes)
                                 : a->src_frame_count;

    size_t frames_per_buf = AC97_DMA_BUF / (AC97_CHANNELS * sizeof(int16_t));

    /*
     * ---- Streaming fast path: native 48 kHz stereo 16-bit 100 % volume ----
     *
     * The data in ra_buf is already in AC97 output format, so we can
     * submit it directly without copying through an intermediate
     * chunk_buf.  One AUDIO_WRITE_BULK syscall queues up to 8 DMA
     * buffers from ra_buf in a single ring-3/ring-0 transition.
     */
    if (a->src_fd >= 0 && !a->pcm &&
        a->src_rate == AC97_RATE && a->src_channels == 2u &&
        a->src_bits == 16u && a->volume_percent == 100) {
        if (a->cursor >= a->out_frames) return 1;

        /* Refill ra_buf if less than one DMA buffer remains. */
        size_t avail = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
        if (avail < AC97_DMA_BUF) {
            size_t leftover = avail;
            if (leftover > 0)
                memmove(a->ra_buf, a->ra_buf + a->ra_pos, leftover);
            a->ra_pos = 0;
            a->ra_len = leftover;
            size_t want = AUDIO_READAHEAD - a->ra_len;
            int got = (int)read(a->src_fd, a->ra_buf + a->ra_len, (int)want);
            if (got > 0) a->ra_len += (size_t)got;
            avail = a->ra_len;
        }

        /*
         * Determine how many bytes to submit.  Align down to 4 KB
         * boundaries so each DMA buffer is exactly full (the kernel
         * zero-pads short chunks, but we avoid unnecessary silence).
         */
        size_t frames_left = a->out_frames - a->cursor;
        size_t bytes_left  = frames_left * AC97_CHANNELS * sizeof(int16_t);
        size_t submit      = avail < bytes_left ? avail : bytes_left;
        size_t aligned      = submit & ~(size_t)(AC97_DMA_BUF - 1u);

        /*
         * If we have a partial tail (less than 4 KB remaining in the
         * file), include it — the kernel will zero-pad the last DMA
         * buffer.  This avoids dropping the final fraction of audio.
         */
        if (aligned == 0 && submit > 0)
            aligned = submit;

        if (aligned == 0) return 1;  /* nothing left */

        int count = eyn_sys_audio_write_bulk(
                        a->ra_buf + a->ra_pos, (int)aligned);
        if (count <= 0) return 0;  /* ring full — retry next frame */

        size_t bytes_queued  = (size_t)count * AC97_DMA_BUF;
        if (bytes_queued > aligned) bytes_queued = aligned;
        size_t frames_queued = bytes_queued / (AC97_CHANNELS * sizeof(int16_t));

        a->ra_pos += bytes_queued;
        a->cursor += frames_queued;
        return (a->cursor >= a->out_frames) ? 1 : 0;
    }

    /* ---- General path: per-buffer write with resample support ---- */
    while (1) {
        if (a->cursor >= a->out_frames) return 1;  /* playback complete */

        size_t remaining = a->out_frames - a->cursor;
        size_t to_send = remaining < frames_per_buf ? remaining : frames_per_buf;
        if (to_send == 0) return 1;

        int16_t chunk_buf[AC97_DMA_BUF / sizeof(int16_t)];

        /*
         * ra_advance: how many bytes to commit from ra_buf after a
         * successful write.  Set during the chunk-build step below,
         * applied only after eyn_sys_audio_write succeeds.  This
         * prevents ra_pos from advancing when the ring is full —
         * which previously caused the audio to skip forward because
         * ra_buf moved ahead of cursor.
         *
         * new_ra_error: similarly, the updated Bresenham error is only
         * committed to a->ra_error after a successful write.
         */
        size_t   ra_advance   = 0;
        uint32_t new_ra_error = a->ra_error;

        if (a->src_fd >= 0 && !a->pcm) {
            /*
             * Streaming resample path: compute how many source bytes we
             * need for this output chunk, then serve from ra_buf.
             * (The native-48 kHz fast path is handled above via bulk
             * write and never reaches here.)
             */
            uint64_t src_last64 = (uint64_t)(a->cursor + to_send - 1u) * a->src_rate;
            uint32_t src_last = (uint32_t)(src_last64 / AC97_RATE);
            uint64_t src_start64 = (uint64_t)a->cursor * a->src_rate;
            uint32_t src_start = (uint32_t)(src_start64 / AC97_RATE);
            uint32_t src_count = src_last - src_start + 1u;
            size_t src_bytes = (size_t)src_count * src_frame_bytes;

            /* Clamp to read-ahead buffer size. */
            if (src_bytes > AUDIO_READAHEAD) return -1;

            /* Refill if not enough source bytes available. */
            if (a->ra_pos + src_bytes > a->ra_len) {
                size_t leftover = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
                if (leftover > 0)
                    memmove(a->ra_buf, a->ra_buf + a->ra_pos, leftover);
                a->ra_pos = 0;
                a->ra_len = leftover;

                size_t want = AUDIO_READAHEAD - a->ra_len;
                int got = (int)read(a->src_fd, a->ra_buf + a->ra_len, (int)want);
                if (got > 0)
                    a->ra_len += (size_t)got;
            }

            const uint8_t* src_ptr = a->ra_buf + a->ra_pos;
            size_t avail = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
            size_t si = 0;
            uint32_t error = a->ra_error;  /* local copy; committed below */

            for (size_t i = 0; i < to_send; ++i) {
                size_t byte_off = si * src_frame_bytes;
                int16_t left = 0, right = 0;
                if (byte_off + src_frame_bytes <= avail) {
                    if (a->src_bits == 16) {
                        const int16_t* fp = (const int16_t*)(src_ptr + byte_off);
                        left  = fp[0];
                        right = (a->src_channels >= 2u) ? fp[1] : left;
                    } else {
                        const uint8_t* fp = src_ptr + byte_off;
                        left  = (int16_t)((int)fp[0] - 128) * 256;
                        right = (a->src_channels >= 2u)
                                ? (int16_t)((int)fp[1] - 128) * 256
                                : left;
                    }
                }
                left  = audio_apply_volume(left,  a->volume_percent);
                right = audio_apply_volume(right, a->volume_percent);
                chunk_buf[i * 2 + 0] = left;
                chunk_buf[i * 2 + 1] = right;

                error += a->src_rate;
                while (error >= AC97_RATE) {
                    error -= AC97_RATE;
                    si++;
                }
            }
            /* Record advance amounts — applied only after successful write. */
            size_t src_consumed = si * src_frame_bytes;
            if (src_consumed > avail) src_consumed = avail;
            ra_advance   = src_consumed;
            new_ra_error = error;

        } else {
            /*
             * Buffered path: entire source PCM is in a->pcm.
             * Bresenham-style resampling from memory.
             */
            uint64_t src_pos64 = (uint64_t)a->cursor * a->src_rate;
            uint32_t src_idx = (uint32_t)(src_pos64 / AC97_RATE);
            uint32_t error   = (uint32_t)(src_pos64 % AC97_RATE);

            /* Fast path: native 48kHz stereo 16-bit, 100% volume. */
            if (a->src_rate == AC97_RATE && a->src_channels == 2u &&
                a->src_bits == 16u && a->volume_percent == 100) {
                memcpy(chunk_buf,
                       a->pcm + a->cursor * (AC97_CHANNELS * sizeof(int16_t)),
                       to_send * AC97_CHANNELS * sizeof(int16_t));
            } else {
                for (size_t i = 0; i < to_send; ++i) {
                    if (src_idx >= src_frames) src_idx = src_frames - 1u;

                    int16_t left = 0, right = 0;
                    if (a->src_bits == 16) {
                        const int16_t* fp =
                            (const int16_t*)(a->pcm + src_idx * src_frame_bytes);
                        left  = fp[0];
                        right = (a->src_channels >= 2u) ? fp[1] : left;
                    } else {
                        const uint8_t* fp = a->pcm + src_idx * src_frame_bytes;
                        left  = (int16_t)((int)fp[0] - 128) * 256;
                        right = (a->src_channels >= 2u)
                                ? (int16_t)((int)fp[1] - 128) * 256
                                : left;
                    }
                    left  = audio_apply_volume(left,  a->volume_percent);
                    right = audio_apply_volume(right, a->volume_percent);
                    chunk_buf[i * 2 + 0] = left;
                    chunk_buf[i * 2 + 1] = right;

                    error += a->src_rate;
                    while (error >= AC97_RATE) {
                        error -= AC97_RATE;
                        src_idx++;
                    }
                }
            }
        }

        size_t bytes = to_send * AC97_CHANNELS * sizeof(int16_t);
        int rc = eyn_sys_audio_write((const void*)chunk_buf, (int)bytes);
        if (rc < 0)
            return 0;  /* ring full — come back next render frame */

        /*
         * Write succeeded: commit read-ahead state.  These are only
         * advanced here so that a full ring (rc < 0 above) leaves ra_pos
         * and ra_error unchanged, and the next call retries the same
         * chunk instead of jumping forward in the file.
         */
        a->ra_pos   += ra_advance;
        a->ra_error  = new_ra_error;
        a->cursor   += to_send;
    }
}

static int ensure_viewbuf(app_t* app) {
    if (!app) return -1;
    if (app->blit_w <= 0 || app->blit_h <= 0) return -1;
    size_t need_px = (size_t)app->blit_w * (size_t)app->blit_h;
    if (need_px <= app->viewbuf_cap && app->viewbuf) return 0;
    uint16_t* grown = (uint16_t*)realloc(app->viewbuf, need_px * sizeof(uint16_t));
    if (!grown) return -1;
    app->viewbuf = grown;
    app->viewbuf_cap = need_px;
    return 0;
}

static void reset_view_transform(app_t* app, int src_w, int src_h) {
    if (!app || src_w <= 0 || src_h <= 0 || app->viewport_w <= 0 || app->viewport_h <= 0) return;

    int zoom_w = (app->viewport_w * 1000) / src_w;
    int zoom_h = (app->viewport_h * 1000) / src_h;
    int zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom < 50) zoom = 50;
    if (zoom > 6000) zoom = 6000;

    app->zoom_permille = zoom;
    int scaled_w = (src_w * zoom + 999) / 1000;
    int scaled_h = (src_h * zoom + 999) / 1000;
    app->origin_x = (app->viewport_w - scaled_w) / 2;
    app->origin_y = (app->viewport_h - scaled_h) / 2;
}

static void zoom_view(app_t* app, int src_w, int src_h, int zoom_in) {
    if (!app || src_w <= 0 || src_h <= 0) return;
    int old_zoom = app->zoom_permille;
    int next_zoom = zoom_in ? (old_zoom * 5) / 4 : (old_zoom * 4) / 5;
    next_zoom = clampi(next_zoom, 50, 8000);
    if (next_zoom == old_zoom) return;

    int cx = app->viewport_w / 2;
    int cy = app->viewport_h / 2;

    int src_cx = ((cx - app->origin_x) * 1000) / old_zoom;
    int src_cy = ((cy - app->origin_y) * 1000) / old_zoom;

    app->zoom_permille = next_zoom;
    app->origin_x = cx - (src_cx * next_zoom) / 1000;
    app->origin_y = cy - (src_cy * next_zoom) / 1000;
}

static void render_framebuffer(app_t* app, const uint16_t* src, int src_w, int src_h) {
    if (!app || !src || !app->viewbuf) return;
    if (app->blit_w <= 0 || app->blit_h <= 0) return;

    /*
     * Render into the blit buffer (blit_w × blit_h, ≤320×200).
     * Each blit pixel (bx, by) is first mapped to a viewport pixel
     * (vpx, vpy) by scaling blit dims → viewport dims.  The viewport
     * coordinate is then mapped to a source image pixel via zoom/origin.
     * The kernel upscales the blit buffer back to viewport dimensions via
     * the dst_w/dst_h fields of gui_blit_rgb565_t.
     */
    uint16_t bg = rgb565(VIEW_BG_R, VIEW_BG_G, VIEW_BG_B);
    size_t total = (size_t)app->blit_w * (size_t)app->blit_h;
    for (size_t i = 0; i < total; ++i) app->viewbuf[i] = bg;

    int zoom = app->zoom_permille;
    if (zoom <= 0) zoom = 1000;

    for (int by = 0; by < app->blit_h; ++by) {
        /* Map blit row → viewport row (integer nearest-neighbour downscale) */
        int vpy = (app->blit_h == app->viewport_h)
                  ? by
                  : (by * app->viewport_h / app->blit_h);
        int sy = ((vpy - app->origin_y) * 1000) / zoom;
        if (sy < 0 || sy >= src_h) continue;
        size_t row_off = (size_t)by * (size_t)app->blit_w;
        size_t src_row = (size_t)sy * (size_t)src_w;
        for (int bx = 0; bx < app->blit_w; ++bx) {
            /* Map blit column → viewport column */
            int vpx = (app->blit_w == app->viewport_w)
                      ? bx
                      : (bx * app->viewport_w / app->blit_w);
            int sx = ((vpx - app->origin_x) * 1000) / zoom;
            if (sx < 0 || sx >= src_w) continue;
            app->viewbuf[row_off + (size_t)bx] = src[src_row + (size_t)sx];
        }
    }
}

static void draw_status(app_t* app, const char* path) {
    gui_rect_t status_bg = {
        .x = 0,
        .y = app->viewport_h,
        .w = app->content_w,
        .h = VIEW_STATUS_H,
        .r = 38,
        .g = 38,
        .b = 38,
        ._pad = 0
    };
    (void)gui_fill_rect(app->handle, &status_bg);

    char left[128];
    char right[128];
    left[0] = '\0';
    right[0] = '\0';
    if (app->video.is_video) {
        uint32_t shown = app->video.next_frame ? app->video.next_frame : 1u;
        str_copy(left, (int)sizeof(left), path);
        str_append(left, (int)sizeof(left), " | frame ");
        str_append_uint(left, (int)sizeof(left), shown);
        str_append(left, (int)sizeof(left), "/");
        str_append_uint(left, (int)sizeof(left), app->video.header.frame_count);
        str_append(left, (int)sizeof(left), " | ");
        str_append(left, (int)sizeof(left), app->video.playing ? "playing" : "paused");
        str_copy(right, (int)sizeof(right), "Space play/pause  +/- zoom  wheel zoom  arrows/mouse pan  Esc quit");
    } else if (app->audio.is_audio) {
        uint32_t pos_ms = audio_playback_ms(&app->audio);
        uint32_t dur_ms = app->audio.duration_ms;
        str_copy(left, (int)sizeof(left), path);
        str_append(left, (int)sizeof(left), " | ");
        str_append_uint(left, (int)sizeof(left), pos_ms / 1000u);
        str_append(left, (int)sizeof(left), ".");
        str_append_uint(left, (int)sizeof(left), (pos_ms / 100u) % 10u);
        str_append(left, (int)sizeof(left), "s / ");
        str_append_uint(left, (int)sizeof(left), dur_ms / 1000u);
        str_append(left, (int)sizeof(left), ".");
        str_append_uint(left, (int)sizeof(left), (dur_ms / 100u) % 10u);
        str_append(left, (int)sizeof(left), "s | ");
        str_append_uint(left, (int)sizeof(left), (uint32_t)app->audio.volume_percent);
        str_append(left, (int)sizeof(left), "% | ");
        str_append(left, (int)sizeof(left), app->audio.playing ? "playing" : "paused");
        str_copy(right, (int)sizeof(right), "Space play/pause  +/- volume  Esc quit");
    } else {
        str_copy(left, (int)sizeof(left), path);
        str_append(left, (int)sizeof(left), " | zoom ");
        str_append_uint(left, (int)sizeof(left), (uint32_t)(app->zoom_permille / 10));
        str_append(left, (int)sizeof(left), "%");
        str_copy(right, (int)sizeof(right), "+/- zoom  wheel zoom  arrows/mouse pan  Esc quit");
    }

    gui_text_t t1 = { .x = 6, .y = app->viewport_h + 5, .r = 222, .g = 222, .b = 222, ._pad = 0, .text = left };
    gui_text_t t2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = 140, .g = 140, .b = 140, ._pad = 0, .text = right };
    (void)gui_draw_text(app->handle, &t1);
    (void)gui_draw_text(app->handle, &t2);
}

static void render_app(app_t* app, const char* path, const uint16_t* src_pixels, int src_w, int src_h) {
    if (!app || !src_pixels) return;

    if (ensure_viewbuf(app) != 0) return;
    render_framebuffer(app, src_pixels, src_w, src_h);

    /*
     * Do NOT call gui_clear here.  The kernel processes the blit buffer
     * first, then draw-commands on top.  Calling gui_clear after
     * gui_blit_rgb565 would paint a solid background over the image.
     * render_framebuffer already fills every viewbuf pixel with the
     * background colour before drawing the image.
     */
    (void)gui_begin(app->handle);

    gui_blit_rgb565_t blit = {
        /*
         * src_w/src_h: the actual pixels in viewbuf (≤320×200 kernel limit).
         * dst_w/dst_h: the full viewport area the kernel scales the blit into.
         */
        .src_w = app->blit_w,
        .src_h = app->blit_h,
        .pixels = app->viewbuf,
        .dst_w = app->viewport_w,
        .dst_h = app->viewport_h,
    };
    (void)gui_blit_rgb565(app->handle, &blit);

    draw_status(app, path);
    (void)gui_present(app->handle);
}

static int key_is_zoom_in(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '+' || ch == '=' || key == 0x2102;
}

static int key_is_zoom_out(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '-' || ch == '_' || key == 0x2103;
}

static void usage(void) {
    puts("Usage: view <file.rei|file.reiv|file.reis|file.wav>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    const char* path = argv[1];
    /*
     * app_t is declared static to keep the 32KB read-ahead buffer (ra_buf)
     * and other large fields off the stack.  The userland stack on EYN-OS
     * is 64 KB; putting a 32 KB+ struct on it would overflow it immediately.
     */
    static app_t app;
    memset(&app, 0, sizeof(app));
    app.video.fd = -1;
    app.audio.src_fd = -1;

    int is_reiv = str_ends_with(path, ".reiv");
    int is_reis = str_ends_with(path, ".reis");
    int is_wav  = str_ends_with(path, ".wav");
    int is_audio = is_reis || is_wav;

    /*
     * Magic-based format detection overrides extension hints.
     * This lets users omit or mistype extensions and still load the right format.
     */
    {
        uint32_t magic = probe_file_magic(path);
        if (magic == REIS_MAGIC) {
            is_reis = 1; is_wav = 0; is_reiv = 0; is_audio = 1;
        } else if (magic == WAV_RIFF_MAGIC && probe_is_wav(path)) {
            is_wav = 1; is_reis = 0; is_reiv = 0; is_audio = 1;
        } else if (magic == REIV_MAGIC) {
            is_reiv = 1; is_reis = 0; is_wav = 0; is_audio = 0;
        } else if (magic == REI_MAGIC) {
            is_reiv = 0; is_reis = 0; is_wav = 0; is_audio = 0;
        }
    }

    if (is_audio) {
        /* ---- Audio path ---- */
        int load_rc = -1;
        if (is_reis) {
            load_rc = audio_load_reis(path, &app.audio);
        } else {
            load_rc = audio_load_wav(path, &app.audio);
        }
        if (load_rc != 0) {
            puts("view: failed to load audio file");
            return 1;
        }

        /* Probe and init the AC97 audio controller */
        if (eyn_sys_audio_probe() != 0) {
            puts("view: no audio hardware detected");
            audio_free(&app.audio);
            return 1;
        }
        if (eyn_sys_audio_init() != 0) {
            puts("view: failed to initialise audio hardware");
            audio_free(&app.audio);
            return 1;
        }
        app.audio.audio_inited = 1;
        app.audio.playing = 1;  /* auto-play */
        app.audio.play_started_ms = audio_now_ms();
        app.audio.played_ms = 0;
        app.audio_ui_last_redraw_ms = 0;
        app.audio_ui_dirty = 1;
    } else if (is_reiv) {
        if (reiv_open(path, &app.video) != 0) {
            puts("view: failed to open REIV file");
            return 1;
        }
        if (reiv_decode_next(&app.video) != 0) {
            puts("view: failed to decode first video frame");
            reiv_close(&app.video);
            return 1;
        }
    } else {
        if (rei_load_file(path, &app.image) != 0) {
            puts("view: failed to load REI image");
            return 1;
        }
    }

    app.handle = gui_create("View", "Esc quit | +/- zoom | wheel zoom | arrows/mouse pan");
    if (app.handle < 0) {
        reiv_close(&app.video);
        rei_free(&app.image);
        audio_free(&app.audio);
        puts("view: gui_create failed");
        return 1;
    }

    (void)gui_set_continuous_redraw(app.handle, 1);
    app.running = 1;

    while (app.running) {
        gui_size_t sz = {0, 0};
        (void)gui_get_content_size(app.handle, &sz);
        if (sz.w <= 0) sz.w = 920;
        if (sz.h <= 0) sz.h = 560;
        app.content_w = sz.w;
        app.content_h = sz.h;
        app.viewport_w = sz.w;
        app.viewport_h = sz.h - VIEW_STATUS_H;
        if (app.viewport_h < 40) app.viewport_h = 40;
        /* Blit buffer is capped at the kernel gui_blit_rgb565 hard limit. */
        app.blit_w = app.viewport_w > 320 ? 320 : app.viewport_w;
        app.blit_h = app.viewport_h > 200 ? 200 : app.viewport_h;

        int src_w = 0;
        int src_h = 0;
        const uint16_t* src_pixels = NULL;

        if (app.audio.is_audio) {
            /* Audio mode: no image to display, use a 1x1 dummy */
            src_w = 1;
            src_h = 1;
            src_pixels = NULL;
        } else {
            src_w = app.video.is_video ? app.video.width : app.image.width;
            src_h = app.video.is_video ? app.video.height : app.image.height;
            src_pixels = app.video.is_video ? app.video.frame : app.image.pixels;
        }

        if (!app.audio.is_audio && (app.frame_counter == 0 || app.zoom_permille <= 0)) {
            reset_view_transform(&app, src_w, src_h);
        }

        gui_event_t ev;
        while (gui_poll_event(app.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                app.running = 0;
                break;
            }

            if (ev.type == GUI_EVENT_KEY) {
                int base = ev.a & 0x0FFF;
                unsigned ch = (unsigned)ev.a & 0xFFu;

                if (ev.a == 27 || ch == 27u || ch == 'q' || ch == 'Q') {
                    app.running = 0;
                    break;
                }

                if (app.audio.is_audio && ch == ' ') {
                    if (app.audio.playing) {
                        app.audio.played_ms = audio_playback_ms(&app.audio);
                        app.audio.playing = 0;
                        eyn_sys_audio_stop();
                    } else {
                        app.audio.play_started_ms = audio_now_ms();
                        app.audio.playing = 1;
                    }
                    app.audio_ui_dirty = 1;
                } else if (app.audio.is_audio && key_is_zoom_in(ev.a)) {
                    if (app.audio.volume_percent < 400) app.audio.volume_percent += 10;
                    app.audio_ui_dirty = 1;
                } else if (app.audio.is_audio && key_is_zoom_out(ev.a)) {
                    if (app.audio.volume_percent > 0) app.audio.volume_percent -= 10;
                    app.audio_ui_dirty = 1;
                } else if (app.video.is_video && ch == ' ') {
                    app.video.playing = !app.video.playing;
                } else if (!app.audio.is_audio && key_is_zoom_in(ev.a)) {
                    zoom_view(&app, src_w, src_h, 1);
                } else if (!app.audio.is_audio && key_is_zoom_out(ev.a)) {
                    zoom_view(&app, src_w, src_h, 0);
                } else if (!app.audio.is_audio && (ev.a == 0x1001 || base == 0x1001)) {
                    app.origin_y += 20;
                } else if (!app.audio.is_audio && (ev.a == 0x1002 || base == 0x1002)) {
                    app.origin_y -= 20;
                } else if (!app.audio.is_audio && (ev.a == 0x1003 || base == 0x1003)) {
                    app.origin_x += 20;
                } else if (!app.audio.is_audio && (ev.a == 0x1004 || base == 0x1004)) {
                    app.origin_x -= 20;
                } else if (!app.audio.is_audio && ch == '0') {
                    reset_view_transform(&app, src_w, src_h);
                }
            } else if (ev.type == GUI_EVENT_MOUSE && !app.audio.is_audio) {
                int left_down = (ev.c & 0x1) != 0;
                int press_edge = left_down && !app.prev_left_down;
                int release_edge = (!left_down) && app.prev_left_down;

                if (ev.d > 0) {
                    zoom_view(&app, src_w, src_h, 1);
                } else if (ev.d < 0) {
                    zoom_view(&app, src_w, src_h, 0);
                }

                if (press_edge) {
                    app.dragging = 1;
                    app.drag_last_x = ev.a;
                    app.drag_last_y = ev.b;
                } else if (left_down && app.dragging) {
                    int dx = ev.a - app.drag_last_x;
                    int dy = ev.b - app.drag_last_y;
                    app.origin_x += dx;
                    app.origin_y += dy;
                    app.drag_last_x = ev.a;
                    app.drag_last_y = ev.b;
                } else if (release_edge) {
                    app.dragging = 0;
                }

                app.prev_left_down = left_down;
            }
        }

        /* ---- Audio pumping ---- */
        if (app.audio.is_audio && app.audio.playing && app.running) {
            int arc = audio_pump(&app.audio);
            if (arc == 1) {
                /* Finished playback — clean stop. */
                app.audio.played_ms = app.audio.duration_ms;
                app.audio.playing = 0;
                eyn_sys_audio_stop();
            }
            /* arc == 0: ring full or transient — retry next frame (normal). */
            /* arc < 0:  guard failed — do not kill playing; let pump retry.  */
        }

        /* ---- Video frame advance ---- */
        if (app.video.is_video && app.video.playing && app.running) {
            int rc = reiv_decode_next(&app.video);
            if (rc < 0) {
                app.running = 0;
            }
        }

        /* ---- Render ---- */
        if (app.audio.is_audio) {
            uint32_t now_ms = audio_now_ms();
            if (app.audio_ui_dirty || app.audio_ui_last_redraw_ms == 0 ||
                (now_ms - app.audio_ui_last_redraw_ms) >= 100u) {
                /* Audio player UI: dark background + progress bar + status */
                (void)gui_begin(app.handle);
                gui_rgb_t bg = { .r = VIEW_BG_R, .g = VIEW_BG_G, .b = VIEW_BG_B, ._pad = 0 };
                (void)gui_clear(app.handle, &bg);

                /* Draw a progress bar in the centre of the viewport */
                int bar_w = app.viewport_w - 40;
                int bar_h = 16;
                int bar_x = 20;
                int bar_y = app.viewport_h / 2 - bar_h / 2;
                if (bar_w < 20) bar_w = 20;

                /* Background bar */
                gui_rect_t bar_bg = {
                    .x = bar_x, .y = bar_y, .w = bar_w, .h = bar_h,
                    .r = 48, .g = 48, .b = 48, ._pad = 0
                };
                (void)gui_fill_rect(app.handle, &bar_bg);

                /* Filled portion */
                int fill_w = 0;
                if (app.audio.duration_ms > 0) {
                    uint32_t pos_ms = audio_playback_ms(&app.audio);
                    fill_w = (int)((uint64_t)pos_ms * (uint64_t)bar_w / app.audio.duration_ms);
                }
                if (fill_w > bar_w) fill_w = bar_w;
                if (fill_w > 0) {
                    gui_rect_t bar_fill = {
                        .x = bar_x, .y = bar_y, .w = fill_w, .h = bar_h,
                        .r = 130, .g = 180, .b = 255, ._pad = 0
                    };
                    (void)gui_fill_rect(app.handle, &bar_fill);
                }

                /* Audio info label */
                char info[128];
                info[0] = '\0';
                str_copy(info, (int)sizeof(info), "Audio: ");
                str_append_uint(info, (int)sizeof(info), (uint32_t)app.audio.src_rate);
                str_append(info, (int)sizeof(info), " Hz, ");
                str_append_uint(info, (int)sizeof(info), (uint32_t)app.audio.src_bits);
                str_append(info, (int)sizeof(info), "-bit, ");
                str_append(info, (int)sizeof(info), app.audio.src_channels == 1 ? "mono" : "stereo");
                str_append(info, (int)sizeof(info), " | vol ");
                str_append_uint(info, (int)sizeof(info), (uint32_t)app.audio.volume_percent);
                str_append(info, (int)sizeof(info), "%");

                gui_text_t info_text = {
                    .x = bar_x, .y = bar_y - 20,
                    .r = 222, .g = 222, .b = 222, ._pad = 0,
                    .text = info
                };
                (void)gui_draw_text(app.handle, &info_text);

                draw_status(&app, path);
                (void)gui_present(app.handle);
                app.audio_ui_last_redraw_ms = now_ms;
                app.audio_ui_dirty = 0;
            }
        } else {
            render_app(&app, path, src_pixels, src_w, src_h);
        }
        app.frame_counter += 1;

        if (app.video.is_video) {
            usleep(app.video.frame_delay_us);
        } else if (app.audio.is_audio) {
            usleep(10000);  /* ~10 ms polling interval for audio */
        } else {
            usleep(16000);
        }
    }

    if (app.audio.is_audio && app.audio.audio_inited) {
        eyn_sys_audio_stop();
    }
    (void)gui_set_continuous_redraw(app.handle, 0);
    if (app.viewbuf) free(app.viewbuf);
    reiv_close(&app.video);
    rei_free(&app.image);
    audio_free(&app.audio);
    return 0;
}
