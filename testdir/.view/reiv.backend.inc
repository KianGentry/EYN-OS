/* REIV backend: stream/decode RGB565 video frames. */
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
