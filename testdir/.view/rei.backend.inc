/* REI backend: image decode into RGB565 framebuffer pixels. */
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
