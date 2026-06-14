#include <zero_copy.h>
#include <util.h>
#include <vga.h>
#include <string.h>
#include <eynfs.h>
#include <stdint.h>
#include <context.h>
#include <misc/sched.h>
#include <rust_alloc.h>

// Global zero-copy state
static zero_copy_file_t* g_zero_copy_files = NULL;  // Dynamic array
static uint8_t g_zero_copy_max_files = 16;  // Default limit
static uint8_t g_zero_copy_fd_count = 0;
static uint32_t g_zero_copy_stats = 0;
static uint32_t g_zero_copy_operations = 0;

#define ZERO_COPY_MAX_ANON_MAPS 32
typedef struct {
    void* addr;
    size_t length;
} zero_copy_anon_map_t;

static zero_copy_anon_map_t g_zero_copy_anon_maps[ZERO_COPY_MAX_ANON_MAPS];
static uint8_t g_zero_copy_anon_count = 0;

static void zero_copy_anon_reset(void) {
    memset(g_zero_copy_anon_maps, 0, sizeof(g_zero_copy_anon_maps));
    g_zero_copy_anon_count = 0;
}

static int zero_copy_writeback(zero_copy_file_t* file) {
    if (!file || !file->dirty) return 0;
    if (!(file->flags & ZERO_COPY_WRITE_ONLY) && !(file->flags & ZERO_COPY_READ_WRITE)) return 0;
    if (!file->mapped_address && file->file_size != 0) return -1;

    eynfs_superblock_t sb;
    if (eynfs_read_superblock(file->drive, 2048, &sb) != 0) {
        printf("%c[ZERO-COPY] Error: Cannot read filesystem for write-back\n", 255, 0, 0);
        return -1;
    }

    // Ensure on-disk metadata matches our current buffer size.
    file->entry.size = file->file_size;

    int n = eynfs_write_file(
        file->drive,
        &sb,
        &file->entry,
        file->mapped_address,
        file->file_size,
        file->parent_block,
        file->entry_index);

    if (n < 0) {
        printf("%c[ZERO-COPY] Error: Write-back failed\n", 255, 0, 0);
        return -1;
    }

    file->block_start = file->entry.first_block;
    file->dirty = 0;
    return 0;
}

static int zero_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void zero_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Get optimal zero-copy file limit based on available memory
static uint8_t get_max_zero_copy_files() {
    // Detect available memory and adjust limits accordingly
    extern multiboot_info_t *g_mbi;
    uint32_t available_memory = 32 * 1024 * 1024; // Default assumption
    
    if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        // Calculate total available memory from memory map
        uint32_t total_ram = 0;
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)g_mbi->mmap_addr;
        uint32_t entries = g_mbi->mmap_length / sizeof(multiboot_memory_map_t);
        
        for (uint32_t i = 0; i < entries && i < 50; i++) {
            if ((i & 0x7u) == 0) {
                zero_ctx_account(SCHED_COST_FS);
            }
            if (mmap[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
                total_ram += mmap[i].len;
            }
        }
        
        if (total_ram > 0) {
            available_memory = total_ram;
        }
    }
    
    // Set limits based on available memory
    if (available_memory < 8 * 1024 * 1024) {        // Less than 8MB
        return 2;   // Very conservative for low-memory systems
    } else if (available_memory < 32 * 1024 * 1024) { // Less than 32MB
        return 8;   // Conservative for medium-memory systems
    } else if (available_memory < 128 * 1024 * 1024) { // Less than 128MB
        return 12;  // Moderate for larger systems
    } else {                                          // 128MB+
        return 16;  // Full limit for high-memory systems
    }
}

// Initialize zero-copy system
void zero_copy_init(void) {
    if (!zero_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return;
    // Calculate optimal file limit based on available memory
    g_zero_copy_max_files = get_max_zero_copy_files();
    
    // Allocate dynamic array
    g_zero_copy_files = (zero_copy_file_t*)malloc(g_zero_copy_max_files * sizeof(zero_copy_file_t));
    if (!g_zero_copy_files) {
        // Fallback to static allocation if dynamic allocation fails
        printf("%c[ZERO-COPY] Primary allocation failed, trying fallback\n", 255, 165, 0);
        g_zero_copy_max_files = 2;
        g_zero_copy_files = (zero_copy_file_t*)malloc(g_zero_copy_max_files * sizeof(zero_copy_file_t));
        if (!g_zero_copy_files) {
            printf("%c[ZERO-COPY] Warning: Failed to allocate zero-copy file array\n", 255, 165, 0);
            g_zero_copy_max_files = 0;
            return;
        }
    }
    
    memset(g_zero_copy_files, 0, g_zero_copy_max_files * sizeof(zero_copy_file_t));
    g_zero_copy_fd_count = 0;
    g_zero_copy_stats = 0;
    g_zero_copy_operations = 0;
    zero_copy_anon_reset();
}

// Open file with zero-copy operations
int zero_copy_open(const char* path, uint8_t flags) {
    if (!zero_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
    printf("%c[ZERO-COPY] Opening file: %s (flags: %d)\n", 0, 255, 0, path, flags);
    
    if (!g_zero_copy_files) {
        printf("%c[ZERO-COPY] Error: Zero-copy system not initialized\n", 255, 0, 0);
        return -1;
    }
    
    if (g_zero_copy_fd_count >= g_zero_copy_max_files) {
        printf("%c[ZERO-COPY] Error: Too many zero-copy files open (limit: %d)\n", 255, 0, 0, g_zero_copy_max_files);
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
    printf("%c[ZERO-COPY] Using drive: %d\n", 0, 255, 0, drive);
    
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, 2048, &sb) != 0) {
        printf("%c[ZERO-COPY] Error: Cannot read filesystem\n", 255, 0, 0);
        return -1;
    }
    printf("%c[ZERO-COPY] Filesystem read successfully\n", 0, 255, 0);
    
    eynfs_dir_entry_t entry;
    uint32_t parent_block, entry_index;
    printf("%c[ZERO-COPY] Looking for file: %s\n", 0, 255, 0, abspath);
    if (eynfs_traverse_path(drive, &sb, abspath, &entry, &parent_block, &entry_index) != 0) {
        printf("%c[ZERO-COPY] Error: File not found: %s\n", 255, 0, 0, abspath);
        return -1;
    }
    printf("%c[ZERO-COPY] File found, size: %d bytes\n", 0, 255, 0, entry.size);
    
    if (entry.type != EYNFS_TYPE_FILE) {
        printf("%c[ZERO-COPY] Error: Not a file: %s\n", 255, 0, 0, abspath);
        return -1;
    }
    
    // Allocate memory for the entire file (zero-copy)
    printf("%c[ZERO-COPY] Allocating %d bytes for file mapping\n", 0, 255, 0, entry.size);
    void* mapped_addr = malloc((int)entry.size);
    if (!mapped_addr) {
        printf("%c[ZERO-COPY] Error: Out of memory for zero-copy mapping\n", 255, 0, 0);
        return -1;
    }
    printf("%c[ZERO-COPY] Memory allocated successfully at %p\n", 0, 255, 0, mapped_addr);
    
    // Read entire file into memory
    printf("%c[ZERO-COPY] Reading file into memory...\n", 0, 255, 0);
    int bytes_read = eynfs_read_file(drive, &sb, &entry, mapped_addr, entry.size, 0);
    if (bytes_read != (int)entry.size) {
        printf("%c[ZERO-COPY] Error: Failed to read file for zero-copy (read %d, expected %d)\n", 255, 0, 0, bytes_read, entry.size);
        free(mapped_addr);
        return -1;
    }
    printf("%c[ZERO-COPY] File read successfully (%d bytes)\n", 0, 255, 0, bytes_read);
    
    // Set up zero-copy file handle
    g_zero_copy_files[g_zero_copy_fd_count].mapped_address = mapped_addr;
    g_zero_copy_files[g_zero_copy_fd_count].file_size = entry.size;
    g_zero_copy_files[g_zero_copy_fd_count].current_offset = 0;
    g_zero_copy_files[g_zero_copy_fd_count].drive = drive;
    g_zero_copy_files[g_zero_copy_fd_count].flags = flags;
    g_zero_copy_files[g_zero_copy_fd_count].block_start = entry.first_block;
    g_zero_copy_files[g_zero_copy_fd_count].block_count = 0;
    g_zero_copy_files[g_zero_copy_fd_count].access_count = 0;
    g_zero_copy_files[g_zero_copy_fd_count].dirty = 0;
    g_zero_copy_files[g_zero_copy_fd_count].parent_block = parent_block;
    g_zero_copy_files[g_zero_copy_fd_count].entry_index = entry_index;
    g_zero_copy_files[g_zero_copy_fd_count].entry = entry;
    
    int fd = g_zero_copy_fd_count;
    g_zero_copy_fd_count++;
    
    printf("%c[ZERO-COPY] File opened successfully with fd: %d\n", 0, 255, 0, fd);
    return fd;
}

// Close zero-copy file
int zero_copy_close(int fd) {
    if (fd < 0 || fd >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptor: %d\n", 255, 0, 0, fd);
        return -1;
    }
    
    zero_copy_file_t* file = &g_zero_copy_files[fd];

    // If file was modified, write back to disk.
    if (file->dirty) {
        if (zero_copy_writeback(file) != 0) {
            // Continue cleanup, but report failure.
            if (file->mapped_address) {
                free(file->mapped_address);
                file->mapped_address = NULL;
            }
            memset(file, 0, sizeof(zero_copy_file_t));
            return -1;
        }
    }
    
    // Free mapped memory
    if (file->mapped_address) {
        free(file->mapped_address);
        file->mapped_address = NULL;
    }
    
    // Clear file handle
    memset(file, 0, sizeof(zero_copy_file_t));
    return 0;
}

// Zero-copy read operation
size_t zero_copy_read(int fd, void* buf, size_t count) {
    if (!zero_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return (size_t)-1;
    if (fd < 0 || fd >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptor: %d\n", 255, 0, 0, fd);
        return -1;
    }
    
    zero_copy_file_t* file = &g_zero_copy_files[fd];
    
    if (!(file->flags & ZERO_COPY_READ_ONLY) && !(file->flags & ZERO_COPY_READ_WRITE)) {
        printf("%cError: File not opened for reading\n", 255, 0, 0);
        return -1;
    }
    
    if (file->current_offset >= file->file_size) {
        return 0;  // End of file
    }
    
    size_t available = file->file_size - file->current_offset;
    if (count > available) count = available;
    
    uint8_t* src = (uint8_t*)file->mapped_address + file->current_offset;
    memcpy(buf, src, count);
    
    file->current_offset += count;
    file->access_count++;
    g_zero_copy_operations++;
    
    return count;
}

// Zero-copy write operation
size_t zero_copy_write(int fd, const void* buf, size_t count) {
    if (!zero_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return (size_t)-1;
    if (fd < 0 || fd >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptor: %d\n", 255, 0, 0, fd);
        return -1;
    }
    
    zero_copy_file_t* file = &g_zero_copy_files[fd];
    
    if (!(file->flags & ZERO_COPY_WRITE_ONLY) && !(file->flags & ZERO_COPY_READ_WRITE)) {
        printf("%cError: File not opened for writing\n", 255, 0, 0);
        return -1;
    }
    
    uint32_t end_off;
    if (count > 0xFFFFFFFFu) return (size_t)-1;
    if (file->current_offset > 0xFFFFFFFFu - (uint32_t)count) {
        printf("%cError: Write size overflow\n", 255, 0, 0);
        return (size_t)-1;
    }
    end_off = file->current_offset + (uint32_t)count;

    if (end_off > file->file_size) {
        // Extend file mapping by reallocating and zero-filling new space.
        if (!zero_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return (size_t)-1;
        uint8_t* new_buf = (uint8_t*)malloc((int)end_off);
        if (!new_buf) {
            printf("%cError: Out of memory for file extension\n", 255, 0, 0);
            return (size_t)-1;
        }
        if (file->mapped_address && file->file_size > 0) {
            memcpy(new_buf, file->mapped_address, file->file_size);
        }
        if (end_off > file->file_size) {
            memset(new_buf + file->file_size, 0, end_off - file->file_size);
        }
        if (file->mapped_address) free(file->mapped_address);
        file->mapped_address = new_buf;
        file->file_size = end_off;
        file->entry.size = end_off;
    }
    
    uint8_t* dest = (uint8_t*)file->mapped_address + file->current_offset;
    memcpy(dest, buf, count);
    
    file->current_offset += count;
    file->access_count++;
    file->dirty = 1;  // Mark as modified
    g_zero_copy_operations++;
    
    return count;
}

// Seek in zero-copy file
int zero_copy_seek(int fd, int32_t offset, int whence) {
    if (!zero_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    if (fd < 0 || fd >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptor: %d\n", 255, 0, 0, fd);
        return -1;
    }
    
    zero_copy_file_t* file = &g_zero_copy_files[fd];
    uint32_t new_offset;
    
    switch (whence) {
        case 0:  new_offset = offset; break;            // SEEK_SET
        case 1:  new_offset = file->current_offset + offset; break; // SEEK_CUR
        case 2:  new_offset = file->file_size + offset; break;      // SEEK_END
        default: printf("%cError: Invalid seek whence: %d\n", 255, 0, 0, whence); return -1;
    }
    
    if (new_offset > file->file_size) {
        printf("%cError: Seek beyond end of file\n", 255, 0, 0);
        return -1;
    }
    
    file->current_offset = new_offset;
    return 0;
}

// Memory mapping for zero-copy operations
void* zero_copy_mmap(void* addr, size_t length, uint8_t flags, int fd, int32_t offset) {
    if (!zero_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return NULL;
    if (fd >= 0 && fd < g_zero_copy_fd_count) {
        zero_copy_file_t* file = &g_zero_copy_files[fd];
        if (offset + length > file->file_size) {
            printf("%cError: Mapping beyond file size\n", 255, 0, 0);
            return NULL;
        }
        return (uint8_t*)file->mapped_address + offset;
    } else {
        void* mapped_addr = malloc((int)length);
        if (!mapped_addr) {
            printf("%cError: Out of memory for anonymous mapping\n", 255, 0, 0);
            return NULL;
        }
        memset(mapped_addr, 0, length);

        if (g_zero_copy_anon_count < ZERO_COPY_MAX_ANON_MAPS) {
            g_zero_copy_anon_maps[g_zero_copy_anon_count].addr = mapped_addr;
            g_zero_copy_anon_maps[g_zero_copy_anon_count].length = length;
            g_zero_copy_anon_count++;
        } else {
            // Still return the mapping, but warn that munmap may not be able to free it.
            printf("%cWarning: Anonymous mapping table full; munmap may leak\n", 255, 165, 0);
        }
        return mapped_addr;
    }
}

// Unmap zero-copy memory
int zero_copy_munmap(void* addr, size_t length) {
    if (!addr) return -1;

    // First, free tracked anonymous mappings.
    for (int i = 0; i < g_zero_copy_anon_count; i++) {
        if (g_zero_copy_anon_maps[i].addr == addr) {
            free(addr);
            // Compact table
            for (int j = i; j < g_zero_copy_anon_count - 1; j++) {
                g_zero_copy_anon_maps[j] = g_zero_copy_anon_maps[j + 1];
            }
            g_zero_copy_anon_count--;
            return 0;
        }
    }

    // If it points into a file mapping, we currently don't support partial unmap.
    for (int i = 0; i < g_zero_copy_fd_count; i++) {
        zero_copy_file_t* file = &g_zero_copy_files[i];
        if (file->mapped_address) {
            uint8_t* base = (uint8_t*)file->mapped_address;
            uint8_t* a = (uint8_t*)addr;
            if (a >= base && a < base + file->file_size) {
                (void)length;
                return 0;
            }
        }
    }

    return -1;
}

// Synchronize zero-copy memory to disk
int zero_copy_msync(void* addr, size_t length, uint8_t flags) {
    if (!zero_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    for (int i = 0; i < g_zero_copy_fd_count; i++) {
        if ((i & 0x7) == 0) {
            zero_ctx_account(SCHED_COST_FS);
        }
        zero_copy_file_t* file = &g_zero_copy_files[i];
        if (file->mapped_address) {
            uint8_t* base = (uint8_t*)file->mapped_address;
            uint8_t* a = (uint8_t*)addr;
            if (a >= base && a < base + file->file_size) {
                if (file->dirty) {
                    (void)length;
                    (void)flags;
                    if (zero_copy_writeback(file) != 0) return -1;
                }
                return 0;
            }
        }
    }
    printf("%cError: Address not mapped\n", 255, 0, 0);
    return -1;
}

// Advanced zero-copy operations
int zero_copy_splice(int fd_in, int fd_out, size_t count) {
    if (!zero_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (fd_in < 0 || fd_in >= g_zero_copy_fd_count || fd_out < 0 || fd_out >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptors\n", 255, 0, 0);
        return -1;
    }
    zero_copy_file_t* file_in = &g_zero_copy_files[fd_in];
    zero_copy_file_t* file_out = &g_zero_copy_files[fd_out];
    size_t available_in = file_in->file_size - file_in->current_offset;
    size_t available_out = file_out->file_size - file_out->current_offset;
    if (count > available_in) count = available_in;
    if (count > available_out) count = available_out;
    uint8_t* src = (uint8_t*)file_in->mapped_address + file_in->current_offset;
    uint8_t* dest = (uint8_t*)file_out->mapped_address + file_out->current_offset;
    memcpy(dest, src, count);
    file_in->current_offset += count;
    file_out->current_offset += count;
    file_out->dirty = 1;
    g_zero_copy_operations++;
    return count;
}

int zero_copy_tee(int fd_in, int fd_out, size_t count) {
    if (!zero_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    if (fd_in < 0 || fd_in >= g_zero_copy_fd_count || fd_out < 0 || fd_out >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptors\n", 255, 0, 0);
        return -1;
    }
    zero_copy_file_t* file_in = &g_zero_copy_files[fd_in];
    zero_copy_file_t* file_out = &g_zero_copy_files[fd_out];
    size_t available_in = file_in->file_size - file_in->current_offset;
    size_t available_out = file_out->file_size - file_out->current_offset;
    if (count > available_in) count = available_in;
    if (count > available_out) count = available_out;
    uint8_t* src = (uint8_t*)file_in->mapped_address + file_in->current_offset;
    uint8_t* dest = (uint8_t*)file_out->mapped_address + file_out->current_offset;
    memcpy(dest, src, count);
    file_out->current_offset += count;
    file_out->dirty = 1;
    g_zero_copy_operations++;
    return count;
}

uint32_t get_zero_copy_stats(void) { return g_zero_copy_operations; }
void reset_zero_copy_stats(void) { g_zero_copy_operations = 0; for (int i=0;i<g_zero_copy_fd_count;i++){g_zero_copy_files[i].access_count=0;}}
void print_zero_copy_stats(void) {
    printf("%c=== Zero-Copy File Statistics ===\n", 255, 255, 255);
    printf("%cTotal operations: %d\n", 255, 255, 255, g_zero_copy_operations);
    printf("%cOpen files: %d\n", 255, 255, 255, g_zero_copy_fd_count);
    for (int i = 0; i < g_zero_copy_fd_count; i++) {
        if ((i & 0x7) == 0) {
            zero_ctx_account(SCHED_COST_FS);
        }
        zero_copy_file_t* file = &g_zero_copy_files[i];
        if (file->mapped_address) {
            printf("%cFile %d: size=%d, offset=%d, accesses=%d, dirty=%d\n", 255, 255, 255, i, file->file_size, file->current_offset, file->access_count, file->dirty);
        }
    }
} 

int zero_copy_truncate(int fd, size_t length) {
    if (!zero_ctx_allow(CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
    if (fd < 0 || fd >= g_zero_copy_fd_count) return -1;
    zero_copy_file_t* file = &g_zero_copy_files[fd];

    if (!(file->flags & ZERO_COPY_WRITE_ONLY) && !(file->flags & ZERO_COPY_READ_WRITE)) {
        printf("%cError: File not opened for truncation\n", 255, 0, 0);
        return -1;
    }

    if (length > 0xFFFFFFFFu) return -1;
    uint32_t new_len = (uint32_t)length;

    if (new_len == file->file_size) return 0;

    if (new_len == 0) {
        if (file->mapped_address) {
            free(file->mapped_address);
            file->mapped_address = NULL;
        }
        file->file_size = 0;
        file->current_offset = 0;
        file->entry.size = 0;
        file->dirty = 1;
        return 0;
    }

    uint8_t* new_buf = (uint8_t*)malloc((int)new_len);
    if (!new_buf) return -1;

    if (file->mapped_address) {
        uint32_t to_copy = (new_len < file->file_size) ? new_len : file->file_size;
        if (to_copy) memcpy(new_buf, file->mapped_address, to_copy);
        if (new_len > to_copy) memset(new_buf + to_copy, 0, new_len - to_copy);
        free(file->mapped_address);
    } else {
        memset(new_buf, 0, new_len);
    }

    file->mapped_address = new_buf;
    file->file_size = new_len;
    if (file->current_offset > new_len) file->current_offset = new_len;
    file->entry.size = new_len;
    file->dirty = 1;
    return 0;
}

int zero_copy_vmsplice(int fd, const void* buf, size_t count, uint8_t flags) {
    (void)flags;
    size_t n = zero_copy_write(fd, buf, count);
    if (n == (size_t)-1) return -1;
    return (int)n;
}

zero_copy_buffer_t* create_zero_copy_buffer(uint32_t size) {
    if (!zero_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return NULL;
#if CONFIG_RUST_ALLOCATOR
    return rust_zero_copy_buffer_create(size);
#else
    zero_copy_buffer_t* b = (zero_copy_buffer_t*)malloc(sizeof(zero_copy_buffer_t));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));

    b->buffer = malloc((int)size);
    if (!b->buffer) {
        free(b);
        return NULL;
    }
    memset(b->buffer, 0, size);
    b->size = size;
    b->offset = 0;
    b->drive = 0;
    b->block_start = 0;
    b->dirty = 0;
    return b;
#endif
}

void destroy_zero_copy_buffer(zero_copy_buffer_t* buffer) {
#if CONFIG_RUST_ALLOCATOR
    rust_zero_copy_buffer_destroy(buffer);
#else
    if (!buffer) return;
    if (buffer->buffer) free(buffer->buffer);
    free(buffer);
#endif
}

int zero_copy_buffer_read(zero_copy_buffer_t* buffer, void* data, uint32_t offset, uint32_t size) {
#if CONFIG_RUST_ALLOCATOR
    return rust_zero_copy_buffer_read(buffer, data, offset, size);
#else
    if (!buffer || !buffer->buffer || !data) return -1;
    if (offset > buffer->size) return -1;
    if (size > buffer->size - offset) return -1;
    memcpy(data, (uint8_t*)buffer->buffer + offset, size);
    return 0;
#endif
}

int zero_copy_buffer_write(zero_copy_buffer_t* buffer, const void* data, uint32_t offset, uint32_t size) {
#if CONFIG_RUST_ALLOCATOR
    return rust_zero_copy_buffer_write(buffer, data, offset, size);
#else
    if (!buffer || !buffer->buffer || (!data && size)) return -1;
    if (offset > buffer->size) return -1;
    if (size > buffer->size - offset) return -1;
    if (size) memcpy((uint8_t*)buffer->buffer + offset, data, size);
    buffer->dirty = 1;
    return 0;
#endif
}

int eynfs_zero_copy_read(uint8_t drive, const eynfs_dir_entry_t* entry,
                        void** addr, size_t* size, uint32_t offset) {
    if (!addr || !size || !entry || entry->type != EYNFS_TYPE_FILE) return -1;
    if (offset >= entry->size) {
        *addr = NULL;
        *size = 0;
        return 0;
    }
    if (!zero_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;

    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, 2048, &sb) != 0) return -1;

    size_t to_read = entry->size - offset;
    void* buf = malloc((int)to_read);
    if (!buf) return -1;
    int n = eynfs_read_file(drive, &sb, entry, buf, to_read, offset);
    if (n < 0 || (size_t)n != to_read) {
        free(buf);
        return -1;
    }
    *addr = buf;
    *size = to_read;
    return 0;
}

// In-place write within an existing file's allocated block chain.
// Does not allocate/free blocks and therefore cannot extend the file.
int eynfs_zero_copy_write(uint8_t drive, eynfs_dir_entry_t* entry,
                        const void* data, size_t size, uint32_t offset) {
    if (!entry || entry->type != EYNFS_TYPE_FILE) return -1;
    if (!data && size) return -1;
    if (offset > entry->size) return -1;
    if (size > (size_t)(entry->size - offset)) return -1;
    if (!zero_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;

    uint32_t block_num = entry->first_block;
    size_t skip = offset;
    size_t bytes_left = size;
    size_t total_written = 0;
    uint8 block[EYNFS_BLOCK_SIZE];

    // Skip blocks and bytes up to offset
    while (block_num && skip >= (EYNFS_BLOCK_SIZE - 4)) {
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        block_num = next_block;
        skip -= (EYNFS_BLOCK_SIZE - 4);
    }

    // Write first partial block
    if (block_num && bytes_left > 0) {
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        size_t block_offset = skip;
        size_t chunk = (EYNFS_BLOCK_SIZE - 4) - block_offset;
        if (chunk > bytes_left) chunk = bytes_left;
        if (chunk) memcpy(block + 4 + block_offset, (const uint8_t*)data, chunk);
        if (eynfs_cache_write_block(drive, block_num, block) != 0) return -1;
        total_written += chunk;
        bytes_left -= chunk;
        block_num = next_block;
    }

    // Write remaining full blocks
    while (block_num && bytes_left > 0) {
        if (eynfs_cache_get_block(drive, block_num, block) != 0) return -1;
        uint32_t next_block = *(uint32_t*)block;
        size_t chunk = (EYNFS_BLOCK_SIZE - 4) < bytes_left ? (EYNFS_BLOCK_SIZE - 4) : bytes_left;
        if (chunk) memcpy(block + 4, (const uint8_t*)data + total_written, chunk);
        if (eynfs_cache_write_block(drive, block_num, block) != 0) return -1;
        total_written += chunk;
        bytes_left -= chunk;
        block_num = next_block;
    }

    return (int)total_written;
}