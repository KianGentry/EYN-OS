#include <fat32.h>
#include <util.h>
#include <string.h>

#include <ata.h>

int printf(const char* fmt, ...);

void to_fat32_83(const char* input, char* output) {
    // convert input like "test.txt" to "TEST    TXT" (8.3, uppercased)
    int i = 0;
    int j = 0;

    // Copy name part (up to dot or 8 chars)
    while (input[i] && input[i] != '.' && j < 8) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        output[j++] = c;
        i++;
    }

    // Pad with spaces
    while (j < 8) output[j++] = ' ';

    // If dot, skip it
    if (input[i] == '.') i++;

    int ext = 0;
    // Copy extension (up to 3 chars)
    while (input[i] && ext < 3) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        output[j++] = c;
        i++;
        ext++;
    }

    // Pad extension with spaces
    while (ext < 3) {
        output[j++] = ' ';
        ext++;
    }

    output[j] = '\0';
}

// Read BPB from disk image
int fat32_read_bpb(void* disk_img, struct fat32_bpb* bpb) {
    if (!disk_img || !bpb) return -1;
    // BPB is at offset 0
            memcpy(bpb, (char*)disk_img, sizeof(struct fat32_bpb));
    return 0;
}

// List root directory entries (simple, no subdirs, no LFN)
int fat32_list_root(void* disk_img, struct fat32_bpb* bpb) {
    if (!disk_img || !bpb) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    
    uint32 cluster = root_clus;
    while (cluster < 0x0FFFFFF8) { // FAT32 end-of-chain
        uint32 dir_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        uint32 dir_offset = dir_sec * byts_per_sec;
        struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)disk_img + dir_offset);
        int entry_count = (byts_per_sec * sec_per_clus) / sizeof(struct fat32_dir_entry);
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].Name[0] == 0x00) break; // No more entries
            if ((entries[i].Attr & 0x0F) == 0x0F) continue; // Skip LFN
            if (entries[i].Name[0] == 0xE5) continue; // Deleted
            // Print name
            char name[12];
            for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
            name[11] = '\0';
            if (entries[i].Attr & 0x10) { // Directory
                printf("%c%s <DIR>\n", 255, 255, 255, name);
            } else {
                printf("%c%s\n", 255, 255, 255, name);
            }
        }
        cluster = fat32_next_cluster(disk_img, bpb, cluster);
    }
    return 0;
}

int fat32_read_file(void* disk_img, struct fat32_bpb* bpb, const char* filename, char* buf, int bufsize) {
    if (!disk_img || !bpb || !filename || !buf) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    
    uint32 cluster = root_clus;
    while (cluster < 0x0FFFFFF8) { // FAT32 end-of-chain
        uint32 dir_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        uint32 dir_offset = dir_sec * byts_per_sec;
        struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)disk_img + dir_offset);
        int entry_count = (byts_per_sec * sec_per_clus) / sizeof(struct fat32_dir_entry);
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].Name[0] == 0x00) break; // No more entries
            if ((entries[i].Attr & 0x0F) == 0x0F) continue; // Skip LFN
            if (entries[i].Name[0] == 0xE5) continue; // Deleted
            // Compare 8.3 name
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].Name[j] != filename[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                // Found file
                uint32 file_size = entries[i].FileSize;
                if (file_size > (uint32)bufsize) file_size = bufsize;
                uint32 first_clus = ((uint32)entries[i].FstClusHI << 16) | entries[i].FstClusLO;
                if (first_clus < 2) return -1;
                // Read file data (no FAT walking, just first cluster)
                uint32 data_sec = first_data_sec + ((first_clus - 2) * sec_per_clus);
                uint32 data_offset = data_sec * byts_per_sec;
                memcpy(buf, (char*)disk_img + data_offset, file_size);
                return file_size;
            }
        }
        cluster = fat32_next_cluster(disk_img, bpb, cluster);
    }
    return -1; // Not found
}

// Helper: get next cluster in chain
uint32 fat32_next_cluster(void* disk_img, struct fat32_bpb* bpb, uint32 cluster) {
    // FAT starts after reserved sectors
    uint32 fat_offset = bpb->RsvdSecCnt * bpb->BytsPerSec;
    uint32* fat = (uint32*)((char*)disk_img + fat_offset);
    return fat[cluster] & 0x0FFFFFFF; // 28 bits for FAT32
}

// List all entries in a directory given its starting cluster
int fat32_list_dir(void* disk_img, struct fat32_bpb* bpb, uint32 start_cluster) {
    if (!disk_img || !bpb || start_cluster < 2) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);

    uint32 cluster = start_cluster;
    while (cluster < 0x0FFFFFF8) { // FAT32 end-of-chain
        uint32 dir_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        uint32 dir_offset = dir_sec * byts_per_sec;
        struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)disk_img + dir_offset);
        for (int i = 0; i < (byts_per_sec * sec_per_clus) / sizeof(struct fat32_dir_entry); i++) {
            if (entries[i].Name[0] == 0x00) break; // No more entries
            if ((entries[i].Attr & 0x0F) == 0x0F) continue; // Skip LFN
            if (entries[i].Name[0] == 0xE5) continue; // Deleted
            char name[12];
            for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
            name[11] = '\0';
            if (entries[i].Attr & 0x10) {
                printf("%c%s <DIR>\n", 255, 255, 255, name);
            } else {
                printf("%c%s\n", 255, 255, 255, name);
            }
        }
        cluster = fat32_next_cluster(disk_img, bpb, cluster);
    }
    return 0;
}

// Find a directory entry by name in a directory, returns cluster or -1 if not found
int fat32_find_entry(void* disk_img, struct fat32_bpb* bpb, uint32 dir_cluster, const char* name, struct fat32_dir_entry* out_entry) {
    if (!disk_img || !bpb || dir_cluster < 2 || !name) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);

    uint32 cluster = dir_cluster;
    while (cluster < 0x0FFFFFF8) {
        uint32 dir_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        uint32 dir_offset = dir_sec * byts_per_sec;
        struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)disk_img + dir_offset);
        for (int i = 0; i < (byts_per_sec * sec_per_clus) / sizeof(struct fat32_dir_entry); i++) {
            if (entries[i].Name[0] == 0x00) break;
            if ((entries[i].Attr & 0x0F) == 0x0F) continue;
            if (entries[i].Name[0] == 0xE5) continue;
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].Name[j] != name[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if (out_entry) *out_entry = entries[i];
                uint32 entry_cluster = ((uint32)entries[i].FstClusHI << 16) | entries[i].FstClusLO;
                return entry_cluster;
            }
        }
        cluster = fat32_next_cluster(disk_img, bpb, cluster);
    }
    return -1;
}

int fat32_write_file(void* disk_img, struct fat32_bpb* bpb, const char* filename, const char* buf, int bufsize) {
    if (!disk_img || !bpb || !filename || !buf || bufsize <= 0) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    uint32 root_dir_sec = first_data_sec + ((root_clus - 2) * sec_per_clus);
    uint32 root_dir_offset = root_dir_sec * byts_per_sec;
    struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)disk_img + root_dir_offset);
    int entry_count = (byts_per_sec * sec_per_clus) / sizeof(struct fat32_dir_entry);

    // 1. Check if file already exists
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].Name[0] == 0x00) break;
        if ((entries[i].Attr & 0x0F) == 0x0F) continue;
        if (entries[i].Name[0] == 0xE5) continue;
        int match = 1;
        for (int j = 0; j < 11; j++) {
            if (entries[i].Name[j] != filename[j]) {
                match = 0;
                break;
            }
        }
        if (match) return -2; // File exists
    }

    // 2. Find a free directory entry
    int free_entry = -1;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].Name[0] == 0x00 || entries[i].Name[0] == 0xE5) {
            free_entry = i;
            break;
        }
    }
    if (free_entry == -1) return -3; // No free dir entry

    // 3. Find a free cluster in the FAT
    uint32 fat_offset = bpb->RsvdSecCnt * bpb->BytsPerSec;
    uint32* fat = (uint32*)((char*)disk_img + fat_offset);
    uint32 total_clusters = (bpb->TotSec32 - first_data_sec) / sec_per_clus;
    uint32 first_free_clus = 2;
    for (; first_free_clus < total_clusters + 2; first_free_clus++) {
        if ((fat[first_free_clus] & 0x0FFFFFFF) == 0) break;
    }
    if (first_free_clus >= total_clusters + 2) return -4; // No free cluster

    // 4. Write data to the cluster
    uint32 data_sec = first_data_sec + ((first_free_clus - 2) * sec_per_clus);
    uint32 data_offset = data_sec * byts_per_sec;
    int max_bytes = byts_per_sec * sec_per_clus;
    int write_bytes = bufsize > max_bytes ? max_bytes : bufsize;
            memcpy((char*)disk_img + data_offset, buf, write_bytes);
            if (write_bytes < max_bytes) memset((char*)disk_img + data_offset + write_bytes, 0, max_bytes - write_bytes);

    // 5. Update FAT (mark cluster as end-of-chain)
    fat[first_free_clus] = 0x0FFFFFF8;
    // Mirror to other FATs if present
    for (uint32 f = 1; f < num_fats; f++) {
        uint32* fat2 = (uint32*)((char*)disk_img + fat_offset + f * fatsz * byts_per_sec);
        fat2[first_free_clus] = 0x0FFFFFF8;
    }

    // 6. Fill directory entry
    struct fat32_dir_entry* entry = &entries[free_entry];
    for (int j = 0; j < 11; j++) entry->Name[j] = filename[j];
    entry->Attr = 0x20; // Archive, normal file
    entry->NTRes = 0;
    entry->CrtTimeTenth = 0;
    entry->CrtTime = 0;
    entry->CrtDate = 0;
    entry->LstAccDate = 0;
    entry->WrtTime = 0;
    entry->WrtDate = 0;
    entry->FstClusHI = (first_free_clus >> 16) & 0xFFFF;
    entry->FstClusLO = first_free_clus & 0xFFFF;
    entry->FileSize = write_bytes;

    return write_bytes;
}

// Read BPB from a given partition start sector
int fat32_read_bpb_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb) {
    if (!bpb) return -1;
    uint8 sector[512];
    if (ata_read_sector(drive, partition_lba_start, sector) != 0) {
        return -1;
    }
            memcpy(bpb, (char*)sector, sizeof(struct fat32_bpb));
    return 0;
}

// List root directory entries from a real drive
int fat32_list_root_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb) {
    if (!bpb) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    
    uint8 sector[512];
    uint32 cluster = root_clus;
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        for (uint32 sec = 0; sec < sec_per_clus; sec++) {
            if (ata_read_sector(drive, partition_lba_start + cluster_first_sec + sec, sector) != 0) {
                return -2;
            }
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            for (int i = 0; i < (byts_per_sec / sizeof(struct fat32_dir_entry)); i++) {
                if (entries[i].Name[0] == 0x00) break;
                if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                if (entries[i].Name[0] == 0xE5) continue;
                char name[12];
                for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
                name[11] = '\0';
                if (entries[i].Attr & 0x10) {
                    printf("%c%s <DIR>\n", 255, 255, 255, name);
                } else {
                    printf("%c%s\n", 255, 255, 255, name);
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, partition_lba_start, bpb, cluster);
    }
    return 0;
}

int fat32_read_file_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, const char* filename, char* buf, int bufsize) {
    if (!bpb || !filename || !buf) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    
    uint8 sector[512];
    uint32 cluster = root_clus;
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
    for (uint32 sec = 0; sec < sec_per_clus; sec++) {
            if (ata_read_sector(drive, partition_lba_start + cluster_first_sec + sec, sector) != 0) {
                return -2;
            }
        struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            for (int i = 0; i < (byts_per_sec / sizeof(struct fat32_dir_entry)); i++) {
                if (entries[i].Name[0] == 0x00) break;
                if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                if (entries[i].Name[0] == 0xE5) continue;
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].Name[j] != filename[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                uint32 file_size = entries[i].FileSize;
                    if (file_size > (uint32)bufsize) file_size = bufsize;
                uint32 first_clus = ((uint32)entries[i].FstClusHI << 16) | entries[i].FstClusLO;
                    if (first_clus < 2) return -3;
                
                uint32 bytes_read = 0;
                    uint32 current_clus = first_clus;
                    while (current_clus < 0x0FFFFFF8 && bytes_read < file_size) {
                        uint32 data_sec = first_data_sec + ((current_clus - 2) * sec_per_clus);
                    for (uint32 s = 0; s < sec_per_clus && bytes_read < file_size; s++) {
                            if (ata_read_sector(drive, partition_lba_start + data_sec + s, sector) != 0) {
                                return -4;
                            }
                        uint32 to_copy = byts_per_sec;
                        if (bytes_read + to_copy > file_size) to_copy = file_size - bytes_read;
                        memcpy(buf + bytes_read, (char*)sector, to_copy);
                        bytes_read += to_copy;
                        }
                        current_clus = fat32_next_cluster_sector(drive, partition_lba_start, bpb, current_clus);
                    }
                    return bytes_read;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, partition_lba_start, bpb, cluster);
    }
    return -1; // Not found
}


int fat32_write_file_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, const char* filename, const char* buf, int bufsize) {
    if (!bpb || !filename || !buf || bufsize <= 0) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 root_clus = bpb->RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);

    uint8 sector[512];
    uint32 free_entry_clus = 0;
    int free_entry_idx = -1;
    uint32 free_entry_sec_in_clus = 0;
    int free_entry_sec_in_disk = 0;

    // First, check if file exists and find a free directory entry
    uint32 cluster = root_clus;
    while(cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        for (uint32 sec = 0; sec < sec_per_clus; sec++) {
            if (ata_read_sector(drive, partition_lba_start + cluster_first_sec + sec, sector) != 0) return -10;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            for (int i = 0; i < (byts_per_sec / sizeof(struct fat32_dir_entry)); i++) {
                if (entries[i].Name[0] == 0x00 || entries[i].Name[0] == 0xE5) {
                    if (free_entry_idx == -1) {
                        free_entry_clus = cluster;
                        free_entry_idx = i;
                        free_entry_sec_in_clus = sec;
                        free_entry_sec_in_disk = partition_lba_start + cluster_first_sec + sec;
                    }
                    continue;
                }
                int match = 1;
                for(int j=0; j<11; j++) if(entries[i].Name[j] != filename[j]) { match=0; break; }
                if(match) return -2; // file exists
            }
        }
        cluster = fat32_next_cluster_sector(drive, partition_lba_start, bpb, cluster);
    }

    if (free_entry_idx == -1) return -3; // no free dir entry

    // Find free cluster in FAT
    uint32 fat_sec_num = rsvd_sec_cnt;
    uint32 total_clusters = (bpb->TotSec32 - first_data_sec) / sec_per_clus;
    uint32 free_clus = 0;

    for(uint32 i = 2; i < total_clusters + 2; i++) {
        uint32 current_fat_sec = rsvd_sec_cnt + (i * 4 / byts_per_sec);
        if (ata_read_sector(drive, partition_lba_start + current_fat_sec, sector) != 0) return -5;
        uint32* fat = (uint32*)sector;
        uint32 fat_idx = i % (byts_per_sec/4);
        if((fat[fat_idx] & 0x0FFFFFFF) == 0) {
            free_clus = i;
            break;
        }
    }

    if (free_clus == 0) return -4; // no free cluster

    // Write data to cluster
    uint32 data_sec = first_data_sec + ((free_clus - 2) * sec_per_clus);
    int max_bytes = byts_per_sec * sec_per_clus;
    int write_bytes = bufsize > max_bytes ? max_bytes : bufsize;

    // We can only write 512 bytes at a time for now.
    if(write_bytes > 512) write_bytes = 512;
            memcpy((char*)sector, buf, write_bytes);
            if(write_bytes < 512) memset(sector + write_bytes, 0, 512 - write_bytes);
    if(ata_write_sector(drive, partition_lba_start + data_sec, sector) != 0) return -6;

    // Update FAT
    uint32 fat_sector_for_free_clus = rsvd_sec_cnt + (free_clus * 4 / byts_per_sec);
    if (ata_read_sector(drive, partition_lba_start + fat_sector_for_free_clus, sector) != 0) return -7;
    uint32* fat = (uint32*)sector;
    uint32 fat_idx = free_clus % (byts_per_sec / 4);
    fat[fat_idx] = 0x0FFFFFF8; // EOC
    if (ata_write_sector(drive, partition_lba_start + fat_sector_for_free_clus, sector) != 0) return -8;

    // Update directory entry
    if(ata_read_sector(drive, free_entry_sec_in_disk, sector) != 0) return -9;
    struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
    struct fat32_dir_entry* entry = &entries[free_entry_idx];
    for (int j=0; j<11; j++) entry->Name[j] = filename[j];
    entry->Attr = 0x20;
    entry->NTRes = 0;
    entry->CrtTimeTenth = 0;
    entry->CrtTime = 0;
    entry->CrtDate = 0;
    entry->LstAccDate = 0;
    entry->FstClusHI = (free_clus >> 16) & 0xFFFF;
    entry->FstClusLO = free_clus & 0xFFFF;
    entry->FileSize = write_bytes;

    if (ata_write_sector(drive, free_entry_sec_in_disk, sector) != 0) return -9;

    return 0;
}

// Helper: get next cluster in chain from sector-based device
uint32 fat32_next_cluster_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, uint32 cluster) {
    uint8 sector[512];
    uint32 fat_offset = bpb->RsvdSecCnt;
    uint32 fat_sec_num = fat_offset + (cluster * 4 / bpb->BytsPerSec);
    if (ata_read_sector(drive, partition_lba_start + fat_sec_num, sector) != 0) {
        return 0x0FFFFFF8; // error condition
    }
    uint32* fat = (uint32*)sector;
    uint32 fat_index = cluster % (bpb->BytsPerSec / 4);
    return fat[fat_index] & 0x0FFFFFFF;
}

// Find a directory entry by 8.3 name in a directory on a real drive
int fat32_find_entry_sector(uint8 drive, struct fat32_bpb* bpb, uint32 dir_cluster, const char* fat_83_name, struct fat32_dir_entry* out_entry) {
    if (!bpb || !fat_83_name || dir_cluster < 2) return -1;
    uint32 byts_per_sec = bpb->BytsPerSec;
    uint32 sec_per_clus = bpb->SecPerClus;
    uint32 rsvd_sec_cnt = bpb->RsvdSecCnt;
    uint32 num_fats = bpb->NumFATs;
    uint32 fatsz = bpb->FATSz32;
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    uint8 sector[512];
    uint32 cluster = dir_cluster;
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        for (uint32 sec = 0; sec < sec_per_clus; ++sec) {
            if (ata_read_sector(drive, part_lba + cluster_first_sec + sec, sector) != 0) return -2;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)sector;
            for (int i = 0; i < (int)(byts_per_sec / sizeof(struct fat32_dir_entry)); ++i) {
                if (entries[i].Name[0] == 0x00) break;
                if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                if (entries[i].Name[0] == 0xE5) continue;
                char name[12];
                for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
                name[11] = '\0';
                if (strcmp(name, fat_83_name) == 0) {
                    if (out_entry) *out_entry = entries[i];
                    uint32 entry_cluster = ((uint32)entries[i].FstClusHI << 16) | entries[i].FstClusLO;
                    return (int)entry_cluster;
                }
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, bpb, cluster);
    }
    return -3;
}

// Find and return the LBA start of the first valid FAT32 partition
uint32 fat32_get_partition_lba_start(uint8 drive) {
    uint8 mbr[512];
    if (ata_read_sector(drive, 0, mbr) != 0) {
        return 0; // Cannot read MBR
    }
    // Check for MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        // No valid MBR, assume it's a superfloppy disk starting at LBA 0
        return 0;
    }
    // Scan partition table
    for (int i = 0; i < 4; i++) {
        uint8* entry = mbr + 0x1BE + i * 16;
        uint8 type = entry[4];
        // Check for FAT32 types (0x0B, 0x0C are common)
        if (type == 0x0B || type == 0x0C) {
            uint32 start_lba = entry[8] | (entry[9]<<8) | (entry[10]<<16) | (entry[11]<<24);
            return start_lba;
        }
    }
    
    // Fallback for user-created partition type, if no standard one is found
    for (int i = 0; i < 4; i++) {
        uint8* entry = mbr + 0x1BE + i * 16;
        uint8 type = entry[4];
        if (type != 0) { // any non-empty partition
            uint32 start_lba = entry[8] | (entry[9]<<8) | (entry[10]<<16) | (entry[11]<<24);
            return start_lba;
        }
    }

    return 0; // No FAT32 partition found
}

int fat32_format_partition(uint8 drive, uint8 partition_num) {
    if (partition_num < 1 || partition_num > 4) return -1;

    uint8 mbr[512];
    if (ata_read_sector(drive, 0, mbr) != 0) return -2;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -3;

    uint8* entry_ptr = mbr + 0x1BE + (partition_num - 1) * 16;
    uint8 type = entry_ptr[4];
    uint32 start_lba = entry_ptr[8] | (entry_ptr[9]<<8) | (entry_ptr[10]<<16) | (entry_ptr[11]<<24);
    uint32 total_sectors = entry_ptr[12] | (entry_ptr[13]<<8) | (entry_ptr[14]<<16) | (entry_ptr[15]<<24);

    printf("%c[Debug] Formatting part %d: type=%d, start=%d, size=%d\n", 0, 255, 255, partition_num, type, start_lba, total_sectors);

    if (type == 0 || total_sectors == 0) return -4;

    struct fat32_bpb bpb;
            memset(&bpb, 0, sizeof(bpb));

    bpb.jmpBoot[0] = 0xEB; bpb.jmpBoot[1] = 0x58; bpb.jmpBoot[2] = 0x90;
            memcpy((char*)bpb.OEMName, "EYN-OS  ", 8);
    bpb.BytsPerSec = 512;
    if (total_sectors < 65536) bpb.SecPerClus = 1;
    else if (total_sectors < 524288) bpb.SecPerClus = 8;
    else bpb.SecPerClus = 16;
    bpb.RsvdSecCnt = 32;
    bpb.NumFATs = 2;
    bpb.Media = 0xF8;
    bpb.HiddSec = start_lba;
    bpb.TotSec32 = total_sectors;

    uint32 data_sec = total_sectors - bpb.RsvdSecCnt;
    uint32 cluster_count = data_sec / bpb.SecPerClus;
    uint32 fat_sz32 = (cluster_count * 4 + bpb.BytsPerSec - 1) / bpb.BytsPerSec;
    bpb.FATSz32 = fat_sz32;

    bpb.RootClus = 2;
    bpb.FSInfo = 1;
    bpb.BkBootSec = 6;
    bpb.DrvNum = 0x80;
    bpb.BootSig = 0x29;
    bpb.VolID = 1337;
            memcpy((char*)bpb.VolLab, "NO NAME    ", 11);
            memcpy((char*)bpb.FilSysType, "FAT32   ", 8);
    
    uint8 sector[512];
            memcpy((char*)sector, (char*)&bpb, sizeof(bpb));
            memset(sector + sizeof(bpb), 0, 512 - sizeof(bpb));
    sector[510] = 0x55; sector[511] = 0xAA;

    if (ata_write_sector(drive, start_lba, sector) != 0) return -5;
    if (ata_write_sector(drive, start_lba + bpb.BkBootSec, sector) != 0) return -6;

    memset(sector, 0, 512);
    sector[0] = 0x52; sector[1] = 0x52; sector[2] = 0x61; sector[3] = 0x41;
    sector[484] = 0x72; sector[485] = 0x72; sector[486] = 0x41; sector[487] = 0x61;
    uint32 free_clusters = cluster_count - 1;
    *((uint32*)(sector + 488)) = free_clusters;
    *((uint32*)(sector + 492)) = 3; // Next free
    sector[510] = 0x55; sector[511] = 0xAA;
    if (ata_write_sector(drive, start_lba + bpb.FSInfo, sector) != 0) return -7;

    memset(sector, 0, 512);
    for (uint32 f = 0; f < bpb.NumFATs; f++) {
        uint32 fat_start = start_lba + bpb.RsvdSecCnt + (f * bpb.FATSz32);
        for (uint32 i = 0; i < bpb.FATSz32; i++) {
            if (ata_write_sector(drive, fat_start + i, sector) != 0) return -8;
        }
    }
    
    memset(sector, 0, 512);
    uint32* fat = (uint32*)sector;
    fat[0] = 0x0FFFFFF8;
    fat[1] = 0x0FFFFFFF;
    fat[2] = 0x0FFFFFFF;
    for (uint32 f = 0; f < bpb.NumFATs; f++) {
        uint32 fat_start = start_lba + bpb.RsvdSecCnt + (f * bpb.FATSz32);
        if (ata_write_sector(drive, fat_start, sector) != 0) return -9;
    }
    
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint32 root_dir_start_sec = start_lba + first_data_sec;
    memset(sector, 0, 512);
    for (uint8 i = 0; i < bpb.SecPerClus; i++) {
        if (ata_write_sector(drive, root_dir_start_sec + i, sector) != 0) return -12;
    }

    return 0;
}
