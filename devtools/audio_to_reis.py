#!/usr/bin/env python3
"""
Convert WAV/MP3 audio files to the REIS (.reis) format used by EYN-OS.

Usage
-----
    python3 audio_to_reis.py input.wav -o output.reis
    python3 audio_to_reis.py input.mp3 -o output.reis
    python3 audio_to_reis.py input.wav --compress -o compressed.reis
    python3 audio_to_reis.py --test -o test_tone.reis

Notes
-----
    By default, output is uncompressed (COMP_NONE).  The EYN-OS viewer
    streams COMP_NONE files directly from disk, so arbitrarily large audio
    files play without loading into the 1 MB userland heap.

    PackBits RLE (--compress) barely helps with audio data and produces
    files that cannot be streamed; they must fit in the heap to decode.

Dependencies
------------
    WAV:  Python standard library (wave module) — no external deps.
    MP3:  Requires ffmpeg on $PATH (converts to temp WAV first).

REIS format summary (see include/drivers/reis.h)
--------------------------------------------------
    32-byte header (little-endian):
      magic(4) version(2) channels(1) bits(1) sample_rate(4) frame_count(4)
      data_offset(4) data_size(4) flags(4) reserved(4)

    Followed by PCM payload: raw interleaved or PackBits-RLE compressed.
"""

import sys
import struct
import argparse
import os
import subprocess
import tempfile
import wave
import math
from typing import Optional, Tuple, List

# ---- REIS format constants ----

REIS_MAGIC   = 0x52454953   # 'REIS' little-endian
REIS_VERSION = 1
REIS_HEADER_SIZE = 32

REIS_COMP_NONE = 0x0
REIS_COMP_RLE  = 0x1

REIS_MAX_SAMPLE_RATE = 48000
REIS_MAX_FRAME_COUNT = 48000 * 60 * 30   # 30 minutes at 48 kHz


# ---- PackBits-style RLE encoder (matches REI/REIV codec) ----

def _encode_rle(data: bytes, pixel_size: int) -> bytes:
    """
    PackBits-style RLE operating on whole 'pixel' units (sample frames).

    Control bytes:
      0..127   → literal run of (n+1) units  (data follows)
      -127..-1 → replicate run of (1-n) copies of 1 unit
      -128     → no-op / padding
    """
    out = bytearray()
    total_units = len(data) // pixel_size
    pos = 0

    while pos < total_units:
        # Look ahead for a replicate run (≥3 identical units).
        run_start = pos
        if pos + 1 < total_units:
            unit_a = data[pos * pixel_size:(pos + 1) * pixel_size]
            run_len = 1
            while (run_start + run_len < total_units and run_len < 128):
                unit_b = data[(run_start + run_len) * pixel_size:
                              (run_start + run_len + 1) * pixel_size]
                if unit_a == unit_b:
                    run_len += 1
                else:
                    break

            if run_len >= 3:
                # Emit replicate run.
                ctrl = 1 - run_len          # -2..-127
                out.append(ctrl & 0xFF)
                out.extend(unit_a)
                pos += run_len
                continue

        # Accumulate literal run (up to 128 units).
        lit_start = pos
        lit_count = 0
        while pos < total_units and lit_count < 128:
            # Stop if a replicate run of ≥3 is about to start.
            if pos + 2 < total_units:
                a = data[pos * pixel_size:(pos + 1) * pixel_size]
                b = data[(pos + 1) * pixel_size:(pos + 2) * pixel_size]
                c = data[(pos + 2) * pixel_size:(pos + 3) * pixel_size]
                if a == b == c:
                    break
            pos += 1
            lit_count += 1

        if lit_count > 0:
            ctrl = lit_count - 1  # 0..127
            out.append(ctrl & 0xFF)
            out.extend(data[lit_start * pixel_size:
                            (lit_start + lit_count) * pixel_size])

    return bytes(out)


# ---- REIS header builder ----

def create_reis_header(channels: int, bits: int, sample_rate: int,
                       frame_count: int, data_offset: int,
                       data_size: int, flags: int) -> bytes:
    """Build a 32-byte REIS header."""
    return struct.pack(
        '<IHBBIIIII',
        REIS_MAGIC,
        REIS_VERSION,
        channels,
        bits,
        sample_rate,
        frame_count,
        data_offset,
        data_size,
        flags,
    ) + b'\x00' * 4  # reserved


# ---- WAV reader ----

def read_wav(path: str) -> Tuple[int, int, int, bytes]:
    """
    Read a WAV file and return (channels, bits, sample_rate, raw_pcm_bytes).

    Handles 8-bit unsigned and 16-bit signed LE PCM.
    """
    with wave.open(path, 'rb') as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()     # bytes per sample
        sample_rate = wf.getframerate()
        frame_count = wf.getnframes()
        raw = wf.readframes(frame_count)

    bits = sampwidth * 8
    if bits not in (8, 16):
        raise ValueError(f'Unsupported WAV sample width: {bits}-bit '
                         f'(only 8 and 16 are supported)')

    return channels, bits, sample_rate, raw


# ---- MP3 → WAV via ffmpeg ----

def mp3_to_wav_temp(mp3_path: str,
                    target_rate: int = 22050,
                    target_channels: int = 1,
                    target_bits: int = 16) -> str:
    """
    Convert an MP3 to a temporary WAV file using ffmpeg.
    Returns the path to the temp WAV.  Caller must delete it.
    """
    if not os.path.isfile(mp3_path):
        raise FileNotFoundError(f'Input file not found: {mp3_path}')

    tmp = tempfile.NamedTemporaryFile(suffix='.wav', delete=False)
    tmp_path = tmp.name
    tmp.close()

    codec = 'pcm_s16le' if target_bits == 16 else 'pcm_u8'
    cmd = [
        'ffmpeg', '-y', '-i', mp3_path,
        '-ar', str(target_rate),
        '-ac', str(target_channels),
        '-acodec', codec,
        tmp_path,
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
    except FileNotFoundError:
        os.unlink(tmp_path)
        raise RuntimeError('ffmpeg not found.  Install ffmpeg to convert MP3 files.')
    except subprocess.CalledProcessError as e:
        os.unlink(tmp_path)
        raise RuntimeError(f'ffmpeg failed: {e.stderr.decode("utf-8", errors="replace")}')

    return tmp_path


# ---- Resample / channel-convert PCM in pure Python ----

def resample_pcm(pcm: bytes, channels: int, bits: int,
                 src_rate: int, dst_rate: int) -> Tuple[bytes, int]:
    """
    Nearest-neighbour resample from src_rate to dst_rate.
    Returns (new_pcm_bytes, new_frame_count).
    """
    if src_rate == dst_rate:
        frame_bytes = channels * (bits // 8)
        return pcm, len(pcm) // frame_bytes

    frame_bytes = channels * (bits // 8)
    src_frames = len(pcm) // frame_bytes
    dst_frames = int(src_frames * dst_rate / src_rate)
    if dst_frames == 0:
        return b'', 0

    out = bytearray(dst_frames * frame_bytes)
    for i in range(dst_frames):
        src_idx = int(i * src_rate / dst_rate)
        if src_idx >= src_frames:
            src_idx = src_frames - 1
        src_off = src_idx * frame_bytes
        dst_off = i * frame_bytes
        out[dst_off:dst_off + frame_bytes] = pcm[src_off:src_off + frame_bytes]

    return bytes(out), dst_frames


def convert_to_mono(pcm: bytes, channels: int, bits: int) -> bytes:
    """Down-mix multi-channel PCM to mono by averaging channels."""
    if channels == 1:
        return pcm
    bps = bits // 8
    frame_bytes = channels * bps
    frame_count = len(pcm) // frame_bytes

    out = bytearray(frame_count * bps)
    for i in range(frame_count):
        off = i * frame_bytes
        total = 0
        for ch in range(channels):
            ch_off = off + ch * bps
            if bits == 16:
                val = struct.unpack_from('<h', pcm, ch_off)[0]
            else:
                val = pcm[ch_off] - 128  # unsigned → signed
            total += val
        avg = total // channels
        if bits == 16:
            struct.pack_into('<h', out, i * bps, max(-32768, min(32767, avg)))
        else:
            out[i] = max(0, min(255, avg + 128))

    return bytes(out)


# ---- Test tone generator ----

def generate_test_tone(freq: float = 440.0, duration: float = 2.0,
                       sample_rate: int = 22050, bits: int = 16,
                       channels: int = 1) -> bytes:
    """Generate a pure sine-wave tone as PCM bytes."""
    frame_count = int(sample_rate * duration)
    out = bytearray()
    for i in range(frame_count):
        t = i / sample_rate
        val = math.sin(2.0 * math.pi * freq * t)
        for _ in range(channels):
            if bits == 16:
                sample = int(val * 30000)
                sample = max(-32768, min(32767, sample))
                out.extend(struct.pack('<h', sample))
            else:
                sample = int((val + 1.0) * 127.5)
                sample = max(0, min(255, sample))
                out.append(sample)
    return bytes(out)


# ---- Main conversion ----

def convert_to_reis(input_path: str, output_path: str,
                    target_rate: int = 22050,
                    target_channels: int = 1,
                    target_bits: int = 16,
                    use_rle: bool = True) -> bool:
    """
    Convert a WAV or MP3 file to REIS format.
    Returns True on success.
    """
    ext = os.path.splitext(input_path)[1].lower()
    tmp_wav = None

    try:
        if ext == '.mp3':
            print(f'Converting MP3 → temporary WAV via ffmpeg ...')
            tmp_wav = mp3_to_wav_temp(input_path,
                                      target_rate=target_rate,
                                      target_channels=target_channels,
                                      target_bits=target_bits)
            wav_path = tmp_wav
        elif ext in ('.wav', '.wave'):
            wav_path = input_path
        else:
            print(f'Error: unsupported input format "{ext}" (use .wav or .mp3)')
            return False

        channels, bits, sample_rate, pcm = read_wav(wav_path)
        print(f'Input: {channels}ch, {bits}-bit, {sample_rate} Hz, '
              f'{len(pcm)} bytes PCM')

        # ---- channel conversion ----
        if channels > target_channels:
            pcm = convert_to_mono(pcm, channels, bits)
            channels = 1

        # ---- bit-depth conversion (only 8↔16) ----
        if bits != target_bits:
            bps = bits // 8
            frame_bytes = channels * bps
            frame_count = len(pcm) // frame_bytes
            new_bps = target_bits // 8
            new_pcm = bytearray(frame_count * channels * new_bps)

            for i in range(frame_count * channels):
                if bits == 8 and target_bits == 16:
                    val = pcm[i] - 128
                    struct.pack_into('<h', new_pcm, i * 2, val * 256)
                elif bits == 16 and target_bits == 8:
                    val = struct.unpack_from('<h', pcm, i * 2)[0]
                    new_pcm[i] = max(0, min(255, (val >> 8) + 128))

            pcm = bytes(new_pcm)
            bits = target_bits

        # ---- resample ----
        if sample_rate != target_rate:
            print(f'Resampling {sample_rate} → {target_rate} Hz ...')
            pcm, fc = resample_pcm(pcm, channels, bits, sample_rate, target_rate)
            sample_rate = target_rate
        else:
            frame_bytes = channels * (bits // 8)
            fc = len(pcm) // frame_bytes

        if fc == 0:
            print('Error: resulting audio has 0 frames')
            return False

        if fc > REIS_MAX_FRAME_COUNT:
            print(f'Warning: truncating from {fc} to {REIS_MAX_FRAME_COUNT} frames')
            fc = REIS_MAX_FRAME_COUNT
            pcm = pcm[:fc * channels * (bits // 8)]

        # ---- compression ----
        frame_size = channels * (bits // 8)
        raw_size = len(pcm)

        if use_rle:
            compressed = _encode_rle(pcm, frame_size)
            savings = 1.0 - len(compressed) / raw_size if raw_size else 0
            if savings > 0.0:
                payload = compressed
                flags = REIS_COMP_RLE
                print(f'RLE: {raw_size} → {len(compressed)} bytes '
                      f'({savings * 100:.1f}% savings)')
            else:
                payload = pcm
                flags = REIS_COMP_NONE
                print('RLE did not save space; storing raw.')
        else:
            payload = pcm
            flags = REIS_COMP_NONE

        # ---- build header + write ----
        data_offset = REIS_HEADER_SIZE
        header = create_reis_header(
            channels=channels,
            bits=bits,
            sample_rate=sample_rate,
            frame_count=fc,
            data_offset=data_offset,
            data_size=len(payload),
            flags=flags,
        )

        assert len(header) == REIS_HEADER_SIZE, \
            f'Header size mismatch: {len(header)} != {REIS_HEADER_SIZE}'

        # If output ends with .reiv/.rei, force .reis
        base, out_ext = os.path.splitext(output_path)
        if out_ext.lower() not in ('.reis',):
            output_path = base + '.reis'

        with open(output_path, 'wb') as f:
            f.write(header)
            f.write(payload)

        total = len(header) + len(payload)
        duration_s = fc / sample_rate if sample_rate else 0
        print(f'Output: {output_path}')
        print(f'  {channels}ch, {bits}-bit, {sample_rate} Hz')
        print(f'  {fc} frames ({duration_s:.2f}s)')
        print(f'  Header: {len(header)} bytes')
        print(f'  Payload: {len(payload)} bytes')
        print(f'  Total: {total} bytes')
        return True

    except Exception as e:
        print(f'Error: {e}')
        return False
    finally:
        if tmp_wav and os.path.exists(tmp_wav):
            os.unlink(tmp_wav)


def main():
    parser = argparse.ArgumentParser(
        description='Convert WAV/MP3 audio to REIS format for EYN-OS')
    parser.add_argument('input', nargs='?', help='Input WAV or MP3 file')
    parser.add_argument('-o', '--output', help='Output .reis file')

    parser.add_argument('--rate', type=int, default=22050,
                        help='Target sample rate in Hz (default: 22050)')
    parser.add_argument('--channels', type=int, choices=[1, 2], default=1,
                        help='Target channel count (default: 1 = mono)')
    parser.add_argument('--bits', type=int, choices=[8, 16], default=16,
                        help='Target bits per sample (default: 16)')

    parser.add_argument('--compress', action='store_true',
                        help='Enable PackBits RLE compression (not recommended '
                             'for audio; disables streaming in the viewer)')

    parser.add_argument('--test', action='store_true',
                        help='Generate a 440 Hz test tone instead of reading a file')
    parser.add_argument('--test-freq', type=float, default=440.0,
                        help='Test tone frequency in Hz (default: 440)')
    parser.add_argument('--test-duration', type=float, default=2.0,
                        help='Test tone duration in seconds (default: 2.0)')

    args = parser.parse_args()

    rle = args.compress

    if args.test:
        output = args.output or 'test_tone.reis'
        pcm = generate_test_tone(
            freq=args.test_freq,
            duration=args.test_duration,
            sample_rate=args.rate,
            bits=args.bits,
            channels=args.channels,
        )
        frame_bytes = args.channels * (args.bits // 8)
        fc = len(pcm) // frame_bytes

        if rle:
            compressed = _encode_rle(pcm, frame_bytes)
            if len(compressed) < len(pcm):
                payload = compressed
                flags = REIS_COMP_RLE
            else:
                payload = pcm
                flags = REIS_COMP_NONE
        else:
            payload = pcm
            flags = REIS_COMP_NONE

        header = create_reis_header(
            channels=args.channels,
            bits=args.bits,
            sample_rate=args.rate,
            frame_count=fc,
            data_offset=REIS_HEADER_SIZE,
            data_size=len(payload),
            flags=flags,
        )

        base, ext = os.path.splitext(output)
        if ext.lower() != '.reis':
            output = base + '.reis'

        with open(output, 'wb') as f:
            f.write(header)
            f.write(payload)

        duration = fc / args.rate if args.rate else 0
        print(f'Generated {args.test_freq} Hz test tone: {output}')
        print(f'  {args.channels}ch, {args.bits}-bit, {args.rate} Hz, '
              f'{fc} frames ({duration:.2f}s), {len(header) + len(payload)} bytes')
        return

    if not args.input:
        print('Error: input file required (or use --test)')
        sys.exit(1)

    output = args.output
    if not output:
        base = os.path.splitext(args.input)[0]
        output = base + '.reis'

    ok = convert_to_reis(
        args.input, output,
        target_rate=args.rate,
        target_channels=args.channels,
        target_bits=args.bits,
        use_rle=rle,
    )
    if not ok:
        sys.exit(1)


if __name__ == '__main__':
    main()
