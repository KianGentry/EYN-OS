/*
 * partition.c — Disk Partition Management Implementation
 *
 * Provides MBR partition table management, virtual drive mapping,
 * and swap partition support for EYN-OS.
 */

#include <partition.h>
#include <types.h>
#include <string.h>
#include <vga.h>
#include <util.h>
#include <ata.h>

// GLOBAL STATE

/* Virtual drive table */
static virtual_drive_t g_vdrives[MAX_VIRTUAL_DRIVES];
static int g_vdrives_initialized = 0;

/* Swap partition state */
static swap_partition_t g_swap_partition;

// PARTITION TABLE OPERATIONS

int partition_read_table(uint8 drive, disk_info_t *info) {
    if (!info) return -1;
    
    memset(info, 0, sizeof(disk_info_t));
    info->drive = drive;
    
    /* Read MBR sector */
    uint8 mbr[512];
    if (ata_read_sector(drive, 0, mbr) != 0) {
        return -1;
    }
    
    /* Check MBR signature */
    uint16 signature = (mbr[MBR_SIGNATURE_OFFSET + 1] << 8) | mbr[MBR_SIGNATURE_OFFSET];
    info->has_valid_mbr = (signature == MBR_SIGNATURE);
    
    /* Parse partition entries */
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        uint8 *entry = mbr + MBR_PARTITION_TABLE_OFFSET + (i * MBR_PARTITION_ENTRY_SIZE);
        
        partition_info_t *part = &info->partitions[i];
        part->drive = drive;
        part->partition_num = i;
        part->bootable = (entry[0] == 0x80);
        part->type = entry[4];
        part->lba_start = entry[8] | (entry[9] << 8) | (entry[10] << 16) | (entry[11] << 24);
        part->sector_count = entry[12] | (entry[13] << 8) | (entry[14] << 16) | (entry[15] << 24);
        part->size_mb = part->sector_count / 2048;  /* 512 bytes/sector, 2048 sectors/MB */
        part->mounted = 0;
        
        /* Generate label based on type */
        if (part->type != PART_TYPE_EMPTY && part->sector_count > 0) {
            info->partition_count++;
            snprintf(part->label, sizeof(part->label), "Part%d", i + 1);
            
            /* Detect filesystem type */
            part->fs_type = partition_detect_fs(drive, part->lba_start);
        }
    }
    
    return 0;
}

int partition_write_table(uint8 drive, const disk_info_t *info) {
    if (!info) return -1;
    
    /* Read existing MBR to preserve boot code */
    uint8 mbr[512];
    if (ata_read_sector(drive, 0, mbr) != 0) {
        /* If read fails, zero the MBR */
        memset(mbr, 0, 512);
    }
    
    /* Write partition entries */
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        const partition_info_t *part = &info->partitions[i];
        uint8 *entry = mbr + MBR_PARTITION_TABLE_OFFSET + (i * MBR_PARTITION_ENTRY_SIZE);
        
        /* Clear entry first */
        memset(entry, 0, MBR_PARTITION_ENTRY_SIZE);
        
        if (part->type != PART_TYPE_EMPTY && part->sector_count > 0) {
            entry[0] = part->bootable ? 0x80 : 0x00;
            entry[4] = part->type;
            entry[8] = part->lba_start & 0xFF;
            entry[9] = (part->lba_start >> 8) & 0xFF;
            entry[10] = (part->lba_start >> 16) & 0xFF;
            entry[11] = (part->lba_start >> 24) & 0xFF;
            entry[12] = part->sector_count & 0xFF;
            entry[13] = (part->sector_count >> 8) & 0xFF;
            entry[14] = (part->sector_count >> 16) & 0xFF;
            entry[15] = (part->sector_count >> 24) & 0xFF;
            
            /* Set CHS values (dummy values for LBA disks) */
            entry[1] = 0xFE;  /* Start head */
            entry[2] = 0xFF;  /* Start sector/cylinder */
            entry[3] = 0xFF;  /* Start cylinder */
            entry[5] = 0xFE;  /* End head */
            entry[6] = 0xFF;  /* End sector/cylinder */
            entry[7] = 0xFF;  /* End cylinder */
        }
    }
    
    /* Set MBR signature */
    mbr[MBR_SIGNATURE_OFFSET] = 0x55;
    mbr[MBR_SIGNATURE_OFFSET + 1] = 0xAA;
    
    /* Write MBR back */
    return ata_write_sector(drive, 0, mbr);
}

int partition_create(uint8 drive, uint32 start_lba, uint32 size_sectors, uint8 type) {
    disk_info_t disk;
    if (partition_read_table(drive, &disk) != 0) {
        return -1;
    }
    
    /* Find free partition slot */
    int free_slot = -1;
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        if (disk.partitions[i].type == PART_TYPE_EMPTY) {
            free_slot = i;
            break;
        }
    }
    
    if (free_slot == -1) {
        printf("%cNo free partition slots available\n", 255, 0, 0);
        return -1;
    }
    
    /* Check for overlap with existing partitions */
    uint32 end_lba = start_lba + size_sectors;
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        if (disk.partitions[i].type != PART_TYPE_EMPTY) {
            uint32 p_start = disk.partitions[i].lba_start;
            uint32 p_end = p_start + disk.partitions[i].sector_count;
            
            if ((start_lba >= p_start && start_lba < p_end) ||
                (end_lba > p_start && end_lba <= p_end) ||
                (start_lba <= p_start && end_lba >= p_end)) {
                printf("%cPartition overlaps with existing partition %d\n", 255, 0, 0, i + 1);
                return -1;
            }
        }
    }
    
    /* Create the partition */
    disk.partitions[free_slot].type = type;
    disk.partitions[free_slot].lba_start = start_lba;
    disk.partitions[free_slot].sector_count = size_sectors;
    disk.partitions[free_slot].bootable = 0;
    disk.partitions[free_slot].size_mb = size_sectors / 2048;
    disk.partition_count++;
    
    /* Write updated partition table */
    if (partition_write_table(drive, &disk) != 0) {
        return -1;
    }
    
    printf("%cCreated partition %d: type=0x%02X, start=%u, size=%u sectors (%u MB)\n",
           0, 255, 0, free_slot + 1, type, start_lba, size_sectors, size_sectors / 2048);
    
    return free_slot;
}

int partition_delete(uint8 drive, uint8 partition_num) {
    if (partition_num >= MBR_MAX_PARTITIONS) return -1;
    
    disk_info_t disk;
    if (partition_read_table(drive, &disk) != 0) {
        return -1;
    }
    
    if (disk.partitions[partition_num].type == PART_TYPE_EMPTY) {
        printf("%cPartition %d is already empty\n", 255, 165, 0, partition_num + 1);
        return -1;
    }
    
    /* Clear the partition */
    memset(&disk.partitions[partition_num], 0, sizeof(partition_info_t));
    disk.partitions[partition_num].drive = drive;
    disk.partitions[partition_num].partition_num = partition_num;
    disk.partition_count--;
    
    return partition_write_table(drive, &disk);
}

int partition_set_bootable(uint8 drive, uint8 partition_num) {
    if (partition_num >= MBR_MAX_PARTITIONS) return -1;
    
    disk_info_t disk;
    if (partition_read_table(drive, &disk) != 0) {
        return -1;
    }
    
    /* Clear bootable flag on all partitions, set on target */
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        disk.partitions[i].bootable = (i == partition_num);
    }
    
    return partition_write_table(drive, &disk);
}

// VIRTUAL DRIVE OPERATIONS

int vdrive_init(void) {
    memset(g_vdrives, 0, sizeof(g_vdrives));
    g_vdrives_initialized = 1;
    return 0;
}

int vdrive_mount(uint8 physical_drive, uint8 partition_num, const char *mount_point) {
    if (!g_vdrives_initialized) vdrive_init();
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_VIRTUAL_DRIVES; i++) {
        if (!g_vdrives[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        printf("%cNo free virtual drive slots\n", 255, 0, 0);
        return -1;
    }
    
    /* Get partition info */
    disk_info_t disk;
    if (partition_read_table(physical_drive, &disk) != 0) {
        return -1;
    }
    
    if (partition_num >= MBR_MAX_PARTITIONS ||
        disk.partitions[partition_num].type == PART_TYPE_EMPTY) {
        printf("%cInvalid partition number\n", 255, 0, 0);
        return -1;
    }
    
    /* Set up virtual drive */
    g_vdrives[slot].in_use = 1;
    g_vdrives[slot].physical_drive = physical_drive;
    g_vdrives[slot].partition_num = partition_num;
    g_vdrives[slot].lba_offset = disk.partitions[partition_num].lba_start;
    g_vdrives[slot].sector_count = disk.partitions[partition_num].sector_count;
    g_vdrives[slot].fs_type = disk.partitions[partition_num].fs_type;
    strncpy(g_vdrives[slot].mount_point, mount_point, sizeof(g_vdrives[slot].mount_point) - 1);
    
    return slot;
}

int vdrive_unmount(const char *mount_point) {
    for (int i = 0; i < MAX_VIRTUAL_DRIVES; i++) {
        if (g_vdrives[i].in_use && strEql(g_vdrives[i].mount_point, mount_point)) {
            g_vdrives[i].in_use = 0;
            return 0;
        }
    }
    return -1;
}

virtual_drive_t* vdrive_get(uint8 drive_num) {
    if (drive_num >= MAX_VIRTUAL_DRIVES || !g_vdrives[drive_num].in_use) {
        return 0;
    }
    return &g_vdrives[drive_num];
}

virtual_drive_t* vdrive_find_by_mount(const char *mount_point) {
    for (int i = 0; i < MAX_VIRTUAL_DRIVES; i++) {
        if (g_vdrives[i].in_use && strEql(g_vdrives[i].mount_point, mount_point)) {
            return &g_vdrives[i];
        }
    }
    return 0;
}

int vdrive_translate_lba(uint8 vdrive, uint32 lba, uint8 *phys_drive, uint32 *phys_lba) {
    virtual_drive_t *vd = vdrive_get(vdrive);
    if (!vd) return -1;
    
    if (lba >= vd->sector_count) return -1;  /* Out of bounds */
    
    *phys_drive = vd->physical_drive;
    *phys_lba = vd->lba_offset + lba;
    return 0;
}

// SWAP PARTITION OPERATIONS

int swap_partition_init(uint8 drive, uint8 partition_num) {
    memset(&g_swap_partition, 0, sizeof(g_swap_partition));
    
    disk_info_t disk;
    if (partition_read_table(drive, &disk) != 0) {
        return -1;
    }
    
    if (partition_num >= MBR_MAX_PARTITIONS) {
        return -1;
    }
    
    partition_info_t *part = &disk.partitions[partition_num];
    if (part->type != PART_TYPE_EYNOS_SWAP && part->type != PART_TYPE_LINUX_SWAP) {
        printf("%cPartition %d is not a swap partition (type=0x%02X)\n",
               255, 165, 0, partition_num + 1, part->type);
        return -1;
    }
    
    g_swap_partition.active = 1;
    g_swap_partition.drive = drive;
    g_swap_partition.lba_start = part->lba_start;
    g_swap_partition.sector_count = part->sector_count;
    /* Each page is 4KB = 8 sectors */
    g_swap_partition.total_pages = part->sector_count / 8;
    g_swap_partition.used_pages = 0;
    
    printf("%cSwap partition initialized: %u pages (%u MB)\n",
           0, 255, 0, g_swap_partition.total_pages, part->size_mb);
    
    return 0;
}

int swap_partition_read_page(uint32 page_num, void *buffer) {
    if (!g_swap_partition.active) return -1;
    if (page_num >= g_swap_partition.total_pages) return -1;
    if (!buffer) return -1;
    
    uint32 lba = g_swap_partition.lba_start + (page_num * 8);
    uint8 *buf = (uint8 *)buffer;
    
    /* Read 8 sectors (4KB page) */
    for (int i = 0; i < 8; i++) {
        if (ata_read_sector(g_swap_partition.drive, lba + i, buf + (i * 512)) != 0) {
            return -1;
        }
    }
    
    return 0;
}

int swap_partition_write_page(uint32 page_num, const void *buffer) {
    if (!g_swap_partition.active) return -1;
    if (page_num >= g_swap_partition.total_pages) return -1;
    if (!buffer) return -1;
    
    uint32 lba = g_swap_partition.lba_start + (page_num * 8);
    const uint8 *buf = (const uint8 *)buffer;
    
    /* Write 8 sectors (4KB page) */
    for (int i = 0; i < 8; i++) {
        if (ata_write_sector(g_swap_partition.drive, lba + i, buf + (i * 512)) != 0) {
            return -1;
        }
    }
    
    return 0;
}

swap_partition_t* swap_partition_get_info(void) {
    return g_swap_partition.active ? &g_swap_partition : 0;
}

// UTILITY FUNCTIONS

const char* partition_type_name(uint8 type) {
    switch (type) {
        case PART_TYPE_EMPTY:       return "Empty";
        case PART_TYPE_FAT12:       return "FAT12";
        case PART_TYPE_FAT16_SMALL: return "FAT16 (<32M)";
        case PART_TYPE_EXTENDED:    return "Extended";
        case PART_TYPE_FAT16:       return "FAT16";
        case PART_TYPE_NTFS:        return "NTFS/HPFS";
        case PART_TYPE_FAT32:       return "FAT32";
        case PART_TYPE_FAT32_LBA:   return "FAT32 LBA";
        case PART_TYPE_FAT16_LBA:   return "FAT16 LBA";
        case PART_TYPE_LINUX:       return "Linux";
        case PART_TYPE_LINUX_SWAP:  return "Linux Swap";
        case PART_TYPE_EYNFS:       return "EYNFS";
        case PART_TYPE_EYNOS_SWAP:  return "EYN-OS Swap";
        default:                    return "Unknown";
    }
}

int partition_detect_fs(uint8 drive, uint32 lba_start) {
    uint8 buf[512];
    if (ata_read_sector(drive, lba_start, buf) != 0) {
        return 0;
    }
    
    /* Check for EYNFS magic at expected offset */
    uint32 *magic = (uint32 *)(buf);
    if (*magic == 0x45594E46) {  /* 'EYNF' */
        return PART_TYPE_EYNFS;
    }
    
    /* Check for FAT signatures */
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        if (buf[82] == 'F' && buf[83] == 'A' && buf[84] == 'T') {
            return PART_TYPE_FAT32;
        }
        if (buf[54] == 'F' && buf[55] == 'A' && buf[56] == 'T') {
            return PART_TYPE_FAT16;
        }
    }
    
    return 0;  /* Unknown */
}

void partition_print_info(const partition_info_t *part) {
    if (!part || part->type == PART_TYPE_EMPTY) return;
    
    printf("  Partition %d: %s%s\n",
           part->partition_num + 1,
           partition_type_name(part->type),
           part->bootable ? " (bootable)" : "");
    printf("    Start: %u, Size: %u sectors (%u MB)\n",
           part->lba_start, part->sector_count, part->size_mb);
}

void disk_print_info(const disk_info_t *disk) {
    if (!disk) return;
    
    printf("%cDisk %d: %s\n", 255, 255, 0, disk->drive,
           disk->has_valid_mbr ? "Valid MBR" : "No valid MBR");
    
    if (disk->partition_count == 0) {
        printf("  No partitions found\n");
        return;
    }
    
    printf("  %d partition(s):\n", disk->partition_count);
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        partition_print_info(&disk->partitions[i]);
    }
}
