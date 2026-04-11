#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "include/drivers/eynfs.h"

#define DEFAULT_SIZE_SECTORS 1024000
#define RESERVED_BLOCKS 4
#define ZERO_BLOCKS 2048

void die(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <disk.img> [size_in_sectors]\n", argv[0]);
        return 1;
    }
    const char* img_path = argv[1];
    uint32_t size = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : DEFAULT_SIZE_SECTORS;
    FILE* f = fopen(img_path, "r+b");
    if (!f) die("Failed to open image file");

    // Zero the first 2048 sectors
    uint8_t zero[EYNFS_BLOCK_SIZE] = {0};
    for (uint32_t i = 0; i < ZERO_BLOCKS; i++) {
        if (fwrite(zero, 1, EYNFS_BLOCK_SIZE, f) != EYNFS_BLOCK_SIZE)
            die("Failed to zero disk");
    }
    fflush(f);
    fseek(f, 0, SEEK_SET);

    // Calculate block LBAs
    uint32_t superblock_lba = ZERO_BLOCKS;
    uint32_t bitmap_lba = ZERO_BLOCKS + 1;
    uint32_t nametable_lba = ZERO_BLOCKS + 2;
    uint32_t rootdir_lba = ZERO_BLOCKS + 3;

    // Write superblock
    eynfs_superblock_t sb = {0};
    sb.magic = EYNFS_MAGIC;
    sb.version = EYNFS_VERSION;
    sb.block_size = EYNFS_BLOCK_SIZE;
    sb.total_blocks = size;
    sb.root_dir_block = rootdir_lba;
    sb.free_block_map = bitmap_lba;
    sb.name_table_block = nametable_lba;
    fseek(f, superblock_lba * EYNFS_BLOCK_SIZE, SEEK_SET);
    if (fwrite(&sb, 1, sizeof(sb), f) != sizeof(sb)) die("Failed to write superblock");

    // Write zeroed free block bitmap, mark metadata blocks as used
    uint8_t bitmap[EYNFS_BLOCK_SIZE] = {0};
    #define SET_BIT(arr, bit) do { (arr)[(bit)/8] |= (uint8_t)(1u << ((bit)%8)); } while(0)
    SET_BIT(bitmap, superblock_lba);
    SET_BIT(bitmap, bitmap_lba);
    SET_BIT(bitmap, nametable_lba);
    SET_BIT(bitmap, rootdir_lba);
    #undef SET_BIT
    fseek(f, bitmap_lba * EYNFS_BLOCK_SIZE, SEEK_SET);
    if (fwrite(bitmap, 1, EYNFS_BLOCK_SIZE, f) != EYNFS_BLOCK_SIZE) die("Failed to write bitmap");

    // Write empty name table
    uint8_t name_table[EYNFS_BLOCK_SIZE] = {0};
    fseek(f, nametable_lba * EYNFS_BLOCK_SIZE, SEEK_SET);
    if (fwrite(name_table, 1, EYNFS_BLOCK_SIZE, f) != EYNFS_BLOCK_SIZE) die("Failed to write name table");

    // Write empty root directory block
    uint8_t root_dir[EYNFS_BLOCK_SIZE] = {0};
    fseek(f, rootdir_lba * EYNFS_BLOCK_SIZE, SEEK_SET);
    if (fwrite(root_dir, 1, EYNFS_BLOCK_SIZE, f) != EYNFS_BLOCK_SIZE) die("Failed to write root directory");

    fflush(f);
    fclose(f);
    printf("EYNFS format complete: %s (%u sectors)\n", img_path, size);
    return 0;
} 