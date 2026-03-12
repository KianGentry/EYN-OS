import sys
import struct
import argparse
import os
import shutil
import subprocess
import re
from typing import Optional, Tuple, Any

try:
    from PIL import Image
except Exception:
    Image = None

# REI format constants
REI_MAGIC = 0x52454900  # 'REI\0'
REI_MAX_WIDTH = 320
REI_MAX_HEIGHT = 200

# REIV (video/animation) container constants
REIV_MAGIC = 0x52455600  # 'REV\0'
REIV_VERSION = 3
REIV_PIXFMT_RGB565LE = 1
REIV_FLAG_LOOP_DEFAULT = 0x01
REIV_FLAG_LOOP_LOCKED = 0x02

REIV_FRAME_FLAG_RLE565 = 0x00000001
REIV_FRAME_FLAG_RLE8 = 0x00000002
REIV_FRAME_FLAG_DELTA_XOR_PREV = 0x00000004

REIV_MAX_WIDTH = 640
REIV_MAX_HEIGHT = 480

def create_rei_header(width, height, depth, flags=0):
    """Create REI header structure"""
    return struct.pack('<IHHBBH',
        REI_MAGIC,      # Magic number
        width,          # Width
        height,         # Height
        depth,          # Colour depth (1=mono, 3=RGB, 4=RGBA)
        flags & 0xFF,   # Flags/Compression (low nibble)
        0               # Reserved2
    )


def create_reiv_header(width, height, fps, flags, frame_count, frames_offset=28):
    """Create REIV header structure (matches include/drivers/reiv.h)."""
    fps_num = int(fps)
    fps_den = 1
    return struct.pack(
        '<IHHBBHIIII',
        REIV_MAGIC,
        int(width),
        int(height),
        REIV_PIXFMT_RGB565LE,
        int(flags) & 0xFF,
        REIV_VERSION,
        int(frame_count),
        int(fps_num),
        int(fps_den),
        int(frames_offset),
    )


def _require_ffmpeg():
    ffmpeg = shutil.which('ffmpeg')
    ffprobe = shutil.which('ffprobe')

    # If system FFmpeg isn't installed, fall back to a user-local binary provided by imageio-ffmpeg.
    # This avoids needing sudo while still meeting the "FFmpeg required" constraint.
    if not ffmpeg:
        try:
            import imageio_ffmpeg  # type: ignore
            ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
        except Exception:
            ffmpeg = None

    # Best-effort ffprobe: if missing, we can probe width/height by parsing `ffmpeg -i` output.
    if not ffprobe and ffmpeg:
        candidate = os.path.join(os.path.dirname(ffmpeg), 'ffprobe')
        if os.path.exists(candidate) and os.access(candidate, os.X_OK):
            ffprobe = candidate

    if not ffmpeg:
        raise RuntimeError(
            'FFmpeg is required for .gif/.mp4 conversion. Install system ffmpeg/ffprobe, '
            'or run: python3 -m pip install --user imageio-ffmpeg'
        )

    return ffmpeg, ffprobe


def _probe_video_size(ffprobe: Optional[str], ffmpeg: str, input_file: str):
    if ffprobe:
        cmd = [
            ffprobe, '-v', 'error',
            '-select_streams', 'v:0',
            '-show_entries', 'stream=width,height',
            '-of', 'csv=p=0:s=x',
            input_file,
        ]
        out = subprocess.check_output(cmd, text=True).strip()
        if 'x' not in out:
            raise RuntimeError(f'ffprobe returned unexpected size string: {out!r}')
        w_str, h_str = out.split('x', 1)
        return int(w_str), int(h_str)

    # Fallback: parse the video stream info from `ffmpeg -hide_banner -i <file>`.
    # This does not require ffprobe and is generally stable across ffmpeg versions.
    proc = subprocess.run(
        [ffmpeg, '-hide_banner', '-i', input_file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    text_out = (proc.stdout or '') + '\n' + (proc.stderr or '')
    m = re.search(r'Video:.*?(\d{2,5})x(\d{2,5})', text_out)
    if not m:
        raise RuntimeError('Unable to determine input dimensions (missing ffprobe and ffmpeg output parse failed).')
    return int(m.group(1)), int(m.group(2))


def _scale_preserve_aspect(src_w, src_h, max_w, max_h):
    if src_w <= 0 or src_h <= 0:
        raise ValueError('Invalid source dimensions.')
    scale = min(max_w / src_w, max_h / src_h, 1.0)
    out_w = max(1, int(src_w * scale))
    out_h = max(1, int(src_h * scale))
    return out_w, out_h


def convert_media_to_reiv(input_file, output_file, max_w=REIV_MAX_WIDTH, max_h=REIV_MAX_HEIGHT, fps=30, loop_locked=False):
    """Convert GIF/MP4 to REIV container (RGB565 frames) using ffmpeg.

    REIV v2 layout:
        header (28 bytes)
        index table: frame_count * 12 bytes (offset,size,flags)
        frame data payloads (raw RGB565 or PackBits-style RLE over RGB565 pixels)
    """
    ffmpeg, ffprobe = _require_ffmpeg()
    src_w, src_h = _probe_video_size(ffprobe, ffmpeg, input_file)
    out_w, out_h = _scale_preserve_aspect(src_w, src_h, max_w, max_h)

    flags = REIV_FLAG_LOOP_DEFAULT
    if loop_locked:
        flags |= REIV_FLAG_LOOP_LOCKED

    frame_size = out_w * out_h * 2  # RGB565LE
    header_size = 28

    # Build ffmpeg pipeline that outputs raw RGB565 frames at requested fps.
    vf = f'scale={out_w}:{out_h},fps={int(fps)}'
    cmd = [
        ffmpeg, '-v', 'error',
        '-i', input_file,
        '-an', '-sn',
        '-vf', vf,
        '-pix_fmt', 'rgb565le',
        '-f', 'rawvideo', 'pipe:1',
    ]

    print(f'Input: {input_file} ({src_w}x{src_h})')
    print(f'Output: {output_file} ({out_w}x{out_h}) fps={fps} pixfmt=rgb565le')
    print('Running:', ' '.join(cmd))

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    assert proc.stdout is not None

    # Stream frames from ffmpeg and write payloads to a temp file while building the v3 index in memory.
    import tempfile
    index = []  # list[(offset,size,flags)]
    raw_total = 0
    payload_total = 0
    prev_frame = None
    tmp = tempfile.NamedTemporaryFile('w+b', delete=False)
    tmp_path = tmp.name
    # Insert a keyframe periodically (roughly once per second) to cap delta-chain length.
    # This improves seek/catch-up behavior and avoids pathological slowdowns.
    try:
        while True:
            frame = proc.stdout.read(frame_size)
            if not frame:
                break
            if len(frame) != frame_size:
                raise RuntimeError(f'Partial frame read: got {len(frame)} bytes, expected {frame_size}')
            raw_total += len(frame)

            frame_idx = len(index)
            force_keyframe = (frame_idx == 0)
            try:
                keyint = int(fps) if int(fps) > 0 else 30
            except Exception:
                keyint = 30
            if keyint < 1:
                keyint = 1
            if frame_idx % keyint == 0:
                force_keyframe = True

            # Keyframes: store as raw or RLE565
            if prev_frame is None or force_keyframe:
                compressed565 = _encode_rle(frame, 2)
                if len(compressed565) < len(frame):
                    data = compressed565
                    fflags = REIV_FRAME_FLAG_RLE565
                else:
                    data = frame
                    fflags = 0
                prev_frame = frame
            else:
                # Interframe: XOR delta vs previous, then bytewise PackBits RLE.
                # This tends to compress well even for “noisy” video due to temporal redundancy.
                delta = bytes((a ^ b) for a, b in zip(prev_frame, frame))
                compressed8 = _encode_rle(delta, 1)
                # Only take delta+RLE8 if it is materially smaller than raw delta.
                # Small wins often cost more CPU than they save in I/O.
                delta_len = len(delta)
                delta_take = (len(compressed8) < delta_len) and (len(compressed8) <= int(delta_len * 0.90))
                if delta_take:
                    data = compressed8
                    fflags = REIV_FRAME_FLAG_DELTA_XOR_PREV | REIV_FRAME_FLAG_RLE8
                else:
                    # Fallback to keyframe (raw or RLE565) if delta doesn't help
                    compressed565 = _encode_rle(frame, 2)
                    if len(compressed565) < len(frame):
                        data = compressed565
                        fflags = REIV_FRAME_FLAG_RLE565
                    else:
                        data = frame
                        fflags = 0
                prev_frame = frame

            off = tmp.tell()
            tmp.write(data)
            index.append((off, len(data), fflags))
            payload_total += len(data)

    finally:
        tmp.flush()
        tmp.close()

    rc = proc.wait()
    if rc != 0:
        raise RuntimeError(f'ffmpeg failed with exit code {rc}')

    frame_count = len(index)
    if frame_count <= 0:
        raise RuntimeError('No frames produced.')

    # Write final v3 file: header + index + payload
    with open(output_file, 'wb') as out_f:
        out_f.write(create_reiv_header(out_w, out_h, fps, flags, frame_count=frame_count, frames_offset=header_size))
        for off, sz, fflags in index:
            out_f.write(struct.pack('<III', int(off), int(sz), int(fflags)))
        with open(tmp_path, 'rb') as in_f:
            shutil.copyfileobj(in_f, out_f)

    try:
        os.unlink(tmp_path)
    except Exception:
        pass

    index_bytes = frame_count * 12
    total_bytes = header_size + index_bytes + payload_total
    ratio = 100.0 * (payload_total + index_bytes) / max(1, raw_total)
    print(f'Wrote {frame_count} frames. Raw={raw_total} bytes, payload={payload_total} bytes, index={index_bytes} bytes, total={total_bytes} bytes ({ratio:.1f}% of raw+header).')
    return True

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
        if Image is None:
            raise RuntimeError('Pillow (PIL) is required for PNG conversion. Install it or use .gif/.mp4 conversion (FFmpeg).')
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
                    px: Any = img.getpixel((x, y))
                    if img.mode == 'RGBA':
                        r, g, b, a = px
                    else:
                        r, g, b = px
                        a = 255
                    # RGBA - preserve provided alpha
                    pixels.append(struct.pack('BBBB', r, g, b, a))
                elif depth == 3:
                    px: Any = img.getpixel((x, y))
                    if img.mode == 'RGBA':
                        r, g, b = px[0], px[1], px[2]
                    else:
                        r, g, b = px
                    # RGB
                    pixels.append(struct.pack('BBB', r, g, b))
                elif depth == 1:
                    # Convert to grayscale from RGB(A)
                    px: Any = img.getpixel((x, y))
                    if img.mode == 'RGBA':
                        r, g, b = px[0], px[1], px[2]
                    else:
                        r, g, b = px
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
    parser = argparse.ArgumentParser(description='Convert images/videos to REI/REIV format')
    parser.add_argument('input', nargs='?', help='Input PNG file')
    parser.add_argument('-o', '--output', help='Output REI file')
    parser.add_argument('-d', '--depth', type=int, choices=[1, 3, 4], default=3,
                       help='Colour depth (1=mono, 3=RGB, 4=RGBA). If the input has an alpha channel, depth will be promoted to 4 unless --no-auto-depth is passed.')
    parser.add_argument('--no-auto-depth', action='store_true', help='Disable auto alpha detection; use the exact depth specified.')
    # Compression options: default is RLE enabled; --no-rle disables.
    # Keep --rle for compatibility; it is redundant when default is on.
    parser.add_argument('--rle', action='store_true', help='Enable PackBits-style RLE compression (default)')
    parser.add_argument('--no-rle', action='store_true', help='Disable RLE compression (write raw pixels)')
    parser.add_argument('--test', action='store_true', help='Create test pattern instead')

    # REIV options
    parser.add_argument('--max-width', type=int, default=REIV_MAX_WIDTH, help='Max width for .gif/.mp4 conversions (aspect preserved)')
    parser.add_argument('--max-height', type=int, default=REIV_MAX_HEIGHT, help='Max height for .gif/.mp4 conversions (aspect preserved)')
    parser.add_argument('--fps', type=int, default=30, help='Frame rate for .gif/.mp4 conversions')
    
    args = parser.parse_args()
    
    if args.test:
        output_file = args.output or 'test_pattern.rei'
        create_test_pattern(output_file, depth=args.depth)
    else:
        if not args.input:
            print("Error: Input file required when not using --test")
            sys.exit(1)
        output_file = args.output or args.input.rsplit('.', 1)[0] + '.rei'

        ext = os.path.splitext(args.input)[1].lower()
        if ext in ('.gif', '.mp4'):
            # GIF is treated as looping animation by default.
            loop_locked = (ext == '.gif')
            try:
                convert_media_to_reiv(args.input, output_file, max_w=args.max_width, max_h=args.max_height, fps=args.fps, loop_locked=loop_locked)
            except Exception as e:
                print(f'Error: {e}')
                sys.exit(1)
            return

        # Effective RLE default is True unless explicitly disabled
        rle_eff = True
        if args.no_rle:
            rle_eff = False
        elif args.rle:
            rle_eff = True
        convert_png_to_rei(args.input, output_file, args.depth, auto=(not args.no_auto_depth), rle=rle_eff)

if __name__ == '__main__':
    main() 