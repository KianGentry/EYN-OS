#include <fs/vfs.h>
#include <eynfs.h>
#include <fat32.h>
#include <string.h>
#include <system.h>
#include <util.h>  // for malloc/free

#define EYNFS_SUPERBLOCK_LBA 2048

// Internal: FAT32 helpers for absolute path traversal (8.3 only for now)
static int fat32_path_to_entry(uint8 drive, struct fat32_bpb* bpb, const char* abspath, struct fat32_dir_entry* out, uint32* out_cluster) {
    if (!abspath || abspath[0] != '/') return -1;
    // Prepare components
    char path[256];
    strncpy(path, abspath, sizeof(path)-1); path[sizeof(path)-1] = '\0';
    // Skip root
    char* p = path;
    if (*p == '/') p++;
    uint32 cur = bpb->RootClus;
    if (*p == '\0') {
        // Root itself
        if (out) memset(out, 0, sizeof(*out));
        if (out_cluster) *out_cluster = cur;
        return 0;
    }
    // Iterate components
    char comp[13];
    while (*p) {
        // Extract next component
        size_t k = 0;
        while (*p && *p != '/' && k < sizeof(comp)-1) comp[k++] = *p++;
        comp[k] = '\0';
        if (*p == '/') p++;
        // Convert to 8.3 upper padded
        char fatname[12];
        to_fat32_83(comp, fatname);
        struct fat32_dir_entry entry;
        int found_cluster = fat32_find_entry_sector(drive, bpb, cur, fatname, &entry);
        if (found_cluster < 0) return -1;
        cur = (uint32)found_cluster;
        if (*p == '\0') {
            if (out) *out = entry;
            if (out_cluster) *out_cluster = cur;
            return 0;
        }
        // Must be a directory to continue
        if (!(entry.Attr & 0x10)) return -1;
    }
    return -1;
}

// Helpers for FAT32 writes (sector I/O)
static int fat32_read_fat_entry(uint8 drive, uint32 part_lba, struct fat32_bpb* bpb, uint32 clus, uint32* out_val) {
    uint8 sector[512];
    uint32 fat_sector = bpb->RsvdSecCnt + (clus * 4) / bpb->BytsPerSec;
    if (ata_read_sector(drive, part_lba + fat_sector, sector) != 0) return -1;
    uint32* fat = (uint32*)sector;
    uint32 idx = clus % (bpb->BytsPerSec / 4);
    *out_val = fat[idx] & 0x0FFFFFFF;
    return 0;
}

static int fat32_write_fat_entry_all(uint8 drive, uint32 part_lba, struct fat32_bpb* bpb, uint32 clus, uint32 val) {
    // Update all FAT copies
    uint8 sector[512];
    uint32 entries_per_sec = bpb->BytsPerSec / 4;
    for (uint32 f = 0; f < bpb->NumFATs; ++f) {
        uint32 fat_sector = bpb->RsvdSecCnt + (clus * 4) / bpb->BytsPerSec + (f * bpb->FATSz32);
        if (ata_read_sector(drive, part_lba + fat_sector, sector) != 0) return -1;
        uint32* fat = (uint32*)sector;
        uint32 idx = clus % entries_per_sec;
        fat[idx] = (fat[idx] & 0xF0000000) | (val & 0x0FFFFFFF);
        if (ata_write_sector(drive, part_lba + fat_sector, sector) != 0) return -2;
    }
    return 0;
}

static int fat32_free_chain_sector_all(uint8 drive, uint32 part_lba, struct fat32_bpb* bpb, uint32 start_clus) {
    if (start_clus < 2) return 0;
    uint32 cur = start_clus;
    for (int guard = 0; guard < 1<<24; ++guard) { // safety
        uint32 next;
        if (fat32_read_fat_entry(drive, part_lba, bpb, cur, &next) != 0) return -1;
        if (fat32_write_fat_entry_all(drive, part_lba, bpb, cur, 0x00000000) != 0) return -2; // free
        if (next >= 0x0FFFFFF8) break;
        cur = next;
    }
    return 0;
}

static int fat32_allocate_chain_sector_all(uint8 drive, uint32 part_lba, struct fat32_bpb* bpb, uint32 clusters_needed, uint32* out_first) {
    if (clusters_needed == 0) { *out_first = 0; return 0; }
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 entries_per_sec = byts_per_sec / 4;
    uint32 first = 0, prev = 0;
    uint8 sector[512];
    uint32 last_fat_sector = 0xFFFFFFFF;
    uint32* fat = NULL;
    for (uint32 clus = 2; clusters_needed > 0 && clus < 0x0FFFFFF0; ++clus) {
        uint32 fat_sector = bpb->RsvdSecCnt + ((clus * 4) / byts_per_sec);
        if (fat_sector != last_fat_sector) {
            if (ata_read_sector(drive, part_lba + fat_sector, sector) != 0) goto fail;
            fat = (uint32*)sector;
            last_fat_sector = fat_sector;
        }
        uint32 idx = clus % entries_per_sec;
        uint32 val = fat[idx] & 0x0FFFFFFF;
        if (val == 0) {
            if (prev != 0) {
                if (fat32_write_fat_entry_all(drive, part_lba, bpb, prev, clus) != 0) goto fail;
            } else {
                first = clus;
            }
            prev = clus;
            clusters_needed--;
            if (fat32_write_fat_entry_all(drive, part_lba, bpb, clus, 0x0FFFFFF8) != 0) goto fail;
        }
    }
    if (clusters_needed == 0) { *out_first = first; return 0; }
fail:
    if (first) fat32_free_chain_sector_all(drive, part_lba, bpb, first);
    return -5;
}

vfs_fs_type_t vfs_detect(uint8 drive) {
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        return VFS_FS_EYNFS;
    }
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(drive, part_lba, &bpb) == 0 && bpb.BytsPerSec == 512 && bpb.FATSz32 != 0) {
        return VFS_FS_FAT32;
    }
    return VFS_FS_NONE;
}

int vfs_get_file_size(uint8 drive, const char* path, uint32* out_size) {
    if (!path || !out_size) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) == 0 && ent.type == EYNFS_TYPE_FILE) {
            *out_size = ent.size; return 0;
        }
        return -2;
    }
    // FAT32
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -3;
    struct fat32_dir_entry dent; uint32 cluster;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &cluster) != 0) return -4;
    if (dent.Attr & 0x10) return -5; // directory
    *out_size = dent.FileSize; return 0;
}

int vfs_read_file(uint8 drive, const char* path, void* buf, int bufsize) {
    if (!path || !buf || bufsize <= 0) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0 || ent.type != EYNFS_TYPE_FILE) return -2;
        int to_read = (ent.size < (uint32)bufsize) ? ent.size : bufsize;
        return eynfs_read_file(drive, &sb, &ent, buf, to_read, 0);
    }
    // FAT32 path
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -3;
    struct fat32_dir_entry dent; uint32 file_first;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &file_first) != 0) return -4;
    if (dent.Attr & 0x10) return -5;
    // Walk cluster chain
    uint32 byts_per_sec = bpb.BytsPerSec;
    uint32 sec_per_clus = bpb.SecPerClus;
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint32 file_size = dent.FileSize;
    if (file_size > (uint32)bufsize) file_size = bufsize;
    uint32 bytes_read = 0;
    uint32 cur = file_first;
    uint8 sector[512];
    while (cur < 0x0FFFFFF8 && bytes_read < file_size) {
        uint32 data_sec = first_data_sec + ((cur - 2) * sec_per_clus);
        for (uint32 s = 0; s < sec_per_clus && bytes_read < file_size; ++s) {
            if (ata_read_sector(drive, part_lba + data_sec + s, sector) != 0) return -6;
            uint32 to_copy = byts_per_sec;
            if (bytes_read + to_copy > file_size) to_copy = file_size - bytes_read;
            memcpy((char*)buf + bytes_read, sector, to_copy);
            bytes_read += to_copy;
        }
        cur = fat32_next_cluster_sector(drive, part_lba, &bpb, cur);
    }
    return (int)bytes_read;
}

int vfs_read_file_at(uint8 drive, const char* path, void* buf, int bufsize, uint32 offset) {
    if (!path || !buf || bufsize <= 0) return -1;

    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0 || ent.type != EYNFS_TYPE_FILE) return -2;
        if (offset >= ent.size) return 0;
        uint32 remaining = ent.size - offset;
        int to_read = (remaining < (uint32)bufsize) ? (int)remaining : bufsize;
        return eynfs_read_file(drive, &sb, &ent, buf, to_read, offset);
    }

    // FAT32 path
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -3;
    struct fat32_dir_entry dent; uint32 file_first;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &file_first) != 0) return -4;
    if (dent.Attr & 0x10) return -5;

    uint32 file_size = dent.FileSize;
    if (offset >= file_size) return 0;
    uint32 remaining = file_size - offset;
    uint32 want = (remaining < (uint32)bufsize) ? remaining : (uint32)bufsize;

    uint32 byts_per_sec = bpb.BytsPerSec;
    uint32 sec_per_clus = bpb.SecPerClus;
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint32 bytes_per_clus = byts_per_sec * sec_per_clus;

    // Seek to the cluster containing the offset.
    uint32 cur = file_first;
    uint32 skip = offset;
    while (cur < 0x0FFFFFF8 && skip >= bytes_per_clus) {
        skip -= bytes_per_clus;
        cur = fat32_next_cluster_sector(drive, part_lba, &bpb, cur);
    }
    if (cur >= 0x0FFFFFF8) return 0;

    uint32 bytes_read = 0;
    uint8 sector[512];
    while (cur < 0x0FFFFFF8 && bytes_read < want) {
        uint32 data_sec = first_data_sec + ((cur - 2) * sec_per_clus);
        for (uint32 s = 0; s < sec_per_clus && bytes_read < want; ++s) {
            if (ata_read_sector(drive, part_lba + data_sec + s, sector) != 0) return -6;

            uint32 sec_off = 0;
            if (skip) {
                if (skip >= byts_per_sec) {
                    skip -= byts_per_sec;
                    continue;
                }
                sec_off = skip;
                skip = 0;
            }

            uint32 avail = byts_per_sec - sec_off;
            uint32 to_copy = avail;
            if (bytes_read + to_copy > want) to_copy = want - bytes_read;
            memcpy((char*)buf + bytes_read, sector + sec_off, to_copy);
            bytes_read += to_copy;
        }
        cur = fat32_next_cluster_sector(drive, part_lba, &bpb, cur);
    }
    return (int)bytes_read;
}

int vfs_stat(uint8 drive, const char* path, vfs_stat_t* st) {
    if (!path || !st) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; if (eynfs_traverse_path(drive, &sb, path, &ent, NULL, NULL) != 0) return -2;
        st->type = (ent.type == EYNFS_TYPE_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
        st->size = ent.size; return 0;
    }
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -3;
    // Root directory special case
    if (strcmp(path, "/") == 0) {
        st->type = VFS_NODE_DIR;
        st->size = 0;
        return 0;
    }
    struct fat32_dir_entry dent; uint32 clus;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &clus) != 0) return -4;
    if (dent.Attr & 0x10) { st->type = VFS_NODE_DIR; st->size = 0; }
    else { st->type = VFS_NODE_FILE; st->size = dent.FileSize; }
    return 0;
}

int vfs_listdir(uint8 drive, const char* path,
                int (*cb)(const char* name, int is_dir, uint32 size, void* user),
                void* user) {
    if (!path || !cb) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        // Resolve path to directory
        eynfs_dir_entry_t dir; if (strcmp(path, "/") == 0) { dir.type = EYNFS_TYPE_DIR; dir.first_block = sb.root_dir_block; }
        else if (eynfs_traverse_path(drive, &sb, path, &dir, NULL, NULL) != 0 || dir.type != EYNFS_TYPE_DIR) return -2;
        // Use heap allocation instead of stack to avoid stack overflow (128 entries * 84 bytes = 10,752 bytes)
        const int max_entries = 48; // Limit to ~4KB heap allocation
        eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(max_entries * sizeof(eynfs_dir_entry_t));
        if (!entries) return -3;
        int count = eynfs_read_dir_table(drive, dir.first_block, entries, max_entries);
        if (count < 0) { free(entries); return -3; }
        for (int i = 0; i < count; ++i) {
            if (entries[i].name[0] == '\0') continue;
            int stop = cb(entries[i].name, entries[i].type == EYNFS_TYPE_DIR, entries[i].size, user);
            if (stop) break;
        }
        free(entries);
        return 0;
    }
    // FAT32 dir list
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -4;
    struct fat32_dir_entry dent; uint32 clus;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &clus) != 0) {
        // If path is root '/'
        if (strcmp(path, "/") == 0) clus = bpb.RootClus; else return -5;
    }
    uint32 byts_per_sec = bpb.BytsPerSec;
    uint32 sec_per_clus = bpb.SecPerClus;
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint8 sector[512];
    while (clus < 0x0FFFFFF8) {
        uint32 base = first_data_sec + ((clus - 2) * sec_per_clus);
        for (uint32 s = 0; s < sec_per_clus; ++s) {
            if (ata_read_sector(drive, part_lba + base + s, sector) != 0) return -6;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            int per = byts_per_sec / sizeof(struct fat32_dir_entry);
            for (int i = 0; i < per; ++i) {
                if (entries[i].Name[0] == 0x00) return 0; // end of dir
                if ((entries[i].Attr & 0x0F) == 0x0F) continue; // skip LFN for now
                if (entries[i].Attr & 0x08) continue; // skip volume label
                if (entries[i].Name[0] == 0xE5) continue; // deleted
                // Build friendly 8.3 display name
                char base[9]; int bi=0; for (int j=0;j<8 && entries[i].Name[j] != ' '; ++j) base[bi++] = entries[i].Name[j]; base[bi] = '\0';
                char ext[4]; int ei=0; for (int j=0;j<3 && entries[i].Name[8+j] != ' '; ++j) ext[ei++] = entries[i].Name[8+j]; ext[ei] = '\0';
                char disp[13]; disp[0]='\0';
                strncpy(disp, base, sizeof(disp)-1); disp[sizeof(disp)-1]='\0';
                if (ei > 0) { strncat(disp, ".", sizeof(disp)-strlen(disp)-1); strncat(disp, ext, sizeof(disp)-strlen(disp)-1); }
                if (disp[0] == '.' && (disp[1] == '\0' || (disp[1]=='.' && disp[2]=='\0'))) continue; // skip . and ..
                int is_dir = (entries[i].Attr & 0x10) ? 1 : 0;
                uint32 size = is_dir ? 0 : entries[i].FileSize;
                int stop = cb(disp, is_dir, size, user);
                if (stop) return 0;
            }
        }
        clus = fat32_next_cluster_sector(drive, part_lba, &bpb, clus);
    }
    return 0;
}

static void vfs_split_path(const char* abspath, char* parent_out, size_t parent_sz, char* base_out, size_t base_sz) {
    if (!abspath || abspath[0] != '/') { if (parent_out) { if (parent_sz) parent_out[0] = '\0'; } if (base_out) { if (base_sz) base_out[0] = '\0'; } return; }
    strncpy(parent_out, abspath, parent_sz-1); parent_out[parent_sz-1] = '\0';
    // remove trailing slash (except root)
    size_t len = strlen(parent_out);
    if (len > 1 && parent_out[len-1] == '/') { parent_out[len-1] = '\0'; len--; }
    // find last slash
    char* last = strrchr(parent_out, '/');
    if (!last) { strncpy(base_out, abspath, base_sz-1); base_out[base_sz-1]='\0'; strncpy(parent_out, "/", parent_sz); return; }
    strncpy(base_out, last+1, base_sz-1); base_out[base_sz-1] = '\0';
    if (last == parent_out) { parent_out[1] = '\0'; } else { *last = '\0'; }
}

int vfs_write_file(uint8 drive, const char* path, const void* buf, uint32 size) {
    if (!path || !buf) return -1;
    // EYNFS fast-path
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0) {
            // create
            char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
            eynfs_dir_entry_t p; if (eynfs_traverse_path(drive, &sb, parent, &p, NULL, NULL) != 0 || p.type != EYNFS_TYPE_DIR) return -2;
            if (eynfs_create_entry(drive, &sb, p.first_block, base, EYNFS_TYPE_FILE) != 0) return -3;
            if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0) return -4;
        }
        int written = eynfs_write_file(drive, &sb, &ent, (void*)buf, size, pb, idx);
        return (written >= 0) ? written : -5;
    }
    // FAT32 write
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -10;
    char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
    // Resolve parent directory cluster
    struct fat32_dir_entry parent_ent; uint32 parent_clus;
    if (strcmp(parent, "/") == 0) { parent_clus = bpb.RootClus; memset(&parent_ent, 0, sizeof(parent_ent)); parent_ent.Attr = 0x10; }
    else {
        if (fat32_path_to_entry(drive, &bpb, parent, &parent_ent, &parent_clus) != 0 || !(parent_ent.Attr & 0x10)) return -11;
    }
    // Convert base to 8.3
    char fatname[12]; to_fat32_83(base, fatname);
    struct fat32_dir_entry exist; int exist_cluster = fat32_find_entry_sector(drive, &bpb, parent_clus, fatname, &exist);
    // If an entry already exists with this name and it's a directory, do not overwrite it with a file
    if (exist_cluster >= 0 && (exist.Attr & 0x10)) {
        return -20; // name exists as directory
    }
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint32 bytes_per_cluster = bpb.BytsPerSec * bpb.SecPerClus;
    uint32 clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    uint32 new_first = 0;
    if (clusters_needed > 0) {
        if (fat32_allocate_chain_sector_all(drive, part_lba, &bpb, clusters_needed, &new_first) != 0) return -12;
    } else {
        new_first = 0; // empty file
    }
    // Write data across allocated chain
    if (clusters_needed > 0) {
        const uint8* p = (const uint8*)buf; uint32 remaining = size; uint32 cur = new_first;
        while (cur >= 2 && cur < 0x0FFFFFF8) {
            uint32 data_sec = first_data_sec + ((cur - 2) * bpb.SecPerClus);
            for (uint32 s = 0; s < bpb.SecPerClus; ++s) {
                uint8 secbuf[512];
                uint32 to_copy = (remaining >= bpb.BytsPerSec) ? bpb.BytsPerSec : remaining;
                if (to_copy > 0) memcpy(secbuf, p, to_copy);
                if (to_copy < bpb.BytsPerSec) memset(secbuf + to_copy, 0, bpb.BytsPerSec - to_copy);
                if (ata_write_sector(drive, part_lba + data_sec + s, secbuf) != 0) { fat32_free_chain_sector_all(drive, part_lba, &bpb, new_first); return -13; }
                if (remaining > to_copy) { remaining -= to_copy; p += to_copy; } else { remaining = 0; }
                if (remaining == 0 && s + 1 < bpb.SecPerClus) {
                    // zero out rest of cluster sectors (already zeroed via memset paths)
                }
                if (remaining == 0) {
                    // still must loop to next sector writes zeroed, but we already zero padded; continue
                }
            }
            // next cluster
            uint32 next; if (fat32_read_fat_entry(drive, part_lba, &bpb, cur, &next) != 0) break;
            if (next >= 0x0FFFFFF8) break;
            cur = next;
        }
    }
    // If replacing existing file, free old chain and update entry
    if (exist_cluster >= 0) {
        uint32 old_first = ((uint32)exist.FstClusHI << 16) | exist.FstClusLO;
        if (old_first >= 2) fat32_free_chain_sector_all(drive, part_lba, &bpb, old_first);
        // Find and update the exact directory entry sector where it resides
        // We can re-scan to find and overwrite
        uint8 sector[512];
        uint32 cluster = parent_clus;
        while (cluster < 0x0FFFFFF8) {
            uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
            for (uint32 sec = 0; sec < bpb.SecPerClus; ++sec) {
                if (ata_read_sector(drive, part_lba + cluster_first_sec + sec, sector) != 0) return -14;
                struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
                int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
                for (int i = 0; i < per; ++i) {
                    if (entries[i].Name[0] == 0x00) break;
                    if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                    if (entries[i].Name[0] == 0xE5) continue;
                    char name[12]; for (int j=0;j<11;j++) name[j] = entries[i].Name[j]; name[11]='\0';
                    if (strcmp(name, fatname) == 0) {
                        // Ensure this entry is marked as a regular file
                        entries[i].Attr = 0x20;
                        entries[i].FstClusHI = (new_first >> 16) & 0xFFFF;
                        entries[i].FstClusLO = new_first & 0xFFFF;
                        entries[i].FileSize = size;
                        if (ata_write_sector(drive, part_lba + cluster_first_sec + sec, sector) != 0) return -15;
                        return (int)size;
                    }
                }
            }
            cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
        }
        return -16; // shouldn't reach
    }
    // Otherwise, create a new directory entry in parent
    uint8 sector[512];
    uint32 cluster = parent_clus;
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 sec = 0; sec < bpb.SecPerClus; ++sec) {
            if (ata_read_sector(drive, part_lba + cluster_first_sec + sec, sector) != 0) return -17;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            for (int i = 0; i < per; ++i) {
                if (entries[i].Name[0] == 0x00 || entries[i].Name[0] == 0xE5) {
                    // write here
                    for (int j=0;j<11;j++) entries[i].Name[j] = fatname[j];
                    entries[i].Attr = 0x20; entries[i].NTRes = 0; entries[i].CrtTimeTenth = 0;
                    entries[i].CrtTime = 0; entries[i].CrtDate = 0; entries[i].LstAccDate = 0; entries[i].WrtTime = 0; entries[i].WrtDate = 0;
                    entries[i].FstClusHI = (new_first >> 16) & 0xFFFF;
                    entries[i].FstClusLO = new_first & 0xFFFF;
                    entries[i].FileSize = size;
                    if (ata_write_sector(drive, part_lba + cluster_first_sec + sec, sector) != 0) return -18;
                    return (int)size;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    return -19; // dir full
}

// Create a directory
int vfs_mkdir(uint8 drive, const char* path) {
    if (!path || path[0] != '/') return -1;
    // EYNFS
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        // Split
        char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
        eynfs_dir_entry_t p;
        if (eynfs_traverse_path(drive, &sb, parent, &p, NULL, NULL) != 0 || p.type != EYNFS_TYPE_DIR) return -2;
        return (eynfs_create_entry(drive, &sb, p.first_block, base, EYNFS_TYPE_DIR) == 0) ? 0 : -3;
    }
    // FAT32
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -10;
    char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
    // Resolve parent dir
    struct fat32_dir_entry pentry; uint32 pclus;
    if (strcmp(parent, "/") == 0) { pclus = bpb.RootClus; memset(&pentry, 0, sizeof(pentry)); pentry.Attr = 0x10; }
    else if (fat32_path_to_entry(drive, &bpb, parent, &pentry, &pclus) != 0 || !(pentry.Attr & 0x10)) return -11;
    // Check for name collision before creating: existing file or directory with same name
    char fatname[12]; to_fat32_83(base, fatname);
    {
        struct fat32_dir_entry exist;
        int exist_cluster = fat32_find_entry_sector(drive, &bpb, pclus, fatname, &exist);
        if (exist_cluster >= 0) {
            if (exist.Attr & 0x10) return -17; // directory already exists
            else return -18; // a file with this name exists
        }
    }
    // Create directory: allocate one cluster and set up '.' and '..' entries
    uint32 bytes_per_cluster = bpb.BytsPerSec * bpb.SecPerClus;
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint32 newclus = 0; if (fat32_allocate_chain_sector_all(drive, part_lba, &bpb, 1, &newclus) != 0) return -12;
    // Initialize first sector
    {
        uint8 sector_init[512]; memset(sector_init, 0, sizeof(sector_init));
        struct fat32_dir_entry* dents = (struct fat32_dir_entry*)sector_init;
        // '.'
        memset(dents[0].Name, ' ', 11); dents[0].Name[0] = '.';
        dents[0].Attr = 0x10; dents[0].NTRes = 0; dents[0].CrtTimeTenth = 0;
        dents[0].CrtTime = 0; dents[0].CrtDate = 0; dents[0].LstAccDate = 0; dents[0].WrtTime = 0; dents[0].WrtDate = 0;
        dents[0].FstClusHI = (newclus >> 16) & 0xFFFF; dents[0].FstClusLO = newclus & 0xFFFF; dents[0].FileSize = 0;
        // '..'
        memset(dents[1].Name, ' ', 11); dents[1].Name[0] = '.'; dents[1].Name[1] = '.';
        dents[1].Attr = 0x10; dents[1].NTRes = 0; dents[1].CrtTimeTenth = 0;
        dents[1].CrtTime = 0; dents[1].CrtDate = 0; dents[1].LstAccDate = 0; dents[1].WrtTime = 0; dents[1].WrtDate = 0;
        uint32 dotdot = pclus; // parent cluster (root uses root cluster)
        dents[1].FstClusHI = (dotdot >> 16) & 0xFFFF; dents[1].FstClusLO = dotdot & 0xFFFF; dents[1].FileSize = 0;
        if (ata_write_sector(drive, part_lba + first_data_sec + ((newclus - 2) * bpb.SecPerClus) + 0, sector_init) != 0) { fat32_free_chain_sector_all(drive, part_lba, &bpb, newclus); return -13; }
    }
    // Zero remaining sectors in the cluster (if any)
    for (uint32 s = 1; s < bpb.SecPerClus; ++s) { uint8 zero[512]; memset(zero, 0, 512); if (ata_write_sector(drive, part_lba + first_data_sec + ((newclus - 2) * bpb.SecPerClus) + s, zero) != 0) { fat32_free_chain_sector_all(drive, part_lba, &bpb, newclus); return -13; } }
    // Write dir entry in parent
    // fatname already computed above
    uint8 sector[512]; uint32 cluster = pclus;
    while (cluster < 0x0FFFFFF8) {
        uint32 csec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 s = 0; s < bpb.SecPerClus; ++s) {
            if (ata_read_sector(drive, part_lba + csec + s, sector) != 0) return -14;
            struct fat32_dir_entry* ents = (struct fat32_dir_entry*)sector; int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            for (int i = 0; i < per; ++i) {
                if (ents[i].Name[0] == 0x00 || ents[i].Name[0] == 0xE5) {
                    for (int j=0;j<11;j++) ents[i].Name[j] = fatname[j];
                    ents[i].Attr = 0x10; ents[i].NTRes = 0; ents[i].CrtTimeTenth = 0; ents[i].CrtTime = 0; ents[i].CrtDate = 0; ents[i].LstAccDate = 0; ents[i].WrtTime = 0; ents[i].WrtDate = 0;
                    ents[i].FstClusHI = (newclus >> 16) & 0xFFFF; ents[i].FstClusLO = newclus & 0xFFFF; ents[i].FileSize = 0;
                    if (ata_write_sector(drive, part_lba + csec + s, sector) != 0) return -15;
                    return 0;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    fat32_free_chain_sector_all(drive, part_lba, &bpb, newclus);
    return -16; // parent full
}

// Remove an empty directory
int vfs_rmdir(uint8 drive, const char* path) {
    if (!path || strcmp(path, "/") == 0) return -1;
    // EYNFS
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0 || ent.type != EYNFS_TYPE_DIR) return -2;
        // Check empty - use heap allocation instead of stack (64*84=5376 bytes would overflow)
        const int max_entries = 16; // Small allocation just to check if empty
        eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(max_entries * sizeof(eynfs_dir_entry_t));
        if (!entries) return -2;
        int count = eynfs_read_dir_table(drive, ent.first_block, entries, max_entries);
        for (int i=0;i<count;i++) { if (entries[i].name[0] != '\0') { free(entries); return -3; } }
        free(entries);
        return (eynfs_delete_entry(drive, &sb, pb, ent.name) == 0) ? 0 : -4;
    }
    // FAT32
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -10;
    struct fat32_dir_entry dent; uint32 dclus;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &dclus) != 0 || !(dent.Attr & 0x10)) return -11;
    // Ensure empty: iterate directory cluster(s), ensure all entries are 0x00/0xE5 or LFN
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint8 sector[512]; uint32 cluster = dclus;
    while (cluster < 0x0FFFFFF8) {
        uint32 csec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 s=0;s<bpb.SecPerClus;++s) {
            if (ata_read_sector(drive, part_lba + csec + s, sector) != 0) return -12;
            struct fat32_dir_entry* ents = (struct fat32_dir_entry*)sector; int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            for (int i=0;i<per;++i) {
                if (ents[i].Name[0] == 0x00) break;
                if ((ents[i].Attr & 0x0F) == 0x0F) continue; // LFN
                if (ents[i].Name[0] == 0xE5) continue; // deleted
                if (ents[i].Name[0] == '.') continue; // skip '.' and '..'
                // skip volume labels
                if (ents[i].Attr & 0x08) continue;
                // anything else is a visible file/dir
                return -13;
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    // Remove directory entry from parent
    char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
    struct fat32_dir_entry pentry; uint32 pclus;
    if (strcmp(parent, "/") == 0) { pclus = bpb.RootClus; memset(&pentry, 0, sizeof(pentry)); pentry.Attr = 0x10; }
    else if (fat32_path_to_entry(drive, &bpb, parent, &pentry, &pclus) != 0 || !(pentry.Attr & 0x10)) return -14;
    char fatname[12]; to_fat32_83(base, fatname);
    cluster = pclus;
    while (cluster < 0x0FFFFFF8) {
        uint32 csec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 s=0;s<bpb.SecPerClus;++s) {
            if (ata_read_sector(drive, part_lba + csec + s, sector) != 0) return -15;
            struct fat32_dir_entry* ents = (struct fat32_dir_entry*)sector; int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            for (int i=0;i<per;++i) {
                if (ents[i].Name[0] == 0x00) break;
                if ((ents[i].Attr & 0x0F) == 0x0F) continue;
                if (ents[i].Name[0] == 0xE5) continue;
                char name[12]; for (int j=0;j<11;j++) name[j] = ents[i].Name[j]; name[11]='\0';
                if (strcmp(name, fatname) == 0) {
                    ents[i].Name[0] = 0xE5; // mark deleted
                    if (ata_write_sector(drive, part_lba + csec + s, sector) != 0) return -16;
                    // free cluster chain of dir
                    uint32 first = ((uint32)dent.FstClusHI << 16) | dent.FstClusLO; if (first >= 2) fat32_free_chain_sector_all(drive, part_lba, &bpb, first);
                    return 0;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    return -17;
}

// Remove a file
int vfs_unlink(uint8 drive, const char* path) {
    if (!path || path[0] != '/') return -1;
    // EYNFS
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t ent; uint32_t pb, idx;
        if (eynfs_traverse_path(drive, &sb, path, &ent, &pb, &idx) != 0 || ent.type != EYNFS_TYPE_FILE) return -2;
        return (eynfs_delete_entry(drive, &sb, pb, ent.name) == 0) ? 0 : -3;
    }
    // FAT32
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -10;
    struct fat32_dir_entry dent; uint32 fclus;
    if (fat32_path_to_entry(drive, &bpb, path, &dent, &fclus) != 0) return -11;
    // If entry is marked as directory, confirm it's a real directory; otherwise treat as file
    if (dent.Attr & 0x10) {
        int is_real_dir = 0;
        if (fclus >= 2) {
            uint32 first_data_sec_chk = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
            uint8 secchk[512];
            if (ata_read_sector(drive, part_lba + first_data_sec_chk + ((fclus - 2) * bpb.SecPerClus), secchk) == 0) {
                struct fat32_dir_entry* ents = (struct fat32_dir_entry*)secchk;
                if (ents[0].Attr & 0x10) {
                    char n0[12]; for (int j=0;j<11;j++) n0[j]=ents[0].Name[j]; n0[11]='\0';
                    char n1[12]; for (int j=0;j<11;j++) n1[j]=ents[1].Name[j]; n1[11]='\0';
                    if (n0[0]=='.' && n1[0]=='.' && n1[1]=='.') is_real_dir = 1;
                }
            }
        }
        if (is_real_dir) return -11; // it's a real directory; use rmdir
    }
    // Remove from parent
    char parent[256], base[128]; vfs_split_path(path, parent, sizeof(parent), base, sizeof(base));
    struct fat32_dir_entry pentry; uint32 pclus;
    if (strcmp(parent, "/") == 0) { pclus = bpb.RootClus; memset(&pentry, 0, sizeof(pentry)); pentry.Attr = 0x10; }
    else if (fat32_path_to_entry(drive, &bpb, parent, &pentry, &pclus) != 0 || !(pentry.Attr & 0x10)) return -12;
    char fatname[12]; to_fat32_83(base, fatname);
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint8 sector[512]; uint32 cluster = pclus;
    while (cluster < 0x0FFFFFF8) {
        uint32 csec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 s=0;s<bpb.SecPerClus;++s) {
            if (ata_read_sector(drive, part_lba + csec + s, sector) != 0) return -13;
            struct fat32_dir_entry* ents = (struct fat32_dir_entry*)sector; int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            for (int i=0;i<per;++i) {
                if (ents[i].Name[0] == 0x00) break;
                if ((ents[i].Attr & 0x0F) == 0x0F) continue;
                if (ents[i].Name[0] == 0xE5) continue;
                char name[12]; for (int j=0;j<11;j++) name[j] = ents[i].Name[j]; name[11]='\0';
                if (strcmp(name, fatname) == 0) {
                    ents[i].Name[0] = 0xE5; // deleted
                    if (ata_write_sector(drive, part_lba + csec + s, sector) != 0) return -14;
                    // Free its cluster chain
                    uint32 first = ((uint32)dent.FstClusHI << 16) | dent.FstClusLO; if (first >= 2) fat32_free_chain_sector_all(drive, part_lba, &bpb, first);
                    return 0;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    return -15;
}
