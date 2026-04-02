/* REIS backend: load/decompress audio stream metadata. */
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
            if (skip_fd_bytes(fd, (size_t)(data_offset
                              - (uint32_t)sizeof(hdr))) != 0) {
                close(fd); return -1;
            }
        }
        a->src_fd = fd;
        a->src_data_offset = data_offset;
        a->pcm = NULL;
        a->pcm_size = 0;
    } else if (comp == REIS_COMP_RLE) {
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
