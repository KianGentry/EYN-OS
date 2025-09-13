#include <zero_copy.h>
#include <util.h>
#include <vga.h>
#include <string.h>
#include <eynfs.h>
#include <stdint.h>

// Global zero-copy state
static zero_copy_file_t* g_zero_copy_files = NULL;  // Dynamic array
static uint8_t g_zero_copy_max_files = 16;  // Default limit
static uint8_t g_zero_copy_fd_count = 0;
static uint32_t g_zero_copy_stats = 0;
static uint32_t g_zero_copy_operations = 0;

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
}

// Open file with zero-copy operations
int zero_copy_open(const char* path, uint8_t flags) {
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
    g_zero_copy_files[g_zero_copy_fd_count].access_count = 0;
    g_zero_copy_files[g_zero_copy_fd_count].dirty = 0;
    
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
    
    // If file was modified, write back to disk (TODO)
    if (file->dirty && (file->flags & ZERO_COPY_WRITE_ONLY || file->flags & ZERO_COPY_READ_WRITE)) {
        // TODO: Implement write-back to filesystem
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
    if (fd < 0 || fd >= g_zero_copy_fd_count) {
        printf("%cError: Invalid file descriptor: %d\n", 255, 0, 0, fd);
        return -1;
    }
    
    zero_copy_file_t* file = &g_zero_copy_files[fd];
    
    if (!(file->flags & ZERO_COPY_WRITE_ONLY) && !(file->flags & ZERO_COPY_READ_WRITE)) {
        printf("%cError: File not opened for writing\n", 255, 0, 0);
        return -1;
    }
    
    if (file->current_offset + count > file->file_size) {
        // TODO: Implement file extension safely
        printf("%cError: Cannot extend file size in zero-copy mode\n", 255, 0, 0);
        return -1;
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
        return mapped_addr;
    }
}

// Unmap zero-copy memory
int zero_copy_munmap(void* addr, size_t length) {
    // TODO: Track anonymous mappings to free them explicitly.
    return 0;
}

// Synchronize zero-copy memory to disk
int zero_copy_msync(void* addr, size_t length, uint8_t flags) {
    for (int i = 0; i < g_zero_copy_fd_count; i++) {
        zero_copy_file_t* file = &g_zero_copy_files[i];
        if (file->mapped_address && 
            addr >= file->mapped_address && 
            addr < (uint8_t*)file->mapped_address + file->file_size) {
            if (file->dirty) {
                // TODO: Implement actual write-back to filesystem
                file->dirty = 0;
            }
            return 0;
        }
    }
    printf("%cError: Address not mapped\n", 255, 0, 0);
    return -1;
}

// Advanced zero-copy operations
int zero_copy_splice(int fd_in, int fd_out, size_t count) {
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
        zero_copy_file_t* file = &g_zero_copy_files[i];
        if (file->mapped_address) {
            printf("%cFile %d: size=%d, offset=%d, accesses=%d, dirty=%d\n", 255, 255, 255, i, file->file_size, file->current_offset, file->access_count, file->dirty);
        }
    }
} 