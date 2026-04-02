#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "view_backend_protocol.h"

#define REI_MAGIC 0x52454900u
#define REI_DEPTH_MONO 1u
#define REI_DEPTH_RGB 3u
#define REI_DEPTH_RGBA 4u
#define REI_COMP_NONE 0x0u
#define REI_COMP_RLE  0x1u
#define REI_COMP_MASK 0x0Fu

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t reserved1;
    uint16_t reserved2;
} rei_header_t;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static int read_full_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t cap = 64u * 1024u;
    size_t total = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }

    for (;;) {
        if (total == cap) {
            size_t next = cap * 2u;
            if (next > (4u * 1024u * 1024u)) {
                free(buf);
                close(fd);
                return -1;
            }
            uint8_t* grown = (uint8_t*)realloc(buf, next);
            if (!grown) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = grown;
            cap = next;
        }

        ssize_t got = read(fd, buf + total, cap - total);
        if (got < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (got == 0) break;
        total += (size_t)got;
    }

    close(fd);
    *out_buf = buf;
    *out_len = total;
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

static int stream_write_all(int handle, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = eynfs_stream_write(handle, p + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int decode_rei_to_rgb565(const char* path, uint16_t** out_pixels, uint16_t* out_w, uint16_t* out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;

    uint8_t* file_buf = NULL;
    size_t file_len = 0;
    if (read_full_file(path, &file_buf, &file_len) != 0) return -1;

    if (file_len < sizeof(rei_header_t)) {
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

    uint8_t* pixel_bytes = (uint8_t*)malloc(in_expected);
    if (!pixel_bytes) {
        free(file_buf);
        return -1;
    }

    uint8_t comp = hdr.reserved1 & REI_COMP_MASK;
    const uint8_t* in = file_buf + sizeof(rei_header_t);
    size_t in_len = file_len - sizeof(rei_header_t);

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

    uint16_t* rgb = (uint16_t*)malloc(px_count * sizeof(uint16_t));
    if (!rgb) {
        free(pixel_bytes);
        free(file_buf);
        return -1;
    }

    for (size_t i = 0; i < px_count; ++i) {
        size_t off = i * (size_t)hdr.depth;
        uint8_t r;
        uint8_t g;
        uint8_t b;

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

    *out_pixels = rgb;
    *out_w = hdr.width;
    *out_h = hdr.height;
    return 0;
}

static int write_frame_file(const char* out_path, uint16_t width, uint16_t height, const uint16_t* pixels) {
    if (!out_path || !pixels || width == 0 || height == 0) return -1;

    uint32_t data_bytes = (uint32_t)((uint32_t)width * (uint32_t)height * 2u);
    view_frame_header_t hdr;
    hdr.magic = VIEW_FRAME_MAGIC;
    hdr.width = width;
    hdr.height = height;
    hdr.format = VIEW_FRAME_FMT_RGB565;
    hdr.reserved = 0;
    hdr.data_bytes = data_bytes;

    int handle = eynfs_stream_begin(out_path);
    if (handle < 0) {
        return -1;
    }

    if (stream_write_all(handle, &hdr, sizeof(hdr)) != 0) {
        (void)eynfs_stream_end(handle);
        return -1;
    }

    if (stream_write_all(handle, pixels, (size_t)data_bytes) != 0) {
        (void)eynfs_stream_end(handle);
        return -1;
    }

    if (eynfs_stream_end(handle) != 0) {
        return -1;
    }

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3 || !argv[1] || !argv[1][0] || !argv[2] || !argv[2][0]) {
        puts("Usage: view_backend_rei <input.rei> <output.frame>");
        return 1;
    }

    uint16_t* pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;

    if (decode_rei_to_rgb565(argv[1], &pixels, &width, &height) != 0) {
        puts("view_backend_rei: decode failed");
        return 1;
    }

    int rc = write_frame_file(argv[2], width, height, pixels);
    free(pixels);

    if (rc != 0) {
        printf("view_backend_rei: frame write failed (%s)\n", argv[2]);
        return 1;
    }

    return 0;
}
