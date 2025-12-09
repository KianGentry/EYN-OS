#ifndef PARTITION_H
#define PARTITION_H

#include <types.h>

// PARTITION TABLE DEFINITIONS

/* MBR partition entry offsets */
#define MBR_PARTITION_TABLE_OFFSET  0x1BE
#define MBR_PARTITION_ENTRY_SIZE    16
#define MBR_MAX_PARTITIONS          4
#define MBR_SIGNATURE               0xAA55
#define MBR_SIGNATURE_OFFSET        510

/* Partition types */
#define PART_TYPE_EMPTY             0x00
#define PART_TYPE_FAT12             0x01
#define PART_TYPE_FAT16_SMALL       0x04
#define PART_TYPE_EXTENDED          0x05
#define PART_TYPE_FAT16             0x06
#define PART_TYPE_NTFS              0x07
#define PART_TYPE_FAT32             0x0B
#define PART_TYPE_FAT32_LBA         0x0C
#define PART_TYPE_FAT16_LBA         0x0E
#define PART_TYPE_LINUX             0x83
#define PART_TYPE_LINUX_SWAP        0x82
#define PART_TYPE_EYNFS             0xEF  /* Custom EYNFS type */
#define PART_TYPE_EYNOS_SWAP        0xE5  /* Custom EYN-OS swap type */

/* Partition entry structure (MBR format) */
typedef struct __attribute__((packed)) {
    uint8  boot_flag;        /* 0x80 = bootable, 0x00 = not bootable */
    uint8  start_head;       /* CHS start head */
    uint8  start_sector;     /* CHS start sector (bits 0-5), cylinder high (bits 6-7) */
    uint8  start_cylinder;   /* CHS start cylinder low 8 bits */
    uint8  type;             /* Partition type */
    uint8  end_head;         /* CHS end head */
    uint8  end_sector;       /* CHS end sector */
    uint8  end_cylinder;     /* CHS end cylinder */
    uint32 lba_start;        /* LBA start sector */
    uint32 sector_count;     /* Number of sectors */
} partition_entry_t;

/* Partition info (runtime representation) */
typedef struct {
    uint8  drive;            /* Physical drive number */
    uint8  partition_num;    /* Partition index (0-3) */
    uint8  type;             /* Partition type */
    uint8  bootable;         /* Is bootable */
    uint32 lba_start;        /* Starting LBA */
    uint32 sector_count;     /* Size in sectors */
    uint32 size_mb;          /* Size in MB (calculated) */
    char   label[16];        /* Partition label */
    uint8  mounted;          /* Is currently mounted */
    uint8  fs_type;          /* Detected filesystem type */
} partition_info_t;

/* Disk info structure */
typedef struct {
    uint8  drive;            /* Physical drive number */
    uint32 total_sectors;    /* Total sectors on disk */
    uint32 total_mb;         /* Total size in MB */
    uint8  partition_count;  /* Number of valid partitions */
    partition_info_t partitions[MBR_MAX_PARTITIONS];
    uint8  has_valid_mbr;    /* MBR signature present */
} disk_info_t;

// VIRTUAL DRIVE MAPPING
// Maps logical "drives" (A:, B:, C:, etc.) to partitions. Drive 0 is the raw
// physical drive; drives 1+ are the exposed partitions.

#define MAX_VIRTUAL_DRIVES  8

typedef struct {
    uint8  in_use;           /* Is this drive slot active */
    uint8  physical_drive;   /* Physical ATA drive */
    uint8  partition_num;    /* Partition index (0xFF = whole disk) */
    uint32 lba_offset;       /* LBA offset for this "drive" */
    uint32 sector_count;     /* Size limit */
    uint8  fs_type;          /* Filesystem type */
    char   mount_point[8];   /* e.g., "A:", "B:", "SWAP" */
} virtual_drive_t;

// SWAP PARTITION

typedef struct {
    uint8  active;           /* Swap partition is active */
    uint8  drive;            /* Physical drive */
    uint32 lba_start;        /* Starting LBA */
    uint32 sector_count;     /* Size in sectors */
    uint32 total_pages;      /* Total 4KB pages available */
    uint32 used_pages;       /* Currently used pages */
} swap_partition_t;

// FUNCTION PROTOTYPES

/* Partition table operations */
int partition_read_table(uint8 drive, disk_info_t *info);
int partition_write_table(uint8 drive, const disk_info_t *info);
int partition_create(uint8 drive, uint32 start_lba, uint32 size_sectors, uint8 type);
int partition_delete(uint8 drive, uint8 partition_num);
int partition_set_bootable(uint8 drive, uint8 partition_num);

/* Virtual drive operations */
int vdrive_init(void);
int vdrive_mount(uint8 physical_drive, uint8 partition_num, const char *mount_point);
int vdrive_unmount(const char *mount_point);
virtual_drive_t* vdrive_get(uint8 drive_num);
virtual_drive_t* vdrive_find_by_mount(const char *mount_point);
int vdrive_translate_lba(uint8 vdrive, uint32 lba, uint8 *phys_drive, uint32 *phys_lba);

/* Swap partition operations */
int swap_partition_init(uint8 drive, uint8 partition_num);
int swap_partition_read_page(uint32 page_num, void *buffer);
int swap_partition_write_page(uint32 page_num, const void *buffer);
swap_partition_t* swap_partition_get_info(void);

/* Utility functions */
const char* partition_type_name(uint8 type);
int partition_detect_fs(uint8 drive, uint32 lba_start);
void partition_print_info(const partition_info_t *part);
void disk_print_info(const disk_info_t *disk);

#endif /* PARTITION_H */
