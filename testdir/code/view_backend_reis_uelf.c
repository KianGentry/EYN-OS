#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Play a REIS audio file in a dedicated backend.", "view_backend_reis /audio/track.reis");

#define REIS_MAGIC       0x52454953u
#define REIS_VERSION     1u
#define REIS_COMP_NONE   0x0u
#define REIS_COMP_RLE    0x1u
#define REIS_COMP_MASK   0x0Fu

#define AC97_RATE     48000u
#define AC97_CHANNELS 2u
#define AC97_BITS     16u
#define AC97_DMA_BUF  4096u

/* Keep below the 64KB userland stack budget. */
#define AUDIO_READAHEAD 32768u

#define VIEW_STATUS_H 18
#define VIEW_BG_R GUI_PAL_BG_R
#define VIEW_BG_G GUI_PAL_BG_G
#define VIEW_BG_B GUI_PAL_BG_B

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

typedef struct {
    int is_audio;
    int playing;
    int audio_inited;
    int volume_percent;
    uint32_t play_started_ms;
    uint32_t played_ms;

    uint8_t* pcm;
    size_t pcm_size;

    int src_fd;
    uint32_t src_data_offset;
    uint32_t src_frame_count;

    int16_t* out_buf;
    size_t out_frames;
    size_t out_size;

    uint32_t src_rate;
    uint8_t src_channels;
    uint8_t src_bits;

    size_t cursor;
    uint32_t duration_ms;

    uint8_t  ra_buf[AUDIO_READAHEAD];
    size_t   ra_len;
    size_t   ra_pos;
    uint32_t ra_error;
} audio_state_t;

typedef struct {
    int handle;
    int running;

    int content_w;
    int content_h;
    int viewport_w;
    int viewport_h;

    uint32_t ui_last_redraw_ms;
    int ui_dirty;

    audio_state_t audio;
} app_t;

static void str_copy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_append(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0 || !src) return;
    int n = (int)strlen(dst);
    if (n >= cap - 1) return;
    int i = 0;
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
    uint32_t pos_ms = (uint32_t)((uint64_t)a->cursor * 1000u / AC97_RATE);
    if (a->duration_ms > 0 && pos_ms > a->duration_ms) pos_ms = a->duration_ms;
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

static int audio_resample_to_ac97(audio_state_t* a) {
    if (!a) return -1;
    if (a->src_rate == 0 || a->src_channels == 0 || a->src_bits == 0) return -1;

    uint32_t src_frame_bytes = (uint32_t)a->src_channels * ((uint32_t)a->src_bits / 8u);
    if (src_frame_bytes == 0) return -1;

    uint32_t src_frames;
    if (a->src_fd >= 0 && !a->pcm) {
        src_frames = a->src_frame_count;
    } else {
        if (!a->pcm || a->pcm_size == 0) return -1;
        src_frames = (uint32_t)(a->pcm_size / src_frame_bytes);
    }
    if (src_frames == 0) return -1;

    uint64_t out_frames64 = ((uint64_t)src_frames * AC97_RATE + a->src_rate - 1u) / a->src_rate;
    if (out_frames64 == 0) return -1;

    a->out_frames = (size_t)out_frames64;
    a->out_size = a->out_frames * AC97_CHANNELS * sizeof(int16_t);
    a->out_buf = NULL;
    a->duration_ms = (uint32_t)((uint64_t)a->out_frames * 1000u / AC97_RATE);

    return 0;
}

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
        if (data_offset > (uint32_t)sizeof(hdr)) {
            if (skip_fd_bytes(fd, (size_t)(data_offset - (uint32_t)sizeof(hdr))) != 0) {
                close(fd);
                return -1;
            }
        }
        a->src_fd = fd;
        a->src_data_offset = data_offset;
        a->pcm = NULL;
        a->pcm_size = 0;
    } else if (comp == REIS_COMP_RLE) {
        uint32_t pcm_bytes = hdr.frame_count * (uint32_t)hdr.channels * ((uint32_t)hdr.bits / 8u);
        if (hdr.data_offset > sizeof(hdr)) {
            if (skip_fd_bytes(fd, (size_t)(hdr.data_offset - (uint32_t)sizeof(hdr))) != 0) {
                close(fd);
                return -1;
            }
        }
        uint8_t* payload = (uint8_t*)malloc(hdr.data_size);
        if (!payload) {
            close(fd);
            return -1;
        }
        if (read_exact_fd(fd, payload, hdr.data_size) != 0) {
            free(payload);
            close(fd);
            return -1;
        }
        close(fd);

        a->pcm = (uint8_t*)malloc(pcm_bytes);
        if (!a->pcm) {
            free(payload);
            return -1;
        }

        int unit = (int)((uint32_t)hdr.channels * ((uint32_t)hdr.bits / 8u));
        if (rle_decode_packbits(payload, hdr.data_size, a->pcm, pcm_bytes, unit) != 0) {
            free(a->pcm);
            a->pcm = NULL;
            free(payload);
            return -1;
        }

        a->pcm_size = pcm_bytes;
        free(payload);
        a->src_fd = -1;
    } else {
        close(fd);
        return -1;
    }

    return audio_resample_to_ac97(a);
}

static int audio_pump(audio_state_t* a) {
    if (!a || !a->is_audio || !a->playing) return -1;
    if (!a->pcm && a->src_fd < 0) return -1;

    uint32_t src_frame_bytes = (uint32_t)a->src_channels * ((uint32_t)a->src_bits / 8u);
    if (src_frame_bytes == 0) return -1;

    uint32_t src_frames = a->pcm ? (uint32_t)(a->pcm_size / src_frame_bytes)
                                 : a->src_frame_count;

    size_t frames_per_buf = AC97_DMA_BUF / (AC97_CHANNELS * sizeof(int16_t));

    if (a->src_fd >= 0 && !a->pcm &&
        a->src_rate == AC97_RATE && a->src_channels == 2u &&
        a->src_bits == 16u && a->volume_percent == 100) {
        if (a->cursor >= a->out_frames) return 1;

        size_t avail = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
        if (avail < AC97_DMA_BUF) {
            size_t leftover = avail;
            if (leftover > 0) memmove(a->ra_buf, a->ra_buf + a->ra_pos, leftover);
            a->ra_pos = 0;
            a->ra_len = leftover;

            size_t want = AUDIO_READAHEAD - a->ra_len;
            int got = (int)read(a->src_fd, a->ra_buf + a->ra_len, (int)want);
            if (got > 0) a->ra_len += (size_t)got;
            avail = a->ra_len;
        }

        size_t frames_left = a->out_frames - a->cursor;
        size_t bytes_left = frames_left * AC97_CHANNELS * sizeof(int16_t);
        size_t submit = avail < bytes_left ? avail : bytes_left;
        size_t aligned = submit & ~(size_t)(AC97_DMA_BUF - 1u);
        if (aligned == 0 && submit > 0) aligned = submit;
        if (aligned == 0) return 1;

        int count = eyn_sys_audio_write_bulk(a->ra_buf + a->ra_pos, (int)aligned);
        if (count <= 0) return 0;

        size_t bytes_queued = (size_t)count * AC97_DMA_BUF;
        if (bytes_queued > aligned) bytes_queued = aligned;
        size_t frames_queued = bytes_queued / (AC97_CHANNELS * sizeof(int16_t));

        a->ra_pos += bytes_queued;
        a->cursor += frames_queued;
        return (a->cursor >= a->out_frames) ? 1 : 0;
    }

    while (1) {
        if (a->cursor >= a->out_frames) return 1;

        size_t remaining = a->out_frames - a->cursor;
        size_t to_send = remaining < frames_per_buf ? remaining : frames_per_buf;
        if (to_send == 0) return 1;

        int16_t chunk_buf[AC97_DMA_BUF / sizeof(int16_t)];
        size_t ra_advance = 0;
        uint32_t new_ra_error = a->ra_error;

        if (a->src_fd >= 0 && !a->pcm) {
            uint64_t src_last64 = (uint64_t)(a->cursor + to_send - 1u) * a->src_rate;
            uint32_t src_last = (uint32_t)(src_last64 / AC97_RATE);
            uint64_t src_start64 = (uint64_t)a->cursor * a->src_rate;
            uint32_t src_start = (uint32_t)(src_start64 / AC97_RATE);
            uint32_t src_count = src_last - src_start + 1u;
            size_t src_bytes = (size_t)src_count * src_frame_bytes;

            if (src_bytes > AUDIO_READAHEAD) return -1;

            if (a->ra_pos + src_bytes > a->ra_len) {
                size_t leftover = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
                if (leftover > 0) memmove(a->ra_buf, a->ra_buf + a->ra_pos, leftover);
                a->ra_pos = 0;
                a->ra_len = leftover;

                size_t want = AUDIO_READAHEAD - a->ra_len;
                int got = (int)read(a->src_fd, a->ra_buf + a->ra_len, (int)want);
                if (got > 0) a->ra_len += (size_t)got;
            }

            const uint8_t* src_ptr = a->ra_buf + a->ra_pos;
            size_t avail = (a->ra_len > a->ra_pos) ? (a->ra_len - a->ra_pos) : 0;
            size_t si = 0;
            uint32_t error = a->ra_error;

            for (size_t i = 0; i < to_send; ++i) {
                size_t byte_off = si * src_frame_bytes;
                int16_t left = 0;
                int16_t right = 0;

                if (byte_off + src_frame_bytes <= avail) {
                    if (a->src_bits == 16) {
                        const int16_t* fp = (const int16_t*)(src_ptr + byte_off);
                        left = fp[0];
                        right = (a->src_channels >= 2u) ? fp[1] : left;
                    } else {
                        const uint8_t* fp = src_ptr + byte_off;
                        left = (int16_t)((int)fp[0] - 128) * 256;
                        right = (a->src_channels >= 2u)
                                ? (int16_t)((int)fp[1] - 128) * 256
                                : left;
                    }
                }

                left = audio_apply_volume(left, a->volume_percent);
                right = audio_apply_volume(right, a->volume_percent);
                chunk_buf[i * 2 + 0] = left;
                chunk_buf[i * 2 + 1] = right;

                error += a->src_rate;
                while (error >= AC97_RATE) {
                    error -= AC97_RATE;
                    si++;
                }
            }

            size_t src_consumed = si * src_frame_bytes;
            if (src_consumed > avail) src_consumed = avail;
            ra_advance = src_consumed;
            new_ra_error = error;
        } else {
            uint64_t src_pos64 = (uint64_t)a->cursor * a->src_rate;
            uint32_t src_idx = (uint32_t)(src_pos64 / AC97_RATE);
            uint32_t error = (uint32_t)(src_pos64 % AC97_RATE);

            if (a->src_rate == AC97_RATE && a->src_channels == 2u &&
                a->src_bits == 16u && a->volume_percent == 100) {
                memcpy(chunk_buf,
                       a->pcm + a->cursor * (AC97_CHANNELS * sizeof(int16_t)),
                       to_send * AC97_CHANNELS * sizeof(int16_t));
            } else {
                for (size_t i = 0; i < to_send; ++i) {
                    if (src_idx >= src_frames) src_idx = src_frames - 1u;

                    int16_t left = 0;
                    int16_t right = 0;
                    if (a->src_bits == 16) {
                        const int16_t* fp = (const int16_t*)(a->pcm + src_idx * src_frame_bytes);
                        left = fp[0];
                        right = (a->src_channels >= 2u) ? fp[1] : left;
                    } else {
                        const uint8_t* fp = a->pcm + src_idx * src_frame_bytes;
                        left = (int16_t)((int)fp[0] - 128) * 256;
                        right = (a->src_channels >= 2u)
                                ? (int16_t)((int)fp[1] - 128) * 256
                                : left;
                    }

                    left = audio_apply_volume(left, a->volume_percent);
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
        if (rc < 0) return 0;

        a->ra_pos += ra_advance;
        a->ra_error = new_ra_error;
        a->cursor += to_send;
    }
}

static int key_is_zoom_in(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '+' || ch == '=' || key == 0x2102;
}

static int key_is_zoom_out(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '-' || ch == '_' || key == 0x2103;
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

    uint32_t pos_ms = audio_playback_ms(&app->audio);
    uint32_t dur_ms = app->audio.duration_ms;

    char left[128];
    char right[128];
    left[0] = '\0';
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

    gui_text_t t1 = { .x = 6, .y = app->viewport_h + 5, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = left };
    gui_text_t t2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = right };
    (void)gui_draw_text(app->handle, &t1);
    (void)gui_draw_text(app->handle, &t2);
}

static void render_audio_ui(app_t* app, const char* path) {
    if (!app) return;

    (void)gui_begin(app->handle);

    gui_rgb_t bg = { .r = VIEW_BG_R, .g = VIEW_BG_G, .b = VIEW_BG_B, ._pad = 0 };
    (void)gui_clear(app->handle, &bg);

    int bar_w = app->viewport_w - 40;
    int bar_h = 16;
    int bar_x = 20;
    int bar_y = app->viewport_h / 2 - bar_h / 2;
    if (bar_w < 20) bar_w = 20;

    gui_rect_t bar_bg = {
        .x = bar_x,
        .y = bar_y,
        .w = bar_w,
        .h = bar_h,
        .r = GUI_PAL_SURFACE_R,
        .g = GUI_PAL_SURFACE_G,
        .b = GUI_PAL_SURFACE_B,
        ._pad = 0
    };
    (void)gui_fill_rect(app->handle, &bar_bg);

    int fill_w = 0;
    if (app->audio.duration_ms > 0) {
        uint32_t pos_ms = audio_playback_ms(&app->audio);
        fill_w = (int)((uint64_t)pos_ms * (uint64_t)bar_w / app->audio.duration_ms);
    }
    if (fill_w > bar_w) fill_w = bar_w;
    if (fill_w > 0) {
        gui_rect_t bar_fill = {
            .x = bar_x,
            .y = bar_y,
            .w = fill_w,
            .h = bar_h,
            .r = GUI_PAL_ACCENT_R,
            .g = GUI_PAL_ACCENT_G,
            .b = GUI_PAL_ACCENT_B,
            ._pad = 0
        };
        (void)gui_fill_rect(app->handle, &bar_fill);
    }

    char info[128];
    info[0] = '\0';
    str_copy(info, (int)sizeof(info), "Audio: ");
    str_append_uint(info, (int)sizeof(info), (uint32_t)app->audio.src_rate);
    str_append(info, (int)sizeof(info), " Hz, ");
    str_append_uint(info, (int)sizeof(info), (uint32_t)app->audio.src_bits);
    str_append(info, (int)sizeof(info), "-bit, ");
    str_append(info, (int)sizeof(info), app->audio.src_channels == 1 ? "mono" : "stereo");
    str_append(info, (int)sizeof(info), " | vol ");
    str_append_uint(info, (int)sizeof(info), (uint32_t)app->audio.volume_percent);
    str_append(info, (int)sizeof(info), "%");

    gui_text_t info_text = {
        .x = bar_x,
        .y = bar_y - 20,
        .r = GUI_PAL_TEXT_R,
        .g = GUI_PAL_TEXT_G,
        .b = GUI_PAL_TEXT_B,
        ._pad = 0,
        .text = info
    };
    (void)gui_draw_text(app->handle, &info_text);

    draw_status(app, path);
    (void)gui_present(app->handle);
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        puts("Usage: view_backend_reis <input.reis>");
        return 1;
    }

    const char* path = argv[1];

    /* Keep large read-ahead buffer off the stack. */
    static app_t app;
    memset(&app, 0, sizeof(app));
    app.audio.src_fd = -1;

    if (audio_load_reis(path, &app.audio) != 0) {
        puts("view_backend_reis: failed to load audio");
        return 1;
    }

    if (eyn_sys_audio_probe() != 0) {
        puts("view_backend_reis: no audio hardware detected");
        audio_free(&app.audio);
        return 1;
    }
    if (eyn_sys_audio_init() != 0) {
        puts("view_backend_reis: failed to initialize audio hardware");
        audio_free(&app.audio);
        return 1;
    }

    app.audio.audio_inited = 1;
    app.audio.playing = 1;
    app.audio.play_started_ms = audio_now_ms();
    app.audio.played_ms = 0;
    app.ui_last_redraw_ms = 0;
    app.ui_dirty = 1;

    app.handle = gui_create("View (REIS)", "Esc quit | Space play/pause | +/- volume");
    if (app.handle < 0) {
        puts("view_backend_reis: gui_create failed");
        eyn_sys_audio_stop();
        audio_free(&app.audio);
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

        gui_event_t ev;
        while (gui_poll_event(app.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                app.running = 0;
                break;
            }
            if (ev.type == GUI_EVENT_KEY) {
                unsigned ch = (unsigned)ev.a & 0xFFu;
                if (ev.a == 27 || ch == 27u || ch == 'q' || ch == 'Q') {
                    app.running = 0;
                    break;
                }
                if (ch == ' ') {
                    if (app.audio.playing) {
                        app.audio.played_ms = audio_playback_ms(&app.audio);
                        app.audio.playing = 0;
                        eyn_sys_audio_stop();
                    } else {
                        app.audio.play_started_ms = audio_now_ms();
                        app.audio.playing = 1;
                    }
                    app.ui_dirty = 1;
                } else if (key_is_zoom_in(ev.a)) {
                    if (app.audio.volume_percent < 400) app.audio.volume_percent += 10;
                    app.ui_dirty = 1;
                } else if (key_is_zoom_out(ev.a)) {
                    if (app.audio.volume_percent > 0) app.audio.volume_percent -= 10;
                    app.ui_dirty = 1;
                }
            }
        }

        if (app.audio.playing && app.running) {
            int rc = audio_pump(&app.audio);
            if (rc == 1) {
                app.audio.played_ms = app.audio.duration_ms;
                app.audio.playing = 0;
                eyn_sys_audio_stop();
                app.ui_dirty = 1;
            }
        }

        uint32_t now = audio_now_ms();
        if (app.ui_dirty || app.ui_last_redraw_ms == 0 || (now - app.ui_last_redraw_ms) >= 100u) {
            render_audio_ui(&app, path);
            app.ui_last_redraw_ms = now;
            app.ui_dirty = 0;
        }

        usleep(10000);
    }

    if (app.audio.audio_inited) {
        eyn_sys_audio_stop();
    }
    (void)gui_set_continuous_redraw(app.handle, 0);
    audio_free(&app.audio);
    return 0;
}
