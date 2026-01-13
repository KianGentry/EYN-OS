import sys, struct, os

EYNFS_MAGIC = 0x45594E46
EYNFS_BLOCK_SIZE = 512
SUPERBLOCK_LBA = 2048

DIR_ENTRY_NAME_MAX = 32
DIR_ENTRY_SIZE = 32 + 1 + 1 + 2 + 4 + 4 + 8  # matches packed struct in eynfs.h

def rd(f, off, size):
    f.seek(off)
    data = f.read(size)
    if len(data) != size:
        raise IOError(f"Short read at {off} for {size} bytes")
    return data


def blk_off(blk):
    # EYNFS block numbers are relative to the start of the EYNFS partition.
    return (SUPERBLOCK_LBA + blk) * EYNFS_BLOCK_SIZE

def bit_get(bmap, idx):
    byte = idx // 8
    bit = idx % 8
    if byte >= len(bmap):
        return 0
    return 1 if (bmap[byte] & (1 << bit)) else 0

def parse_superblock(b):
    # struct eynfs_superblock_t { u32 magic, version, block_size, total_blocks, root_dir_block, free_block_map, name_table_block, u32[2] };
    fields = struct.unpack('<8I', b[:32])
    return {
        'magic': fields[0],
        'version': fields[1],
        'block_size': fields[2],
        'total_blocks': fields[3],
        'root_dir_block': fields[4],
        'free_block_map': fields[5],
        'name_table_block': fields[6],
    }

def walk_dir_chain(f, start_block, total_blocks):
    chain = []
    seen = set()
    blk = start_block
    limit = 4096
    while blk != 0 and limit > 0:
        if blk >= total_blocks:
            return chain, [f"Dir block {blk} out of range (total {total_blocks})"], seen
        if blk in seen:
            return chain, [f"Cycle detected in dir chain at block {blk}"] , seen
        seen.add(blk)
        chain.append(blk)
        raw = rd(f, blk_off(blk), EYNFS_BLOCK_SIZE)
        (next_blk,) = struct.unpack('<I', raw[:4])
        blk = next_blk
        limit -= 1
    if limit == 0:
        return chain, ["Dir chain too long (limit 4096)"] , seen
    return chain, [], seen

def check_dir_entries(f, dir_block, total_blocks):
    errs = []
    raw = rd(f, blk_off(dir_block), EYNFS_BLOCK_SIZE)
    entries_bytes = raw[4:]
    per_block = (EYNFS_BLOCK_SIZE - 4) // DIR_ENTRY_SIZE
    for i in range(per_block):
        off = i * DIR_ENTRY_SIZE
        ent = entries_bytes[off:off+DIR_ENTRY_SIZE]
        if len(ent) < DIR_ENTRY_SIZE:
            break
        name = ent[:DIR_ENTRY_NAME_MAX]
        name = name.split(b'\x00', 1)[0]
        if not name:
            continue
        typ = ent[32]
        size, first_block = struct.unpack('<II', ent[36:44])
        # Basic name validation
        if any(c < 0x20 and c not in (9,10,13) for c in name):
            errs.append(f"Entry with control chars in name at dir block {dir_block}, slot {i}")
        if len(name) >= DIR_ENTRY_NAME_MAX and ent[31] != 0:
            errs.append(f"Non-terminated name at dir block {dir_block}, slot {i}")
        # Type validation
        if typ not in (1,2):
            errs.append(f"Invalid type {typ} at dir block {dir_block}, slot {i}")
        # First block sanity
        if first_block != 0 and first_block >= total_blocks:
            errs.append(f"first_block {first_block} out of range at dir block {dir_block}, slot {i}")
        # Size plausibility (optional)
        if size > (total_blocks * EYNFS_BLOCK_SIZE):
            errs.append(f"Unreasonable size {size} at dir block {dir_block}, slot {i}")
    return errs

def main():
    img = sys.argv[1] if len(sys.argv) >= 2 else 'eynfs.img'
    if not os.path.exists(img):
        print(f"Image not found: {img}")
        return 2
    size = os.path.getsize(img)
    # Total blocks in the partition is recorded in the superblock. We keep the
    # raw file-derived total blocks as a sanity check only.
    file_total_blocks = size // EYNFS_BLOCK_SIZE
    errs = []
    warns = []
    with open(img, 'rb') as f:
        # Superblock expected at LBA 2048
        sb_off = SUPERBLOCK_LBA * EYNFS_BLOCK_SIZE
        sb_raw = rd(f, sb_off, EYNFS_BLOCK_SIZE)
        sb = parse_superblock(sb_raw)
        print(f"Superblock: magic=0x{sb['magic']:08X} ver={sb['version']} block={sb['block_size']} total_blocks={sb['total_blocks']} root={sb['root_dir_block']} bitmap={sb['free_block_map']} nametab={sb['name_table_block']}")
        if sb['magic'] != EYNFS_MAGIC:
            errs.append("Bad magic in superblock")
        if sb['block_size'] != EYNFS_BLOCK_SIZE:
            errs.append(f"Unexpected block size {sb['block_size']}")
        if (SUPERBLOCK_LBA + sb['total_blocks']) > file_total_blocks:
            warns.append(f"Superblock total_blocks {sb['total_blocks']} exceeds image size ({file_total_blocks - SUPERBLOCK_LBA} blocks available after partition start)")
        # Bitmap
        bmap = rd(f, blk_off(sb['free_block_map']), EYNFS_BLOCK_SIZE)
        # Expect metadata LBAs reserved; formatter currently reserves first 4 only; flag that
        meta = [2048, 2049, 2050, 2051]
        for m in meta:
            if bit_get(bmap, m) != 1:
                warns.append(f"Bitmap does not mark metadata block {m} as used")
        # Walk root directory chain
        chain, derrs, _ = walk_dir_chain(f, sb['root_dir_block'], sb['total_blocks'])
        errs.extend(derrs)
        if chain:
            print(f"Root directory chain blocks: {chain}")
        else:
            warns.append("Empty or invalid root directory chain")
        # Validate entries in each dir block in chain
        for blk in chain:
            errs.extend(check_dir_entries(f, blk, total_blocks))

    print()
    if errs:
        print("Errors:")
        for e in errs:
            print(f"  - {e}")
    if warns:
        print("Warnings:")
        for w in warns:
            print(f"  - {w}")
    print()
    if errs:
        print(f"fsck_eynfs: FAIL ({len(errs)} error(s), {len(warns)} warning(s))")
        return 1
    else:
        print(f"fsck_eynfs: OK ({len(warns)} warning(s))")
        return 0

if __name__ == '__main__':
    sys.exit(main())
