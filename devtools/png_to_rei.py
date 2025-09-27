#!/usr/bin/env python3
"""
PNG to REI Converter
Converts PNG images to EYN-OS REI format
"""

import sys
import struct
from PIL import Image
import argparse

# REI format constants
REI_MAGIC = 0x52454900  # 'REI\0'
REI_MAX_WIDTH = 320
REI_MAX_HEIGHT = 200

def create_rei_header(width, height, depth):
    """Create REI header structure"""
    return struct.pack('<IHHBBH',
        REI_MAGIC,      # Magic number
        width,          # Width
        height,         # Height
        depth,          # Color depth (1=mono, 3=RGB, 4=RGBA)
        0,              # Reserved1
        0               # Reserved2
    )

def convert_png_to_rei(input_file, output_file, depth=3, auto=True):
    """Convert PNG to REI format"""
    try:
        # Open and convert image
        img = Image.open(input_file)

        # Auto-detect alpha if requested and promote to RGBA
        src_mode = img.mode
        # Detect per-pixel alpha either by channels or palette transparency flag
        has_alpha = ('A' in img.getbands()) or (src_mode in ('LA', 'RGBA', 'PA')) or ('transparency' in img.info)
        if auto and has_alpha and depth != 4:
            print("Alpha channel detected; promoting REI depth to 4 (RGBA) to preserve transparency.")
            depth = 4

        # Convert to desired base mode depending on (possibly updated) depth
        if depth == 4:
            # Preserve alpha if present
            if img.mode != 'RGBA':
                img = img.convert('RGBA')
        elif depth == 3:
            if img.mode != 'RGB':
                img = img.convert('RGB')
        elif depth == 1:
            # We'll downmix per-pixel below; keep as RGB(A) for sampling
            if img.mode not in ('RGB', 'RGBA'):
                img = img.convert('RGB')
        
        # Resize if too large
        if img.width > REI_MAX_WIDTH or img.height > REI_MAX_HEIGHT:
            img.thumbnail((REI_MAX_WIDTH, REI_MAX_HEIGHT), Image.Resampling.LANCZOS)
            print(f"Resized image to {img.width}x{img.height}")
        
        width, height = img.size
        print(f"Converting {width}x{height} image to REI format (depth={depth})...")
        
        # Create header
        header = create_rei_header(width, height, depth)
        
        # Convert pixels
        pixels = []
        for y in range(height):
            for x in range(width):
                if depth == 4:
                    r, g, b, a = img.getpixel((x, y)) if img.mode == 'RGBA' else (*img.getpixel((x, y)), 255)
                    # RGBA - preserve provided alpha
                    pixels.append(struct.pack('BBBB', r, g, b, a))
                elif depth == 3:
                    r, g, b = img.getpixel((x, y)) if img.mode != 'RGBA' else img.getpixel((x, y))[:3]
                    # RGB
                    pixels.append(struct.pack('BBB', r, g, b))
                elif depth == 1:
                    # Convert to grayscale from RGB(A)
                    if img.mode == 'RGBA':
                        r, g, b, _ = img.getpixel((x, y))
                    else:
                        r, g, b = img.getpixel((x, y))
                    # Convert to grayscale
                    gray = int(0.299 * r + 0.587 * g + 0.114 * b)
                    pixels.append(struct.pack('B', gray))
        
        # Write REI file
        with open(output_file, 'wb') as f:
            f.write(header)
            for pixel in pixels:
                f.write(pixel)
        
        print(f"Successfully created {output_file}")
        print(f"Header size: {len(header)} bytes")
        print(f"Data size: {len(pixels) * depth} bytes")
        print(f"Total size: {len(header) + len(pixels) * depth} bytes")
        
    except Exception as e:
        print(f"Error: {e}")
        return False
    
    return True

def create_test_pattern(output_file, width=64, height=48, depth=3):
    """Create a simple test pattern"""
    print(f"Creating {width}x{height} test pattern...")
    
    # Create header
    header = create_rei_header(width, height, depth)
    
    # Create simple gradient pattern
    pixels = []
    for y in range(height):
        for x in range(width):
            r = int((x / width) * 255)
            g = int((y / height) * 255)
            b = 128
            
            if depth == 1:
                gray = int(0.299 * r + 0.587 * g + 0.114 * b)
                pixels.append(struct.pack('B', gray))
            elif depth == 3:
                pixels.append(struct.pack('BBB', r, g, b))
            elif depth == 4:
                pixels.append(struct.pack('BBBB', r, g, b, 255))
    
    # Write REI file
    with open(output_file, 'wb') as f:
        f.write(header)
        for pixel in pixels:
            f.write(pixel)
    
    print(f"Created test pattern: {output_file}")

def main():
    parser = argparse.ArgumentParser(description='Convert PNG to REI format')
    parser.add_argument('input', nargs='?', help='Input PNG file')
    parser.add_argument('-o', '--output', help='Output REI file')
    parser.add_argument('-d', '--depth', type=int, choices=[1, 3, 4], default=3,
                       help='Color depth (1=mono, 3=RGB, 4=RGBA). If the input has an alpha channel, depth will be promoted to 4 unless --no-auto-depth is passed.')
    parser.add_argument('--no-auto-depth', action='store_true', help='Disable auto alpha detection; use the exact depth specified.')
    parser.add_argument('--test', action='store_true', help='Create test pattern instead')
    
    args = parser.parse_args()
    
    if args.test:
        output_file = args.output or 'test_pattern.rei'
        create_test_pattern(output_file, depth=args.depth)
    else:
        if not args.input:
            print("Error: Input file required when not using --test")
            sys.exit(1)
        output_file = args.output or args.input.rsplit('.', 1)[0] + '.rei'
        convert_png_to_rei(args.input, output_file, args.depth, auto=(not args.no_auto_depth))

if __name__ == '__main__':
    main() 