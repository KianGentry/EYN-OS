#include <reiv.h>
#include <string.h>

int reiv_read_header(const uint8_t* data, size_t size, reiv_header_t* header) {
    if (!data || !header || size < sizeof(reiv_header_t)) {
        return -1;
    }
    memcpy(header, (const char*)data, sizeof(reiv_header_t));
    return 0;
}

int reiv_validate_header(const reiv_header_t* header) {
    if (!header) return -1;
    if (header->magic != REIV_MAGIC) return -1;
    if (header->version != REIV_VERSION_V1 && header->version != REIV_VERSION_V2 && header->version != REIV_VERSION_V3) return -1;
    if (header->width == 0 || header->width > REIV_MAX_WIDTH) return -1;
    if (header->height == 0 || header->height > REIV_MAX_HEIGHT) return -1;
    if (header->pixfmt != REIV_PIXFMT_RGB565LE) return -1;
    if (header->fps_den == 0) return -1;
    if (header->frame_count == 0) return -1;
    if (header->frames_offset < sizeof(reiv_header_t)) return -1;
    return 0;
}

uint32_t reiv_frame_size_bytes(const reiv_header_t* header) {
    if (!header) return 0;
    if (header->pixfmt == REIV_PIXFMT_RGB565LE) {
        return (uint32_t)header->width * (uint32_t)header->height * 2u;
    }
    return 0;
}

uint32_t reiv_frame_offset_bytes(const reiv_header_t* header, uint32_t frame_index) {
    if (!header) return 0;
    if (header->version != REIV_VERSION_V1) return 0;
    uint32_t fs = reiv_frame_size_bytes(header);
    return header->frames_offset + frame_index * fs;
}

uint32_t reiv_index_size_bytes(const reiv_header_t* header) {
    if (!header) return 0;
    if (header->version != REIV_VERSION_V2 && header->version != REIV_VERSION_V3) return 0;
    return header->frame_count * (uint32_t)sizeof(reiv_frame_entry_t);
}

uint32_t reiv_data_offset_bytes(const reiv_header_t* header) {
    if (!header) return 0;
    if (header->version == REIV_VERSION_V1) return header->frames_offset;
    if (header->version == REIV_VERSION_V2 || header->version == REIV_VERSION_V3) return header->frames_offset + reiv_index_size_bytes(header);
    return 0;
}
