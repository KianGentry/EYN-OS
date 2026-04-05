#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Play a REIV video file in a dedicated backend.", "view_backend_reiv /videos/demo.reiv");

#define REIV_MAGIC 0x52455600u
#define REIV_VERSION_V1 1u
#define REIV_VERSION_V2 2u
#define REIV_VERSION_V3 3u
#define REIV_PIXFMT_RGB565LE 1u
#define REIV_FLAG_LOOP_DEFAULT 0x01u

#define REIV_FRAME_FLAG_RLE565 0x00000001u
#define REIV_FRAME_FLAG_RLE8 0x00000002u
#define REIV_FRAME_FLAG_DELTA_XOR_PREV 0x00000004u

#define VIEW_STATUS_H 18
#define VIEW_BG_R GUI_PAL_BG_R
#define VIEW_BG_G GUI_PAL_BG_G
#define VIEW_BG_B GUI_PAL_BG_B

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

typedef struct {
    int handle;
    int running;

    int content_w;
    int content_h;
    int viewport_w;
    int viewport_h;
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
} app_t;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static int clampi(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

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
        if (read_exact_fd(fd, video->index, index_bytes) != 0) {
            return -1;
        }
    } else {
        if (skip_fd_bytes(fd, index_bytes) != 0) {
            return -1;
        }
    }

    video->stream_data_base = hdr.frames_offset + (uint32_t)index_bytes;
    return 0;
}

static int reiv_rewind(reiv_stream_t* video) {
    if (!video) return -1;

    video->next_frame = 0u;
    video->stream_payload_off = 0u;

    if (video->fd >= 0 && video->stream_data_base > 0u) {
        long new_off = lseek(video->fd, (long)video->stream_data_base, SEEK_SET);
        if (new_off == (long)video->stream_data_base) {
            memset(video->prev_frame, 0, (size_t)video->frame_size_bytes);
            return 0;
        }
    }

    if (video->fd >= 0) {
        close(video->fd);
        video->fd = -1;
    }

    if (!video->path[0]) return -1;

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

    uint16_t bg = rgb565(VIEW_BG_R, VIEW_BG_G, VIEW_BG_B);
    size_t total = (size_t)app->blit_w * (size_t)app->blit_h;
    for (size_t i = 0; i < total; ++i) app->viewbuf[i] = bg;

    int zoom = app->zoom_permille;
    if (zoom <= 0) zoom = 1000;

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

static int key_is_zoom_in(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '+' || ch == '=' || key == 0x2102;
}

static int key_is_zoom_out(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '-' || ch == '_' || key == 0x2103;
}

static void draw_status(app_t* app, const char* path, const reiv_stream_t* video) {
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
    uint32_t shown = video->next_frame ? video->next_frame : 1u;

    left[0] = '\0';
    str_copy(left, (int)sizeof(left), path);
    str_append(left, (int)sizeof(left), " | frame ");
    str_append_uint(left, (int)sizeof(left), shown);
    str_append(left, (int)sizeof(left), "/");
    str_append_uint(left, (int)sizeof(left), video->header.frame_count);
    str_append(left, (int)sizeof(left), " | ");
    str_append(left, (int)sizeof(left), video->playing ? "playing" : "paused");
    str_copy(right, (int)sizeof(right), "Space play/pause  +/- zoom  wheel zoom  arrows/mouse pan  Esc quit");

    gui_text_t t1 = { .x = 6, .y = app->viewport_h + 5, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = left };
    gui_text_t t2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = right };
    (void)gui_draw_text(app->handle, &t1);
    (void)gui_draw_text(app->handle, &t2);
}

static void render_video(app_t* app, const char* path, const reiv_stream_t* video) {
    if (!app || !video || !video->frame) return;

    if (ensure_viewbuf(app) != 0) return;
    render_framebuffer(app, video->frame, video->width, video->height);

    if (gui_begin(app->handle) != 0) return;

    gui_blit_rgb565_t blit = {
        .src_w = app->blit_w,
        .src_h = app->blit_h,
        .pixels = app->viewbuf,
        .dst_w = app->viewport_w,
        .dst_h = app->viewport_h,
    };
    (void)gui_blit_rgb565(app->handle, &blit);

    draw_status(app, path, video);
    (void)gui_present(app->handle);
}

static uint32_t now_ms(void) {
    return (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        puts("Usage: view_backend_reiv <input.reiv>");
        return 1;
    }

    const char* path = argv[1];

    reiv_stream_t video;
    memset(&video, 0, sizeof(video));
    video.fd = -1;

    if (reiv_open(path, &video) != 0) {
        puts("view_backend_reiv: failed to open video");
        return 1;
    }
    if (reiv_decode_next(&video) != 0) {
        puts("view_backend_reiv: failed to decode first frame");
        reiv_close(&video);
        return 1;
    }

    app_t app;
    memset(&app, 0, sizeof(app));

    app.handle = gui_create("View (REIV)", "Esc quit | Space play/pause | +/- zoom | wheel zoom | arrows/mouse pan");
    if (app.handle < 0) {
        puts("view_backend_reiv: gui_create failed");
        reiv_close(&video);
        return 1;
    }

    (void)gui_set_continuous_redraw(app.handle, 1);
    app.running = 1;

    uint32_t video_base_ms = now_ms();
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

        app.blit_w = app.viewport_w > 320 ? 320 : app.viewport_w;
        app.blit_h = app.viewport_h > 200 ? 200 : app.viewport_h;

        if (app.zoom_permille <= 0) {
            reset_view_transform(&app, video.width, video.height);
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

                if (ch == ' ') {
                    video.playing = !video.playing;
                } else if (key_is_zoom_in(ev.a)) {
                    zoom_view(&app, video.width, video.height, 1);
                } else if (key_is_zoom_out(ev.a)) {
                    zoom_view(&app, video.width, video.height, 0);
                } else if (ev.a == 0x1001 || base == 0x1001) {
                    app.origin_y += 20;
                } else if (ev.a == 0x1002 || base == 0x1002) {
                    app.origin_y -= 20;
                } else if (ev.a == 0x1003 || base == 0x1003) {
                    app.origin_x += 20;
                } else if (ev.a == 0x1004 || base == 0x1004) {
                    app.origin_x -= 20;
                } else if (ch == '0') {
                    reset_view_transform(&app, video.width, video.height);
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                int left_down = (ev.c & 0x1) != 0;
                int press_edge = left_down && !app.prev_left_down;
                int release_edge = (!left_down) && app.prev_left_down;

                if (ev.d > 0) {
                    zoom_view(&app, video.width, video.height, 1);
                } else if (ev.d < 0) {
                    zoom_view(&app, video.width, video.height, 0);
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

        if (video.playing && app.running) {
            int rc = reiv_decode_next(&video);
            if (rc < 0) {
                app.running = 0;
            }
        }

        render_video(&app, path, &video);

        uint32_t cur_frame = video.next_frame;
        if (cur_frame < video_last_frame) {
            video_base_ms = now_ms();
        }
        video_last_frame = cur_frame;

        uint32_t target_ms = video_base_ms
            + (uint32_t)(((uint64_t)cur_frame * (uint64_t)video.frame_delay_us) / 1000u);
        uint32_t now = now_ms();
        if (now < target_ms) {
            usleep((target_ms - now) * 1000u);
        }
    }

    (void)gui_set_continuous_redraw(app.handle, 0);
    if (app.viewbuf) free(app.viewbuf);
    reiv_close(&video);
    return 0;
}
