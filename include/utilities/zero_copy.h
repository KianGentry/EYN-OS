#ifndef ZERO_COPY_H
#define ZERO_COPY_H

#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>
#include <eynfs.h>

// File operation modes
#define ZERO_COPY_READ_ONLY  0x01
#define ZERO_COPY_WRITE_ONLY 0x02
#define ZERO_COPY_READ_WRITE 0x03
#define ZERO_COPY_APPEND     0x04

// Memory mapping flags
#define ZERO_COPY_PRIVATE    0x10  // Copy-on-write
#define ZERO_COPY_SHARED     0x20  // Shared mapping
#define ZERO_COPY_ANONYMOUS  0x40  // Anonymous mapping

// Zero-copy file handle structure
typedef struct {
    void* mapped_address;           // Memory-mapped address
    uint32_t file_size;            // Size of mapped file
    uint32_t current_offset;       // Current read/write position
    uint8_t drive;                 // Drive number
    uint8_t flags;                 // Operation flags
    uint32_t block_start;          // Starting block number
    uint32_t block_count;          // Number of blocks mapped
    uint32_t access_count;         // Access counter for statistics
    uint8_t dirty;                 // Modified flag for write-back

    // Filesystem metadata needed for safe write-back.
    // These are captured at open() time (from eynfs_traverse_path) and are
    // required to update the directory entry atomically on write-back.
    uint32_t parent_block;
    uint32_t entry_index;
    eynfs_dir_entry_t entry;
} zero_copy_file_t;

// Zero-copy buffer structure for efficient I/O
typedef struct {
    void* buffer;                  // Buffer address
    uint32_t size;                 // Buffer size
    uint32_t offset;               // Offset in file
    uint8_t drive;                 // Drive number
    uint32_t block_start;          // Starting block
    uint8_t dirty;                 // Modified flag
} zero_copy_buffer_t;

// Function prototypes for zero-copy file operations
void zero_copy_init(void);
int zero_copy_open(const char* path, uint8_t flags);
int zero_copy_close(int fd);
size_t zero_copy_read(int fd, void* buf, size_t count);
size_t zero_copy_write(int fd, const void* buf, size_t count);
int zero_copy_seek(int fd, int32_t offset, int whence);
int zero_copy_truncate(int fd, size_t length);

// Memory mapping functions
void* zero_copy_mmap(void* addr, size_t length, uint8_t flags, int fd, int32_t offset);
int zero_copy_munmap(void* addr, size_t length);
int zero_copy_msync(void* addr, size_t length, uint8_t flags);

// Advanced zero-copy operations
int zero_copy_splice(int fd_in, int fd_out, size_t count);
int zero_copy_tee(int fd_in, int fd_out, size_t count);
int zero_copy_vmsplice(int fd, const void* buf, size_t count, uint8_t flags);

// Performance monitoring
uint32_t get_zero_copy_stats(void);
void reset_zero_copy_stats(void);
void print_zero_copy_stats(void);

// Buffer management
zero_copy_buffer_t* create_zero_copy_buffer(uint32_t size);
void destroy_zero_copy_buffer(zero_copy_buffer_t* buffer);
int zero_copy_buffer_read(zero_copy_buffer_t* buffer, void* data, uint32_t offset, uint32_t size);
int zero_copy_buffer_write(zero_copy_buffer_t* buffer, const void* data, uint32_t offset, uint32_t size);

// File system integration
int eynfs_zero_copy_read(uint8_t drive, const eynfs_dir_entry_t* entry, 
                        void** addr, size_t* size, uint32_t offset);
int eynfs_zero_copy_write(uint8_t drive, eynfs_dir_entry_t* entry,
                        const void* data, size_t size, uint32_t offset);

#endif // ZERO_COPY_H 