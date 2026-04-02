#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "view_backend_protocol.h"

#define BMP_MAGIC_B0 0x42u
#define BMP_MAGIC_B1 0x4Du
#define BMP_BI_RGB 0u
#define BMP_BI_BITFIELDS 3u
#define BMP_FILEHEADER_SIZE 14u
#define BMP_INFOHEADER_MIN 40u

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
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

static int decode_bmp_to_rgb565(const char* path, uint16_t** out_pixels, uint16_t* out_w, uint16_t* out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    uint8_t fh[BMP_FILEHEADER_SIZE];
    if (read_exact_fd(fd, fh, BMP_FILEHEADER_SIZE) != 0) { close(fd); return -1; }
    if (fh[0] != BMP_MAGIC_B0 || fh[1] != BMP_MAGIC_B1) { close(fd); return -1; }

    uint32_t bf_off_bits = (uint32_t)fh[10] | ((uint32_t)fh[11] << 8)
                         | ((uint32_t)fh[12] << 16) | ((uint32_t)fh[13] << 24);

    uint8_t ih[124];
    memset(ih, 0, sizeof(ih));
    if (read_exact_fd(fd, ih, 4) != 0) { close(fd); return -1; }
    uint32_t hdr_size = (uint32_t)ih[0] | ((uint32_t)ih[1] << 8)
                      | ((uint32_t)ih[2] << 16) | ((uint32_t)ih[3] << 24);
    if (hdr_size < BMP_INFOHEADER_MIN || hdr_size > sizeof(ih)) { close(fd); return -1; }
    if (read_exact_fd(fd, ih + 4, hdr_size - 4) != 0) { close(fd); return -1; }

    int32_t bi_width = (int32_t)((uint32_t)ih[4] | ((uint32_t)ih[5] << 8)
                   | ((uint32_t)ih[6] << 16) | ((uint32_t)ih[7] << 24));
    int32_t bi_height = (int32_t)((uint32_t)ih[8] | ((uint32_t)ih[9] << 8)
                    | ((uint32_t)ih[10] << 16) | ((uint32_t)ih[11] << 24));
    uint16_t bi_bit_count = (uint16_t)((uint16_t)ih[14] | ((uint16_t)ih[15] << 8));
    uint32_t bi_compression = (uint32_t)ih[16] | ((uint32_t)ih[17] << 8)
                            | ((uint32_t)ih[18] << 16) | ((uint32_t)ih[19] << 24);

    if (bi_width <= 0 || bi_width > 4096) { close(fd); return -1; }

    int top_down = 0;
    int height;
    if (bi_height < 0) {
        top_down = 1;
        height = -bi_height;
    } else {
        height = bi_height;
    }

    if (height <= 0 || height > 4096) { close(fd); return -1; }

    int bytes_per_pixel;
    if (bi_bit_count == 24 && bi_compression == BMP_BI_RGB) {
        bytes_per_pixel = 3;
    } else if (bi_bit_count == 32 &&
               (bi_compression == BMP_BI_RGB || bi_compression == BMP_BI_BITFIELDS)) {
        bytes_per_pixel = 4;
    } else {
        close(fd);
        return -1;
    }

    int width = (int)bi_width;
    size_t row_bytes_raw = (size_t)width * (size_t)bytes_per_pixel;
    size_t row_stride = (row_bytes_raw + 3u) & ~(size_t)3u;

    size_t header_total = BMP_FILEHEADER_SIZE + hdr_size;
    if (bf_off_bits > header_total) {
        if (skip_fd_bytes(fd, bf_off_bits - header_total) != 0) { close(fd); return -1; }
    }

    size_t px_count = (size_t)width * (size_t)height;
    uint16_t* rgb = (uint16_t*)malloc(px_count * sizeof(uint16_t));
    if (!rgb) { close(fd); return -1; }

    uint8_t* row_buf = (uint8_t*)malloc(row_stride);
    if (!row_buf) {
        free(rgb);
        close(fd);
        return -1;
    }

    for (int row = 0; row < height; ++row) {
        if (read_exact_fd(fd, row_buf, row_stride) != 0) {
            free(row_buf);
            free(rgb);
            close(fd);
            return -1;
        }

        int dest_row = top_down ? row : (height - 1 - row);
        uint16_t* dst = rgb + (size_t)dest_row * (size_t)width;
        for (int col = 0; col < width; ++col) {
            size_t off = (size_t)col * (size_t)bytes_per_pixel;
            uint8_t b = row_buf[off + 0];
            uint8_t g = row_buf[off + 1];
            uint8_t r = row_buf[off + 2];
            dst[col] = rgb565(r, g, b);
        }
    }

    free(row_buf);
    close(fd);

    *out_pixels = rgb;
    *out_w = (uint16_t)width;
    *out_h = (uint16_t)height;
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
        puts("Usage: view_backend_bmp <input.bmp> <output.frame>");
        return 1;
    }

    uint16_t* pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;

    if (decode_bmp_to_rgb565(argv[1], &pixels, &width, &height) != 0) {
        puts("view_backend_bmp: decode failed");
        return 1;
    }

    int rc = write_frame_file(argv[2], width, height, pixels);
    free(pixels);

    if (rc != 0) {
        printf("view_backend_bmp: frame write failed (%s)\n", argv[2]);
        return 1;
    }

    return 0;
}
