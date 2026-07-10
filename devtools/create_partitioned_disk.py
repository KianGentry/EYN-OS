#!/usr/bin/env python3
"""
create_partitioned_disk.py - Create a partitioned disk image for EYN-OS

Creates a 50MB disk image with:
- MBR partition table
- Partition 1: 40MB EYNFS (user files)
- Partition 2: 10MB Swap

Usage: python3 create_partitioned_disk.py [output.img]
"""

import os
import struct
import sys

# Constants
SECTOR_SIZE = 512
MB = 1024 * 1024

# Partition types
PART_TYPE_EYNFS = 0xEF
PART_TYPE_SWAP = 0xE5

# EYNFS constants
EYNFS_MAGIC = 0x45594E46  # 'EYNF'
EYNFS_VERSION = 11


def create_mbr(partitions):
    """Create MBR with partition table."""
    mbr = bytearray(512)
    
    # Boot code (minimal - just halts)
    mbr[0] = 0xF4  # HLT
    mbr[1] = 0xEB  # JMP -2
    mbr[2] = 0xFD
    
    # Write partition entries at offset 0x1BE
    for i, part in enumerate(partitions):
        if i >= 4:
            break
        offset = 0x1BE + i * 16
        
        # Boot flag
        mbr[offset] = 0x80 if part.get('bootable', False) else 0x00
        
        # CHS start (dummy values for LBA)
        mbr[offset + 1] = 0xFE
        mbr[offset + 2] = 0xFF
        mbr[offset + 3] = 0xFF
        
        # Partition type
        mbr[offset + 4] = part['type']
        
        # CHS end (dummy values for LBA)
        mbr[offset + 5] = 0xFE
        mbr[offset + 6] = 0xFF
        mbr[offset + 7] = 0xFF
        
        # LBA start (little-endian)
        lba_start = part['lba_start']
        mbr[offset + 8] = lba_start & 0xFF
        mbr[offset + 9] = (lba_start >> 8) & 0xFF
        mbr[offset + 10] = (lba_start >> 16) & 0xFF
        mbr[offset + 11] = (lba_start >> 24) & 0xFF
        
        # Sector count (little-endian)
        sectors = part['sectors']
        mbr[offset + 12] = sectors & 0xFF
        mbr[offset + 13] = (sectors >> 8) & 0xFF
        mbr[offset + 14] = (sectors >> 16) & 0xFF
        mbr[offset + 15] = (sectors >> 24) & 0xFF
    
    # MBR signature
    mbr[510] = 0x55
    mbr[511] = 0xAA
    
    return bytes(mbr)


def create_eynfs_superblock(total_blocks, root_lba, bitmap_lba, nametable_lba):
    """Create EYNFS superblock sector."""
    sb = bytearray(512)
    
    # Superblock structure
    struct.pack_into('<I', sb, 0, EYNFS_MAGIC)        # magic
    struct.pack_into('<I', sb, 4, EYNFS_VERSION)      # version
    struct.pack_into('<I', sb, 8, SECTOR_SIZE)        # block_size
    struct.pack_into('<I', sb, 12, total_blocks)      # total_blocks
    struct.pack_into('<I', sb, 16, root_lba)          # root_dir_block
    struct.pack_into('<I', sb, 20, bitmap_lba)        # free_block_map
    struct.pack_into('<I', sb, 24, nametable_lba)     # name_table_block
    
    return bytes(sb)


def create_eynfs_bitmap(total_blocks, used_blocks):
    """Create EYNFS free block bitmap.

    The bitmap spans as many 512-byte blocks as needed to cover total_blocks.
    """
    bitmap_bytes = (total_blocks + 7) // 8
    bitmap_blocks = (bitmap_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
    bitmap = bytearray(bitmap_blocks * SECTOR_SIZE)

    for block in used_blocks:
        byte_idx = block // 8
        bit_idx = block % 8
        if 0 <= byte_idx < len(bitmap):
            bitmap[byte_idx] |= (1 << bit_idx)

    return bytes(bitmap)


def create_swap_header(total_pages):
    """Create swap partition header."""
    header = bytearray(512)
    
    # Simple swap signature
    header[0:4] = b'SWAP'
    struct.pack_into('<I', header, 4, total_pages)
    
    return bytes(header)


def create_partitioned_disk(
    filename,
    total_size_mb=10,
    part1_size_mb=5,
    part2_size_mb=5,
    part1_start_sector=2048,
    total_sectors_override=None,
    part1_sectors_override=None,
    part2_sectors_override=None,
):
    """Create a partitioned disk image."""

    total_sectors = int(total_sectors_override) if total_sectors_override is not None else (total_size_mb * MB) // SECTOR_SIZE
    
    # Partition 1 start sector is configurable. Default keeps historical 1MB offset,
    # but installer RAM media can use a smaller offset to reduce module byte size.
    part1_start = max(1, int(part1_start_sector))
    
    # Calculate available space after reserved area
    available_sectors = total_sectors - part1_start
    
    if part1_sectors_override is not None:
        part1_sectors = int(part1_sectors_override)
    else:
        part1_sectors = (part1_size_mb * MB) // SECTOR_SIZE

    if part2_sectors_override is not None:
        part2_sectors = int(part2_sectors_override)
    else:
        part2_sectors = (part2_size_mb * MB) // SECTOR_SIZE

    # If requested sizes exceed available and overrides were not provided, scale down proportionally.
    requested_sectors = part1_sectors + part2_sectors
    if requested_sectors > available_sectors and part1_sectors_override is None and part2_sectors_override is None:
        scale = available_sectors / requested_sectors
        part1_size_mb = int(part1_size_mb * scale)
        part2_size_mb = int(part2_size_mb * scale)
        print(f"  Scaled partitions to fit: {part1_size_mb}MB + {part2_size_mb}MB")
        part1_sectors = (part1_size_mb * MB) // SECTOR_SIZE
        part2_sectors = (part2_size_mb * MB) // SECTOR_SIZE
    
    # Adjust part2 to use remaining space
    part1_end = part1_start + part1_sectors
    part2_start = part1_end
    part2_sectors = min(part2_sectors, total_sectors - part2_start)
    part2_end = part2_start + part2_sectors
    
    print(f"Creating partitioned disk: {filename}")
    print(f"  Total size: {total_sectors} sectors")
    print(f"  Partition 1 (EYNFS): {part1_sectors} sectors @ sector {part1_start}")
    print(f"  Partition 2 (Swap):  {part2_sectors} sectors @ sector {part2_start}")
    
    # Verify we have enough space
    if part2_end > total_sectors:
        print(f"Error: Partitions exceed disk size!")
        print(f"  Need {part2_end} sectors, have {total_sectors}")
        sys.exit(1)
    
    # Create partition definitions
    partitions = [
        {
            'type': PART_TYPE_EYNFS,
            'lba_start': part1_start,
            'sectors': part1_sectors,
            'bootable': True
        },
        {
            'type': PART_TYPE_SWAP,
            'lba_start': part2_start,
            'sectors': part2_sectors,
            'bootable': False
        }
    ]
    
    # Create the disk image
    with open(filename, 'wb') as f:
        # Write zeros for entire disk
        f.write(b'\x00' * (total_sectors * SECTOR_SIZE))
        
        # Write MBR at sector 0
        f.seek(0)
        f.write(create_mbr(partitions))
        
        # === Format Partition 1 as EYNFS ===
        # EYNFS layout within partition:
        #   Sector 0: Superblock
        #   Sector 1..N: Free block bitmap (N depends on partition size)
        #   Sector (1+N): Name table
        #   Sector (2+N): Root directory
        
        sb_lba = part1_start

        # On-disk EYNFS fields store *filesystem-relative* block numbers.
        sb_block = 0
        bitmap_block = 1

        bitmap_bytes = (part1_sectors + 7) // 8
        bitmap_blocks = (bitmap_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
        nametable_block = bitmap_block + bitmap_blocks
        rootdir_block = nametable_block + 1

        bitmap_lba = part1_start + bitmap_block
        nametable_lba = part1_start + nametable_block
        rootdir_lba = part1_start + rootdir_block
        
        # Write superblock
        f.seek(sb_lba * SECTOR_SIZE)
        f.write(create_eynfs_superblock(
            part1_sectors, rootdir_block, bitmap_block, nametable_block
        ))
        
        # Write bitmap.
        # Reserve: superblock, bitmap blocks, name table, root dir.
        reserved_blocks = list(range(0, rootdir_block + 1))
        f.seek(bitmap_lba * SECTOR_SIZE)
        f.write(create_eynfs_bitmap(part1_sectors, reserved_blocks))
        
        # Name table and root directory are already zeroed
        
        # === Format Partition 2 as Swap ===
        swap_pages = part2_sectors // 8  # 8 sectors per 4KB page
        f.seek(part2_start * SECTOR_SIZE)
        f.write(create_swap_header(swap_pages))
    
    print(f"Disk image created: {filename}")
    print(f"  EYNFS partition: formatted with {part1_sectors} blocks")
    print(f"  Swap partition: {swap_pages} pages available")
    
    return filename


def main():
    output_file = sys.argv[1] if len(sys.argv) > 1 else 'eynfs.img'
    
    # Default: 50MB total, 40MB EYNFS, 10MB Swap
    create_partitioned_disk(output_file, total_size_mb=50, part1_size_mb=40, part2_size_mb=10)


if __name__ == '__main__':
    main()
