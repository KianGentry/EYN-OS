#include <fdisk_commands.h>
#include <types.h>
#include <vga.h>
#include <util.h>
#include <system.h>
#include <string.h>
#include <stdint.h>
#include <shell_command_info.h>
#include <context.h>
#include <misc/sched.h>

static int fdisk_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// fdisk_list implementation
void fdisk_list() {
    if (!fdisk_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    uint8 mbr[512];
    if (ata_read_sector(0, 0, mbr) != 0) {
        printf("%cFailed to read MBR from drive 0\n", 255, 0, 0);
        return;
    }
    uint16_t signature = (mbr[511] << 8) | mbr[510];
    if (signature != 0xAA55) {
        printf("%cWarning: MBR signature is invalid (found %d, expected 43605).\n", 255, 165, 0, signature);
    } else {
        printf("%cMBR signature is valid.\n", 0, 255, 0);
    }
    printf("%cPartition Table (drive 0):\n", 255, 255, 0);
    for (int i = 0; i < 4; i++) {
        uint8* entry = mbr + 0x1BE + i * 16;
        uint8 boot = entry[0];
        uint8 type = entry[4];
        uint32 start_lba = entry[8] | (entry[9]<<8) | (entry[10]<<16) | (entry[11]<<24);
        uint32 size = entry[12] | (entry[13]<<8) | (entry[14]<<16) | (entry[15]<<24);
        printf("%cPart %d: Boot=%d Type=%d Start=%d Size=%d\n", 255,255,255, i+1, boot, type, start_lba, size);
    }
}

// fdisk_create_partition implementation
void fdisk_create_partition(uint32 start_lba, uint32 size, uint8 type) {
    if (!fdisk_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    uint8 mbr[512];
    if (ata_read_sector(0, 0, mbr) != 0) {
        printf("%cFailed to read MBR from drive 0\n", 255, 0, 0);
        return;
    }
    int free_idx = -1;
    for (int i = 0; i < 4; i++) {
        uint8* entry = mbr + 0x1BE + i * 16;
        uint8 ptype = entry[4];
        uint32 pstart = entry[8] | (entry[9]<<8) | (entry[10]<<16) | (entry[11]<<24);
        uint32 psize = entry[12] | (entry[13]<<8) | (entry[14]<<16) | (entry[15]<<24);
        if (ptype == 0 && pstart == 0 && psize == 0) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == -1) {
        printf("%cNo free partition entry available (max 4).\n", 255, 0, 0);
        return;
    }
    uint8* entry = mbr + 0x1BE + free_idx * 16;
    entry[0] = 0x00;
    entry[1] = 0; entry[2] = 0; entry[3] = 0;
    entry[4] = type;
    entry[5] = 0; entry[6] = 0; entry[7] = 0;
    entry[8]  = (start_lba & 0xFF);
    entry[9]  = (start_lba >> 8) & 0xFF;
    entry[10] = (start_lba >> 16) & 0xFF;
    entry[11] = (start_lba >> 24) & 0xFF;
    entry[12] = (size & 0xFF);
    entry[13] = (size >> 8) & 0xFF;
    entry[14] = (size >> 16) & 0xFF;
    entry[15] = (size >> 24) & 0xFF;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (ata_write_sector(0, 0, mbr) != 0) {
        printf("%cFailed to write MBR to drive 0\n", 255, 0, 0);
        return;
    }
    printf("%cPartition created: entry %d, type=%d, start=%d, size=%d\n", 0,255,0, free_idx+1, type, start_lba, size);
}

// fdisk_cmd_handler implementation
void fdisk_cmd_handler(const shell_args_t* args) {
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        fdisk_list();
        return;
    }

    const char* subcmd = args->argv[1];
    if (strEql(subcmd, "create")) {
        if (args->argc < 5 || !args->argv[2] || !args->argv[3] || !args->argv[4]) {
            printf("%cUsage: fdisk create <start_lba> <size> <type>\n", 255, 255, 255);
            return;
        }

        uint32 start_lba = str_to_uint(args->argv[2]);
        uint32 size = str_to_uint(args->argv[3]);

        const char* type_str = args->argv[4];
        uint8 type = 0;
        if (type_str[0] == '0' && (type_str[1] == 'x' || type_str[1] == 'X')) {
            uint32 i = 2;
            while (type_str[i]) {
                char c = type_str[i];
                if (c >= '0' && c <= '9') type = (uint8)((type << 4) | (uint8)(c - '0'));
                else if (c >= 'a' && c <= 'f') type = (uint8)((type << 4) | (uint8)(c - 'a' + 10));
                else if (c >= 'A' && c <= 'F') type = (uint8)((type << 4) | (uint8)(c - 'A' + 10));
                else break;
                i++;
            }
        } else {
            type = (uint8)str_to_uint(type_str);
        }

        fdisk_create_partition(start_lba, size, type);
    } else {
        printf("%cUnknown fdisk subcommand: %s\n", 255, 0, 0, subcmd);
    }
}

REGISTER_SHELL_COMMAND(fdisk, "fdisk", fdisk_cmd_handler, CMD_STREAMING, "List partition table or create partitions.\nUsage: fdisk [create <start_lba> <size> <type>]", "fdisk create 2048 1024000 0x0C"); 