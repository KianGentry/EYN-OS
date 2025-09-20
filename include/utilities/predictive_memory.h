#ifndef PREDICTIVE_MEMORY_H
#define PREDICTIVE_MEMORY_H

#include <types.h>
#include <stdint.h>
#include <stddef.h>

// Predictive memory management system
// Works alongside existing my_malloc/my_free system

// Memory access pattern tracking
#define PATTERN_HISTORY_SIZE 256
#define PREDICTION_WINDOW 64
#define PREFETCH_THRESHOLD 0.7f

// Memory prediction structure
typedef struct {
    uint32_t access_pattern[PATTERN_HISTORY_SIZE];  // Track access patterns
    uint32_t allocation_sizes[64];                  // Track allocation sizes
    uint32_t access_frequency[1024];               // Track memory access frequency
    float prediction_score;                         // Likelihood of future access
    uint8_t prefetch_enabled;                      // Enable prefetching
    uint32_t last_access_time;                     // Last access timestamp
    uint32_t access_count;                         // Total access count
} memory_prediction_t;

// Predictive allocation structure
typedef struct {
    void* predicted_blocks[16];     // Pre-allocated blocks
    uint32_t block_sizes[16];       // Sizes of predicted blocks
    uint8_t block_used[16];         // Usage status of blocks
    uint32_t prediction_hits;       // Number of successful predictions
    uint32_t prediction_misses;     // Number of failed predictions
    float hit_rate;                 // Prediction accuracy
} predictive_allocator_t;

// Zero-copy file mapping structure
typedef struct {
    void* mapped_address;           // Memory-mapped address
    uint32_t file_size;            // Size of mapped file
    uint32_t block_start;          // Starting block number
    uint32_t block_count;          // Number of blocks mapped
    uint8_t drive;                 // Drive number
    uint8_t read_only;             // Read-only flag
    uint32_t access_count;         // Access counter
} file_mapping_t;

// Function prototypes for predictive memory management
void predictive_memory_init(void);
void* predictive_malloc(size_t size, memory_prediction_t* pattern);
void predictive_free(void* ptr);
void predictive_memory_analyze(void);
void predictive_memory_optimize(void);

// Function prototypes for zero-copy file operations
int eynfs_mmap(const char* path, void** addr, size_t* size, uint8_t read_only);
int eynfs_munmap(void* addr, size_t size);
int eynfs_msync(void* addr, size_t size);
void* eynfs_mmap_readonly(const char* path, size_t* size);

// Memory prediction analysis functions
void update_access_pattern(uint32_t address, uint32_t size);
float calculate_prediction_score(memory_prediction_t* pattern);
void prefetch_memory_blocks(void);
void adaptive_memory_optimization(void);

// Performance monitoring functions
uint32_t get_prediction_hit_rate(void);
uint32_t get_memory_access_count(void);
void reset_prediction_stats(void);
void print_prediction_stats(void);

// Memory mapping utilities
int is_memory_mapped(void* addr);
file_mapping_t* get_file_mapping(void* addr);
void invalidate_mapped_file(const char* path);

#endif // PREDICTIVE_MEMORY_H 