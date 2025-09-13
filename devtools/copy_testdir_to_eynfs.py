#!/usr/bin/env python3
import os
import struct
import sys

EYNFS_BLOCK_SIZE = 512
EYNFS_NAME_MAX = 32
EYNFS_TYPE_FILE = 1
EYNFS_TYPE_DIR = 2
SUPERBLOCK_LBA = 2048

# EYNFS superblock structure
SUPERBLOCK_STRUCT = '<IIIIIII8s'
# EYNFS directory entry structure
DIR_ENTRY_STRUCT = f'<{EYNFS_NAME_MAX}sBBHIIII'
DIR_ENTRY_SIZE = struct.calcsize(DIR_ENTRY_STRUCT)

# Default values
DEFAULT_TESTDIR = 'testdir/'
DEFAULT_IMG = 'eynfs.img'


def read_superblock(f):
    f.seek(SUPERBLOCK_LBA * EYNFS_BLOCK_SIZE)
    data = f.read(struct.calcsize(SUPERBLOCK_STRUCT))
    fields = struct.unpack(SUPERBLOCK_STRUCT, data)
    return {
        'magic': fields[0],
        'version': fields[1],
        'block_size': fields[2],
        'total_blocks': fields[3],
        'root_dir_block': fields[4],
        'free_block_map': fields[5],
        'name_table_block': fields[6],
    }

def read_dir_chain(f, start_block):
    """Read the full directory chain into a list of entries and block numbers."""
    entries = []
    blocks = []
    block = start_block
    while block:
        f.seek(block * EYNFS_BLOCK_SIZE)
        data = f.read(EYNFS_BLOCK_SIZE)
        next_block = struct.unpack('<I', data[:4])[0]
        block_entries = []
        for i in range(4, EYNFS_BLOCK_SIZE, DIR_ENTRY_SIZE):
            entry_data = data[i:i+DIR_ENTRY_SIZE]
            if len(entry_data) < DIR_ENTRY_SIZE:
                break
            entry = struct.unpack(DIR_ENTRY_STRUCT, entry_data)
            entries.append(entry)
            block_entries.append(entry)
        blocks.append((block, next_block))
        block = next_block
    return entries, blocks

def find_free_dir_slot(entries):
    for idx, entry in enumerate(entries):
        name = entry[0].split(b'\0',1)[0]
        if not name:
            return idx
    return len(entries)

def find_free_block(f, sb):
    # Simple scan for a free block in the bitmap
    bitmap_block = sb['free_block_map']
    f.seek(bitmap_block * EYNFS_BLOCK_SIZE)
    bitmap = bytearray(f.read(EYNFS_BLOCK_SIZE))
    for i in range(sb['total_blocks']):
        byte = i // 8
        bit = i % 8
        if not (bitmap[byte] & (1 << bit)):
            # Mark as used
            bitmap[byte] |= (1 << bit)
            f.seek(bitmap_block * EYNFS_BLOCK_SIZE)
            f.write(bitmap)
            return i
    raise RuntimeError('No free blocks')

def write_file_data(f, sb, data):
    # Write file data to new blocks, return first block and size
    prev_block = 0
    first_block = 0
    size = len(data)
    written = 0
    while written < size:
        block = find_free_block(f, sb)
        if not first_block:
            first_block = block
        chunk = data[written:written+EYNFS_BLOCK_SIZE-4]
        block_data = bytearray(EYNFS_BLOCK_SIZE)
        # next_block = 0 for now
        struct.pack_into('<I', block_data, 0, 0)
        block_data[4:4+len(chunk)] = chunk
        if prev_block:
            # Update previous block's next pointer (only 4 bytes)
            f.seek(prev_block * EYNFS_BLOCK_SIZE)
            f.write(struct.pack('<I', block))
        f.seek(block * EYNFS_BLOCK_SIZE)
        f.write(block_data)
        prev_block = block
        written += len(chunk)
    return first_block, size

def update_dir_entry(f, block, entry_idx, entry):
    f.seek(block * EYNFS_BLOCK_SIZE + 4 + entry_idx * DIR_ENTRY_SIZE)
    f.write(struct.pack(DIR_ENTRY_STRUCT, *entry))

def find_dir_block(f, sb, path):
    # Traverse path, return block number of directory
    if path in ('', '/'): return sb['root_dir_block']
    parts = [p for p in path.strip('/').split('/') if p]
    block = sb['root_dir_block']
    for part in parts:
        entries, _ = read_dir_chain(f, block)
        found = False
        for entry in entries:
            name = entry[0].split(b'\0',1)[0].decode('utf-8')
            if name == part and entry[1] == EYNFS_TYPE_DIR:
                block = entry[5]
                found = True
                break
        if not found:
            return None
    return block

def add_dir(f, sb, parent_block, dirname):
    entries, blocks = read_dir_chain(f, parent_block)
    slot = find_free_dir_slot(entries)
    
    # If no free slot in existing blocks, allocate a new block
    if slot >= len(entries):
        # Find the last block in the chain
        last_block = blocks[-1][0] if blocks else parent_block
        
        # Allocate a new directory block
        new_parent_block = find_free_block(f, sb)
        
        # Link the last block to the new block
        f.seek(last_block * EYNFS_BLOCK_SIZE)
        data = bytearray(f.read(EYNFS_BLOCK_SIZE))
        struct.pack_into('<I', data, 0, new_parent_block)  # Set next_block pointer
        f.seek(last_block * EYNFS_BLOCK_SIZE)
        f.write(data)
        
        # Initialize the new block
        f.seek(new_parent_block * EYNFS_BLOCK_SIZE)
        new_block_data = bytearray(EYNFS_BLOCK_SIZE)
        struct.pack_into('<I', new_block_data, 0, 0)  # next_block = 0
        f.write(new_block_data)
        
        # Update our tracking
        blocks.append((new_parent_block, 0))
        slot = len(entries)  # First entry in the new block
        print(f"Allocated new parent directory block {new_parent_block} for {dirname}")
    
    new_block = find_free_block(f, sb)
    name_bytes = dirname.encode('utf-8')[:EYNFS_NAME_MAX-1] + b'\0'
    name_bytes = name_bytes.ljust(EYNFS_NAME_MAX, b'\0')
    entry = (
        name_bytes, EYNFS_TYPE_DIR, 0, 0, 0, new_block, 0, 0
    )
    block_num, _ = blocks[slot // ((EYNFS_BLOCK_SIZE-4)//DIR_ENTRY_SIZE)]
    entry_idx = slot % ((EYNFS_BLOCK_SIZE-4)//DIR_ENTRY_SIZE)
    update_dir_entry(f, block_num, entry_idx, entry)
    # Zero out the new directory block
    f.seek(new_block * EYNFS_BLOCK_SIZE)
    f.write(bytearray(EYNFS_BLOCK_SIZE))
    print(f"Created directory {dirname} at block {new_block}")
    return new_block

def add_file(f, sb, dir_block, filename, filedata):
    entries, blocks = read_dir_chain(f, dir_block)
    slot = find_free_dir_slot(entries)
    
    # If no free slot in existing blocks, allocate a new block
    if slot >= len(entries):
        # Find the last block in the chain
        last_block = blocks[-1][0] if blocks else dir_block
        
        # Allocate a new directory block
        new_block = find_free_block(f, sb)
        
        # Link the last block to the new block
        f.seek(last_block * EYNFS_BLOCK_SIZE)
        data = bytearray(f.read(EYNFS_BLOCK_SIZE))
        struct.pack_into('<I', data, 0, new_block)  # Set next_block pointer
        f.seek(last_block * EYNFS_BLOCK_SIZE)
        f.write(data)
        
        # Initialize the new block
        f.seek(new_block * EYNFS_BLOCK_SIZE)
        new_block_data = bytearray(EYNFS_BLOCK_SIZE)
        struct.pack_into('<I', new_block_data, 0, 0)  # next_block = 0
        f.write(new_block_data)
        
        # Update our tracking
        blocks.append((new_block, 0))
        slot = len(entries)  # First entry in the new block
        print(f"Allocated new directory block {new_block} for {filename}")
    
    first_block, size = write_file_data(f, sb, filedata)
    name_bytes = filename.encode('utf-8')[:EYNFS_NAME_MAX-1] + b'\0'
    name_bytes = name_bytes.ljust(EYNFS_NAME_MAX, b'\0')
    entry = (
        name_bytes, EYNFS_TYPE_FILE, 0, 0, size, first_block, 0, 0
    )
    block_num, _ = blocks[slot // ((EYNFS_BLOCK_SIZE-4)//DIR_ENTRY_SIZE)]
    entry_idx = slot % ((EYNFS_BLOCK_SIZE-4)//DIR_ENTRY_SIZE)
    update_dir_entry(f, block_num, entry_idx, entry)
    print(f"Copied {filename} to EYNFS. Size: {size} bytes, First block: {first_block}")

def clear_root_directory(f, sb):
    # Zero out all directory entries in the root directory chain
    block = sb['root_dir_block']
    while block:
        f.seek(block * EYNFS_BLOCK_SIZE)
        data = bytearray(f.read(EYNFS_BLOCK_SIZE))
        next_block = struct.unpack('<I', data[:4])[0]
        # Zero out all entries (but keep the next pointer)
        for i in range(4, EYNFS_BLOCK_SIZE):
            data[i] = 0
        f.seek(block * EYNFS_BLOCK_SIZE)
        f.write(data)
        block = next_block

def main():
    # parse command line arguments
    if len(sys.argv) < 2:
        print("Usage: python3 copy_testdir_to_eynfs.py <source_directory> [image_file]")
        print(f"Default image: {DEFAULT_IMG}")
        print(f"Example: python3 copy_testdir_to_eynfs.py {DEFAULT_TESTDIR}")
        print(f"Example: python3 copy_testdir_to_eynfs.py src/ source.img")
        sys.exit(1)
    
    source_dir = sys.argv[1]
    img_file = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_IMG
    
    # validate source directory
    if not os.path.isdir(source_dir):
        print(f"Error: Source directory '{source_dir}' does not exist")
        sys.exit(1)
    
    # validate image file
    if not os.path.exists(img_file):
        print(f"Error: Image file '{img_file}' does not exist")
        sys.exit(1)
    
    print(f"Copying '{source_dir}' to EYNFS image '{img_file}'...")
    
    with open(img_file, 'r+b') as f:
        sb = read_superblock(f)
        clear_root_directory(f, sb)
        for root, dirs, files in os.walk(source_dir):
            rel_dir = os.path.relpath(root, source_dir)
            eynfs_path = '' if rel_dir == '.' else rel_dir.replace('\\', '/')
            parent_path = os.path.dirname(eynfs_path)
            parent_block = find_dir_block(f, sb, parent_path)
            if eynfs_path and parent_block is not None:
                add_dir(f, sb, parent_block, os.path.basename(eynfs_path))
            dir_block = find_dir_block(f, sb, eynfs_path)
            for file in files:
                src_file = os.path.join(root, file)
                with open(src_file, 'rb') as infile:
                    data = infile.read()
                add_file(f, sb, dir_block, file, data)
    
    print(f"Successfully copied '{source_dir}' to '{img_file}'")

if __name__ == '__main__':
    main() 