#include <eynfs.h>
#include <misc/types.h>
#include <string.h>
#include <vga.h>
#include <util.h>
#include <math.h> // For quicksort and boyer-moore
#include <stdint.h>
#include <drivers/serial.h>
#include <ata.h>
#include <watchdog.h>
#include <context.h>
#include <misc/sched.h>
#include <fs/fs_txn.h>

#define EYNFS_BLOCK_SIZE 512 // For now, fixed block size
#define EYNFS_SUPERBLOCK_LBA 2048 // Standard superblock location

// Optional integration point: when the interactive shell is linked in, it
// exposes the current physical drive. Keep this weak so the driver can still
// link in non-shell contexts.
__attribute__((weak)) uint8_t get_current_physical_drive(void);

static uint8_t eynfs_default_drive(void) {
    // If the weak symbol is not provided by the final link, its address is 0.
    if (&get_current_physical_drive) return get_current_physical_drive();
    return 0;
}

// Accept an optional drive prefix of the form "N:/path" (N is decimal).
// On success, returns 1 and updates *path_inout to point past the prefix.
static int eynfs_parse_drive_prefix(const char** path_inout, uint8_t* out_drive) {
    const char* p = path_inout ? *path_inout : NULL;
    if (!p || !out_drive) return 0;

    // Parse decimal digits.
    uint32_t drive = 0;
    int ndigits = 0;
    while (*p >= '0' && *p <= '9') {
        drive = drive * 10u + (uint32_t)(*p - '0');
        ndigits++;
        if (drive > 255u) return 0;
        p++;
    }
    if (ndigits == 0) return 0;
    if (p[0] != ':' || p[1] != '/') return 0;

    *out_drive = (uint8_t)drive;
    *path_inout = p + 1; // Keep leading '/', skip ':'
    return 1;
}

// EYNFS uses filesystem-relative block numbers on disk:
//   block 0 => superblock at absolute LBA EYNFS_SUPERBLOCK_LBA
//   block 1 => bitmap, etc.
static inline uint32_t eynfs_block_to_lba(uint32_t fs_block) {
    return EYNFS_SUPERBLOCK_LBA + fs_block;
}

// Performance optimization: Block cache
#define EYNFS_CACHE_SIZE 16
typedef struct {
    uint32_t block_num;
    uint8_t data[EYNFS_BLOCK_SIZE];
    uint8_t dirty;
    uint8_t valid;
} eynfs_cache_entry_t;

static eynfs_cache_entry_t block_cache[EYNFS_CACHE_SIZE];
static uint32_t cache_hits = 0;
static uint32_t cache_misses = 0;

// Performance optimization: Free block tracking
#define EYNFS_FREE_BLOCK_CACHE_SIZE 64
static uint32_t free_block_cache[EYNFS_FREE_BLOCK_CACHE_SIZE];
static uint32_t free_block_cache_count = 0;
static uint8_t free_block_cache_valid = 0;

// Performance optimization: Directory entry cache
typedef struct {
    uint32_t dir_block;
    eynfs_dir_entry_t* entries;
    int count;
    uint8_t sorted; // 1 if entries are sorted by name
} eynfs_dir_cache_entry_t;

#define EYNFS_DIR_CACHE_SIZE 8
static eynfs_dir_cache_entry_t dir_cache[EYNFS_DIR_CACHE_SIZE];

static int eynfs_ctx_check(uint32 cap, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, cap)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Initialize caches
static void eynfs_init_caches() {
    // Initialize block cache
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        block_cache[i].valid = 0;
        block_cache[i].dirty = 0;
    }
    
    // Initialize directory cache
    for (int i = 0; i < EYNFS_DIR_CACHE_SIZE; i++) {
        dir_cache[i].entries = NULL;
        dir_cache[i].count = 0;
        dir_cache[i].sorted = 0;
    }
    
    // Initialize free block cache
    free_block_cache_valid = 0;
    free_block_cache_count = 0;
}

// Block cache functions
int eynfs_cache_get_block(uint8 drive, uint32_t block_num, uint8_t* data) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    // Look for block in cache
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        if (block_cache[i].valid && block_cache[i].block_num == block_num) {
            // Cache hit - copy data
            memcpy(data, block_cache[i].data, EYNFS_BLOCK_SIZE);
            cache_hits++;
            return 0;
        }
    }
    
    // Cache miss - read from disk
    uint32_t lba = eynfs_block_to_lba(block_num);
    if (ata_read_sector(drive, lba, data) != 0) {
        static int g_eynfs_read_fail_printed = 0;
        if (!g_eynfs_read_fail_printed) {
            g_eynfs_read_fail_printed = 1;
            char sbuf[160];
            int n = snprintf(sbuf, sizeof(sbuf), "[EYNFS] read fail drive=%d fs_block=%d lba=%d\n", (int)drive, (int)block_num, (int)lba);
            if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
        }
        return -1;
    }
    
    // Find least recently used cache entry
    int lru_index = 0;
    uint32_t oldest_time = 0xFFFFFFFF;
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        if (!block_cache[i].valid) {
            lru_index = i;
            break;
        }
        // Simple LRU: use block number as time (lower = older)
        if (block_cache[i].block_num < oldest_time) {
            oldest_time = block_cache[i].block_num;
            lru_index = i;
        }
    }
    
    // Write dirty block if needed
    if (block_cache[lru_index].valid && block_cache[lru_index].dirty) {
        if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
        ata_write_sector(drive, eynfs_block_to_lba(block_cache[lru_index].block_num), block_cache[lru_index].data);
    }
    
    // Cache the new block
    block_cache[lru_index].block_num = block_num;
    memcpy(block_cache[lru_index].data, data, EYNFS_BLOCK_SIZE);
    block_cache[lru_index].valid = 1;
    block_cache[lru_index].dirty = 0;
    
    cache_misses++;
    return 0;
}

int eynfs_cache_write_block(uint8 drive, uint32_t block_num, const uint8_t* data) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    // Look for block in cache
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        if (block_cache[i].valid && block_cache[i].block_num == block_num) {
            // Update cache
            memcpy(block_cache[i].data, data, EYNFS_BLOCK_SIZE);
            block_cache[i].dirty = 1;
            return 0;
        }
    }
    
    // Not in cache - write directly to disk
    return ata_write_sector(drive, eynfs_block_to_lba(block_num), data);
}

static void eynfs_cache_flush(uint8 drive) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return;
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        if (block_cache[i].valid && block_cache[i].dirty) {
            ata_write_sector(drive, eynfs_block_to_lba(block_cache[i].block_num), block_cache[i].data);
            block_cache[i].dirty = 0;
        }
    }
}

static void eynfs_cache_invalidate_block(uint32_t block_num) {
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        if (block_cache[i].valid && block_cache[i].block_num == block_num) {
            block_cache[i].valid = 0;
            block_cache[i].dirty = 0;
        }
    }
}

static void eynfs_dir_cache_invalidate(uint32_t dir_block) {
    for (int i = 0; i < EYNFS_DIR_CACHE_SIZE; i++) {
        if (dir_cache[i].entries && dir_cache[i].dir_block == dir_block) {
            free(dir_cache[i].entries);
            dir_cache[i].entries = NULL;
            dir_cache[i].count = 0;
            dir_cache[i].sorted = 0;
        }
    }
}

// Directory cache functions
static eynfs_dir_cache_entry_t* eynfs_dir_cache_find(uint32_t dir_block) {
    for (int i = 0; i < EYNFS_DIR_CACHE_SIZE; i++) {
        if (dir_cache[i].entries && dir_cache[i].dir_block == dir_block) {
            // Don't set sorted flag since we're not actually sorting
            return &dir_cache[i];
        }
    }
    return NULL;
}

static eynfs_dir_cache_entry_t* eynfs_dir_cache_alloc() {
    // Find free slot or evict least recently used
    for (int i = 0; i < EYNFS_DIR_CACHE_SIZE; i++) {
        if (!dir_cache[i].entries) {
            // Allocate space for directory entries (4KB limit for low-memory systems)
            dir_cache[i].entries = (eynfs_dir_entry_t*)malloc(4096);
            if (dir_cache[i].entries) {
                return &dir_cache[i];
            }
        }
    }
    
    // Evict first entry (simple LRU)
    if (dir_cache[0].entries) {
        free(dir_cache[0].entries);
        dir_cache[0].entries = (eynfs_dir_entry_t*)malloc(4096);
        if (dir_cache[0].entries) {
            return &dir_cache[0];
        }
    }
    
    return NULL;
}

// Binary search for directory entries (requires sorted entries)
static int eynfs_binary_search_dir(const eynfs_dir_entry_t* entries, int count, const char* name) {
    int left = 0;
    int right = count - 1;
    
    while (left <= right) {
        int mid = (left + right) / 2;
        if (entries[mid].name[0] == '\0') {
            // Skip empty entries
            right = mid - 1;
            continue;
        }
        
        int cmp = strncmp(entries[mid].name, name, EYNFS_NAME_MAX);
        if (cmp == 0) {
            return mid;
        } else if (cmp < 0) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    
    return -1;
}

// Helper: Read the free block bitmap
static uint32_t eynfs_bitmap_block_count(const eynfs_superblock_t *sb) {
    // 1 bit per filesystem block.
    uint32_t bitmap_bytes = (sb->total_blocks + 7u) / 8u;
    return (bitmap_bytes + EYNFS_BLOCK_SIZE - 1u) / EYNFS_BLOCK_SIZE;
}

static int eynfs_read_bitmap_block(uint8 drive, const eynfs_superblock_t *sb, uint32_t bitmap_block_idx, uint8 *bitmap_block) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    uint32_t fs_block = sb->free_block_map + bitmap_block_idx;
    return ata_read_sector(drive, eynfs_block_to_lba(fs_block), bitmap_block);
}

static int eynfs_write_bitmap_block(uint8 drive, const eynfs_superblock_t *sb, uint32_t bitmap_block_idx, const uint8 *bitmap_block) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    uint32_t fs_block = sb->free_block_map + bitmap_block_idx;
    return ata_write_sector(drive, eynfs_block_to_lba(fs_block), bitmap_block);
}

static int eynfs_bitmap_set_used(uint8 drive, const eynfs_superblock_t *sb, uint32_t fs_block_num, uint8 used) {
    if (fs_block_num >= sb->total_blocks) return -1;
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_SUPERBLOCK, 1u);
    }

    uint32_t byte_index = fs_block_num / 8u;
    uint32_t bit_index = fs_block_num % 8u;

    uint32_t bitmap_block_idx = byte_index / EYNFS_BLOCK_SIZE;
    uint32_t offset_in_block = byte_index % EYNFS_BLOCK_SIZE;

    uint32_t bitmap_blocks = eynfs_bitmap_block_count(sb);
    if (bitmap_block_idx >= bitmap_blocks) return -1;

    uint8 bitmap[EYNFS_BLOCK_SIZE];
    if (eynfs_read_bitmap_block(drive, sb, bitmap_block_idx, bitmap) != 0) return -1;

    if (used) {
        bitmap[offset_in_block] |= (uint8)(1u << bit_index);
    } else {
        bitmap[offset_in_block] &= (uint8)~(1u << bit_index);
    }

    if (eynfs_write_bitmap_block(drive, sb, bitmap_block_idx, bitmap) != 0) return -1;
    return 0;
}

// Optimized block allocation using free block cache
static int eynfs_alloc_block_optimized(uint8 drive, eynfs_superblock_t *sb) {
    // Try cache first
    if (free_block_cache_valid && free_block_cache_count > 0) {
        uint32_t block = free_block_cache[--free_block_cache_count];
        return block;
    }
    
    // Refill cache (bitmap can span multiple blocks)
    free_block_cache_count = 0;
    uint32_t bitmap_blocks = eynfs_bitmap_block_count(sb);

    for (uint32_t b = 0; b < bitmap_blocks && free_block_cache_count < EYNFS_FREE_BLOCK_CACHE_SIZE; ++b) {
        if (current_command_context && ((b & 0x3Fu) == 0u)) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        uint8 bitmap[EYNFS_BLOCK_SIZE];
        if (eynfs_read_bitmap_block(drive, sb, b, bitmap) != 0) return -1;

        for (uint32_t byte = 0; byte < EYNFS_BLOCK_SIZE && free_block_cache_count < EYNFS_FREE_BLOCK_CACHE_SIZE; ++byte) {
            if (bitmap[byte] == 0xFF) continue;
            for (uint32_t bit = 0; bit < 8 && free_block_cache_count < EYNFS_FREE_BLOCK_CACHE_SIZE; ++bit) {
                if (bitmap[byte] & (1u << bit)) continue;

                uint32_t fs_block_num = (b * EYNFS_BLOCK_SIZE + byte) * 8u + bit;
                if (fs_block_num >= sb->total_blocks) break;
                free_block_cache[free_block_cache_count++] = fs_block_num;
            }
        }
    }
    
    if (free_block_cache_count > 0) {
        free_block_cache_valid = 1;
        uint32_t block = free_block_cache[--free_block_cache_count];
        return block;
    }
    
    return -1; // No free blocks
}

// Read the EYNFS superblock from disk
int eynfs_read_superblock(uint8 drive, uint32 lba, eynfs_superblock_t *sb) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_SUPERBLOCK, (uint32)sizeof(*sb));
    }
    uint8 buf[EYNFS_BLOCK_SIZE];
    if (ata_read_sector(drive, lba, buf) != 0) {
        return -1;
    }
    memcpy(sb, buf, sizeof(eynfs_superblock_t));
    
    // Initialize caches on first superblock read
    static int caches_initialized = 0;
    if (!caches_initialized) {
        eynfs_init_caches();
        caches_initialized = 1;
    }
    
    return 0;
}

// Write the EYNFS superblock to disk
int eynfs_write_superblock(uint8 drive, uint32 lba, const eynfs_superblock_t *sb) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_SUPERBLOCK, (uint32)sizeof(*sb));
    }
    uint8 buf[EYNFS_BLOCK_SIZE] = {0};
    memcpy(buf, sb, sizeof(eynfs_superblock_t));
    if (ata_write_sector(drive, lba, buf) != 0) {
        return -1;
    }
    return 0;
}

// Read a directory table from disk (multi-block chain)
int eynfs_read_dir_table(uint8 drive, uint32 lba, eynfs_dir_entry_t *entries, size_t max_entries) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    size_t total_entries = 0;
    uint32_t current_block = lba;
    uint8 buf[EYNFS_BLOCK_SIZE];
    while (current_block && total_entries < max_entries) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        if (ata_read_sector(drive, eynfs_block_to_lba(current_block), buf) != 0) return -1;
        uint32_t next_block = *(uint32_t*)buf;
        size_t entry_count = (EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t);
        size_t entries_to_copy = entry_count;
        if (max_entries - total_entries < entry_count) entries_to_copy = max_entries - total_entries;
        memcpy(&entries[total_entries], buf + 4, entries_to_copy * sizeof(eynfs_dir_entry_t));
        // Ensure all names are null-terminated
        for (size_t j = 0; j < entries_to_copy; ++j) {
            entries[total_entries + j].name[EYNFS_NAME_MAX - 1] = '\0';
        }
        total_entries += entries_to_copy;
        if (total_entries >= max_entries) break;
        current_block = next_block;
    }
    return (int)total_entries;
}

// Helper: Write a single directory block
static int eynfs_write_dir_block(uint8 drive, uint32_t block_num, const eynfs_dir_entry_t *entries, 
                                size_t num_entries, uint32_t next_block) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    uint8 buf[EYNFS_BLOCK_SIZE] = {0};
    *(uint32_t*)buf = next_block;
    size_t entries_to_write = (EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t);
    if (num_entries < entries_to_write) entries_to_write = num_entries;
    memcpy(buf + 4, entries, entries_to_write * sizeof(eynfs_dir_entry_t));
    return ata_write_sector(drive, eynfs_block_to_lba(block_num), buf);
}

// Helper: Count directory entries without allocating memory
int eynfs_count_dir_entries(uint8 drive, uint32_t lba) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    int total_entries = 0;
    uint32_t current_block = lba;
    uint8 buf[EYNFS_BLOCK_SIZE];
    int block_count = 0;
    
    // Limit to reasonable number of blocks to prevent excessive allocation
    const int max_blocks = 32; // 32 blocks = ~288 entries max (much more reasonable)
    
    while (current_block && block_count < max_blocks) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        if (ata_read_sector(drive, eynfs_block_to_lba(current_block), buf) != 0) return -1;
        uint32_t next_block = *(uint32_t*)buf;
        size_t entry_count = (EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t);
        total_entries += entry_count;
        current_block = next_block;
        block_count++;
    }
    
    // If we hit the limit, return a conservative estimate
    if (block_count >= max_blocks && current_block) {
        printf("%cWarning: Directory has more than %d blocks, limiting allocation\n", 255, 165, 0, max_blocks);
    }
    
    return total_entries;
}

// Helper: Free a chain of directory blocks
static int eynfs_free_dir_chain(uint8 drive, uint32_t first_block) {
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) != 0) return -1;
    
    uint32_t block_num = first_block;
    while (block_num != 0) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        uint8 buf[EYNFS_BLOCK_SIZE];
        if (ata_read_sector(drive, eynfs_block_to_lba(block_num), buf) != 0) break;
        uint32_t next_block = *(uint32_t*)buf;
        eynfs_free_block(drive, &sb, block_num);
        block_num = next_block;
    }
    return 0;
}

// Write a directory table to disk (multi-block chain) - Simplified version
int eynfs_write_dir_table(uint8 drive, uint32 lba, const eynfs_dir_entry_t *entries, size_t num_entries) {
    if (!entries && num_entries > 0) return -1;
    if (num_entries == 0) return 0;
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_DIR, (uint32)(num_entries * sizeof(*entries)));
    }
    
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) != 0) return -1;
    
    // First, read the existing block chain to preserve it
    uint32_t original_blocks[32]; // Support up to 32 blocks
    int block_count = 0;
    uint32_t current_block = lba;
    
    while (current_block && block_count < 32) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        original_blocks[block_count++] = current_block;
        uint8 buf[EYNFS_BLOCK_SIZE];
        if (ata_read_sector(drive, eynfs_block_to_lba(current_block), buf) != 0) break;
        current_block = *(uint32_t*)buf;
    }
    
    uint32_t prev_block = 0;
    size_t written = 0;
    size_t entries_per_block = (EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t);
    
    // Write all entries using the preserved block chain
    uint32_t allocated_next_block = 0; // Store the allocated block for the second loop
    for (int block_idx = 0; block_idx < block_count && written < num_entries; block_idx++) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        uint32_t current_block = original_blocks[block_idx];
        
        // Calculate how many entries to write in this block
        size_t to_write = num_entries - written;
        if (to_write > entries_per_block) to_write = entries_per_block;
        
        // Determine next block (0 if this is the last block or last entry)
        uint32_t next_block = 0;
        if (written + to_write < num_entries) {
            if (block_idx + 1 < block_count) {
                next_block = original_blocks[block_idx + 1];
            } else {
                // Need to allocate a new block for remaining entries
                next_block = eynfs_alloc_block(drive, &sb);
                if (next_block < 0) {
                    printf("%c[DEBUG] eynfs_write_dir_table: Failed to allocate new block\n", 0, 255, 0);
                    return -1;
                }
                allocated_next_block = next_block; // Store for the second loop
            }
        }
        
        // Write the block
        if (eynfs_write_dir_block(drive, current_block, &entries[written], to_write, next_block) != 0) {
            return -1;
        }
        
        written += to_write;
        
        // If we just wrote to the last original block and there are more entries,
        // we need to continue with newly allocated blocks
        if (written < num_entries && block_idx == block_count - 1) {
            break; // Exit this loop and continue with new blocks
        }
    }
    
    // Continue writing to newly allocated blocks if needed
    while (written < num_entries) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        // Use the allocated_next_block that was allocated in the previous loop
        uint32_t new_block = allocated_next_block;
        if (new_block == 0) {
            // If no allocated_next_block was allocated, allocate one now
            new_block = eynfs_alloc_block(drive, &sb);
            if (new_block < 0) {
                printf("%c[DEBUG] eynfs_write_dir_table: Failed to allocate additional block\n", 0, 255, 0);
                return -1;
            }
        }
        
        size_t to_write = num_entries - written;
        if (to_write > entries_per_block) to_write = entries_per_block;
        
        uint32_t next_block = 0;
        if (written + to_write < num_entries) {
            next_block = eynfs_alloc_block(drive, &sb);
            if (next_block < 0) {
                printf("%c[DEBUG] eynfs_write_dir_table: Failed to allocate next block\n", 0, 255, 0);
                return -1;
            }
        }
        
        if (eynfs_write_dir_block(drive, new_block, &entries[written], to_write, next_block) != 0) {
            return -1;
        }
        
        written += to_write;
    }
    
    return (int)written;
}

// Helper: Validate superblock
static int eynfs_validate_superblock(const eynfs_superblock_t *sb) {
    if (!sb) return -1;
    if (sb->magic != EYNFS_MAGIC) return -1;
    if (sb->version != EYNFS_VERSION) return -1;
    if (sb->block_size != EYNFS_BLOCK_SIZE) return -1;
    if (sb->total_blocks == 0) return -1;
    if (sb->root_dir_block >= sb->total_blocks) return -1;
    if (sb->free_block_map >= sb->total_blocks) return -1;
    return 0;
}

// Helper: Validate directory entry
static int eynfs_validate_dir_entry(const eynfs_dir_entry_t *entry) {
    if (!entry) return -1;
    if (entry->name[0] == '\0') return -1; // Empty name
    if (entry->type != EYNFS_TYPE_FILE && entry->type != EYNFS_TYPE_DIR) return -1;
    return 0;
}

// Allocate a free block, mark it as used in the bitmap, and return its block number
int eynfs_alloc_block(uint8 drive, eynfs_superblock_t *sb) {
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    // Use optimized allocation with caching
    int block = eynfs_alloc_block_optimized(drive, sb);
    if (block >= 0) {
        if (eynfs_bitmap_set_used(drive, sb, (uint32_t)block, 1) != 0) {
            // Conservative: invalidate cache so we don't hand out the same block again.
            free_block_cache_valid = 0;
            free_block_cache_count = 0;
            return -1;
        }
        return block;
    }

    return -1; // No free block found
}

// Free a block (mark as unused in the bitmap)
int eynfs_free_block(uint8 drive, eynfs_superblock_t *sb, uint32_t block) {
    if (!eynfs_ctx_check(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (block >= sb->total_blocks) return -1;
    if (eynfs_bitmap_set_used(drive, sb, block, 0) != 0) return -1;

    // The cache may now be stale.
    free_block_cache_valid = 0;
    free_block_cache_count = 0;
    return 0;
}

// Find an entry by name in a directory block
// Returns 0 if found, -1 if not found
int eynfs_find_in_dir(uint8 drive, const eynfs_superblock_t *sb, uint32_t dir_block, const char *name, eynfs_dir_entry_t *out_entry, uint32_t *out_index) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    if (!sb || !name || !name[0]) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_DIR, (uint32)sizeof(eynfs_dir_entry_t));
    }

    // Check directory cache first (best-effort). The cache may be partial (4KB),
    // so a cache miss must fall back to a full on-disk scan.
    eynfs_dir_cache_entry_t* cache_entry = eynfs_dir_cache_find(dir_block);
    if (cache_entry) {
        for (int i = 0; i < cache_entry->count; ++i) {
            if (cache_entry->entries[i].name[0] == '\0') continue;
            if (strncmp(cache_entry->entries[i].name, name, EYNFS_NAME_MAX) == 0) {
                if (out_entry) *out_entry = cache_entry->entries[i];
                if (out_index) *out_index = (uint32_t)i;
                return 0;
            }
        }
        // fall through to full scan
    }

    // Stream-scan the directory table by walking the on-disk block chain.
    // This avoids large allocations and works for large directories.
    const uint32_t entries_per_block = (uint32_t)((EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t));
    uint32_t current_block = dir_block;
    uint32_t base_index = 0;
    int block_count = 0;
    const int max_blocks = 4096; // safety bound

    while (current_block && block_count < max_blocks) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        uint8 buf[EYNFS_BLOCK_SIZE];
        if (eynfs_cache_get_block(drive, current_block, buf) != 0) return -1;

        uint32_t next_block = *(uint32_t*)buf;
        const eynfs_dir_entry_t* entries = (const eynfs_dir_entry_t*)(buf + 4);

        for (uint32_t i = 0; i < entries_per_block; ++i) {
            if (entries[i].name[0] == '\0') continue;
            if (strncmp(entries[i].name, name, EYNFS_NAME_MAX) == 0) {
                if (out_entry) *out_entry = entries[i];
                if (out_index) *out_index = base_index + i;
                return 0;
            }
        }

        base_index += entries_per_block;
        current_block = next_block;
        block_count++;
    }

    return -1;
}

// Traverse a path from root, return the entry for the last component
// Optionally returns parent directory block and entry index
// Returns 0 if found, -1 if not found
int eynfs_traverse_path(uint8 drive, const eynfs_superblock_t *sb, const char *path, eynfs_dir_entry_t *out_entry, uint32_t *parent_block, uint32_t *entry_index) {
    if (!path || path[0] != '/') return -1;
    // PATCH: handle root directory as a valid path
    if (strcmp(path, "/") == 0) {
        if (out_entry) {
            memset(out_entry, 0, sizeof(eynfs_dir_entry_t));
            out_entry->type = EYNFS_TYPE_DIR;
            out_entry->first_block = sb->root_dir_block;
        }
        if (parent_block) *parent_block = 0;
        if (entry_index) *entry_index = 0;
        return 0;
    }
    char temp_path[256];
    strncpy(temp_path, path, sizeof(temp_path));
    temp_path[sizeof(temp_path)-1] = '\0';
    char *token, *saveptr;
    uint32_t current_block = sb->root_dir_block;
    uint32_t last_parent = 0;
    uint32_t last_index = 0;
    eynfs_dir_entry_t entry;
    token = strtok_r(temp_path, "/", &saveptr);
    while (token) {
        if (eynfs_find_in_dir(drive, sb, current_block, token, &entry, &last_index) != 0) {
            return -1;
        }
        last_parent = current_block;
        current_block = entry.first_block;
        token = strtok_r(NULL, "/", &saveptr);
        if (token && entry.type != EYNFS_TYPE_DIR) {
            return -1; // Not a directory in the path
        }
    }
    if (out_entry) *out_entry = entry;
    if (parent_block) *parent_block = last_parent;
    if (entry_index) *entry_index = last_index;
    return 0;
}

// Create a file or directory entry in the given parent directory
int eynfs_create_entry(uint8 drive, eynfs_superblock_t *sb, uint32_t parent_block, const char *name, uint8_t type) {
    if (!sb) return -1;
    if (!name || !name[0]) return -1;
    if (type != EYNFS_TYPE_FILE && type != EYNFS_TYPE_DIR) return -1;
    if (parent_block == 0) return -1;
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_DIR, (uint32)sizeof(eynfs_dir_entry_t));
        if (type == EYNFS_TYPE_FILE) {
            fs_txn_touch(drive, FS_TXN_TAG_FILE, 0u);
        }
    }

    // Create without heap allocations: scan the directory-table block chain looking
    // for an empty slot; if none, append a new directory-table block.
    const size_t entries_per_block = (EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t);
    uint32_t cur = parent_block;
    uint32_t last = 0;

    while (cur != 0) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        uint8 buf[EYNFS_BLOCK_SIZE];
        if (eynfs_cache_get_block(drive, cur, buf) != 0) return -1;

        uint32_t next = *(uint32_t*)buf;
        eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)(buf + 4);

        int free_idx = -1;
        for (size_t i = 0; i < entries_per_block; ++i) {
            // Empty slot
            if (entries[i].name[0] == '\0') {
                if (free_idx < 0) free_idx = (int)i;
                continue;
            }
            // Existing entry
            if (strncmp(entries[i].name, name, EYNFS_NAME_MAX) == 0) {
                return -1; // already exists
            }
        }

        if (free_idx >= 0) {
            int new_block = eynfs_alloc_block(drive, sb);
            if (new_block < 0) return -1;

            memset(&entries[free_idx], 0, sizeof(eynfs_dir_entry_t));
            strncpy(entries[free_idx].name, name, EYNFS_NAME_MAX - 1);
            entries[free_idx].name[EYNFS_NAME_MAX - 1] = '\0';
            entries[free_idx].type = type;
            entries[free_idx].first_block = (uint32_t)new_block;
            entries[free_idx].size = 0;

            if (type == EYNFS_TYPE_DIR) {
                uint8 zero_block[EYNFS_BLOCK_SIZE] = {0};
                if (ata_write_sector(drive, eynfs_block_to_lba((uint32_t)new_block), zero_block) != 0) {
                    eynfs_free_block(drive, sb, (uint32_t)new_block);
                    return -1;
                }
            }

            if (eynfs_cache_write_block(drive, cur, buf) != 0) {
                eynfs_free_block(drive, sb, (uint32_t)new_block);
                return -1;
            }

            // Directory contents and path traversal results are cached.
            eynfs_dir_cache_invalidate(parent_block);
            eynfs_cache_flush(drive);
            return 0;
        }

        last = cur;
        cur = next;
    }

    // No free slots: append a new directory-table block to the chain.
    if (last == 0) return -1;

    int new_dir_block = eynfs_alloc_block(drive, sb);
    if (new_dir_block < 0) return -1;

    // Allocate the data/child block for the new entry.
    int new_entry_block = eynfs_alloc_block(drive, sb);
    if (new_entry_block < 0) {
        eynfs_free_block(drive, sb, (uint32_t)new_dir_block);
        return -1;
    }

    // Link the last directory-table block to the new directory-table block.
    {
        uint8 last_buf[EYNFS_BLOCK_SIZE];
        if (eynfs_cache_get_block(drive, last, last_buf) != 0) {
            eynfs_free_block(drive, sb, (uint32_t)new_entry_block);
            eynfs_free_block(drive, sb, (uint32_t)new_dir_block);
            return -1;
        }
        *(uint32_t*)last_buf = (uint32_t)new_dir_block;
        if (eynfs_cache_write_block(drive, last, last_buf) != 0) {
            eynfs_free_block(drive, sb, (uint32_t)new_entry_block);
            eynfs_free_block(drive, sb, (uint32_t)new_dir_block);
            return -1;
        }
    }

    // Initialize the new directory-table block and install the entry at index 0.
    {
        uint8 new_buf[EYNFS_BLOCK_SIZE];
        memset(new_buf, 0, sizeof(new_buf));
        *(uint32_t*)new_buf = 0; // next = 0
        eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)(new_buf + 4);
        memset(&entries[0], 0, sizeof(eynfs_dir_entry_t));
        strncpy(entries[0].name, name, EYNFS_NAME_MAX - 1);
        entries[0].name[EYNFS_NAME_MAX - 1] = '\0';
        entries[0].type = type;
        entries[0].first_block = (uint32_t)new_entry_block;
        entries[0].size = 0;

        if (type == EYNFS_TYPE_DIR) {
            uint8 zero_block[EYNFS_BLOCK_SIZE] = {0};
            if (ata_write_sector(drive, eynfs_block_to_lba((uint32_t)new_entry_block), zero_block) != 0) {
                // Best-effort cleanup: unlinking is complex, just free blocks.
                eynfs_free_block(drive, sb, (uint32_t)new_entry_block);
                eynfs_free_block(drive, sb, (uint32_t)new_dir_block);
                return -1;
            }
        }

        // Ensure any stale cached version of this block can't be served.
        eynfs_cache_invalidate_block((uint32_t)new_dir_block);
        if (eynfs_cache_write_block(drive, (uint32_t)new_dir_block, new_buf) != 0) {
            eynfs_free_block(drive, sb, (uint32_t)new_entry_block);
            eynfs_free_block(drive, sb, (uint32_t)new_dir_block);
            return -1;
        }
    }

    eynfs_dir_cache_invalidate(parent_block);
    eynfs_cache_flush(drive);
    return 0;
}

// Delete a file or directory entry by name from the given parent directory
int eynfs_delete_entry(uint8 drive, eynfs_superblock_t *sb, uint32_t parent_block, const char *name) {
    if (!name || !name[0]) return -1;
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_DIR, (uint32)sizeof(eynfs_dir_entry_t));
        fs_txn_touch(drive, FS_TXN_TAG_FILE, 0u);
    }
    
    // Use stack-local buffer to avoid heap allocations
    enum { DELETE_MAX_ENTRIES = 64 };
    eynfs_dir_entry_t entries[DELETE_MAX_ENTRIES];
    memset(entries, 0, sizeof(entries));
    
    int entry_count = eynfs_count_dir_entries(drive, parent_block);
    if (entry_count < 0) return -1;
    if (entry_count > DELETE_MAX_ENTRIES) entry_count = DELETE_MAX_ENTRIES;
    
    int count = eynfs_read_dir_table(drive, parent_block, entries, entry_count);
    if (count < 0) return -1;
    
    for (int i = 0; i < count; ++i) {
        if (entries[i].name[0] == '\0') continue;
        if (strncmp(entries[i].name, name, EYNFS_NAME_MAX) == 0) {
            // Free all blocks in the chain
            uint32_t block_num = entries[i].first_block;
            while (block_num != 0) {
                if (current_command_context) {
                    scheduler_account(current_command_context->wo, SCHED_COST_FS);
                    scheduler_yield_if_needed(current_command_context->wo);
                    if (sched_det_is_enabled()) current_command_context->det_seq++;
                }
                uint8 tmp[EYNFS_BLOCK_SIZE];
                if (ata_read_sector(drive, eynfs_block_to_lba(block_num), tmp) != 0) break;
                uint32_t next_block = *(uint32_t*)tmp;
                eynfs_free_block(drive, sb, block_num);
                block_num = next_block;
            }
            
            // Clear the entry
            memset(&entries[i], 0, sizeof(eynfs_dir_entry_t));
            
            int res = eynfs_write_dir_table(drive, parent_block, entries, count);
            if (res < 0) return -1;
            
            // Clear cache to ensure deleted entries are no longer visible
            eynfs_cache_clear();
            
            return 0;
        }
    }
    return -1; // Entry not found
}

// Read up to bufsize bytes from a file's data block chain, starting at offset
// Returns number of bytes read, or -1 on error
int eynfs_read_file(uint8 drive, const eynfs_superblock_t *sb, const eynfs_dir_entry_t *entry, void *buf, size_t bufsize, size_t offset) {
    if (!eynfs_ctx_check(CAP_READ_FS, SCHED_COST_FS)) return -1;
    if (!entry || entry->type != EYNFS_TYPE_FILE) return -1;
    if (offset >= entry->size) return 0;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_FILE, (uint32)bufsize);
    }
    uint32_t block_num = entry->first_block;
    size_t bytes_left = entry->size - offset;
    if (bufsize < bytes_left) bytes_left = bufsize;
    size_t total_read = 0;
    uint8 block[EYNFS_BLOCK_SIZE];
    size_t skip = offset;
    // Skip blocks and bytes up to offset
    uint32_t kick_ctr = 0;
    while (block_num && skip >= (EYNFS_BLOCK_SIZE-4)) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        if (((kick_ctr++) & 0x3u) == 0) watchdog_kick("eynfs-read");
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        block_num = next_block;
        skip -= (EYNFS_BLOCK_SIZE-4);
    }
    // Now at the block containing the offset
    if (block_num && bytes_left > 0) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        if (((kick_ctr++) & 0x3u) == 0) watchdog_kick("eynfs-read");
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        size_t block_offset = skip;
        size_t chunk = (EYNFS_BLOCK_SIZE-4) - block_offset;
        if (chunk > bytes_left) chunk = bytes_left;
        memcpy((uint8*)buf, block+4+block_offset, chunk);
        total_read += chunk;
        bytes_left -= chunk;
        block_num = next_block;
    }
    // Read remaining blocks
    while (block_num && bytes_left > 0) {
        if (current_command_context) {
            scheduler_account(current_command_context->wo, SCHED_COST_FS);
            scheduler_yield_if_needed(current_command_context->wo);
            if (sched_det_is_enabled()) current_command_context->det_seq++;
        }
        if (((kick_ctr++) & 0x3u) == 0) watchdog_kick("eynfs-read");
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        size_t chunk = (EYNFS_BLOCK_SIZE-4) < bytes_left ? (EYNFS_BLOCK_SIZE-4) : bytes_left;
        memcpy((uint8*)buf + total_read, block+4, chunk);
        total_read += chunk;
        bytes_left -= chunk;
        block_num = next_block;
    }
    return (int)total_read;
}

// Write data to a file, creating a chain of blocks as needed
// Returns number of bytes written, or -1 on error
int eynfs_write_file(uint8 drive, eynfs_superblock_t *sb, eynfs_dir_entry_t *entry, const void *buf, size_t size, uint32_t parent_block, uint32_t entry_index) {
    if (!entry || entry->type != EYNFS_TYPE_FILE) return -1;
    if (!buf && size > 0) return -1;
    if (!eynfs_ctx_check(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (current_command_context) {
        fs_txn_touch(drive, FS_TXN_TAG_FILE, (uint32)size);
    }
    
    // Free existing blocks if this is a rewrite
    if (entry->first_block != 0) {
        uint32_t block_num = entry->first_block;
        while (block_num != 0) {
            uint8 tmp[EYNFS_BLOCK_SIZE];
            if (ata_read_sector(drive, eynfs_block_to_lba(block_num), tmp) != 0) break;
            uint32_t next_block = *(uint32_t*)tmp;
            // Ensure any cached data for this block can't be served after it's freed/reused.
            eynfs_cache_invalidate_block(block_num);
            eynfs_free_block(drive, sb, block_num);
            block_num = next_block;
        }
    }
    
    uint32_t prev_block = 0;
    uint32_t first_block = 0;
    size_t bytes_left = size;
    size_t total_written = 0;
    const uint8* data = (const uint8*)buf;
    
    while (bytes_left > 0) {
        int new_block = eynfs_alloc_block(drive, sb);
        if (new_block < 0) return -1;

        // This block may have been previously cached under the same number.
        eynfs_cache_invalidate_block((uint32_t)new_block);
        
        if (!first_block) first_block = new_block;
        
        if (prev_block) {
            // Update previous block's next_block pointer
            uint8 tmp[EYNFS_BLOCK_SIZE];
            if (ata_read_sector(drive, eynfs_block_to_lba(prev_block), tmp) != 0) return -1;
            *(uint32_t*)tmp = new_block;
            if (eynfs_cache_write_block(drive, prev_block, tmp) != 0) return -1;
        }
        
        uint8 block[EYNFS_BLOCK_SIZE] = {0};
        size_t chunk = bytes_left < (EYNFS_BLOCK_SIZE-4) ? bytes_left : (EYNFS_BLOCK_SIZE-4);
        *(uint32_t*)block = 0; // next_block = 0 for now
        
        // Bounds check for memory copy
        if (chunk <= (EYNFS_BLOCK_SIZE-4) && (data+total_written) && (block+4)) {
            memcpy(block+4, data+total_written, chunk);
        } else {
            printf("%cWarning: Memory copy bounds check failed in write_file\n", 255, 165, 0);
            return -1;
        }
        
        if (eynfs_cache_write_block(drive, (uint32_t)new_block, block) != 0) return -1;
        total_written += chunk;
        bytes_left -= chunk;
        prev_block = new_block;
    }
    
    // Update entry with new first block and size
    entry->first_block = first_block;
    entry->size = size;
    
    // Update directory table - count entries first, then allocate exactly what we need
    int entry_count = eynfs_count_dir_entries(drive, parent_block);
    if (entry_count < 0) {
        printf("Error: Failed to count directory entries\n");
        return -1;
    }
    
    size_t allocation_size = sizeof(eynfs_dir_entry_t) * entry_count;
    
    // Safety check: limit allocation to prevent memory exhaustion
    if (allocation_size > 4096) { // 4KB limit for directory operations
        printf("%cWarning: Directory allocation too large (%d bytes), limiting to 4KB\n", 255, 165, 0, allocation_size);
        entry_count = 4096 / sizeof(eynfs_dir_entry_t);
        allocation_size = 4096;
    }
    
    eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(allocation_size);
    if (!entries) {
        printf("Error: Out of memory for directory update\n");
        return -1;
    }
    
    int count = eynfs_read_dir_table(drive, parent_block, entries, entry_count);
    if (count < 0) {
        free(entries);
        return -1;
    }
    
    // Validate entry_index
    if (entry_index >= (uint32_t)count) {
        printf("Warning: entry_index %d >= count %d, truncating\n", entry_index, count);
        entry_index = count - 1;
    }
    
    // Update the entry
    entries[entry_index] = *entry;
    
    // Write back the directory table with error checking
    int write_result = eynfs_write_dir_table(drive, parent_block, entries, count);
    free(entries);
    if (write_result < 0) {
        printf("Error: Failed to write directory table\n");
        return -1;
    }

    // Directory contents and path traversal results are cached; invalidate so metadata updates are visible immediately.
    eynfs_dir_cache_invalidate(parent_block);
    
    return (int)size;
} 

// Unix-like File Table and Open/Close Implementation ---
#define EYNFS_MAX_OPEN_FILES 32

typedef struct {
    int used;
    uint8 drive;
    eynfs_superblock_t sb;
    eynfs_dir_entry_t entry;
    uint32_t offset;
    int mode; // 0 = read, 1 = write, 2 = append
    uint32_t parent_block;
    uint32_t entry_index;
} eynfs_file_t;

static eynfs_file_t eynfs_files[EYNFS_MAX_OPEN_FILES];

// EYNFS-specific file operations with full path support
int eynfs_open(const char* path, int mode) {
    if (!path) return -1;
    
    // Find free file descriptor
    int fd = -1;
    for (int i = 0; i < EYNFS_MAX_OPEN_FILES; i++) {
        if (!eynfs_files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;
    
    // Initialize file descriptor
    eynfs_files[fd].used = 1;
    const char* resolved_path = path;
    uint8 drive = eynfs_default_drive();
    (void)eynfs_parse_drive_prefix(&resolved_path, &drive);
    eynfs_files[fd].drive = drive;
    eynfs_files[fd].offset = 0;
    eynfs_files[fd].mode = mode;

    uint8_t disk = drive;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &eynfs_files[fd].sb) != 0 || 
        eynfs_files[fd].sb.magic != EYNFS_MAGIC) {
        eynfs_files[fd].used = 0;
        return -1;
    }
    
    // Handle root directory specially
    if (strcmp(resolved_path, "/") == 0) {
        if (mode != 0) return -1; // Root can only be read
        memset(&eynfs_files[fd].entry, 0, sizeof(eynfs_dir_entry_t));
        eynfs_files[fd].entry.type = EYNFS_TYPE_DIR;
        eynfs_files[fd].entry.first_block = eynfs_files[fd].sb.root_dir_block;
        eynfs_files[fd].parent_block = 0;
        eynfs_files[fd].entry_index = 0;
        return fd;
    }
    
    // Traverse path to find file/directory
    uint32_t parent_block, entry_idx;
    if (eynfs_traverse_path(disk, &eynfs_files[fd].sb, resolved_path, &eynfs_files[fd].entry, &parent_block, &entry_idx) != 0) {
        // File doesn't exist
        if (mode == 1 || mode == 2) { // Write or append mode
            // Create the file
            const char* filename = strrchr(resolved_path, '/');
            if (!filename) filename = resolved_path;
            else filename++; // Skip the '/'
            
            // Get parent directory path
            char parent_path[256];
            strncpy(parent_path, resolved_path, sizeof(parent_path)-1);
            parent_path[sizeof(parent_path)-1] = '\0';
            char* last_slash = strrchr(parent_path, '/');
            if (last_slash && last_slash != parent_path) {
                *last_slash = '\0';
            } else {
                strcpy(parent_path, "/");
            }
            
            // Get parent directory
            eynfs_dir_entry_t parent_entry;
            if (eynfs_traverse_path(disk, &eynfs_files[fd].sb, parent_path, &parent_entry, &parent_block, NULL) != 0) {
                eynfs_files[fd].used = 0;
                return -1;
            }
            
            // Create the file
            if (eynfs_create_entry(disk, &eynfs_files[fd].sb, parent_entry.first_block, filename, EYNFS_TYPE_FILE) != 0) {
                eynfs_files[fd].used = 0;
                return -1;
            }
            
            // Find the created entry
            if (eynfs_find_in_dir(disk, &eynfs_files[fd].sb, parent_entry.first_block, filename, 
                                 &eynfs_files[fd].entry, &entry_idx) != 0) {
                eynfs_files[fd].used = 0;
                return -1;
            }
            
            eynfs_files[fd].parent_block = parent_entry.first_block;
            eynfs_files[fd].entry_index = entry_idx;
            
            if (mode == 2) { // Append mode
                eynfs_files[fd].offset = eynfs_files[fd].entry.size;
            }
        } else {
            eynfs_files[fd].used = 0;
            return -1;
        }
    } else {
        // File exists
        if (mode == 1) { // Write mode - truncate file
            eynfs_files[fd].entry.size = 0;
            eynfs_files[fd].entry.first_block = 0;
        } else if (mode == 2) { // Append mode
            eynfs_files[fd].offset = eynfs_files[fd].entry.size;
        }
        eynfs_files[fd].parent_block = parent_block;
        eynfs_files[fd].entry_index = entry_idx;
    }
    
    return fd;
}

// Legacy open function for compatibility
int open(const char* path, int mode) {
    return eynfs_open(path, mode);
}

// Close a file descriptor
int close(int fd) {
    if (fd < 0 || fd >= EYNFS_MAX_OPEN_FILES || !eynfs_files[fd].used)
        return -1;
    eynfs_files[fd].used = 0;
    return 0;
} 

// Read from a file descriptor
int read(int fd, void* buf, int size) {
    if (fd < 0 || fd >= EYNFS_MAX_OPEN_FILES || !eynfs_files[fd].used)
        return -1;
    eynfs_file_t* f = &eynfs_files[fd];
    
    // Check if it's a directory
    if (f->entry.type == EYNFS_TYPE_DIR) {
        // For directories, return directory listing
        if (f->offset == 0) {
            // Read directory entries - use heap allocation (128*84=10752 bytes would overflow stack)
            const int max_entries = 48; // Limit to ~4KB heap allocation
            eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(max_entries * sizeof(eynfs_dir_entry_t));
            if (!entries) return -1;
            int count = eynfs_read_dir_table(f->drive, f->entry.first_block, entries, max_entries);
            if (count < 0) { free(entries); return -1; }
            
            // Format as text listing
            char* text_buf = (char*)buf;
            int written = 0;
            for (int i = 0; i < count && written < size - 100; i++) {
                if (entries[i].name[0] != '\0') {
                    int len = snprintf(text_buf + written, size - written, 
                                     "%s%s\n", entries[i].name,
                                     entries[i].type == EYNFS_TYPE_DIR ? "/" : "");
                    if (len > 0) written += len;
                }
            }
            free(entries);
            f->offset = 1; // Mark as read
            return written;
        }
        return 0; // Directory already read
    }
    
    // Regular file read
    if (f->offset >= f->entry.size) return 0;
    int to_read = size;
    if (f->offset + to_read > f->entry.size)
        to_read = f->entry.size - f->offset;
    int n = eynfs_read_file(f->drive, &f->sb, &f->entry, buf, to_read, f->offset);
    if (n > 0) f->offset += n;
    return n;
}

// Write to a file descriptor
int write(int fd, const void* buf, int size) {
    if (fd < 0 || fd >= EYNFS_MAX_OPEN_FILES || !eynfs_files[fd].used)
        return -1;
    eynfs_file_t* f = &eynfs_files[fd];
    if (f->mode != 1 && f->mode != 2)
        return -1;
    
    // Can't write to directories
    if (f->entry.type == EYNFS_TYPE_DIR) return -1;
    
    // For append mode, we need to read existing data first
    if (f->mode == 2 && f->offset > 0) {
        // Read existing data
        uint8_t* existing_data = (uint8_t*)malloc(f->entry.size);
        if (!existing_data) return -1;
        
        int existing_read = eynfs_read_file(f->drive, &f->sb, &f->entry, existing_data, f->entry.size, 0);
        if (existing_read < 0) {
            free(existing_data);
            return -1;
        }
        
        // Combine existing data with new data
        uint8_t* combined_data = (uint8_t*)malloc(existing_read + size);
        if (!combined_data) {
            free(existing_data);
            return -1;
        }
        
        memcpy(combined_data, existing_data, existing_read);
        memcpy(combined_data + existing_read, buf, size);
        
        // Write combined data
        int n = eynfs_write_file(f->drive, &f->sb, &f->entry, combined_data, existing_read + size, 
                                f->parent_block, f->entry_index);
        
        free(existing_data);
        free(combined_data);
        
        if (n > 0) {
            f->offset = n;
            f->entry.size = n;
        }
        return n;
    } else {
        // Regular write (overwrite or new file)
        int n = eynfs_write_file(f->drive, &f->sb, &f->entry, buf, size, f->parent_block, f->entry_index);
        if (n > 0) {
            f->offset = n;
            f->entry.size = n;
        }
        return n;
    }
} 

// Performance monitoring functions
void eynfs_get_cache_stats(uint32_t* hits, uint32_t* misses) {
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
}

void eynfs_reset_cache_stats() {
    cache_hits = 0;
    cache_misses = 0;
}

void eynfs_cache_clear() {
    eynfs_cache_flush(0); // Flush all dirty blocks
    for (int i = 0; i < EYNFS_CACHE_SIZE; i++) {
        block_cache[i].valid = 0;
        block_cache[i].dirty = 0;
    }
    
    // Clear directory cache
    for (int i = 0; i < EYNFS_DIR_CACHE_SIZE; i++) {
        if (dir_cache[i].entries) {
            free(dir_cache[i].entries);
            dir_cache[i].entries = NULL;
        }
        dir_cache[i].count = 0;
        dir_cache[i].sorted = 0;
    }
    
    // Clear free block cache
    free_block_cache_valid = 0;
    free_block_cache_count = 0;
}

// Enhanced block allocation with performance tracking
int eynfs_alloc_block_fast(uint8 drive, eynfs_superblock_t *sb) {
    return eynfs_alloc_block_optimized(drive, sb);
} 

// Streaming writer implementation ---
#ifndef EYNFS_STREAM_DEBUG
#define EYNFS_STREAM_DEBUG 0
#endif

int eynfs_stream_begin(uint8 drive, const char* path, eynfs_stream_t* s) {
    if (!s || !path) return -1;
    memset(s, 0, sizeof(*s));
    s->drive = drive;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &s->sb) != 0) {
        if (EYNFS_STREAM_DEBUG) printf("[EYNFS:stream_begin] read_superblock failed\n");
        return -1;
    }
    // Determine parent dir and file name
    char parent_path[256];
    const char* filename = strrchr(path, '/');
    if (filename) {
        size_t plen = filename - path;
        if (plen >= sizeof(parent_path)) {
            if (EYNFS_STREAM_DEBUG) printf("[EYNFS:stream_begin] parent path too long\n");
            return -1;
        }
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        filename++;
        if (parent_path[0] == '\0') strcpy(parent_path, "/");
    } else {
        strcpy(parent_path, "/");
        filename = path;
    }
    eynfs_dir_entry_t parent_entry;
    uint32_t parent_block, entry_idx;
    if (eynfs_traverse_path(drive, &s->sb, parent_path, &parent_entry, &parent_block, NULL) != 0) {
        if (EYNFS_STREAM_DEBUG) printf("[EYNFS:stream_begin] traverse parent failed: '%s'\n", parent_path);
        return -1;
    }
    if (parent_entry.type != EYNFS_TYPE_DIR) {
        if (EYNFS_STREAM_DEBUG) printf("[EYNFS:stream_begin] parent not dir: '%s'\n", parent_path);
        return -1;
    }
    // If file exists, delete it to simplify streaming overwrite
    eynfs_dir_entry_t tmp;
    if (eynfs_find_in_dir(drive, &s->sb, parent_entry.first_block, filename, &tmp, &entry_idx) == 0) {
        int delrc = eynfs_delete_entry(drive, &s->sb, parent_entry.first_block, filename);
        if (EYNFS_STREAM_DEBUG) {
            printf("[EYNFS:stream_begin] overwriting '%s/%s' delrc=%d\n", parent_path, filename, delrc);
        }
    }
    int cr = eynfs_create_entry(drive, &s->sb, parent_entry.first_block, filename, EYNFS_TYPE_FILE);
    if (cr != 0) {
        if (EYNFS_STREAM_DEBUG) {
            printf("[EYNFS:stream_begin] create_entry failed parent='%s' name='%s' rc=%d\n", parent_path, filename, cr);
        }
        return -1;
    }
    if (eynfs_find_in_dir(drive, &s->sb, parent_entry.first_block, filename, &s->entry, &s->entry_index) != 0) {
        if (EYNFS_STREAM_DEBUG) {
            printf("[EYNFS:stream_begin] find-after-create failed parent='%s' name='%s'\n", parent_path, filename);
        }
        return -1;
    }
    s->parent_block = parent_entry.first_block;
    // `eynfs_create_entry()` allocates a data block for files. Reuse it as the
    // first streaming block to avoid leaking an unreferenced block.
    s->curr_block = s->entry.first_block;
    s->first_block = s->entry.first_block;
    s->size = 0;
    s->pos_in_block = 0;

    // Initialize the first block: next=0 and data cleared.
    if (s->curr_block != 0) {
        uint8 block[EYNFS_BLOCK_SIZE] = {0};
        *(uint32_t*)block = 0;
        eynfs_cache_invalidate_block(s->curr_block);
        if (eynfs_cache_write_block(s->drive, s->curr_block, block) != 0) return -1;
    }
    return 0;
}

int eynfs_stream_write(eynfs_stream_t* s, const void* buf, size_t size) {
    if (!s || !buf || size==0) return 0;
    const uint8* p = (const uint8*)buf;
    size_t remaining = size;
    size_t total_written = 0;
    while (remaining > 0) {
        // Ensure we have a current block with space
        if (s->curr_block == 0 || s->pos_in_block >= (EYNFS_BLOCK_SIZE - 4)) {
            int new_block = eynfs_alloc_block(s->drive, &s->sb);
            if (new_block < 0) return -1;
            eynfs_cache_invalidate_block((uint32_t)new_block);
            // Initialize the new block's link to 0 and clear data area
            uint8 block[EYNFS_BLOCK_SIZE] = {0};
            *(uint32_t*)block = 0;
            if (eynfs_cache_write_block(s->drive, (uint32_t)new_block, block) != 0) return -1;
            // Link from previous current block if it exists
            if (s->curr_block != 0) {
                uint8 prev[EYNFS_BLOCK_SIZE];
                if (eynfs_cache_get_block(s->drive, s->curr_block, prev) != 0) return -1;
                *(uint32_t*)prev = (uint32_t)new_block; // store block number, not LBA
                eynfs_cache_invalidate_block(s->curr_block);
                if (eynfs_cache_write_block(s->drive, s->curr_block, prev) != 0) return -1;
            }
            s->curr_block = (uint32_t)new_block;
            if (s->first_block == 0) s->first_block = s->curr_block;
            s->pos_in_block = 0;
        }
        // Write into current block at current position
        uint8 block[EYNFS_BLOCK_SIZE];
        if (eynfs_cache_get_block(s->drive, s->curr_block, block) != 0) return -1;
        size_t space = (EYNFS_BLOCK_SIZE - 4) - s->pos_in_block;
        size_t chunk = remaining < space ? remaining : space;
        memcpy(block + 4 + s->pos_in_block, p + total_written, chunk);
        eynfs_cache_invalidate_block(s->curr_block);
        if (eynfs_cache_write_block(s->drive, s->curr_block, block) != 0) return -1;
        s->pos_in_block += (uint16_t)chunk;
        s->size += chunk;
        total_written += chunk;
        remaining -= chunk;
    }
    return (int)total_written;
}

int eynfs_stream_end(eynfs_stream_t* s) {
    if (!s) return -1;
    // Update entry size/first_block.
    // NOTE: The directory table can span multiple blocks; do not read it into a
    // fixed 4KB buffer (that can miss the entry in large directories).
    s->entry.first_block = s->first_block;
    s->entry.size = s->size;

    const uint32_t entries_per_block = (uint32_t)((EYNFS_BLOCK_SIZE - 4) / sizeof(eynfs_dir_entry_t));
    const uint32_t block_steps = s->entry_index / entries_per_block;
    const uint32_t slot = s->entry_index % entries_per_block;

    uint32_t cur = s->parent_block;
    const uint32_t max_blocks = 4096; // safety bound
    for (uint32_t i = 0; i < block_steps; ++i) {
        if (cur == 0 || i >= max_blocks) return -1;
        uint8 buf[EYNFS_BLOCK_SIZE];
        if (eynfs_cache_get_block(s->drive, cur, buf) != 0) return -1;
        cur = *(uint32_t*)buf;
    }
    if (cur == 0) return -1;

    uint8 dirblk[EYNFS_BLOCK_SIZE];
    if (eynfs_cache_get_block(s->drive, cur, dirblk) != 0) return -1;
    eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)(dirblk + 4);
    entries[slot] = s->entry;

    eynfs_cache_invalidate_block(cur);
    if (eynfs_cache_write_block(s->drive, cur, dirblk) != 0) return -1;
    eynfs_dir_cache_invalidate(s->parent_block);
    return 0;
}