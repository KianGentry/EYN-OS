# REI Image Format

REI (Raw EYN Image) is a simple, efficient image format for EYN-OS.

## Overview

- Minimal 12-byte header
- Raw pixel data (optionally compressed)
- Mono (1 Bpp), RGB (3 Bpp), RGBA (4 Bpp)

## File Extension

- `.rei`

## Header Structure (12 bytes)

| Offset | Size | Field   | Description                                                    |
|--------|------|---------|----------------------------------------------------------------|
| 0x00   | 4    | Magic   | 0x52454900 ("REI\0")                                         |
| 0x04   | 2    | Width   | Image width in pixels (max 320)                                |
| 0x06   | 2    | Height  | Image height in pixels (max 200)                               |
| 0x08   | 1    | Depth   | 1=mono, 3=RGB, 4=RGBA                                          |
| 0x09   | 1    | Flags   | Low nibble: compression (0=none, 1=RLE)                        |
| 0x0A   | 2    | Reserved| Reserved                                                        |

## Pixel Data

Uncompressed size is:

```
data_size = width × height × depth
```

- If flags compression=0: raw pixels follow header.
- If compression=1 (RLE): a compressed bytestream follows the header and decompresses to the size above.

## Compression

Low-nibble of Flags indicates compression:

- 0x0: None (raw)
- 0x1: RLE (PackBits-style), operating on whole pixels (1/3/4 bytes).

PackBits-like rules:

- Read signed int8 `n`
  - 0..127: copy next (n+1) pixels literally
  - -127..-1: repeat next single pixel (1 - n) times
  - -128: no-op

Note: A "pixel" is `depth` bytes; output must exactly equal `width×height×depth` bytes.

## Usage

View images in EYN-OS:

```bash
view image.rei
vieww image.rei
```

## Conversion (PNG → REI)

Tool: `devtools/png_to_rei.py`

```bash
# RLE compressed by default
python3 devtools/png_to_rei.py image.png -o image.rei

# Create test pattern
python3 devtools/png_to_rei.py --test -o test.rei

# Select depth
python3 devtools/png_to_rei.py image.png -d 1  # Mono
python3 devtools/png_to_rei.py image.png -d 3  # RGB
python3 devtools/png_to_rei.py image.png -d 4  # RGBA

# Disable compression (write raw pixels)
python3 devtools/png_to_rei.py image.png -o image.raw.rei --no-rle
```

Options:

- `-o, --output`: Output filename
- `-d, --depth`: 1, 3, or 4
- `--test`: Generate test pattern
- `--rle`: Enable RLE (default)
- `--no-rle`: Disable RLE (raw output)

## Implementation

EYN-OS integration:

- `include/drivers/rei.h` — format and API
- `src/drivers/rei.c` — parsing, optional RLE decompression, rendering helpers
- `src/utilities/shell/image_viewer_gui.c` — GUI viewer

## Notes

- Endianness: little-endian
- Alignment: no row padding
- Images are fully loaded into memory
- GUI compositor respects alpha in overlays
- **Subcommand support** via `read_image`
- **Help system integration** with proper documentation
- **Error handling** for invalid or corrupted files

The REI format exemplifies EYN-OS's philosophy of simple, powerful, custom solutions that integrate seamlessly with the operating system. 