import sys
import struct
from PIL import Image
import argparse

# REI format constants
REI_MAGIC = 0x52454900  # 'REI\0'
REI_MAX_WIDTH = 320
REI_MAX_HEIGHT = 200

def create_rei_header(width, height, depth, flags=0):
    """Create REI header structure"""
    return struct.pack('<IHHBBH',
        REI_MAGIC,      # Magic number
        width,          # Width
        height,         # Height
        depth,          # Color depth (1=mono, 3=RGB, 4=RGBA)
        flags & 0xFF,   # Flags/Compression (low nibble)
        0               # Reserved2
    )

def _encode_rle(pixels_bytes: bytes, pixel_size: int) -> bytes:
    """PackBits-style RLE operating on whole pixels (of pixel_size bytes)."""
    out = bytearray()
    n = len(pixels_bytes)
    i = 0
    # Helper to compare pixels
    def same_px(a_off, b_off):
        return pixels_bytes[a_off:a_off+pixel_size] == pixels_bytes[b_off:b_off+pixel_size]
    while i < n:
        # Try to find a run of repeated pixels
        run_start = i
        i += pixel_size
        run_len = 1
        while i < n and same_px(i, run_start) and run_len < 128:
            run_len += 1
            i += pixel_size
        if run_len >= 2:
            # Emit replicate packet: count encoded as (1 - count) signed byte
            out.append((256 + (1 - run_len)) & 0xFF)
            out.extend(pixels_bytes[run_start:run_start+pixel_size])
            continue
        # Otherwise, build a literal run until a repetition or limit
        lit_start = run_start
        lit_count = 1
        while i < n and lit_count < 128:
            # Peek if a repetition starts at i
            next_is_run = False
            if i + pixel_size <= n and i + 2*pixel_size <= n:
                next_is_run = same_px(i, i + pixel_size)
            if next_is_run:
                break
            # Consume one literal pixel
            i += pixel_size
            lit_count += 1
        # Emit literal packet: count-1 in control byte
        out.append((lit_count - 1) & 0x7F)
        out.extend(pixels_bytes[lit_start:lit_start + lit_count*pixel_size])
    return bytes(out)


def convert_png_to_rei(input_file, output_file, depth=3, auto=True, rle=False):
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
        
        # Optionally compress with RLE on pixel boundaries
        flags = 0
        if rle:
            pixel_bytes = b''.join(pixels)
            compressed = _encode_rle(pixel_bytes, depth)
            # Only keep if it helps
            if len(compressed) < len(pixel_bytes):
                payload = compressed
                flags = 0x01  # RLE
                print(f"RLE compressed: {len(pixel_bytes)} -> {len(compressed)} bytes ({100.0*len(compressed)/max(1,len(pixel_bytes)):.1f}%)")
            else:
                payload = pixel_bytes
                print("RLE not effective; keeping raw data")
        else:
            payload = b''.join(pixels)

        # Create header
        header = create_rei_header(width, height, depth, flags)

        # Write REI file
        with open(output_file, 'wb') as f:
            f.write(header)
            f.write(payload)
        
        print(f"Successfully created {output_file}")
        print(f"Header size: {len(header)} bytes")
        payload_size = len(payload)
        print(f"Data size: {payload_size} bytes (uncomp {width*height*depth} bytes)")
        print(f"Total size: {len(header) + payload_size} bytes")
        
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
    # Compression options: default is RLE enabled; --no-rle disables.
    # Keep --rle for compatibility; it is redundant when default is on.
    parser.add_argument('--rle', action='store_true', help='Enable PackBits-style RLE compression (default)')
    parser.add_argument('--no-rle', action='store_true', help='Disable RLE compression (write raw pixels)')
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
        # Effective RLE default is True unless explicitly disabled
        rle_eff = True
        if args.no_rle:
            rle_eff = False
        elif args.rle:
            rle_eff = True
        convert_png_to_rei(args.input, output_file, args.depth, auto=(not args.no_auto_depth), rle=rle_eff)

if __name__ == '__main__':
    main() 