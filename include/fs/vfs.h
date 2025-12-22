// Simple VFS facade to unify access to EYNFS and FAT32 for shell commands
#ifndef VFS_H
#define VFS_H

#include <types.h>
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
} vfs_node_type_t;

typedef struct {
	vfs_node_type_t type; // file or dir
	uint32 size;          // size for files; 0 for directories
} vfs_stat_t;

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

#endif // VFS_H
