#ifndef FAT32_H
#define FAT32_H
#include <types.h>
#include "multiboot.h"

// BIOS Parameter Block (BPB) for FAT32
struct fat32_bpb {
    uint8  jmpBoot[3];
    uint8  OEMName[8];
    uint16 BytsPerSec;
    uint8  SecPerClus;
    uint16 RsvdSecCnt;
    uint8  NumFATs;
    uint16 RootEntCnt;
    uint16 TotSec16;
    uint8  Media;
    uint16 FATSz16;
    uint16 SecPerTrk;
    uint16 NumHeads;
    uint32 HiddSec;
    uint32 TotSec32;
    uint32 FATSz32;
    uint16 ExtFlags;
    uint16 FSVer;
    uint32 RootClus;
    uint16 FSInfo;
    uint16 BkBootSec;
    uint8  Reserved[12];
    uint8  DrvNum;
    uint8  Reserved1;
    uint8  BootSig;
    uint32 VolID;
    uint8  VolLab[11];
    uint8  FilSysType[8];
} __attribute__((packed));

// Directory entry
struct fat32_dir_entry {
    uint8  Name[11];
    uint8  Attr;
    uint8  NTRes;
    uint8  CrtTimeTenth;
    uint16 CrtTime;
    uint16 CrtDate;
    uint16 LstAccDate;
    uint16 FstClusHI;
    uint16 WrtTime;
    uint16 WrtDate;
    uint16 FstClusLO;
    uint32 FileSize;
} __attribute__((packed));

// Public functions
int fat32_read_bpb(void* disk_img, struct fat32_bpb* bpb);
int fat32_list_root(void* disk_img, struct fat32_bpb* bpb);
int fat32_read_file(void* disk_img, struct fat32_bpb* bpb, const char* filename, char* buf, int bufsize);
uint32 fat32_next_cluster(void* disk_img, struct fat32_bpb* bpb, uint32 cluster);
int fat32_list_dir(void* disk_img, struct fat32_bpb* bpb, uint32 start_cluster);
int fat32_find_entry(void* disk_img, struct fat32_bpb* bpb, uint32 dir_cluster, const char* name, struct fat32_dir_entry* out_entry);
int fat32_write_file(void* disk_img, struct fat32_bpb* bpb, const char* filename, const char* buf, int bufsize);

// Sector-based functions for real hardware
int fat32_format_partition(uint8 drive, uint8 partition_num);
uint32 fat32_get_partition_lba_start(uint8 drive);
int fat32_read_bpb_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb);
int fat32_list_root_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb);
int fat32_read_file_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, const char* filename, char* buf, int bufsize);
int fat32_write_file_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, const char* filename, const char* buf, int bufsize);
uint32 fat32_next_cluster_sector(uint8 drive, uint32 partition_lba_start, struct fat32_bpb* bpb, uint32 cluster);

void to_fat32_83(const char* input, char* output);

#endif 