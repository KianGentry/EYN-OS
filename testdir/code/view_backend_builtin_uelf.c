#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Open an REI, BMP image, REIV video, REIS audio, or WAV audio viewer.", "view /images/picture.rei");

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

/* BMP / DIB magic: first two bytes are always 'B','M' (0x42, 0x4D). */
#define BMP_MAGIC_B0     0x42u
#define BMP_MAGIC_B1     0x4Du
#define BMP_BI_RGB       0u   /* uncompressed */
#define BMP_BI_BITFIELDS 3u   /* channel masks in header */
#define BMP_FILEHEADER_SIZE 14u
#define BMP_INFOHEADER_MIN  40u

/* AC97 output format: 48 kHz stereo 16-bit signed LE */
#define AC97_RATE     48000u
#define AC97_CHANNELS 2u
#define AC97_BITS     16u
#define AC97_DMA_BUF  4096u

#define VIEW_STATUS_H 18
#define VIEW_BG_R GUI_PAL_BG_R
#define VIEW_BG_G GUI_PAL_BG_G
#define VIEW_BG_B GUI_PAL_BG_B

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
     * Source PCM -- two modes:
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
    int16_t*  out_buf;       /* pre-resampled buffer (NULL -- not used) */
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
     * ra_buf   -- raw source bytes
     * ra_len   -- bytes currently valid in ra_buf
     * ra_pos   -- read cursor within ra_buf (bytes consumed so far)
     * ra_error -- Bresenham fractional error carried between refills
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

/* Backends are split into .view modules and compiled into this frontend. */
#include "../.view/bmp.backend.inc"
#include "../.view/rei.backend.inc"
#include "../.view/reiv.backend.inc"

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

#include "../.view/reis.backend.inc"
#include "../.view/wav.backend.inc"

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
         * file), include it -- the kernel will zero-pad the last DMA
         * buffer.  This avoids dropping the final fraction of audio.
         */
        if (aligned == 0 && submit > 0)
            aligned = submit;

        if (aligned == 0) return 1;  /* nothing left */

        int count = eyn_sys_audio_write_bulk(
                        a->ra_buf + a->ra_pos, (int)aligned);
        if (count <= 0) return 0;  /* ring full -- retry next frame */

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
         * prevents ra_pos from advancing when the ring is full --
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
            /* Record advance amounts -- applied only after successful write. */
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
            return 0;  /* ring full -- come back next render frame */

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

    /*
     * Precompute source-X for each blit column once, outside the row loop.
     * The inner loop previously did two integer divisions per pixel: one for
     * the blit→viewport column scale and one for the viewport→source zoom.
     * Both depend only on bx (not by), so they can be hoisted entirely.
     * blit_w is ≤320 by construction; sx_table is indexed [0, blit_w).
     */
    int sx_table[320];
    int ncols = (app->blit_w < 320) ? app->blit_w : 320;
    for (int bx = 0; bx < ncols; ++bx) {
        int vpx = (app->blit_w == app->viewport_w)
                  ? bx
                  : (bx * app->viewport_w / app->blit_w);
        int sx = ((vpx - app->origin_x) * 1000) / zoom;
        sx_table[bx] = (sx >= 0 && sx < src_w) ? sx : -1;
    }

    for (int by = 0; by < app->blit_h; ++by) {
        /* Map blit row → viewport row (integer nearest-neighbour downscale) */
        int vpy = (app->blit_h == app->viewport_h)
                  ? by
                  : (by * app->viewport_h / app->blit_h);
        int sy = ((vpy - app->origin_y) * 1000) / zoom;
        if (sy < 0 || sy >= src_h) continue;
        size_t row_off = (size_t)by * (size_t)app->blit_w;
        size_t src_row = (size_t)sy * (size_t)src_w;
        for (int bx = 0; bx < ncols; ++bx) {
            int sx = sx_table[bx];
            if (sx < 0) continue;
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
        .r = GUI_PAL_STATUS_R,
        .g = GUI_PAL_STATUS_G,
        .b = GUI_PAL_STATUS_B,
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

    gui_text_t t1 = { .x = 6, .y = app->viewport_h + 5, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = left };
    gui_text_t t2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = right };
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

typedef enum {
    VIEW_BACKEND_NONE = 0,
    VIEW_BACKEND_REI,
    VIEW_BACKEND_REIV,
    VIEW_BACKEND_REIS,
    VIEW_BACKEND_WAV,
    VIEW_BACKEND_BMP,
} view_backend_id_t;

typedef struct {
    int dir_available;
    int rei;
    int reiv;
    int reis;
    int wav;
    int bmp;
} view_backend_registry_t;

static const char* view_backend_name(view_backend_id_t id) {
    switch (id) {
        case VIEW_BACKEND_REI: return "rei";
        case VIEW_BACKEND_REIV: return "reiv";
        case VIEW_BACKEND_REIS: return "reis";
        case VIEW_BACKEND_WAV: return "wav";
        case VIEW_BACKEND_BMP: return "bmp";
        default: return "none";
    }
}

static void view_scan_backends(view_backend_registry_t* reg) {
    if (!reg) return;
    memset(reg, 0, sizeof(*reg));

    int fd = open("/.view", O_RDONLY, 0);
    if (fd < 0) return;
    reg->dir_available = 1;

    typedef struct {
        uint8_t is_dir;
        uint8_t _pad[3];
        uint32_t size;
        char name[56];
    } view_dirent_t;

    view_dirent_t entries[16];
    for (;;) {
        int bytes = getdents(fd, (void*)entries, sizeof(entries));
        if (bytes <= 0) break;
        int count = bytes / (int)sizeof(view_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (entries[i].is_dir || !entries[i].name[0]) continue;
            if (strcmp(entries[i].name, "rei.backend") == 0) reg->rei = 1;
            else if (strcmp(entries[i].name, "reiv.backend") == 0) reg->reiv = 1;
            else if (strcmp(entries[i].name, "reis.backend") == 0) reg->reis = 1;
            else if (strcmp(entries[i].name, "wav.backend") == 0) reg->wav = 1;
            else if (strcmp(entries[i].name, "bmp.backend") == 0) reg->bmp = 1;
        }
    }
    close(fd);
}

static void view_print_available_backends(const view_backend_registry_t* reg) {
    if (!reg || !reg->dir_available) {
        puts("view: backend directory /.view not found");
        return;
    }

    puts("view: installed backends in /.view:");
    if (reg->rei) puts("  - rei");
    if (reg->reiv) puts("  - reiv");
    if (reg->reis) puts("  - reis");
    if (reg->wav) puts("  - wav");
    if (reg->bmp) puts("  - bmp");
}

static view_backend_id_t view_select_backend(const char* path, const view_backend_registry_t* reg) {
    if (!path || !reg) return VIEW_BACKEND_NONE;

    uint32_t magic = probe_file_magic(path);
    int ext_rei = str_ends_with(path, ".rei");
    int ext_reiv = str_ends_with(path, ".reiv");
    int ext_reis = str_ends_with(path, ".reis");
    int ext_wav = str_ends_with(path, ".wav");
    int ext_bmp = str_ends_with(path, ".bmp");

    if (reg->reis && (magic == REIS_MAGIC || ext_reis)) return VIEW_BACKEND_REIS;
    if (reg->wav && ((magic == WAV_RIFF_MAGIC && probe_is_wav(path)) || ext_wav)) return VIEW_BACKEND_WAV;
    if (reg->reiv && (magic == REIV_MAGIC || ext_reiv)) return VIEW_BACKEND_REIV;
    if (reg->bmp && (ext_bmp || probe_is_bmp(path))) return VIEW_BACKEND_BMP;
    if (reg->rei && (magic == REI_MAGIC || ext_rei)) return VIEW_BACKEND_REI;
    return VIEW_BACKEND_NONE;
}

static void usage(void) {
    puts("Usage: view <file>");
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

    view_backend_registry_t backend_reg;
    view_scan_backends(&backend_reg);

    view_backend_id_t backend = view_select_backend(path, &backend_reg);
    if (backend == VIEW_BACKEND_NONE) {
        puts("view: no backend in /.view supports this file");
        view_print_available_backends(&backend_reg);
        return 1;
    }

    if (backend == VIEW_BACKEND_REIS || backend == VIEW_BACKEND_WAV) {
        int load_rc = (backend == VIEW_BACKEND_REIS)
            ? audio_load_reis(path, &app.audio)
            : audio_load_wav(path, &app.audio);
        if (load_rc != 0) {
            printf("view: backend '%s' failed to load audio\n", view_backend_name(backend));
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
    } else if (backend == VIEW_BACKEND_REIV) {
        if (reiv_open(path, &app.video) != 0) {
            puts("view: backend 'reiv' failed to open video");
            return 1;
        }
        if (reiv_decode_next(&app.video) != 0) {
            puts("view: backend 'reiv' failed to decode first frame");
            reiv_close(&app.video);
            return 1;
        }
    } else if (backend == VIEW_BACKEND_BMP) {
        if (bmp_load_file(path, &app.image) != 0) {
            puts("view: backend 'bmp' failed to load image");
            return 1;
        }
    } else if (backend == VIEW_BACKEND_REI) {
        if (rei_load_file(path, &app.image) != 0) {
            puts("view: backend 'rei' failed to load image");
            return 1;
        }
    } else {
        puts("view: backend selected but no handler is implemented");
        view_print_available_backends(&backend_reg);
            return 1;
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

    /*
     * Deadline-based video frame pacing.
     *
     * Instead of sleeping a fixed frame_delay_us after every frame (which
     * accumulates work_time as drift), we record when playback began and
     * compute the wall-clock deadline for each frame.  Sleep only the
     * remaining time to hit that deadline.  If we're already past it
     * (decode + render took too long), skip the sleep entirely.
     *
     * On loop/rewind (next_frame wraps back), reset the base so the
     * new cycle starts fresh.
     */
    uint32_t video_base_ms = audio_now_ms();
    uint32_t video_last_frame = 0;

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
                /* Finished playback -- clean stop. */
                app.audio.played_ms = app.audio.duration_ms;
                app.audio.playing = 0;
                eyn_sys_audio_stop();
            }
            /* arc == 0: ring full or transient -- retry next frame (normal). */
            /* arc < 0:  guard failed -- do not kill playing; let pump retry.  */
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
                    .r = GUI_PAL_SURFACE_R, .g = GUI_PAL_SURFACE_G, .b = GUI_PAL_SURFACE_B, ._pad = 0
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
                        .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0
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
                    .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0,
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
            uint32_t cur_frame = app.video.next_frame;
            if (cur_frame < video_last_frame) {
                /* Rewind occurred -- reset deadline base. */
                video_base_ms = audio_now_ms();
            }
            video_last_frame = cur_frame;
            uint32_t target_ms = video_base_ms
                + (uint32_t)(((uint64_t)cur_frame * (uint64_t)app.video.frame_delay_us) / 1000u);
            uint32_t now = audio_now_ms();
            if (now < target_ms) {
                usleep((target_ms - now) * 1000u);
            }
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
