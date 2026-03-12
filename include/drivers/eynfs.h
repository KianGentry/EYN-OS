#ifndef EYNFS_H
#define EYNFS_H

#include <stdint.h>
#include <stddef.h>
#include <misc/types.h>

/*
 * FS-INVARIANT: Absolute LBA of the EYNFS superblock.
 *
 * Why: Boot tools, fsck, and the kernel driver all locate the filesystem by
 *      reading the superblock at this fixed sector.
 * Invariant: The EYNFS on-disk format assumes its superblock resides at this
 *           absolute LBA; changing it breaks mounting existing images.
 * Breakage if changed: Existing disk images will not be recognized and may be
 *                      treated as unformatted/invalid.
 * ABI-sensitive: No.
 * Disk-format-sensitive: Yes.
 * Security-critical: Yes (mount-time format identification boundary).
 */
#define EYNFS_SUPERBLOCK_LBA 2048u

// EYNFS magic number ('EYNF')
#define EYNFS_MAGIC 0x45594E46
#define EYNFS_NAME_MAX 32

// Filesystem version
#define EYNFS_VERSION 11

// Block size for EYNFS (used throughout the FS)
#define EYNFS_BLOCK_SIZE 512

// Directory entry types
typedef enum {
    EYNFS_TYPE_FILE = 1,
    EYNFS_TYPE_DIR  = 2
} eynfs_entry_type_t;

// Superblock structure (on-disk)
typedef struct {
    uint32_t magic;         // Magic number to identify EYNFS
    uint32_t version;       // Filesystem version
    uint32_t block_size;    // Block size in bytes
    uint32_t total_blocks;  // Total number of blocks
    uint32_t root_dir_block;// Block number of root directory
    uint32_t free_block_map;// Block number of free block bitmap (optional/future)
    uint32_t name_table_block; // Block number of name table
    uint32_t reserved[2];   // Reserved for future use
} eynfs_superblock_t;

// Directory entry structure (on-disk)
typedef struct __attribute__((packed)) {
    char name[EYNFS_NAME_MAX]; // Null-terminated name
    uint8_t type;              // File or directory
    uint8_t flags;             // Feature flags (e.g., permissions, symlink, etc.)
    uint16_t reserved;         // Reserved for future use
    uint32_t size;             // Size in bytes (for files)
    uint32_t first_block;      // First data block or subdir table block
    uint32_t extra[2];         // Reserved for future expansion
} eynfs_dir_entry_t;

// Modular FS API (function pointers for generic file operations)
typedef struct {
    int (*open)(const char *path, int mode);
    int (*read)(int fd, void *buf, size_t size);
    int (*write)(int fd, const void *buf, size_t size);
    int (*close)(int fd);
    int (*listdir)(const char *path, void *buf, size_t bufsize);
    int (*mkdir)(const char *path);
    int (*remove)(const char *path);
    // Extend with more operations as needed
} fs_ops_t;

// Function prototypes for EYNFS API
int eynfs_read_superblock(uint8 drive, uint32 lba, eynfs_superblock_t *sb);
int eynfs_write_superblock(uint8 drive, uint32 lba, const eynfs_superblock_t *sb);
int eynfs_read_dir_table(uint8 drive, uint32 lba, eynfs_dir_entry_t *entries, size_t max_entries);
int eynfs_write_dir_table(uint8 drive, uint32 lba, const eynfs_dir_entry_t *entries, size_t num_entries);
int eynfs_count_dir_entries(uint8 drive, uint32_t lba);
int eynfs_find_in_dir(uint8 drive, const eynfs_superblock_t *sb, uint32_t dir_block, const char *name, eynfs_dir_entry_t *out_entry, uint32_t *out_index);
int eynfs_traverse_path(uint8 drive, const eynfs_superblock_t *sb, const char *path, eynfs_dir_entry_t *out_entry, uint32_t *parent_block, uint32_t *entry_index);
int eynfs_create_entry(uint8 drive, eynfs_superblock_t *sb, uint32_t parent_block, const char *name, uint8_t type);
int eynfs_delete_entry(uint8 drive, eynfs_superblock_t *sb, uint32_t parent_block, const char *name);
int eynfs_read_file(uint8 drive, const eynfs_superblock_t *sb, const eynfs_dir_entry_t *entry, void *buf, size_t bufsize, size_t offset);

/*
 * ABI-INVARIANT: cursor-aware EYNFS file read.
 *
 * Replaces repeated eynfs_read_file() calls for sequential streaming by
 * caching the last reached block pair so the next read need not re-traverse
 * from first_block.
 *
 * first_block      -- entry->first_block of the target file.
 * file_size        -- entry->size (used for EOF clamping).
 * buf/bufsize      -- kernel-side destination buffer.
 * offset           -- logical byte offset within the file.
 * p_cur_block      -- in/out: 0 = uninitialized (restart from first_block).
 *                    Updated to the block where the read ended.
 * p_cur_block_off  -- in/out: file byte offset at start of *p_cur_block's
 *                    data payload.  Updated in tandem with p_cur_block.
 *
 * Returns number of bytes read, 0 at EOF, -1 on error.
 * Both cursor fields are updated on success.
 */
int eynfs_read_file_fast(uint8 drive, uint32_t first_block, uint32_t file_size,
                         void *buf, size_t bufsize, size_t offset,
                         uint32_t *p_cur_block, uint32_t *p_cur_block_off);

int eynfs_write_file(uint8 drive, eynfs_superblock_t *sb, eynfs_dir_entry_t *entry, const void *buf, size_t size, uint32_t parent_block, uint32_t entry_index);
int eynfs_alloc_block(uint8 drive, eynfs_superblock_t *sb);
int eynfs_free_block(uint8 drive, eynfs_superblock_t *sb, uint32_t block);
int eynfs_format_partition(uint8 drive, uint8 partition_num);

// New improved file operations
int eynfs_open(const char* path, int mode);
int eynfs_seek(int fd, size_t offset, int whence);
int eynfs_close(int fd);

int read(int fd, void* buf, int size);
int close(int fd);
int open(const char* path, int mode);

// File operation modes
#define EYNFS_READ  0
#define EYNFS_WRITE 1
#define EYNFS_APPEND 2

// Seek modes
#define EYNFS_SEEK_SET 0
#define EYNFS_SEEK_CUR 1
#define EYNFS_SEEK_END 2

// Performance monitoring functions
void eynfs_get_cache_stats(uint32_t* hits, uint32_t* misses);
void eynfs_reset_cache_stats();
void eynfs_cache_clear();
int eynfs_alloc_block_fast(uint8 drive, eynfs_superblock_t *sb);
int eynfs_cache_get_block(uint8 drive, uint32_t block_num, uint8_t* data);
int eynfs_cache_write_block(uint8 drive, uint32_t block_num, const uint8_t* data);

// Streaming writer for low-memory environments
typedef struct {
    uint8 drive;
    eynfs_superblock_t sb;
    eynfs_dir_entry_t entry;
    uint32_t parent_block;
    uint32_t entry_index;
    uint32_t curr_block;   // current block being filled
    uint32_t first_block;
    uint32_t size;
    uint16_t pos_in_block; // write cursor within current block's data area [0..EYNFS_BLOCK_SIZE-4]
} eynfs_stream_t;

// Begin a streaming write to the given path (overwrites if exists)
int eynfs_stream_begin(uint8 drive, const char* path, eynfs_stream_t* s);
// Write a chunk (any size); splits into filesystem blocks internally
int eynfs_stream_write(eynfs_stream_t* s, const void* buf, size_t size);
// Finalize and update directory entry
int eynfs_stream_end(eynfs_stream_t* s);

#endif // EYNFS_H