import sys
import struct

EYNFS_MAGIC = 0x45594E46
EYNFS_NAME_MAX = 32
EYNFS_BLOCK_SIZE = 512
SUPERBLOCK_LBA = 2048

# EYNFS superblock struct
SUPERBLOCK_STRUCT = '<IIIIIIII'
# EYNFS dir entry struct
DIRENT_STRUCT = f'<{EYNFS_NAME_MAX}sBBHII2I'
DIRENT_SIZE = struct.calcsize(DIRENT_STRUCT)


def read_superblock(f):
    f.seek(SUPERBLOCK_LBA * EYNFS_BLOCK_SIZE)
    data = f.read(struct.calcsize(SUPERBLOCK_STRUCT))
    sb = struct.unpack(SUPERBLOCK_STRUCT, data)
    return {
        'magic': sb[0],
        'version': sb[1],
        'block_size': sb[2],
        'total_blocks': sb[3],
        'root_dir_block': sb[4],
        'free_block_map': sb[5],
        'name_table_block': sb[6],
        'reserved': sb[7:]
    }

print(f"DIRENT_SIZE (Python): {DIRENT_SIZE}")


def blk_off(block):
    # EYNFS block numbers in the superblock are relative to the start of the
    # EYNFS partition, which begins at SUPERBLOCK_LBA.
    return (SUPERBLOCK_LBA + block) * EYNFS_BLOCK_SIZE

def read_dir_table(f, start_block):
    entries = []
    current_block = start_block
    while current_block:
        f.seek(blk_off(current_block))
        block_data = f.read(EYNFS_BLOCK_SIZE)
        if len(block_data) < EYNFS_BLOCK_SIZE:
            break
        print(f"RAW BLOCK DATA: {block_data[:64].hex()}")
        next_block = struct.unpack('<I', block_data[:4])[0]
        offset = 4
        while offset + DIRENT_SIZE <= EYNFS_BLOCK_SIZE:
            data = block_data[offset:offset+DIRENT_SIZE]
            if not data or len(data) < DIRENT_SIZE:
                break
            entry = struct.unpack(DIRENT_STRUCT, data)
            raw_name = entry[0]
            name = raw_name.split(b'\0', 1)[0].decode('utf-8')
            print(f"DEBUG: raw name bytes: {raw_name.hex()}, parsed name: '{name}'")
            if not name:
                offset += DIRENT_SIZE
                continue
            entries.append({
                'name': name,
                'type': entry[1],
                'flags': entry[2],
                'reserved': entry[3],
                'size': entry[4],
                'first_block': entry[5],
                'extra': entry[6:8]
            })
            offset += DIRENT_SIZE
        current_block = next_block
    return entries

def extract_file(f, entry, out_path):
    size = entry['size']
    block = entry['first_block']
    with open(out_path, 'wb') as out:
        # Each filesystem block starts with a 4-byte next-block pointer, followed by payload
        payload_size = EYNFS_BLOCK_SIZE - 4
        while size > 0 and block != 0:
            f.seek(blk_off(block))
            block_data = f.read(EYNFS_BLOCK_SIZE)
            if not block_data or len(block_data) < 4:
                break
            next_block = struct.unpack('<I', block_data[:4])[0]
            # read only payload (skip header)
            to_take = min(size, payload_size)
            payload = block_data[4:4+to_take]
            out.write(payload)
            size -= len(payload)
            block = next_block

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <eynfs.img> <filename_in_root> <output>")
        sys.exit(1)
    img_path, filename, out_path = sys.argv[1:4]
    with open(img_path, 'rb') as f:
        sb = read_superblock(f)
        print(f"Root directory block: {sb['root_dir_block']}")
        if sb['magic'] != EYNFS_MAGIC:
            print("Not a valid EYNFS image.")
            sys.exit(1)
        # Print raw bytes of first 4 directory entries
        f.seek(blk_off(sb['root_dir_block']))
        print("First 4 raw directory entries:")
        for i in range(4):
            data = f.read(DIRENT_SIZE)
            print(data.hex())
        # Now read and parse entries as before
        entries = read_dir_table(f, sb['root_dir_block'])
        print("Files in root directory:")
        for entry in entries:
            print(f"  '{entry['name']}'")
        for entry in entries:
            if entry['name'] == filename:
                extract_file(f, entry, out_path)
                print(f"Extracted {filename} to {out_path}")
                return
        print(f"File {filename} not found in root directory.")
        sys.exit(1)

if __name__ == '__main__':
    main() 