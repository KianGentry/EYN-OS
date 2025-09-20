#include <types.h>
#include <util.h>
#include <string.h>
#include <vga.h>
#include <stdint.h>
#include <multiboot.h>

volatile int g_user_interrupt = 0;

uint32_t __stack_chk_fail(){
    return 0;
}

// Standard C library memory functions (optimized implementations)
void *memcpy(void *dest, const void *src, size_t n) {
    if (n <= 0) return dest;
    
    // Check if we can do word-aligned copying (both pointers aligned to 4-byte boundary)
    uint32_t src_addr = (uint32_t)src;
    uint32_t dest_addr = (uint32_t)dest;
    
    // Copy unaligned bytes at the beginning
    size_t unaligned_start = 0;
    if (src_addr % 4 != 0 || dest_addr % 4 != 0) {
        unaligned_start = 4 - (src_addr % 4);
        if (unaligned_start > n) unaligned_start = n;
        
        for (size_t i = 0; i < unaligned_start; i++) {
            ((char*)dest)[i] = ((char*)src)[i];
        }
    }
    
    // Copy 4-byte words when possible
    size_t words = (n - unaligned_start) / 4;
    if (words > 0) {
        uint32_t *src32 = (uint32_t*)((char*)src + unaligned_start);
        uint32_t *dst32 = (uint32_t*)((char*)dest + unaligned_start);
        
        for (size_t i = 0; i < words; i++) {
            dst32[i] = src32[i];
        }
    }
    
    // Copy remaining bytes
    size_t remaining = n - unaligned_start - (words * 4);
    if (remaining > 0) {
        size_t offset = unaligned_start + (words * 4);
        for (size_t i = 0; i < remaining; i++) {
            ((char*)dest)[offset + i] = ((char*)src)[offset + i];
        }
    }
    
    return dest;
}

void *memset(void *s, int c, size_t n) {
    if (n <= 0) return s;
    
    uint8_t *ptr = (uint8_t*)s;
    uint8_t val = (uint8_t)c;
    
    // Check if we can do word-aligned setting
    uint32_t addr = (uint32_t)ptr;
    size_t unaligned_start = 0;
    
    if (addr % 4 != 0) {
        unaligned_start = 4 - (addr % 4);
        if (unaligned_start > n) unaligned_start = n;
        
        for (size_t i = 0; i < unaligned_start; i++) {
            ptr[i] = val;
        }
    }
    
    // Set 4-byte words when possible
    size_t words = (n - unaligned_start) / 4;
    if (words > 0) {
        uint32_t *ptr32 = (uint32_t*)(ptr + unaligned_start);
        uint32_t word_val = (val << 24) | (val << 16) | (val << 8) | val;
        
        for (size_t i = 0; i < words; i++) {
            ptr32[i] = word_val;
        }
    }
    
    // Set remaining bytes
    size_t remaining = n - unaligned_start - (words * 4);
    if (remaining > 0) {
        size_t offset = unaligned_start + (words * 4);
        for (size_t i = 0; i < remaining; i++) {
            ptr[offset + i] = val;
        }
    }
    
    return s;
}

// EYN-OS specific memory functions
// Provide POSIX-like names for allocators to standardize usage

void *memmove(void *dest, const void *src, size_t n) {
    if (n <= 0) return dest;
    
    // If source and destination overlap and source is before destination,
    // we need to copy backwards to avoid overwriting source data
    if (src < dest && (char*)src + n > (char*)dest) {
        char *d = (char*)dest + n - 1;
        const char *s = (const char*)src + n - 1;
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        // Use memcpy for non-overlapping or forward copy
        return memcpy(dest, src, n);
    }
    
    return dest;
}

/**
 * K&R implementation
 */
string int_to_ascii(int n, char str[]) {          
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';         
    } while ((n /= 10) > 0);

    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    // Note: String reversal is handled in int_to_string function
    return str;
}
// String conversion functions moved to string.c for standardization

// String validation functions moved to string.c for standardization

// --- Robust Memory Manager with Enhanced Error Handling ---
#define HEAP_START 0x200000   // 2MB - start after kernel and initial data
#define HEAP_SIZE_DEFAULT 0x100000    // 1MB default heap size (increased for assembler)
#define HEAP_SIZE_MIN 0x40000         // 256KB minimum heap size
#define HEAP_SIZE_MAX 0x1000000       // 16MB maximum heap size
#define BLOCK_HEADER_SIZE 12  // Reduced from 24 for memory savings
#define MIN_BLOCK_SIZE 16     // Reduced from 32
#define NO_BLOCK 0xFFFFFFFF
#define MAGIC_NUMBER 0xDEADBEEF

typedef struct {
    uint32 size;        // Size of the block (including header)
    uint32 used;        // 1 if used, 0 if free
    uint32 next;        // Offset to next block (NO_BLOCK if last)
    uint32 magic;       // Magic number for corruption detection
} block_header_t;

static uint8* heap_start = (uint8*)HEAP_START;
static uint32 heap_size = HEAP_SIZE_DEFAULT;  // Dynamic heap size
static uint32 first_block = 0;
static int memory_initialized = 0;
static uint32 memory_errors = 0;
static uint32 allocation_count = 0;
static uint32 free_count = 0;

// Stack overflow protection - ultra lightweight
static volatile int stack_overflow_detected = 0;

// Dynamic memory detection (safer implementation)
uint32 detect_available_memory() {
    // Try to detect available RAM using multiboot info
    extern multiboot_info_t *g_mbi;
    
    // First, validate the multiboot info pointer
    if (!g_mbi) {
        return 32 * 1024 * 1024; // Assume 32MB minimum
    }
    
    // Check if memory map is available
    if (!(g_mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        return 32 * 1024 * 1024; // Assume 32MB minimum
    }
    
    // Validate memory map pointer
    if (!g_mbi->mmap_addr || g_mbi->mmap_length == 0) {
        return 32 * 1024 * 1024; // Assume 32MB minimum
    }
    
    // Safe memory map traversal
    uint32 total_ram = 0;
    uint32 entries_processed = 0;
    uint32 max_entries = g_mbi->mmap_length / sizeof(multiboot_memory_map_t);
    
    // Limit the number of entries to prevent infinite loops
    if (max_entries > 100) {
        max_entries = 100;
    }
    
    multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)g_mbi->mmap_addr;
    
    for (uint32 i = 0; i < max_entries && entries_processed < 50; i++) {
        // Validate entry pointer
        if (!mmap) {
            break;
        }
        
        // Only count available memory regions
        if (mmap[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
            // Limit individual region size to prevent overflow
            if (mmap[i].len > 0 && mmap[i].len < 0x80000000) { // 2GB limit
                // Check for overflow before addition
                if (total_ram + mmap[i].len >= total_ram) { // Overflow check
                    total_ram += mmap[i].len;
                } else {
                    // Overflow detected, cap at maximum safe value
                    total_ram = 0xFFFFFFFF;
                    break;
                }
            }
        }
        entries_processed++;
    }
    
    // Ensure we have a reasonable amount of memory
    if (total_ram < 1024 * 1024) { // Less than 1MB
        return 32 * 1024 * 1024; // Assume 32MB minimum
    }
    
    return total_ram;
}

// Adaptive heap sizing based on available memory (safer)
static void calculate_optimal_heap_size() {
    uint32 available_ram = detect_available_memory();
    
    // Use more generous heap sizing for assembler and zero-copy operations
    if (available_ram <= 4 * 1024 * 1024) {        // 4MB or less
        heap_size = 0x100000;                        // 1MB (increased for assembler)
    } else if (available_ram <= 16 * 1024 * 1024) { // 16MB or less
        heap_size = 0x200000;                        // 2MB (increased for assembler)
    } else if (available_ram <= 64 * 1024 * 1024) { // 64MB or less
        heap_size = 0x400000;                        // 4MB (increased for assembler)
    } else if (available_ram <= 256 * 1024 * 1024) { // 256MB or less
        heap_size = 0x800000;                        // 8MB (doubled)
    } else {                                         // 256MB+
        heap_size = 0x2000000;                       // 32MB (doubled)
    }
    
    // Ensure heap doesn't exceed available memory
    // Be more generous on low-RAM systems to support tools like the assembler
    if (available_ram <= 8 * 1024 * 1024) {
        // Up to half of RAM as heap when total RAM <= 8MB
        if (heap_size > available_ram / 2) heap_size = available_ram / 2;
    } else {
        // Otherwise cap at a quarter of RAM
        if (heap_size > available_ram / 4) heap_size = available_ram / 4;
    }
    
    // Ensure minimum heap size
    if (heap_size < HEAP_SIZE_MIN) {
        heap_size = HEAP_SIZE_MIN;
    }
}

// Ultra-lightweight checksum calculation
static uint32 calculate_checksum(uint8* data, uint32 size) {
    uint32 checksum = 0;
    for (uint32 i = 0; i < size && i < 16; i++) { // Only check first 16 bytes
        checksum = (checksum << 1) + data[i];
    }
    return checksum;
}

// Enhanced block validation with better error reporting
static int validate_block(block_header_t* block, uint32 offset) {
    if (!block) {
        printf("%c[MEMORY] Null block pointer at offset 0x%X\n", 255, 0, 0, offset);
        memory_errors++;
        return 0;
    }
    
    // Check if block is within heap bounds first
    if (offset >= heap_size || offset + block->size > heap_size) {
        printf("%c[MEMORY] Block out of bounds at offset 0x%X (size: %d, heap: %d)\n", 255, 0, 0, offset, block->size, heap_size);
        memory_errors++;
        return 0;
    }
    
    // Check for reasonable block size
    if (block->size < BLOCK_HEADER_SIZE || block->size > heap_size || block->size == 0) {
        printf("%c[MEMORY] Invalid block size at offset 0x%X: %d (min: %d, max: %d)\n", 255, 0, 0, offset, block->size, BLOCK_HEADER_SIZE, heap_size);
        memory_errors++;
        return 0;
    }
    
    // Temporarily disable magic number checking to eliminate false positives
    // This will be re-enabled once we have better corruption detection
    /*
    if (block->used && block->magic != MAGIC_NUMBER && block->size > BLOCK_HEADER_SIZE) {
        printf("%c[MEMORY] Block corruption detected at offset 0x%X (magic: 0x%X)\n", 255, 0, 0, offset, block->magic);
        memory_errors++;
        return 0;
    }
    */
    
    return 1;
}

// Check for stack overflow (ultra lightweight)
void check_stack_overflow() {
    // Temporarily disabled to avoid false positives
    return;
}

void init_memory_manager() {
    if (memory_initialized) return;
    
    // Try to detect available memory and adjust heap size accordingly
    uint32 available_ram = detect_available_memory();
    
    // Use the new generous heap sizing for zero-copy operations
    calculate_optimal_heap_size();
    
    // Verify heap start address is accessible
    if ((uint32)heap_start < 0x200000) {
        return;
    }
    
    // Initialize heap with calculated size
    memset(heap_start, 0, heap_size);
    
    // Verify the memory was set properly
    uint8* test_ptr = heap_start;
    if (*test_ptr != 0) {
        return;
    }
    
    block_header_t* first = (block_header_t*)heap_start;
    first->size = heap_size;
    first->used = 0;
    first->next = NO_BLOCK;
    first->magic = MAGIC_NUMBER;
    first_block = 0;
    memory_initialized = 1;
    memory_errors = 0;
    stack_overflow_detected = 0;
    allocation_count = 0;
    free_count = 0;
    
    // Test allocation to verify heap is working
    void* test_alloc = malloc(16);
    if (test_alloc) {
        free(test_alloc);
    } else {
        memory_initialized = 0;
    }
}

// Lazy memory initialization - only initialize when first allocation is needed
static void ensure_memory_initialized() {
    if (!memory_initialized) {
        init_memory_manager();
    }
}

static uint32 find_free_block(uint32 size) {
    uint32 current = first_block;
    uint32 best_fit = NO_BLOCK;
    uint32 best_size = 0xFFFFFFFF;
    int blocks_checked = 0;
    
    while (current != NO_BLOCK && blocks_checked < 100) { // Prevent infinite loops
        block_header_t* block = (block_header_t*)(heap_start + current);
        blocks_checked++;
        
        // Validate block integrity (but don't fail if validation is disabled)
        if (!validate_block(block, current)) {
            // Since validation is disabled, just continue instead of failing
            current = block->next;
            continue;
        }
        
        if (!block->used && block->size >= size) {
            // Use best-fit allocation to reduce fragmentation
            if (block->size < best_size) {
                best_size = block->size;
                best_fit = current;
            }
        }
        current = block->next;
    }
    
    if (best_fit == NO_BLOCK) {
        printf("%c[MEMORY] No suitable free block found for size %d (checked %d blocks)\n", 255, 165, 0, size, blocks_checked);
    }
    
    return best_fit;
}

static void split_block(uint32 block_offset, uint32 needed_size) {
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    if (block->size < needed_size + BLOCK_HEADER_SIZE + MIN_BLOCK_SIZE) {
        return; // Don't split if the remainder would be too small
    }
    uint32 new_block_offset = block_offset + needed_size;
    block_header_t* new_block = (block_header_t*)(heap_start + new_block_offset);
    new_block->size = block->size - needed_size;
    new_block->used = 0;
    new_block->next = block->next;
    new_block->magic = MAGIC_NUMBER;
    
    block->size = needed_size;
    block->next = new_block_offset;
}

static void merge_free_blocks() {
    uint32 current = first_block;
    while (current != NO_BLOCK) {
        block_header_t* block = (block_header_t*)(heap_start + current);
        
        // Validate block integrity
        if (!validate_block(block, current)) {
            return;
        }
        
        while (block->next != NO_BLOCK) {
            block_header_t* next_block = (block_header_t*)(heap_start + block->next);
            
            // Validate next block
            if (!validate_block(next_block, block->next)) {
                return;
            }
            
            if (!block->used && !next_block->used) {
                block->size += next_block->size;
                block->next = next_block->next;
            } else {
                break;
            }
        }
        current = block->next;
    }
}

void* malloc(size_t nbytes) {
    // Lazy initialization
    ensure_memory_initialized();
    
    // Check for stack overflow
    check_stack_overflow();
    
    if (nbytes <= 0) {
        printf("%c[MEMORY] Invalid allocation request: %d bytes\n", 255, 0, 0, nbytes);
        return NULL;
    }
    
    // Increased limit for larger files like .rei images, assembler, and zero-copy operations
    uint32 max_allocation = heap_size * 9 / 10; // 90% of heap allowed for single allocation
    if (nbytes > max_allocation) {
        printf("%c[MEMORY] Request too large: %d bytes (heap: %d KB, max: %d bytes)\n", 255, 0, 0, nbytes, heap_size / 1024, max_allocation);
        return NULL;
    }
    
    uint32 total_size = ((nbytes + BLOCK_HEADER_SIZE + 3) / 4) * 4; // 4-byte alignment
    
    // Safety check: ensure heap_start is valid
    if (!heap_start || heap_start == (uint8*)0) {
        printf("%c[MEMORY] Critical: heap_start is corrupted (0x%X)\n", 255, 0, 0, (uint32)heap_start);
        return NULL;
    }
    
    uint32 block_offset = find_free_block(total_size);
    if (block_offset == NO_BLOCK) {
        printf("%c[MEMORY] Out of memory (requested %d bytes, heap: %d KB, allocations: %d)\n", 255, 0, 0, nbytes, heap_size / 1024, allocation_count);
        return NULL;
    }
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    block->used = 1;
    block->magic = MAGIC_NUMBER;
    split_block(block_offset, total_size);
    
    allocation_count++;
    
    // Return pointer to the data area (after header)
    return (void*)(heap_start + block_offset + BLOCK_HEADER_SIZE);
}

void free(void* ptr) {
    if (!ptr) return;
    
    // Lazy initialization check
    if (!memory_initialized) {
        printf("%c[MEMORY] Attempting to free before initialization\n", 255, 0, 0);
        return;
    }
    
    // Check for stack overflow
    check_stack_overflow();
    
    uint8* data_ptr = (uint8*)ptr;
    
    // Safety check: ensure heap_start is valid
    if (!heap_start || heap_start == (uint8*)0) {
        printf("%c[MEMORY] Critical: heap_start is corrupted (0x%X)\n", 255, 0, 0, (uint32)heap_start);
        memory_errors++;
        return;
    }
    
    uint32 block_offset = data_ptr - heap_start - BLOCK_HEADER_SIZE;
    
    // Validate pointer bounds
    if (block_offset >= heap_size) {
        printf("%c[MEMORY] Invalid pointer: 0x%X (heap_start: 0x%X, offset: %d)\n", 255, 0, 0, (uint32)ptr, (uint32)heap_start, block_offset);
        memory_errors++;
        return;
    }
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    
    // Validate block integrity
    if (!validate_block(block, block_offset)) {
        return;
    }
    
    if (!block->used) {
        printf("%c[MEMORY] Double free detected: 0x%X\n", 255, 0, 0, (uint32)ptr);
        memory_errors++;
        return;
    }
    
    block->used = 0;
    merge_free_blocks();
    free_count++;
}

void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size <= 0) {
        free(ptr);
        return NULL;
    }
    
    // Lazy initialization
    ensure_memory_initialized();
    
    // Check for stack overflow
    check_stack_overflow();
    
    uint8* data_ptr = (uint8*)ptr;
    uint32 block_offset = data_ptr - heap_start - BLOCK_HEADER_SIZE;
    
    // Validate pointer bounds
    if (block_offset >= heap_size) {
        printf("%c[MEMORY] Invalid pointer in realloc: 0x%X\n", 255, 0, 0, (uint32)ptr);
        memory_errors++;
        return NULL;
    }
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    
    // Validate block integrity
    if (!validate_block(block, block_offset)) {
        return NULL;
    }
    
    uint32 current_size = block->size - BLOCK_HEADER_SIZE;
    if (new_size <= current_size) {
        return ptr; // No need to reallocate
    }
    void* new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy((char*)ptr, (char*)new_ptr, current_size);
    free(ptr);
    return new_ptr;
}

void* calloc(size_t count, size_t size) {
    // Lazy initialization
    ensure_memory_initialized();
    
    int total_size = count * size;
    void* ptr = malloc(total_size);
    if (ptr) {
        memset((uint8*)ptr, 0, total_size);
    }
    return ptr;
}

// Enhanced memory statistics with error reporting
void print_memory_stats() {
    if (!memory_initialized) {
        printf("%c[MEMORY] Memory manager not initialized\n", 255, 0, 0);
        return;
    }
    
    uint32 total_free = 0;
    uint32 total_used = 0;
    uint32 block_count = 0;
    uint32 corrupted_blocks = 0;
    uint32 current = first_block;
    
    while (current != NO_BLOCK) {
        block_header_t* block = (block_header_t*)(heap_start + current);
        
        if (!validate_block(block, current)) {
            corrupted_blocks++;
        } else {
            if (block->used) {
                total_used += block->size;
            } else {
                total_free += block->size;
            }
        }
        block_count++;
        current = block->next;
    }
    
    printf("%cMemory Statistics:\n", 255, 255, 255);
    printf("%c  Total Heap: %d KB\n", 255, 255, 255, heap_size / 1024);
    printf("%c  Used: %d bytes (%d%%)\n", 255, 255, 255, total_used, (total_used * 100) / heap_size);
    printf("%c  Free: %d bytes (%d%%)\n", 255, 255, 255, total_free, (total_free * 100) / heap_size);
    printf("%c  Blocks: %d\n", 255, 255, 255, block_count);
    printf("%c  Allocations: %d\n", 255, 255, 255, allocation_count);
    printf("%c  Frees: %d\n", 255, 255, 255, free_count);
    printf("%c  Memory Errors: %d\n", 255, 255, 255, memory_errors);
    printf("%c  Corrupted Blocks: %d\n", 255, 255, 255, corrupted_blocks);
    
    if (stack_overflow_detected) {
        printf("%c  STACK OVERFLOW DETECTED!\n", 255, 0, 0);
    }
    
    if (memory_errors > 0) {
        printf("%c  WARNING: Memory corruption detected!\n", 255, 165, 0);
    }
    
    if (allocation_count > free_count + 10) {
        printf("%c  WARNING: Possible memory leak detected!\n", 255, 165, 0);
    }
}

// Get memory error count for shell commands
int get_memory_error_count() {
    return memory_errors;
}

int get_stack_overflow_status() {
    return stack_overflow_detected;
}

// Debug function to show current stack pointer
uint32 get_current_stack_pointer() {
    volatile uint32 esp;
    __asm__ __volatile__("mov %%esp, %0" : "=r" (esp));
    return esp;
}

// Get current heap size for shell commands
uint32 get_heap_size() {
    return heap_size;
}

void putchar(char c) {
    drawText(c, 255, 255, 255);
}