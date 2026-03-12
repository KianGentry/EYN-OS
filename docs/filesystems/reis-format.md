# REIS Audio Format

REIS (Raw EYN Image Sound) is a simple audio format in the REI family of formats for EYN-OS.

## Overview

- Compact 32-byte header
- Raw PCM sample data (optionally RLE-compressed)
- 8-bit or 16-bit, mono or stereo
- Sample rates up to 48 kHz

## File Extension

- `.reis`

## Header Structure (32 bytes)

| Offset | Size | Field       | Description                                         |
|--------|------|-------------|-----------------------------------------------------|
| 0x00   | 4    | Magic       | 0x52454953 ("REIS" little-endian)                    |
| 0x04   | 1    | Version     | Format version (currently 1)                         |
| 0x08   | 1    | Channels    | 1 = mono, 2 = stereo                                |
| 0x09   | 1    | Bits        | Bits per sample: 8 or 16                             |
| 0x0A   | 2    | Reserved1   | Must be 0                                           |
| 0x0C   | 4    | Sample Rate | Samples per second (e.g. 22050, 44100, 48000)        |
| 0x10   | 4    | Frame Count | Total number of sample frames                        |
| 0x14   | 4    | Data Offset | Byte offset from file start to sample data           |
| 0x18   | 4    | Data Size   | Size of (possibly compressed) sample data in bytes   |
| 0x1C   | 1    | Flags       | Bit 0: RLE compression (0=raw, 1=RLE)               |
| 0x1D   | 3    | Reserved2   | Must be 0                                           |

A "frame" is one sample per channel. For stereo 16-bit audio, one frame = 4 bytes.

## Sample Data

Uncompressed PCM size:

```
pcm_size = frame_count × channels × (bits / 8)
```

- Samples are signed little-endian for 16-bit, unsigned for 8-bit (matching WAV conventions).
- Stereo samples are interleaved: L, R, L, R, ...

## Compression

Bit 0 of Flags controls compression:

- 0: Raw PCM follows the header at `data_offset`.
- 1: PackBits-style RLE compressed data follows; decompresses to the full PCM size.

PackBits-like rules (same codec as REI/REIV):

- Read signed int8 `n`
  - 0..127: copy next (n+1) samples literally
  - -127..-1: repeat next single sample (1 - n) times
  - -128: no-op

A "sample" in the RLE context is `channels × (bits / 8)` bytes (i.e. one frame).

## Usage

Play audio in EYN-OS:

```bash
view audio.reis
view music.wav
```

The viewer displays a progress bar and playback status. Press **Space** to play/pause, **Esc** to quit.

## Conversion (WAV/MP3 → REIS)

Tool: `devtools/audio_to_reis.py`

```bash
# Convert WAV to REIS (default: 22050 Hz mono 16-bit, RLE)
python3 devtools/audio_to_reis.py input.wav -o output.reis

# Convert MP3 (requires ffmpeg)
python3 devtools/audio_to_reis.py input.mp3 -o output.reis

# Generate a 440 Hz test tone
python3 devtools/audio_to_reis.py --test -o test.reis

# Custom parameters
python3 devtools/audio_to_reis.py input.wav -o out.reis --rate 44100 --channels 2 --bits 16

# Disable compression
python3 devtools/audio_to_reis.py input.wav -o out.reis --no-rle
```

Options:

- `-o, --output`: Output filename
- `--rate`: Target sample rate (default: 22050)
- `--channels`: 1 (mono) or 2 (stereo), default: 1
- `--bits`: 8 or 16, default: 16
- `--rle`: Enable RLE compression (default)
- `--no-rle`: Disable compression
- `--test`: Generate test tone instead of converting

## Implementation

EYN-OS integration:

- `include/drivers/reis.h` — format header struct and API
- `src/drivers/reis.c` — kernel-side parsing and RLE decompression
- `include/drivers/ac97.h` — AC97 audio driver header
- `src/drivers/ac97.c` — AC97 audio driver (PCI, DMA, 48 kHz stereo s16le output)

## Audio Syscalls

The kernel provides five audio syscalls (113–117) for userland programs. All audio output goes through the AC97 driver at 48 kHz stereo 16-bit signed LE. User programs must resample and convert their audio to this format before calling `audio_write`.

| # | Name | Capability | Description |
|---|------|-----------|-------------|
| 113 | `AUDIO_PROBE` | `CAP_DEV_AUDIO` | Scan PCI for AC97 hardware; returns 0 if found |
| 114 | `AUDIO_INIT` | `CAP_DEV_AUDIO`, `CAP_ALLOC_MEMORY` | Initialize AC97 (DMA, IRQ); returns 0 on success |
| 115 | `AUDIO_WRITE` | `CAP_DEV_AUDIO` | Submit PCM buffer (EBX=ptr, ECX=size); returns bytes queued |
| 116 | `AUDIO_STOP` | — | Stop playback and reset DMA |
| 117 | `AUDIO_IS_AVAILABLE` | — | Returns 1 if AC97 is initialized, 0 otherwise |

## Notes

- Endianness: all header fields are little-endian
- The AC97 driver output is fixed at 48 kHz stereo 16-bit signed LE; the viewer handles resampling from the source format
- REIS files are designed to be small and efficient for the low-RAM EYN-OS environment
- The format name "REIS" follows the REI family convention (REI for images, REIV for video, REIS for sound)
