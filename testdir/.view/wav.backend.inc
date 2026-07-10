/* WAV backend: probe RIFF/WAVE and prepare streaming PCM playback. */
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

static int audio_load_wav(const char* path, audio_state_t* a) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

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
    long data_file_offset = 0;
    int got_data = 0;

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
    if (audio_format != 1) { close(fd); return -1; }
    if (channels < 1 || channels > 2) { close(fd); return -1; }
    if (bits_per_sample != 8 && bits_per_sample != 16) { close(fd); return -1; }

    uint32_t frame_bytes = (uint32_t)channels * ((uint32_t)bits_per_sample / 8u);
    if (frame_bytes == 0) { close(fd); return -1; }

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
