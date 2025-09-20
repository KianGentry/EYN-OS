#ifndef REI_H
#define REI_H

#include <types.h>
#include <stdint.h>
#include <stddef.h>

// REI magic number ('REI\0')
#define REI_MAGIC 0x52454900

// REI format version
#define REI_VERSION 1

// Maximum dimensions (VGA text mode limits)
#define REI_MAX_WIDTH 320
#define REI_MAX_HEIGHT 200

// Color depth constants
#define REI_DEPTH_MONO  1
#define REI_DEPTH_RGB   3
#define REI_DEPTH_RGBA  4

// REI header structure
typedef struct {
    uint32_t magic;        // Magic number to identify REI
    uint16_t width;        // Image width in pixels
    uint16_t height;       // Image height in pixels
    uint8_t depth;         // Color depth (1, 3, or 4 bytes per pixel)
    uint8_t reserved1;     // Reserved for future use
    uint16_t reserved2;    // Reserved for future use
} rei_header_t;

// REI image structure (in-memory)
typedef struct {
    rei_header_t header;
    uint8_t* data;         // Raw pixel data
    size_t data_size;      // Size of pixel data
} rei_image_t;

// Function prototypes for REI API
int rei_read_header(const uint8_t* data, size_t size, rei_header_t* header);
int rei_validate_header(const rei_header_t* header);
int rei_calculate_data_size(const rei_header_t* header);
int rei_parse_image(const uint8_t* data, size_t size, rei_image_t* image);
void rei_free_image(rei_image_t* image);

// Display functions
int rei_display_image(const rei_image_t* image, int x, int y);
int rei_display_image_centered(const rei_image_t* image);

// Utility functions
int rei_get_pixel_offset(const rei_header_t* header, int x, int y);
uint32_t rei_get_pixel_color(const rei_image_t* image, int x, int y);
void rei_set_pixel_color(rei_image_t* image, int x, int y, uint32_t color);

// Color conversion utilities
uint32_t rei_rgb_to_vga(uint8_t r, uint8_t g, uint8_t b);
uint32_t rei_mono_to_vga(uint8_t mono);

#endif // REI_H 