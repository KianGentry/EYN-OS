#include <format_command.h>
#include <shell_command_info.h>
#include <misc/types.h>
#include <stdint.h>
#include <system.h>
#include <string.h>
#include <util.h>
#include <eynfs.h>
#include <fat32.h>
#include <vga.h> // for printf
#include <context.h>
#include <misc/sched.h>

extern char* readStr();

static int format_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void format_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Helper function to format a partition as EYNFS
int eynfs_format_partition(uint8 drive, uint8 part_num) {
    if (!format_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;
    printf("%cStarting EYNFS format for partition %d...\n", 255, 255, 0, part_num);
    
    // Read MBR to get partition info
    uint8 mbr[512];
    if (ata_read_sector(drive, 0, mbr) != 0) {
        printf("%cFailed to read MBR\n", 255, 0, 0);
        return -1;
    }
    
    uint32 start_lba = 0;
    uint32 size = 0;
    
    // Check for MBR signature
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        // Valid MBR exists, try to read partition info
        uint8* entry = mbr + 0x1BE + (part_num - 1) * 16;
        start_lba = entry[8] | (entry[9]<<8) | (entry[10]<<16) | (entry[11]<<24);
        size = entry[12] | (entry[13]<<8) | (entry[14]<<16) | (entry[15]<<24);
        
        if (start_lba == 0 || size == 0) {
            start_lba = 0;
            size = 1024000; // Assume 500MB disk (1024000 sectors)
        }
    } else {
        // No valid MBR, treat as superfloppy
        start_lba = 0;
        size = 1024000; // Assume 500MB disk (1024000 sectors)
    }
    
    printf("%cUsing start_lba=%d, size=%d\n", 255, 255, 0, start_lba, size);
    
    // ERASE THE DISK: Zero out the first 2048 sectors to remove old FAT32 data
    printf("%cErasing disk...\n", 255, 255, 0);
    uint8 zero_sector[EYNFS_BLOCK_SIZE] = {0};
    for (uint32 i = 0; i < 2048; i++) {
        if ((i & 0x3F) == 0) format_ctx_account(SCHED_COST_FS);
        int write_result = ata_write_sector(drive, start_lba + i, zero_sector);
        if (write_result != 0) {
            printf("%cFailed to erase sector %d\n", 255, 0, 0, i);
            return -7;
        }
        // No delay needed for modern disk controllers
    }
    
    printf("%cWriting EYNFS structures...\n", 255, 255, 0);
    
    // EYNFS layout: superblock at start_lba + 2048
    uint32 eynfs_superblock_lba = start_lba + 2048;
    uint32 eynfs_bitmap_lba = start_lba + 2049;
    uint32 eynfs_nametable_lba = start_lba + 2050;
    uint32 eynfs_rootdir_lba = start_lba + 2051;
    
    // Write superblock
    eynfs_superblock_t sb = {0};
    sb.magic = EYNFS_MAGIC;
    sb.version = EYNFS_VERSION;
    sb.block_size = EYNFS_BLOCK_SIZE;
    sb.total_blocks = size;
    sb.root_dir_block = eynfs_rootdir_lba;
    sb.free_block_map = eynfs_bitmap_lba;
    sb.name_table_block = eynfs_nametable_lba;
    if (eynfs_write_superblock(drive, eynfs_superblock_lba, &sb) != 0) {
        printf("%cFailed to write superblock\n", 255, 0, 0);
        return -3;
    }
    
    // Write zeroed free block bitmap
    uint8 bitmap[EYNFS_BLOCK_SIZE] = {0};
    // Mark reserved blocks as used (superblock, bitmap, nametable, rootdir)
    for (int i = 0; i < 4; i++) bitmap[i/8] |= (1 << (i%8));
    if (ata_write_sector(drive, eynfs_bitmap_lba, bitmap) != 0) {
        printf("%cFailed to write bitmap\n", 255, 0, 0);
        return -4;
    }
    
    // Write empty name table
    uint8 name_table[EYNFS_BLOCK_SIZE] = {0};
    if (ata_write_sector(drive, eynfs_nametable_lba, name_table) != 0) {
        printf("%cFailed to write name table\n", 255, 0, 0);
        return -5;
    }
    
    // Write empty root directory block
    uint8 root_dir[EYNFS_BLOCK_SIZE] = {0};
    if (ata_write_sector(drive, eynfs_rootdir_lba, root_dir) != 0) {
        printf("%cFailed to write root directory\n", 255, 0, 0);
        return -6;
    }
    
    printf("%cEYNFS format completed successfully\n", 0, 255, 0);
    return 0;
}

// format_cmd_handler implementation
void format_cmd_handler(const shell_args_t* args) {
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        printf("%cUsage: format <partition_num (0-3)> [filesystem_type]\n", 255, 255, 255);
        printf("%cFilesystem types: fat32, eynfs\n", 255, 255, 255);
        printf("%cExample: format 0 fat32\n", 255, 255, 255);
        printf("%cExample: format 1 eynfs\n", 255, 255, 255);
        return;
    }

    uint8 part_num = (uint8)str_to_int(args->argv[1]);
    if (part_num > 3) {
        printf("%cInvalid partition number. Must be 0-3.\n", 255, 0, 0);
        return;
    }
    int format_eynfs = 0;
    if (args->argc >= 3 && args->argv[2] && args->argv[2][0]) {
        const char* fs_type = args->argv[2];
        if (strEql(fs_type, "eynfs")) {
            format_eynfs = 1;
        } else if (!strEql(fs_type, "fat32")) {
            printf("%cUnknown filesystem type: %s\n", 255, 0, 0, fs_type);
            printf("%cSupported types: fat32, eynfs\n", 255, 255, 255);
            return;
        }
    }
    if (!format_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    printf("%cThis will erase all data on partition %d. Are you sure? (y/n): ", 255, 165, 0, part_num);
    string confirm = readStr();
    printf("\n");
    if (strEql(confirm, "y") || strEql(confirm, "Y")) {
        printf("%cFormatting partition %d as %s...\n", 255, 255, 255, part_num, format_eynfs ? "EYNFS" : "FAT32");
        int res;
        if (format_eynfs) {
            res = eynfs_format_partition(0, part_num);
        } else {
            res = fat32_format_partition(0, part_num);
        }
        if (res == 0) {
            printf("%cPartition %d formatted successfully as %s.\n", 0, 255, 0, part_num, format_eynfs ? "EYNFS" : "FAT32");
        } else {
            printf("%cFailed to format partition %d. Error %d\n", 255, 0, 0, part_num, res);
        }
    } else {
        printf("%cFormat cancelled.\n", 255, 255, 255);
    }
} 

REGISTER_SHELL_COMMAND(format, "format", format_cmd_handler, CMD_STREAMING, "Format partition n (0-3) as FAT32 or EYNFS.\nFAT32: widely supported, max 4GB files.\nEYNFS: native, supports long filenames, fast directory access.\nUsage: format <partition_num> <filesystem_type>", "format 1 fat32"); 