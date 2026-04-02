/* BMP backend: probes BMP signature and decodes to RGB565. */
static int probe_is_bmp(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    uint8_t sig[2];
    ssize_t n = read(fd, sig, 2);
    close(fd);
    if (n < 2) return 0;
    return (sig[0] == BMP_MAGIC_B0 && sig[1] == BMP_MAGIC_B1) ? 1 : 0;
}

static int bmp_load_file(const char* path, rei_image_t* out_image) {
    if (!path || !out_image) return -1;

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

    int32_t bi_width  = (int32_t)((uint32_t)ih[4]  | ((uint32_t)ih[5]  << 8)
                      | ((uint32_t)ih[6]  << 16) | ((uint32_t)ih[7]  << 24));
    int32_t bi_height = (int32_t)((uint32_t)ih[8]  | ((uint32_t)ih[9]  << 8)
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
    if (!row_buf) { free(rgb); close(fd); return -1; }

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

    out_image->loaded = 1;
    out_image->width = width;
    out_image->height = height;
    out_image->pixels = rgb;
    return 0;
}
