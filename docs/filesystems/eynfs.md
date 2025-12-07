# EYNFS - EYN-OS Native Filesystem

EYNFS is the native filesystem for EYN-OS, designed for simplicity and performance.

## Overview

EYNFS is a simple, block-based filesystem that supports:
- Files and directories
- Multi-block directory chains (supports up to 128 entries per directory)
- Efficient block allocation
- Caching for performance
- Memory protection and process isolation

## Filesystem Structure

### Beginner's Guide: How EYNFS is Organized
Think of the hard drive as a giant notebook with numbered pages (Blocks).
- **The Cover (Superblock)**: Page 2048. It tells you the title (EYNFS), how many pages there are, and where the Table of Contents starts.
- **Table of Contents (Root Directory)**: Lists the files and folders.
- **Chapters (Files)**: The actual data.
- **Page Numbers (LBA)**: "Logical Block Address". A unique number for every 512-byte chunk of the disk.

### Disk Layout Diagram
```
LBA 0       LBA 2048    LBA 2049    LBA 2050...
┌───────────┬───────────┬───────────┬───────────────────────┐
│   MBR     │Superblock │ Root Dir  │     Data Blocks       │
│ Partition │  (Info)   │  (Start)  │   (Files & Dirs)      │
└───────────┴───────────┴───────────┴───────────────────────┘
```

### Superblock Structure
Located at LBA 2048 (start of partition).
```c
struct eynfs_superblock {
    char magic[4];      // "EYNF"
    uint32 version;     // Filesystem version
    uint32 block_size;  // 512 bytes
    uint32 total_blocks;// Partition size
    uint32 root_block;  // LBA of root directory
    // ...
};
```

### Directory Entry Structure
Each file or folder is described by a 52-byte entry.
```
 0                   32  33  34      36          40          44          52
 ┌───────────────────┬───┬───┬───────┬───────────┬───────────┬───────────┐
 │     Filename      │Typ│Flg│ Rsvd  │ File Size │Start Block│ Reserved  │
 │    (32 bytes)     │ e │ s │       │ (4 bytes) │ (4 bytes) │ (8 bytes) │
 └───────────────────┴───┴───┴───────┴───────────┴───────────┴───────────┘
```
- **Type**: 1=File, 2=Directory
- **Start Block**: Where the file's data begins.

### Directory Block Chaining
Directories can grow larger than one block. They form a linked list.
```
Block N (Directory)        Block N+1 (Directory)
┌──────────────────┐      ┌──────────────────┐
│ Entry 1          │      │ Entry 10         │
│ Entry 2          │      │ Entry 11         │
│ ...              │      │ ...              │
│ Entry 9          │      │ Entry 18         │
├──────────────────┤      ├──────────────────┤
│ Next Block: N+1  │─────►│ Next Block: 0    │ (End)
└──────────────────┘      └──────────────────┘
```

## Key Features

### Multi-Block Directory Support
- **Previous Limitation**: Only 9 entries per directory
- **Current Implementation**: Supports up to 128 entries per directory
- **Block Chaining**: Directories automatically span multiple blocks as needed
- **Efficient Reading**: Directory reading traverses block chains seamlessly

### Performance Optimizations
- Block caching (16-entry cache)
- Directory entry caching (8-entry cache)
- Free block tracking
- Binary search for directory entries

### Security Features
- Memory bounds checking
- Process isolation
- Dangerous opcode detection
- File access validation

## Usage

### Creating Files
```bash
write filename.txt "content"
```

### Creating Directories
```bash
makedir directory_name
```

### Listing Contents
```bash
ls [depth]
```

### File Operations
```bash
read filename.txt
copy source.txt dest.txt
del filename.txt
```

## Implementation Details

### Directory Reading
The `eynfs_read_dir_table` function:
- Reads directory blocks in sequence
- Follows next_block pointers
- Supports up to 128 entries (configurable)
- Returns actual number of entries found

### Memory Management
- Dynamic allocation for large directory listings
- Proper memory cleanup
- Stack overflow prevention

### Block Allocation
- Efficient free block tracking
- Automatic block chaining for directories
- Garbage collection for deleted files

## Performance Characteristics

- **Directory Reading**: O(n) where n is number of entries
- **File Access**: O(1) for small files, O(n) for large files
- **Memory Usage**: Configurable limits (128 entries max)
- **Cache Hit Rate**: Typically >80% for repeated access

## Future Enhancements

- Extended attributes
- Symbolic links
- Compression
- Encryption
- Journaling for crash recovery