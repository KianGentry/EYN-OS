// Simple VFS facade to unify access to EYNFS and FAT32 for shell commands
#ifndef VFS_H
#define VFS_H

#include <misc/types.h>
#include <stddef.h>

// Filesystem types known to the VFS
typedef enum {
	VFS_FS_NONE = 0,
	VFS_FS_EYNFS = 1,
	VFS_FS_FAT32 = 2,
} vfs_fs_type_t;

// Node types
typedef enum {
	VFS_NODE_NONE = 0,
	VFS_NODE_FILE = 1,
	VFS_NODE_DIR  = 2,
	VFS_NODE_FIFO = 3,
} vfs_node_type_t;

typedef struct {
	vfs_node_type_t type; // file or dir
	uint32 size;          // size for files; 0 for directories
} vfs_stat_t;

/*
 * ABI-INVARIANT: Reserved logical drive ID for read-only RAM install media.
 *
 * Why: Installer workflows need a stable, non-ATA-backed source namespace
 *      (`RAM:/`) for files loaded by the bootloader as multiboot modules.
 * Invariant: No physical ATA device is mapped to this ID.
 * Breakage if changed: Userland paths and installer tooling that rely on
 *                      `RAM:/` would stop resolving to the in-memory payload.
 * ABI-sensitive: Yes (path/drive contract).
 * Security-critical: Yes (explicitly isolates memory-backed media from disk IDs).
 */
#define VFS_DRIVE_RAM 0xFEu

// Detect which filesystem is present on the given drive
vfs_fs_type_t vfs_detect(uint8 drive);

// Read an entire file by absolute or relative path into buf. Returns bytes read or <0 on error.
// Paths are resolved in the caller before passing here when needed.
int vfs_read_file(uint8 drive, const char* path, void* buf, int bufsize);

// Read a file starting at byte offset into buf. Returns bytes read or <0 on error.
int vfs_read_file_at(uint8 drive, const char* path, void* buf, int bufsize, uint32 offset);

// Get file size for a path. Returns 0 on success and fills out_size; <0 on error.
int vfs_get_file_size(uint8 drive, const char* path, uint32* out_size);

// Stat: get node type and size
int vfs_stat(uint8 drive, const char* path, vfs_stat_t* st);

typedef int (*vfs_listdir_typed_cb_t)(const char* name, vfs_node_type_t type, uint32 size, void* user);

// List directory entries with explicit node type information.
int vfs_listdir_typed(uint8 drive, const char* path, vfs_listdir_typed_cb_t cb, void* user);

// List directory entries via callback; callback returns 0 to continue, non-zero to stop
// name: entry name, is_dir: 1 if directory, size: file size for files
int vfs_listdir(uint8 drive, const char* path,
				int (*cb)(const char* name, int is_dir, uint32 size, void* user),
				void* user);

// Write a file, creating or overwriting it at path. Returns bytes written or <0 on error.
// For now, on FAT32 supports 8.3 names and overwrites by truncating the old chain.
int vfs_write_file(uint8 drive, const char* path, const void* buf, uint32 size);

// Create a directory at path (parent must exist). Returns 0 on success, <0 on error.
int vfs_mkdir(uint8 drive, const char* path);

// Remove an empty directory at path (must be empty). Returns 0 on success, <0 on error.
int vfs_rmdir(uint8 drive, const char* path);

// Remove a file at path. Returns 0 on success, <0 on error.
int vfs_unlink(uint8 drive, const char* path);

// RAM disk raw sector bridge used by low-level storage paths when drive == VFS_DRIVE_RAM.
int vfs_ramdisk_read_sector(uint32 lba, uint8 out[512]);
int vfs_ramdisk_write_sector(uint32 lba, const uint8 in[512]);
int vfs_ramdisk_total_sectors(uint32* out_total);

#endif // VFS_H
