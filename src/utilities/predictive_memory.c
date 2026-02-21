#include <predictive_memory.h>
#include <util.h>
#include <vga.h>
#include <string.h>
#include <eynfs.h>
#include <stdint.h>
#include <context.h>
#include <misc/sched.h>

// Global predictive memory state
static predictive_allocator_t g_predictive_allocator;
static memory_prediction_t g_memory_pattern;
static file_mapping_t g_file_mappings[32];  // Support up to 32 mapped files
static uint8_t g_mapping_count = 0;
static uint32_t g_total_access_count = 0;
static uint32_t g_prediction_timer = 0;

// Size-based hash table for O(1) block lookups
static uint8_t g_size_hash_table[64]; // Maps common sizes to block indices
static int g_hash_table_initialized = 0;

static int pred_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void pred_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Initialize predictive memory management system
void predictive_memory_init(void) {
    // Initialize predictive allocator
    memset(&g_predictive_allocator, 0, sizeof(predictive_allocator_t));
    memset(&g_memory_pattern, 0, sizeof(memory_prediction_t));
    memset(g_file_mappings, 0, sizeof(g_file_mappings));
    
    g_mapping_count = 0;
    g_total_access_count = 0;
    g_prediction_timer = 0;
    
    // Initialize size hash table
    for (int i = 0; i < 64; i++) {
        g_size_hash_table[i] = 0xFF; // Invalid index
    }
    g_hash_table_initialized = 1;
    
    // Do NOT pre-allocate blocks at boot to avoid false-positive leak stats.
    // Blocks will be allocated lazily on first use and freed when returned.
    for (int i = 0; i < 16; i++) {
        g_predictive_allocator.predicted_blocks[i] = NULL;
        g_predictive_allocator.block_sizes[i] = 0;
        g_predictive_allocator.block_used[i] = 0;
    }
}

// Helper function to get size hash index
static uint8_t get_size_hash_index(uint32_t size) {
    // Simple hash function for common allocation sizes
    if (size <= 16) return 0;
    if (size <= 32) return 1;
    if (size <= 64) return 2;
    if (size <= 128) return 3;
    if (size <= 256) return 4;
    if (size <= 512) return 5;
    if (size <= 1024) return 6;
    if (size <= 2048) return 7;
    if (size <= 4096) return 8;
    return (size >> 8) % 55 + 9; // For larger sizes, use upper bits
}

// Helper function to update size hash table
static void update_size_hash_table(uint32_t size, uint8_t block_index) {
    if (!g_hash_table_initialized) return;
    
    uint8_t hash_index = get_size_hash_index(size);
    if (hash_index < 64) {
        g_size_hash_table[hash_index] = block_index;
    }
}

// Predictive memory allocation with pattern analysis and O(1) lookup optimization
void* predictive_malloc(size_t size, memory_prediction_t* pattern) {
    if (!pred_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
    // First, try O(1) lookup using size hash table
    if (g_hash_table_initialized) {
        uint8_t hash_index = get_size_hash_index(size);
        if (hash_index < 64 && g_size_hash_table[hash_index] != 0xFF) {
            uint8_t block_idx = g_size_hash_table[hash_index];
            if (block_idx < 16 && 
                g_predictive_allocator.predicted_blocks[block_idx] &&
                !g_predictive_allocator.block_used[block_idx] &&
                g_predictive_allocator.block_sizes[block_idx] >= size) {
                
                g_predictive_allocator.block_used[block_idx] = 1;
                g_predictive_allocator.prediction_hits++;
                update_access_pattern((uint32_t)g_predictive_allocator.predicted_blocks[block_idx], (uint32_t)size);
                return g_predictive_allocator.predicted_blocks[block_idx];
            }
        }
    }
    
    // Fallback to linear search if hash lookup fails
    for (int i = 0; i < 16; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (g_predictive_allocator.predicted_blocks[i] &&
            !g_predictive_allocator.block_used[i] &&
            g_predictive_allocator.block_sizes[i] >= size) {
            g_predictive_allocator.block_used[i] = 1;
            g_predictive_allocator.prediction_hits++;
            update_access_pattern((uint32_t)g_predictive_allocator.predicted_blocks[i], (uint32_t)size);
            return g_predictive_allocator.predicted_blocks[i];
        }
    }
    
    // No cached block available; allocate a new one
    void* ptr = malloc((int)size);
    if (ptr) {
        // Remember this allocation for potential reuse later
        for (int i = 0; i < 16; i++) {
            if (g_predictive_allocator.predicted_blocks[i] == NULL) {
                g_predictive_allocator.predicted_blocks[i] = ptr;
                g_predictive_allocator.block_sizes[i] = (uint32_t)size;
                g_predictive_allocator.block_used[i] = 1;
                
                // Update hash table for O(1) future lookups
                update_size_hash_table(size, i);
                break;
            }
        }
        g_predictive_allocator.prediction_misses++;
        update_access_pattern((uint32_t)ptr, (uint32_t)size);
    }
    
    return ptr;
}

// Predictive memory deallocation
void predictive_free(void* ptr) {
    if (!ptr) return;
    
    // If this was tracked as a predictive block, mark free and free memory when idle
    for (int i = 0; i < 16; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (g_predictive_allocator.predicted_blocks[i] == ptr) {
            g_predictive_allocator.block_used[i] = 0;
            
            // Clear hash table entry for this block
            if (g_hash_table_initialized) {
                uint32_t size = g_predictive_allocator.block_sizes[i];
                uint8_t hash_index = get_size_hash_index(size);
                if (hash_index < 64 && g_size_hash_table[hash_index] == i) {
                    g_size_hash_table[hash_index] = 0xFF; // Mark as invalid
                }
            }
            
            // Free immediately to keep allocation/free counts balanced and avoid leak warnings
            free(ptr);
            g_predictive_allocator.predicted_blocks[i] = NULL;
            g_predictive_allocator.block_sizes[i] = 0;
            return;
        }
    }
    
    // Fallback: regular free
    free(ptr);
}

// Update memory access patterns
void update_access_pattern(uint32_t address, uint32_t size) {
    g_total_access_count++;
    g_prediction_timer++;
    
    // Update access frequency
    uint32_t index = (address >> 8) % 1024;  // Use upper bits for indexing
    g_memory_pattern.access_frequency[index]++;
    
    // Update allocation size patterns
    for (int i = 0; i < 63; i++) {
        if ((i & 0xF) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        g_memory_pattern.allocation_sizes[i] = g_memory_pattern.allocation_sizes[i + 1];
    }
    g_memory_pattern.allocation_sizes[63] = size;
    
    // Update access pattern history
    for (int i = 0; i < PATTERN_HISTORY_SIZE - 1; i++) {
        if ((i & 0x3F) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        g_memory_pattern.access_pattern[i] = g_memory_pattern.access_pattern[i + 1];
    }
    g_memory_pattern.access_pattern[PATTERN_HISTORY_SIZE - 1] = address;
    
    g_memory_pattern.access_count++;
    g_memory_pattern.last_access_time = g_prediction_timer;
}

// Calculate prediction score based on access patterns
float calculate_prediction_score(memory_prediction_t* pattern) {
    if (!pattern || pattern->access_count < 10) return 0.0f;
    
    // Analyze recent allocation sizes
    uint32_t common_size = 0;
    uint32_t size_count = 0;
    for (int i = 0; i < 64; i++) {
        if ((i & 0xF) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (pattern->allocation_sizes[i] > 0) {
            common_size = pattern->allocation_sizes[i];
            size_count++;
        }
    }
    
    // Calculate frequency of similar allocations
    float frequency_score = (float)size_count / 64.0f;
    
    // Calculate temporal locality (recent access patterns)
    uint32_t recent_accesses = 0;
    for (int i = PATTERN_HISTORY_SIZE - 16; i < PATTERN_HISTORY_SIZE; i++) {
        if (pattern->access_pattern[i] > 0) recent_accesses++;
    }
    float temporal_score = (float)recent_accesses / 16.0f;
    
    // Combine scores
    pattern->prediction_score = (frequency_score + temporal_score) / 2.0f;
    return pattern->prediction_score;
}

// Prefetch memory blocks based on predictions
void prefetch_memory_blocks(void) {
    float score = calculate_prediction_score(&g_memory_pattern);
    
    if (score > PREFETCH_THRESHOLD) {
        // Analyze common allocation sizes and prepare cache entries (no allocation yet)
        uint32_t common_sizes[4] = {0};
        uint32_t size_counts[4] = {0};
        
        for (int i = 0; i < 64; i++) {
            if ((i & 0xF) == 0) {
                pred_ctx_account(SCHED_COST_ALLOC);
            }
            uint32_t size = g_memory_pattern.allocation_sizes[i];
            if (size > 0) {
                for (int j = 0; j < 4; j++) {
                    if (common_sizes[j] == size) { size_counts[j]++; break; }
                    if (common_sizes[j] == 0) { common_sizes[j] = size; size_counts[j] = 1; break; }
                }
            }
        }
        
        // Mark slots for these sizes; actual allocation will happen on demand
        for (int i = 0; i < 4; i++) {
            if (size_counts[i] > 5) {
                for (int j = 0; j < 16; j++) {
                    if ((j & 0x7) == 0) {
                        pred_ctx_account(SCHED_COST_ALLOC);
                    }
                    if (g_predictive_allocator.predicted_blocks[j] == NULL) {
                        g_predictive_allocator.block_sizes[j] = common_sizes[i];
                        g_predictive_allocator.block_used[j] = 0;
                        break;
                    }
                }
            }
        }
    }
}

// Adaptive memory optimization
void adaptive_memory_optimization(void) {
    // Update hit rate
    uint32_t total_predictions = g_predictive_allocator.prediction_hits + g_predictive_allocator.prediction_misses;
    if (total_predictions > 0) {
        g_predictive_allocator.hit_rate = (float)g_predictive_allocator.prediction_hits / total_predictions;
    }
    
    // If hit rate is low, clear unused cached slots to reduce memory footprint
    if (g_predictive_allocator.hit_rate < 0.3f) {
        for (int i = 0; i < 16; i++) {
            if ((i & 0x7) == 0) {
                pred_ctx_account(SCHED_COST_ALLOC);
            }
            if (g_predictive_allocator.predicted_blocks[i] && !g_predictive_allocator.block_used[i]) {
                free(g_predictive_allocator.predicted_blocks[i]);
                g_predictive_allocator.predicted_blocks[i] = NULL;
                g_predictive_allocator.block_sizes[i] = 0;
            }
        }
    }
    
    // Prepare sizes for future allocations without reserving memory
    prefetch_memory_blocks();
}

// Memory mapping for zero-copy file operations
int eynfs_mmap(const char* path, void** addr, size_t* size, uint8_t read_only) {
    if (!pred_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
    if (g_mapping_count >= 32) {
        printf("%cError: Too many mapped files\n", 255, 0, 0);
        return -1;
    }
    
    // Resolve path relative to current working directory
    extern char shell_current_path[128];
    extern void resolve_path(const char* input, const char* cwd, char* out, size_t outsz);
    char abspath[128];
    resolve_path(path, shell_current_path, abspath, sizeof(abspath));

    // Use current drive
    extern uint8 g_current_drive;
    uint8_t drive = g_current_drive;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, 2048, &sb) != 0) {
        printf("%cError: Cannot read filesystem\n", 255, 0, 0);
        return -1;
    }
    
    eynfs_dir_entry_t entry;
    uint32_t parent_block, entry_index;
    if (eynfs_traverse_path(drive, &sb, abspath, &entry, &parent_block, &entry_index) != 0) {
        printf("%cError: File not found: %s\n", 255, 0, 0, abspath);
        return -1;
    }
    
    if (entry.type != EYNFS_TYPE_FILE) {
        printf("%cError: Not a file: %s\n", 255, 0, 0, abspath);
        return -1;
    }
    
    // Allocate memory for the entire file
    void* mapped_addr = malloc((int)entry.size);
    if (!mapped_addr) {
        printf("%cError: Out of memory for mapping\n", 255, 0, 0);
        return -1;
    }
    
    // Read entire file into memory
    int bytes_read = eynfs_read_file(drive, &sb, &entry, mapped_addr, entry.size, 0);
    if (bytes_read != (int)entry.size) {
        printf("%cError: Failed to read file for mapping\n", 255, 0, 0);
        free(mapped_addr);
        return -1;
    }
    
    // Set up mapping structure
    g_file_mappings[g_mapping_count].mapped_address = mapped_addr;
    g_file_mappings[g_mapping_count].file_size = entry.size;
    g_file_mappings[g_mapping_count].block_start = entry.first_block;
    g_file_mappings[g_mapping_count].block_count = 0;
    g_file_mappings[g_mapping_count].drive = drive;
    g_file_mappings[g_mapping_count].read_only = read_only;
    g_file_mappings[g_mapping_count].access_count = 0;

    strncpy(g_file_mappings[g_mapping_count].path, abspath, sizeof(g_file_mappings[g_mapping_count].path) - 1);
    g_file_mappings[g_mapping_count].path[sizeof(g_file_mappings[g_mapping_count].path) - 1] = '\0';
    g_file_mappings[g_mapping_count].parent_block = parent_block;
    g_file_mappings[g_mapping_count].entry_index = entry_index;
    g_file_mappings[g_mapping_count].entry = entry;
    
    *addr = mapped_addr;
    *size = entry.size;
    
    g_mapping_count++;
    printf("%cFile mapped: %s (%d bytes)\n", 0, 255, 0, abspath, entry.size);
    
    return 0;
}

// Unmap a memory-mapped file
int eynfs_munmap(void* addr, size_t size) {
    if (!pred_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    for (int i = 0; i < g_mapping_count; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (g_file_mappings[i].mapped_address == addr) {
            free(addr);
            
            // Remove from mapping array
            for (int j = i; j < g_mapping_count - 1; j++) {
                g_file_mappings[j] = g_file_mappings[j + 1];
            }
            g_mapping_count--;
            
            printf("%cFile unmapped\n", 0, 255, 0);
            return 0;
        }
    }
    
    printf("%cError: Address not mapped\n", 255, 0, 0);
    return -1;
}

// Synchronize memory-mapped file to disk
int eynfs_msync(void* addr, size_t size) {
    if (!pred_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    for (int i = 0; i < g_mapping_count; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_FS);
        }
        if (g_file_mappings[i].mapped_address == addr) {
            if (g_file_mappings[i].read_only) {
                printf("%cWarning: Cannot sync read-only mapping\n", 255, 165, 0);
                return 0;
            }

            // Write back the full mapping to disk.
            // Note: This is intentionally simple (rewrite) to keep semantics predictable.
            eynfs_superblock_t sb;
            if (eynfs_read_superblock(g_file_mappings[i].drive, 2048, &sb) != 0) {
                printf("%cError: Cannot read filesystem for msync\n", 255, 0, 0);
                return -1;
            }

            if (size > 0 && size != g_file_mappings[i].file_size) {
                // Caller-provided size mismatch; prefer the mapping's authoritative size.
                (void)size;
            }

            // Keep entry size consistent with the mapped size.
            g_file_mappings[i].entry.size = g_file_mappings[i].file_size;

            int n = eynfs_write_file(
                g_file_mappings[i].drive,
                &sb,
                &g_file_mappings[i].entry,
                g_file_mappings[i].mapped_address,
                g_file_mappings[i].file_size,
                g_file_mappings[i].parent_block,
                g_file_mappings[i].entry_index);

            if (n < 0) {
                printf("%cError: Failed to write back mapping\n", 255, 0, 0);
                return -1;
            }

            // Update cached metadata after rewrite.
            g_file_mappings[i].block_start = g_file_mappings[i].entry.first_block;
            printf("%cFile synchronized to disk\n", 0, 255, 0);
            return 0;
        }
    }
    
    printf("%cError: Address not mapped\n", 255, 0, 0);
    return -1;
}

void invalidate_mapped_file(const char* path) {
    if (!path || !path[0]) return;

    // Resolve path relative to current working directory
    extern char shell_current_path[128];
    extern void resolve_path(const char* input, const char* cwd, char* out, size_t outsz);
    char abspath[128];
    resolve_path(path, shell_current_path, abspath, sizeof(abspath));

    for (int i = 0; i < g_mapping_count; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (strcmp(g_file_mappings[i].path, abspath) == 0) {
            (void)eynfs_munmap(g_file_mappings[i].mapped_address, g_file_mappings[i].file_size);
            return;
        }
    }
}

// Read-only memory mapping
void* eynfs_mmap_readonly(const char* path, size_t* size) {
    void* addr;
    size_t file_size;
    
    if (eynfs_mmap(path, &addr, &file_size, 1) == 0) {
        *size = file_size;
        return addr;
    }
    
    return NULL;
}

// Check if address is memory-mapped
int is_memory_mapped(void* addr) {
    for (int i = 0; i < g_mapping_count; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (g_file_mappings[i].mapped_address == addr) {
            return 1;
        }
    }
    return 0;
}

// Get file mapping information
file_mapping_t* get_file_mapping(void* addr) {
    for (int i = 0; i < g_mapping_count; i++) {
        if ((i & 0x7) == 0) {
            pred_ctx_account(SCHED_COST_ALLOC);
        }
        if (g_file_mappings[i].mapped_address == addr) {
            return &g_file_mappings[i];
        }
    }
    return NULL;
}

// Performance monitoring functions
uint32_t get_prediction_hit_rate(void) {
    uint32_t total = g_predictive_allocator.prediction_hits + g_predictive_allocator.prediction_misses;
    if (total == 0) return 0;
    return (g_predictive_allocator.prediction_hits * 100) / total;
}

uint32_t get_memory_access_count(void) {
    return g_total_access_count;
}

void reset_prediction_stats(void) {
    g_predictive_allocator.prediction_hits = 0;
    g_predictive_allocator.prediction_misses = 0;
    g_total_access_count = 0;
}

void print_prediction_stats(void) {
    printf("%c=== Predictive Memory Statistics ===\n", 255, 255, 255);
    printf("%cTotal accesses: %d\n", 255, 255, 255, g_total_access_count);
    printf("%cPrediction hits: %d\n", 255, 255, 255, g_predictive_allocator.prediction_hits);
    printf("%cPrediction misses: %d\n", 255, 255, 255, g_predictive_allocator.prediction_misses);
    printf("%cHit rate: %d%%\n", 255, 255, 255, get_prediction_hit_rate());
    printf("%cMapped files: %d\n", 255, 255, 255, g_mapping_count);
    printf("%cPrediction score: %.2f\n", 255, 255, 255, g_memory_pattern.prediction_score);
}

// Memory analysis and optimization
void predictive_memory_analyze(void) {
    calculate_prediction_score(&g_memory_pattern);
    adaptive_memory_optimization();
}

void predictive_memory_optimize(void) {
    // Perform optimization every 1000 accesses
    if (g_total_access_count % 1000 == 0) {
        predictive_memory_analyze();
    }
} 