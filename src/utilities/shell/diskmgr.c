/*
 * diskmgr.c - Disk Management Shell Commands
 *
 * Provides commands for viewing and managing disk partitions.
 * Commands: diskmgr, partitions, mount, unmount, mkswap
 */

#include <misc/types.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <partition.h>
#include <shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <eynfs.h>
#include <ata.h>
#include <context.h>
#include <misc/sched.h>

// DISK MANAGER COMMAND

static int diskmgr_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void diskmgr_show_help(void) {
    printf("Disk Manager Commands:\n");
    printf("  diskmgr                    - Show all disks and partitions\n");
    printf("  diskmgr info <drive>       - Show detailed disk info\n");
    printf("  diskmgr create <drive> <start> <size_mb> <type>\n");
    printf("                             - Create partition (type: eynfs, swap, fat32)\n");
    printf("  diskmgr delete <drive> <part#>\n");
    printf("                             - Delete partition\n");
    printf("  diskmgr format <drive> <part#> <type>\n");
    printf("                             - Format partition (eynfs, swap)\n");
    printf("  diskmgr mount <drive> <part#> <name>\n");
    printf("                             - Mount partition as virtual drive\n");
    printf("  diskmgr unmount <name>     - Unmount virtual drive\n");
    printf("  diskmgr swap <drive> <part#>\n");
    printf("                             - Initialize swap partition\n");
}

static void diskmgr_show_all(void) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    printf("%c=== Disk Manager ===%c\n", 0, 255, 255, 255, 255, 255);
    
    /* Check drives 0 and 1 */
    for (uint8 drive = 0; drive < 2; drive++) {
        disk_info_t disk;
        if (partition_read_table(drive, &disk) == 0) {
            disk_print_info(&disk);
        }
    }
    
    /* Show swap status */
    swap_partition_t *swap = swap_partition_get_info();
    if (swap) {
        printf("\n%cSwap Partition:%c Active on drive %d\n", 0, 255, 0, 255, 255, 255, swap->drive);
        printf("  %u pages available (%u MB)\n", swap->total_pages, swap->total_pages / 256);
        printf("  %u pages used\n", swap->used_pages);
    } else {
        printf("\n%cSwap Partition:%c Not configured\n", 255, 165, 0, 255, 255, 255);
    }
}

static void diskmgr_create_partition(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    /* Parse: <drive> <start_lba> <size_mb> <type> */
    uint8 drive = 0;
    uint32 start_lba = 0;
    uint32 size_mb = 0;
    char type_str[16] = {0};
    
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    /* Parse drive number */
    drive = (uint8)(args[pos] - '0');
    pos++;
    while (args[pos] == ' ') pos++;
    
    /* Parse start LBA */
    start_lba = str_to_uint(args + pos);
    while (args[pos] && args[pos] != ' ') pos++;
    while (args[pos] == ' ') pos++;
    
    /* Parse size in MB */
    size_mb = str_to_uint(args + pos);
    while (args[pos] && args[pos] != ' ') pos++;
    while (args[pos] == ' ') pos++;
    
    /* Parse type string */
    int ti = 0;
    while (args[pos] && args[pos] != ' ' && ti < 15) {
        type_str[ti++] = args[pos++];
    }
    type_str[ti] = '\0';
    
    if (size_mb == 0 || type_str[0] == '\0') {
        printf("%cUsage: diskmgr create <drive> <start_lba> <size_mb> <type>\n", 255, 255, 255);
        printf("Types: eynfs, swap, fat32, linux\n");
        return;
    }
    
    /* Convert type string to partition type */
    uint8 type = PART_TYPE_EMPTY;
    if (strEql(type_str, "eynfs")) type = PART_TYPE_EYNFS;
    else if (strEql(type_str, "swap")) type = PART_TYPE_EYNOS_SWAP;
    else if (strEql(type_str, "fat32")) type = PART_TYPE_FAT32_LBA;
    else if (strEql(type_str, "linux")) type = PART_TYPE_LINUX;
    else {
        printf("%cUnknown partition type: %s\n", 255, 0, 0, type_str);
        return;
    }
    
    /* Convert MB to sectors (2048 sectors per MB) */
    uint32 size_sectors = size_mb * 2048;
    
    partition_create(drive, start_lba, size_sectors, type);
}

static void diskmgr_delete_partition(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    uint8 drive = (uint8)(args[pos] - '0');
    pos++;
    while (args[pos] == ' ') pos++;
    
    uint8 part_num = (uint8)(str_to_uint(args + pos) - 1);
    
    if (partition_delete(drive, part_num) == 0) {
        printf("%cPartition %d deleted\n", 0, 255, 0, part_num + 1);
    } else {
        printf("%cFailed to delete partition\n", 255, 0, 0);
    }
}

static void diskmgr_format_partition(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    uint8 drive = (uint8)(args[pos] - '0');
    pos++;
    while (args[pos] == ' ') pos++;
    
    uint8 part_num = (uint8)(str_to_uint(args + pos) - 1);
    while (args[pos] && args[pos] != ' ') pos++;
    while (args[pos] == ' ') pos++;
    
    char type_str[16] = {0};
    int ti = 0;
    while (args[pos] && args[pos] != ' ' && ti < 15) {
        type_str[ti++] = args[pos++];
    }
    
    if (type_str[0] == '\0') {
        printf("%cUsage: diskmgr format <drive> <part#> <type>\n", 255, 255, 255);
        printf("Types: eynfs, swap\n");
        return;
    }
    
    /* Get partition info */
    disk_info_t disk;
    if (partition_read_table(drive, &disk) != 0) {
        printf("%cFailed to read partition table\n", 255, 0, 0);
        return;
    }
    
    if (part_num >= MBR_MAX_PARTITIONS || disk.partitions[part_num].type == PART_TYPE_EMPTY) {
        printf("%cInvalid partition number\n", 255, 0, 0);
        return;
    }
    
    partition_info_t *part = &disk.partitions[part_num];
    
    if (strEql(type_str, "eynfs")) {
        printf("Formatting partition %d as EYNFS...\n", part_num + 1);
        
        /* Format as EYNFS at the partition's start LBA */
        uint32 superblock_lba = part->lba_start;
        uint32 bitmap_lba = superblock_lba + 1;
        uint32 nametable_lba = superblock_lba + 2;
        uint32 rootdir_lba = superblock_lba + 3;
        
        /* Write superblock */
        uint8 buf[512] = {0};
        eynfs_superblock_t *sb = (eynfs_superblock_t *)buf;
        sb->magic = 0x45594E46;  /* 'EYNF' */
        sb->version = 11;
        sb->block_size = 512;
        sb->total_blocks = part->sector_count;
        sb->root_dir_block = rootdir_lba;
        sb->free_block_map = bitmap_lba;
        sb->name_table_block = nametable_lba;
        
        extern int ata_write_sector(uint8 drive, uint32 lba, const uint8 *buf);
        if (ata_write_sector(drive, superblock_lba, buf) != 0) {
            printf("%cFailed to write superblock\n", 255, 0, 0);
            return;
        }
        
        /* Initialize bitmap - mark first 4 blocks as used */
        memset(buf, 0, 512);
        buf[0] = 0x0F;  /* First 4 bits set (superblock, bitmap, nametable, rootdir) */
        if (ata_write_sector(drive, bitmap_lba, buf) != 0) {
            printf("%cFailed to write bitmap\n", 255, 0, 0);
            return;
        }
        
        /* Initialize name table and root directory */
        memset(buf, 0, 512);
        ata_write_sector(drive, nametable_lba, buf);
        ata_write_sector(drive, rootdir_lba, buf);
        
        printf("%cPartition formatted as EYNFS\n", 0, 255, 0);
        
    } else if (strEql(type_str, "swap")) {
        printf("Initializing partition %d as swap space...\n", part_num + 1);
        
        /* Update partition type */
        part->type = PART_TYPE_EYNOS_SWAP;
        partition_write_table(drive, &disk);
        
        /* Zero first sector as swap header */
        uint8 buf[512] = {0};
        /* Write a simple swap signature */
        buf[0] = 'S';
        buf[1] = 'W';
        buf[2] = 'A';
        buf[3] = 'P';
        *(uint32 *)(buf + 4) = part->sector_count / 8;  /* Number of pages */
        
        extern int ata_write_sector(uint8 drive, uint32 lba, const uint8 *buf);
        ata_write_sector(drive, part->lba_start, buf);
        
        printf("%cSwap space initialized: %u pages\n", 0, 255, 0, part->sector_count / 8);
        
    } else {
        printf("%cUnknown format type: %s\n", 255, 0, 0, type_str);
    }
}

static void diskmgr_mount_partition(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    uint8 drive = (uint8)(args[pos] - '0');
    pos++;
    while (args[pos] == ' ') pos++;
    
    uint8 part_num = (uint8)(str_to_uint(args + pos) - 1);
    while (args[pos] && args[pos] != ' ') pos++;
    while (args[pos] == ' ') pos++;
    
    char mount_name[8] = {0};
    int mi = 0;
    while (args[pos] && args[pos] != ' ' && mi < 7) {
        mount_name[mi++] = args[pos++];
    }
    
    if (mount_name[0] == '\0') {
        printf("%cUsage: diskmgr mount <drive> <part#> <name>\n", 255, 255, 255);
        return;
    }
    
    int slot = vdrive_mount(drive, part_num, mount_name);
    if (slot >= 0) {
        printf("%cMounted partition %d as '%s' (virtual drive %d)\n",
               0, 255, 0, part_num + 1, mount_name, slot);
    } else {
        printf("%cFailed to mount partition\n", 255, 0, 0);
    }
}

static void diskmgr_unmount(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    char mount_name[8] = {0};
    int mi = 0;
    while (args[pos] && args[pos] != ' ' && mi < 7) {
        mount_name[mi++] = args[pos++];
    }
    
    if (vdrive_unmount(mount_name) == 0) {
        printf("%cUnmounted '%s'\n", 0, 255, 0, mount_name);
    } else {
        printf("%cFailed to unmount '%s'\n", 255, 0, 0, mount_name);
    }
}

static void diskmgr_init_swap(const char *args) {
    if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    int pos = 0;
    while (args[pos] == ' ') pos++;
    
    uint8 drive = (uint8)(args[pos] - '0');
    pos++;
    while (args[pos] == ' ') pos++;
    
    uint8 part_num = (uint8)(str_to_uint(args + pos) - 1);
    
    if (swap_partition_init(drive, part_num) == 0) {
        printf("%cSwap partition activated\n", 0, 255, 0);
    } else {
        printf("%cFailed to initialize swap\n", 255, 0, 0);
    }
}

void diskmgr_cmd_handler(const shell_args_t* args) {
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        diskmgr_show_all();
        return;
    }

    const char* subcmd = args->argv[1];
    const char* rest = shell_args_rest_raw(args, 2);
    if (!rest) rest = "";

    if (strEql(subcmd, "help")) {
        diskmgr_show_help();
    } else if (strEql(subcmd, "info")) {
        if (!diskmgr_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
        if (args->argc < 3 || !args->argv[2] || !args->argv[2][0]) {
            printf("%cUsage: diskmgr info <drive>\n", 255, 255, 255);
            return;
        }
        uint8 drive = (uint8)str_to_uint(args->argv[2]);
        disk_info_t disk;
        if (partition_read_table(drive, &disk) == 0) {
            disk_print_info(&disk);
        } else {
            printf("%cFailed to read disk %d\n", 255, 0, 0, drive);
        }
    } else if (strEql(subcmd, "create")) {
        diskmgr_create_partition(rest);
    } else if (strEql(subcmd, "delete")) {
        diskmgr_delete_partition(rest);
    } else if (strEql(subcmd, "format")) {
        diskmgr_format_partition(rest);
    } else if (strEql(subcmd, "mount")) {
        diskmgr_mount_partition(rest);
    } else if (strEql(subcmd, "unmount")) {
        diskmgr_unmount(rest);
    } else if (strEql(subcmd, "swap")) {
        diskmgr_init_swap(rest);
    } else {
        printf("%cUnknown subcommand: %s\n", 255, 0, 0, subcmd);
        diskmgr_show_help();
    }
}

REGISTER_SHELL_COMMAND(diskmgr, "diskmgr", diskmgr_cmd_handler, CMD_STREAMING,
    "Disk partition manager - view, create, format, and mount partitions.\n"
    "Usage: diskmgr [subcommand] [args]\n"
    "Subcommands: info, create, delete, format, mount, unmount, swap, help",
    "diskmgr\ndiskmgr create 0 2048 5 eynfs\ndiskmgr format 0 1 eynfs");
