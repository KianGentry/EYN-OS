# REI Image Format API

This document describes the REI (Raw EYN Image) format API functions available in EYN-OS.

## Header File

```c
#include "rei.h"
```

## Data Structures

### rei_header_t
```c
typedef struct {
    uint32_t magic;        // Magic number to identify REI
    uint16_t width;        // Image width in pixels
    uint16_t height;       // Image height in pixels
    uint8_t depth;         // Colour depth (1, 3, or 4 bytes per pixel)
    uint8_t reserved1;     // Flags (low nibble: compression; 0=none, 1=RLE)
    uint16_t reserved2;    // Reserved for future use
} rei_header_t;
```

### rei_image_t
```c
typedef struct {
    rei_header_t header;
    uint8_t* data;         // Raw pixel data
    size_t data_size;      // Size of pixel data
} rei_image_t;
```

## Constants

```c
#define REI_MAGIC 0x52454900        // Magic number ('REI\0')
#define REI_MAX_WIDTH 320           // Maximum image width
#define REI_MAX_HEIGHT 200          // Maximum image height
#define REI_DEPTH_MONO 1            // Monochrome (1 byte per pixel)
#define REI_DEPTH_RGB 3             // RGB (3 bytes per pixel)
#define REI_DEPTH_RGBA 4            // RGBA (4 bytes per pixel)
// Compression flags (low nibble of reserved1)
#define REI_COMP_NONE 0x0
#define REI_COMP_RLE  0x1
#define REI_COMP_MASK 0x0F
```

## Core Functions

### rei_parse_image
```c
int rei_parse_image(const uint8_t* data, size_t size, rei_image_t* image);
```

Parses a complete REI image from raw or compressed data. If RLE compression is indicated by the header flags, the pixel data is decompressed into memory.

**Parameters:**
- `data`: Pointer to REI file data
- `size`: Size of data in bytes
- `image`: Output image structure

**Returns:**
- `0` on success
- `-1` on error (invalid format, insufficient data, etc.)

**Example:**
```c
uint8_t* file_data = /* ... */;
size_t file_size = /* ... */;
rei_image_t image;

if (rei_parse_image(file_data, file_size, &image) == 0) {
    // Image parsed successfully
    rei_display_image(&image, 0, 0);
    rei_free_image(&image);
}
```

### rei_display_image
```c
int rei_display_image(const rei_image_t* image, int x, int y);
```

Displays an REI image with pixel-perfect rendering to the VGA framebuffer.

**Parameters:**
- `image`: Pointer to parsed REI image
- `x`, `y`: Display position (0,0 centers the image)

**Returns:**
- `0` on success
- `-1` on error

**Features:**
- Automatic centering if x=0, y=0
- Smart scaling for oversized images
- Clears screen before rendering
- Pixel-perfect VGA framebuffer rendering

### rei_free_image
```c
void rei_free_image(rei_image_t* image);
```

Frees memory allocated for an REI image.

**Parameters:**
- `image`: Pointer to image structure to free

**Note:** Always call this function after using an image to prevent memory leaks.

## Utility Functions

### rei_read_header
```c
int rei_read_header(const uint8_t* data, size_t size, rei_header_t* header);
```

Reads and validates REI header from data.

**Parameters:**
- `data`: Pointer to data containing REI header
- `size`: Size of data
- `header`: Output header structure

**Returns:**
- `0` on success
- `-1` on error

### rei_validate_header
```c
int rei_validate_header(const rei_header_t* header);
```

Validates REI header for correctness.

**Parameters:**
- `header`: Pointer to header to validate

**Returns:**
- `0` if header is valid
- `-1` if header is invalid

**Checks:**
- Magic number
- Dimensions (non-zero, within limits)
- Colour depth (valid values)

### rei_calculate_data_size
```c
int rei_calculate_data_size(const rei_header_t* header);
```

Calculates expected pixel data size based on header.

**Parameters:**
- `header`: Pointer to REI header

**Returns:**
- Expected data size in bytes
- `-1` on error

### rei_get_pixel_offset
```c
int rei_get_pixel_offset(const rei_header_t* header, int x, int y);
```

Calculates byte offset for pixel at coordinates.

**Parameters:**
- `header`: Pointer to REI header
- `x`, `y`: Pixel coordinates

**Returns:**
- Byte offset in pixel data
- `-1` if coordinates are invalid

### rei_get_pixel_colour
```c
uint32_t rei_get_pixel_colour(const rei_image_t* image, int x, int y);
```

Gets pixel colour at coordinates.

**Parameters:**
- `image`: Pointer to REI image
- `x`, `y`: Pixel coordinates

**Returns:**
- 32-bit colour value (format depends on depth)

### rei_set_pixel_colour
```c
void rei_set_pixel_colour(rei_image_t* image, int x, int y, uint32_t colour);
```

Sets pixel colour at coordinates.

**Parameters:**
- `image`: Pointer to REI image
- `x`, `y`: Pixel coordinates
- `colour`: 32-bit colour value

## Colour Conversion Functions

### rei_rgb_to_vga
```c
uint32_t rei_rgb_to_vga(uint8_t r, uint8_t g, uint8_t b);
```

Converts RGB values to VGA colour format.

**Parameters:**
- `r`, `g`, `b`: RGB components (0-255)

**Returns:**
- VGA colour value

### rei_mono_to_vga
```c
uint32_t rei_mono_to_vga(uint8_t mono);
```

Converts monochrome value to VGA colour.

**Parameters:**
- `mono`: Monochrome value (0-255)

**Returns:**
- VGA colour value

## Error Handling

All REI functions use consistent error handling:

- **Return values**: Functions return `0` on success, `-1` on error
- **Memory management**: Always call `rei_free_image()` after use
- **Validation**: Functions validate input parameters
- **Bounds checking**: Coordinates and sizes are validated

## Memory Management

REI images allocate memory dynamically:

```c
rei_image_t image;
if (rei_parse_image(data, size, &image) == 0) {
    // Use image...
    rei_free_image(&image);  // Always free!
}
```

## Performance Notes

- **Direct framebuffer access**: Uses `drawPixel()` for optimal rendering
- **Lightweight compression**: Optional PackBits-style RLE supported; decompressed into RAM for fast rendering
- **Memory efficient**: Only loads necessary data
- **Fast parsing**: Minimal header validation

## Integration with Shell

REI images are automatically detected by the `read` command:

```bash
read image.rei        # Smart detection
read_image image.rei  # Direct subcommand
```

The shell integration handles:
- File loading and parsing
- Error reporting
- Memory cleanup
- User feedback 