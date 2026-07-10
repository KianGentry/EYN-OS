import os
import struct
import sys
from collections import deque

EYNFS_BLOCK_SIZE = 512
EYNFS_NAME_MAX = 32
EYNFS_TYPE_FILE = 1
EYNFS_TYPE_DIR = 2
SUPERBLOCK_LBA = 2048

# Partition support
PART_TYPE_EYNFS = 0xEF
MBR_PARTITION_TABLE_OFFSET = 0x1BE

# EYNFS superblock structure
SUPERBLOCK_STRUCT = '<IIIIIII8s'
# EYNFS directory entry structure
DIR_ENTRY_STRUCT = f'<{EYNFS_NAME_MAX}sBBHIIII'
DIR_ENTRY_SIZE = struct.calcsize(DIR_ENTRY_STRUCT)
DIR_ENTRIES_PER_BLOCK = (EYNFS_BLOCK_SIZE - 4) // DIR_ENTRY_SIZE

# Default values
DEFAULT_TESTDIR = 'testdir/'
DEFAULT_IMG = 'eynfs.img'

VERBOSE = os.environ.get('EYNFS_COPY_VERBOSE', '0').lower() not in ('0', 'false', 'no', '')


def vprint(message: str) -> None:
    if VERBOSE:
        print(message)


def _abs_lba(sb, block_num: int) -> int:
    """Translate filesystem-relative block numbers to absolute disk LBA."""
    return int(sb['_partition_start']) + int(block_num)


def find_eynfs_partition(f):
    """Find the first EYNFS partition in the MBR and return its start LBA."""
    f.seek(0)
    mbr = f.read(512)
    
    # Check MBR signature
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        # No valid MBR, assume old-style image with superblock at sector 2048
        return SUPERBLOCK_LBA
    
    # Parse partition table
    for i in range(4):
        offset = MBR_PARTITION_TABLE_OFFSET + i * 16
        part_type = mbr[offset + 4]
        lba_start = struct.unpack('<I', mbr[offset + 8:offset + 12])[0]
        sectors = struct.unpack('<I', mbr[offset + 12:offset + 16])[0]
        
        if part_type == PART_TYPE_EYNFS and sectors > 0:
            print(f"Found EYNFS partition {i+1} at LBA {lba_start}")
            return lba_start
    
    # No EYNFS partition found, assume old-style
    print("No EYNFS partition found, using default superblock location")
    return SUPERBLOCK_LBA


def read_superblock(f, partition_start=None):
    if partition_start is None:
        partition_start = find_eynfs_partition(f)
    f.seek(partition_start * EYNFS_BLOCK_SIZE)
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
        '_partition_start': partition_start,
    }

def read_dir_chain(f, sb, start_block):
    """Read the full directory chain into a list of entries and block numbers."""
    entries = []
    blocks = []
    block = start_block
    while block:
        f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE)
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

class FreeBlockAllocator:
    """
    Bitmap-backed block allocator with in-memory caching.

    Efficiency notes:
    - Reads the free bitmap once.
    - Uses a moving hint to avoid rescanning from block 0 on each allocation.
    - Flushes bitmap updates once at the end instead of per block allocation.
    """

    def __init__(self, f, sb):
        self.f = f
        self.sb = sb
        self.bitmap_block = sb['free_block_map']
        self.total_blocks = sb['total_blocks']
        self.bitmap_bytes = (self.total_blocks + 7) // 8
        self.bitmap_blocks = (self.bitmap_bytes + EYNFS_BLOCK_SIZE - 1) // EYNFS_BLOCK_SIZE
        self.hint = 0
        self.dirty = False

        self.f.seek(_abs_lba(self.sb, self.bitmap_block) * EYNFS_BLOCK_SIZE)
        self.bitmap = bytearray(self.f.read(self.bitmap_blocks * EYNFS_BLOCK_SIZE))

    def _is_used(self, block_num: int) -> bool:
        byte = block_num // 8
        bit = block_num % 8
        return (self.bitmap[byte] & (1 << bit)) != 0

    def _mark_used(self, block_num: int) -> None:
        byte = block_num // 8
        bit = block_num % 8
        self.bitmap[byte] |= (1 << bit)
        self.dirty = True

    def alloc(self) -> int:
        total = self.total_blocks
        start = self.hint

        for i in range(start, total):
            if not self._is_used(i):
                self._mark_used(i)
                self.hint = i + 1
                if self.hint >= total:
                    self.hint = 0
                return i

        for i in range(0, start):
            if not self._is_used(i):
                self._mark_used(i)
                self.hint = i + 1
                if self.hint >= total:
                    self.hint = 0
                return i

        raise RuntimeError('No free blocks')

    def flush(self) -> None:
        if not self.dirty:
            return
        self.f.seek(_abs_lba(self.sb, self.bitmap_block) * EYNFS_BLOCK_SIZE)
        self.f.write(self.bitmap)
        self.dirty = False


def _load_dir_state(f, sb, start_block):
    """Read a directory chain once and return a mutable state cache."""
    blocks = []
    free_slots = deque()
    entries_by_name = {}
    slot = 0
    block = start_block

    while block:
        f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE)
        data = f.read(EYNFS_BLOCK_SIZE)
        next_block = struct.unpack('<I', data[:4])[0]
        blocks.append(block)

        for i in range(4, EYNFS_BLOCK_SIZE, DIR_ENTRY_SIZE):
            entry_data = data[i:i+DIR_ENTRY_SIZE]
            if len(entry_data) < DIR_ENTRY_SIZE:
                break

            entry = struct.unpack(DIR_ENTRY_STRUCT, entry_data)
            raw_name = entry[0].split(b'\0', 1)[0]
            if not raw_name:
                free_slots.append(slot)
            else:
                try:
                    name = raw_name.decode('utf-8')
                except UnicodeDecodeError:
                    name = None
                if name:
                    entries_by_name[name] = (entry[1], entry[5], slot)
            slot += 1

        block = next_block

    return {
        'blocks': blocks,
        'free_slots': free_slots,
        'total_slots': slot,
        'entries_by_name': entries_by_name,
    }


def _new_empty_dir_state(block_num):
    return {
        'blocks': [block_num],
        'free_slots': deque(range(DIR_ENTRIES_PER_BLOCK)),
        'total_slots': DIR_ENTRIES_PER_BLOCK,
        'entries_by_name': {},
    }


def _alloc_dir_entry_slot(f, sb, dir_state, allocator, owner_name):
    if not dir_state['free_slots']:
        last_block = dir_state['blocks'][-1]
        new_block = allocator.alloc()

        # Link old tail -> new block.
        f.seek(_abs_lba(sb, last_block) * EYNFS_BLOCK_SIZE)
        f.write(struct.pack('<I', new_block))

        # Initialize new directory block (next pointer + entries all zero).
        f.seek(_abs_lba(sb, new_block) * EYNFS_BLOCK_SIZE)
        f.write(bytearray(EYNFS_BLOCK_SIZE))

        dir_state['blocks'].append(new_block)
        start = dir_state['total_slots']
        end = start + DIR_ENTRIES_PER_BLOCK
        for s in range(start, end):
            dir_state['free_slots'].append(s)
        dir_state['total_slots'] = end
        vprint(f"Allocated new directory block {new_block} for {owner_name}")

    slot = dir_state['free_slots'].popleft()
    block_num = dir_state['blocks'][slot // DIR_ENTRIES_PER_BLOCK]
    entry_idx = slot % DIR_ENTRIES_PER_BLOCK
    return block_num, entry_idx, slot


def _encode_name(name):
    name_bytes = name.encode('utf-8')[:EYNFS_NAME_MAX - 1] + b'\0'
    return name_bytes.ljust(EYNFS_NAME_MAX, b'\0')


def _write_file_data_from_path(f, sb, src_path, allocator):
    """Stream file data from disk into EYNFS data blocks."""
    size = os.path.getsize(src_path)
    if size == 0:
        return 0, 0

    prev_block = 0
    first_block = 0
    remaining = size

    with open(src_path, 'rb') as infile:
        while remaining > 0:
            block = allocator.alloc()
            if first_block == 0:
                first_block = block

            chunk_len = min(EYNFS_BLOCK_SIZE - 4, remaining)
            chunk = infile.read(chunk_len)
            if len(chunk) != chunk_len:
                raise IOError(f"Short read while copying {src_path}")

            block_data = bytearray(EYNFS_BLOCK_SIZE)
            block_data[4:4+chunk_len] = chunk

            if prev_block:
                f.seek(_abs_lba(sb, prev_block) * EYNFS_BLOCK_SIZE)
                f.write(struct.pack('<I', block))

            f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE)
            f.write(block_data)

            prev_block = block
            remaining -= chunk_len

    return first_block, size

def update_dir_entry(f, sb, block, entry_idx, entry):
    f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE + 4 + entry_idx * DIR_ENTRY_SIZE)
    f.write(struct.pack(DIR_ENTRY_STRUCT, *entry))

def find_dir_block(f, sb, path):
    # Traverse path, return block number of directory
    if path in ('', '/'): return sb['root_dir_block']
    parts = [p for p in path.strip('/').split('/') if p]
    block = sb['root_dir_block']
    for part in parts:
        entries, _ = read_dir_chain(f, sb, block)
        found = False
        for entry in entries:
            raw_name = entry[0].split(b'\0', 1)[0]
            if not raw_name:
                continue
            try:
                name = raw_name.decode('utf-8')
            except UnicodeDecodeError:
                continue
            if name == part and entry[1] == EYNFS_TYPE_DIR:
                block = entry[5]
                found = True
                break
        if not found:
            return None
    return block

def add_dir(f, sb, parent_state, dirname, allocator):
    existing = parent_state['entries_by_name'].get(dirname)
    if existing:
        if existing[0] == EYNFS_TYPE_DIR:
            return existing[1]
        raise RuntimeError(f"Cannot create directory '{dirname}': file exists with same name")

    new_block = allocator.alloc()
    f.seek(_abs_lba(sb, new_block) * EYNFS_BLOCK_SIZE)
    f.write(bytearray(EYNFS_BLOCK_SIZE))

    block_num, entry_idx, slot = _alloc_dir_entry_slot(f, sb, parent_state, allocator, dirname)
    entry = (_encode_name(dirname), EYNFS_TYPE_DIR, 0, 0, 0, new_block, 0, 0)
    update_dir_entry(f, sb, block_num, entry_idx, entry)
    parent_state['entries_by_name'][dirname] = (EYNFS_TYPE_DIR, new_block, slot)
    vprint(f"Created directory {dirname} at block {new_block}")
    return new_block


def add_file(f, sb, dir_state, filename, src_path, allocator):
    first_block, size = _write_file_data_from_path(f, sb, src_path, allocator)
    block_num, entry_idx, slot = _alloc_dir_entry_slot(f, sb, dir_state, allocator, filename)
    entry = (_encode_name(filename), EYNFS_TYPE_FILE, 0, 0, size, first_block, 0, 0)
    update_dir_entry(f, sb, block_num, entry_idx, entry)
    dir_state['entries_by_name'][filename] = (EYNFS_TYPE_FILE, first_block, slot)
    vprint(f"Copied {filename} ({size} bytes, first block {first_block})")
    return size

def clear_root_directory(f, sb):
    # Zero out all directory entries in the root directory chain
    block = sb['root_dir_block']
    while block:
        f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE)
        data = bytearray(f.read(EYNFS_BLOCK_SIZE))
        next_block = struct.unpack('<I', data[:4])[0]
        # Zero out all entries (but keep the next pointer)
        for i in range(4, EYNFS_BLOCK_SIZE):
            data[i] = 0
        f.seek(_abs_lba(sb, block) * EYNFS_BLOCK_SIZE)
        f.write(data)
        block = next_block


def _copy_tree_to_eynfs(f, sb, allocator, source_dir, dest_prefix, path_to_block, dir_states, stats):
    """
    Copy one source tree into EYNFS.

    dest_prefix examples:
      ''        -> copy to root
      'fonts'   -> copy under /fonts
      'include' -> copy under /include
    """
    for root, dirs, files in os.walk(source_dir):
        dirs.sort()
        files.sort()

        rel_dir = os.path.relpath(root, source_dir)
        rel_dir = '' if rel_dir == '.' else rel_dir.replace('\\', '/')

        if dest_prefix:
            eynfs_path = dest_prefix if not rel_dir else f"{dest_prefix}/{rel_dir}"
        else:
            eynfs_path = rel_dir

        if eynfs_path not in path_to_block:
            parent_path = os.path.dirname(eynfs_path)
            if parent_path not in path_to_block:
                raise RuntimeError(f"Parent path not found while creating '{eynfs_path}'")

            parent_state = dir_states[parent_path]
            dir_block = add_dir(f, sb, parent_state, os.path.basename(eynfs_path), allocator)
            path_to_block[eynfs_path] = dir_block
            dir_states[eynfs_path] = _new_empty_dir_state(dir_block)
            stats['dirs'] += 1

        dir_state = dir_states[eynfs_path]

        for file in files:
            src_file = os.path.join(root, file)
            size = add_file(f, sb, dir_state, file, src_file, allocator)
            stats['files'] += 1
            stats['bytes'] += size

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
        allocator = FreeBlockAllocator(f, sb)

        # Directory/path caches avoid repeated root-to-leaf scans for each file.
        root_block = sb['root_dir_block']
        path_to_block = {'': root_block}
        dir_states = {'': _load_dir_state(f, sb, root_block)}
        stats = {'dirs': 0, 'files': 0, 'bytes': 0}

        _copy_tree_to_eynfs(f, sb, allocator, source_dir, '', path_to_block, dir_states, stats)

        # Also copy the repository's top-level fonts/ directory (if present)
        # into /fonts so default system fonts are available in the image.
        copy_repo_fonts = os.environ.get('EYNFS_COPY_FONTS', '1') != '0'
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
        fonts_dir = os.path.join(repo_root, 'fonts')
        if copy_repo_fonts and os.path.isdir(fonts_dir):
            _copy_tree_to_eynfs(f, sb, allocator, fonts_dir, 'fonts', path_to_block, dir_states, stats)

        # Also copy userland/include into /include so userland compilers can
        # resolve #include <...> without depending on repo layout.
        copy_repo_headers = os.environ.get('EYNFS_COPY_HEADERS', '1') != '0'
        userland_include_dir = os.path.join(repo_root, 'userland', 'include')
        if copy_repo_headers and os.path.isdir(userland_include_dir):
            _copy_tree_to_eynfs(f, sb, allocator, userland_include_dir, 'include', path_to_block, dir_states, stats)

        allocator.flush()
    
    print(f"Copied {stats['files']} files ({stats['bytes']} bytes), created {stats['dirs']} directories")
    print(f"Successfully copied '{source_dir}' to '{img_file}'")

if __name__ == '__main__':
    main() 