# REI Image Format

**REI** (Raw EYN Image) is a custom image format designed specifically for EYN-OS. It provides a simple, efficient way to store and display images without the complexity of modern formats like PNG or JPEG.

## Overview

REI is a raw pixel format with a minimal header, designed for:
- **Simplicity**: Easy to parse and implement
- **Efficiency**: Direct pixel data access
- **Extensibility**: Reserved fields for future features
- **EYN-OS Integration**: Native support in the OS

## File Extension

REI files use the `.rei` extension (Raw EYN Image).

## Format Specification

### Header Structure (12 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00   | 4    | Magic | Magic number: `0x52454900` ("REI\0") |
| 0x04   | 2    | Width | Image width in pixels (16-bit, max 320) |
| 0x06   | 2    | Height | Image height in pixels (16-bit, max 200) |
| 0x08   | 1    | Depth | Color depth (1=mono, 3=RGB, 4=RGBA) |
| 0x09   | 1    | Reserved1 | Reserved for future use |
| 0x0A   | 2    | Reserved2 | Reserved for future use |

### Pixel Data

Raw pixel data follows immediately after the header. The data size is calculated as:
```
data_size = width × height × depth
```

### Color Depths

#### Mono (1 byte per pixel)
- Single grayscale value (0-255)
- 0 = black, 255 = white

#### RGB (3 bytes per pixel)
- Red component (0-255)
- Green component (0-255)  
- Blue component (0-255)

#### RGBA (4 bytes per pixel)
- Red component (0-255)
- Green component (0-255)
- Blue component (0-255)
- Alpha component (0-255)

Rendering notes:
- In GUI compositor overlays (icons, cursor, window buttons), alpha is respected; fully transparent pixels are skipped and high alpha is drawn opaque. Low alpha may be clamped to improve contrast over bright backgrounds.
- In some legacy paths, RGBA may be treated as RGB. Prefer assets with proper alpha for UI elements.

### Data Layout

Pixels are stored in row-major order (left to right, top to bottom):

```
Pixel (0,0) | Pixel (1,0) | ... | Pixel (width-1,0)
Pixel (0,1) | Pixel (1,1) | ... | Pixel (width-1,1)
...
Pixel (0,height-1) | ... | Pixel (width-1,height-1)
```

## Usage in EYN-OS

### Viewing REI Files

Use the GUI viewer commands:

```bash
view image.rei              # Open in a tile
vieww image.rei             # Open in a floating window
```

### Display Features

- **Pixel-perfect rendering** to VGA framebuffer
- **Automatic centering** on screen
- **Smart scaling** for oversized images
- **Support for all color depths**

## Conversion Tools

### PNG to REI Converter

Located at `devtools/png_to_rei.py`:

```bash
# Convert PNG to REI
python3 devtools/png_to_rei.py image.png -o image.rei

# Create test pattern
python3 devtools/png_to_rei.py --test -o test.rei

# Specify color depth
python3 devtools/png_to_rei.py image.png -d 1  # Mono
python3 devtools/png_to_rei.py image.png -d 3  # RGB
python3 devtools/png_to_rei.py image.png -d 4  # RGBA
```

### Options

- `-o, --output`: Specify output filename
- `-d, --depth`: Color depth (1, 3, or 4)
- `--test`: Create test pattern instead of converting

## Implementation Details

### OS Integration

REI support is implemented in:
- `include/rei.h` - Header definitions and function prototypes
- `src/drivers/rei.c` - Core REI parsing and rendering
- `src/utilities/shell/image_viewer_gui.c` - GUI viewer commands `view` and `vieww`

### Key Functions

```c
// Parse REI image from data
int rei_parse_image(const uint8_t* data, size_t size, rei_image_t* image);

// Display image with pixel-perfect rendering
int rei_display_image(const rei_image_t* image, int x, int y);

// Get pixel color at coordinates
uint32_t rei_get_pixel_color(const rei_image_t* image, int x, int y);

// Free image memory
void rei_free_image(rei_image_t* image);
```

### Rendering Process

1. **Parse Header**: Validate magic number and dimensions
2. **Load Pixel Data**: Copy raw pixel data to memory
3. **Calculate Position**: Center image on screen if needed
4. **Render Pixels**: Draw each pixel to VGA framebuffer
5. **Handle Scaling**: Scale down oversized images automatically

## Limitations

- **Maximum dimensions**: 320×200 pixels (VGA text mode limits)
- **No compression**: Raw pixel data only
- **Limited color depth**: Mono, RGB, and RGBA only
- **No metadata**: No support for image metadata or color profiles

## Future Extensions

The REI format is designed for extensibility:

- **Animation support**: Multiple frames in single file
- **Compression**: Simple RLE or delta compression
- **Palette support**: Indexed color mode
- **Metadata**: Image properties and color profiles
- **Layers**: Multiple image layers

## Examples

### Test Files

- `testdir/test_pattern.rei` - 64×48 RGB gradient pattern
- `testdir/eynos.rei` - 128×64 EYN-OS logo
- `testdir/mono_test.rei` - 64×48 monochrome test

### Creating Custom Images

```python
# Simple gradient pattern
import struct

# Header
magic = 0x52454900
width = 64
height = 48
depth = 3  # RGB

header = struct.pack('<IHHBBH', magic, width, height, depth, 0, 0)

# Pixel data
pixels = []
for y in range(height):
    for x in range(width):
        r = int((x / width) * 255)
        g = int((y / height) * 255)
        b = 128
        pixels.append(struct.pack('BBB', r, g, b))

# Write REI file
with open('gradient.rei', 'wb') as f:
    f.write(header)
    for pixel in pixels:
        f.write(pixel)
```

## Technical Notes

- **Endianness**: All multi-byte values are little-endian
- **Alignment**: No padding between pixels or rows
- **Memory**: Images are loaded entirely into memory
- **Performance**: Direct framebuffer access for optimal rendering

## Integration with EYN-OS

REI is fully integrated into EYN-OS's file reading system:

- **Smart detection** in `read` command
- **Subcommand support** via `read_image`
- **Help system integration** with proper documentation
- **Error handling** for invalid or corrupted files

The REI format exemplifies EYN-OS's philosophy of simple, powerful, custom solutions that integrate seamlessly with the operating system. 