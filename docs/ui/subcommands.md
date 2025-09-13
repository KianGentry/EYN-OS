# EYN-OS Sub-Command System

## Overview

EYN-OS features a comprehensive sub-command system that extends the functionality of base commands through specialized extensions. This system provides **21 new commands** organized into **4 categories**, offering powerful tools for file management, system administration, and debugging.

## Architecture

The sub-command system is implemented in a unified architecture:

- **Single Source File**: `src/utilities/shell/subcommands.c`
- **Single Header**: `include/subcommands.h`
- **Modular Design**: Each category is clearly separated with section headers
- **Consistent Interface**: All sub-commands follow the same parameter parsing pattern

## Command Categories

### Search Sub-Commands (4 commands)

Extend the basic `search` command with specialized file finding capabilities.

#### `search_size <operator> <size>`
Find files based on their size using various comparison operators.

**Operators:**
- `>`, `gt` - Greater than
- `<`, `lt` - Less than  
- `=`, `eq` - Equal to
- `>=`, `gte` - Greater than or equal
- `<=`, `lte` - Less than or equal

**Examples:**
```bash
search_size > 1000          # Files larger than 1000 bytes
search_size < 50            # Files smaller than 50 bytes
search_size = 247           # Files exactly 247 bytes
search_size >= 100          # Files >= 100 bytes
search_size <= 200          # Files <= 200 bytes
```

#### `search_type <extension>`
Find files with specific file extensions.

**Examples:**
```bash
search_type .txt            # Find all .txt files
search_type .game           # Find all .game files
search_type .asm            # Find all assembly files
```

#### `search_empty`
Find empty files and directories throughout the filesystem.

**Output:**
- Empty files are marked as "(empty file)"
- Empty directories are marked as "(empty directory)"

#### `search_depth <max_depth> <pattern>`
Perform pattern-based search with depth limitation.

**Examples:**
```bash
search_depth 2 hello        # Search for "hello" within 2 levels
search_depth 3 .txt         # Search for ".txt" within 3 levels
search_depth 1 test         # Search for "test" in current level only
```

### Help Sub-Commands (7 commands)

Provide detailed, categorized help for different command groups.

#### `help_write`
Detailed help for the text editor and `write` command.

**Covers:**
- Usage syntax
- Editor controls (Ctrl+S, Ctrl+Q, arrow keys)
- File creation and editing features

#### `help_search`
Comprehensive help for search commands and sub-commands.

**Covers:**
- Basic `search` command usage
- All search sub-commands
- Size operators and examples
- Pattern matching techniques

#### `help_fs`
Complete filesystem command reference.

**Covers:**
- Navigation commands (`ls`, `cd`)
- File operations (`read`, `write`, `del`, `copy`, `move`, `size`)
- Directory operations (`makedir`, `deldir`)
- Drive management (`drive`, `drives`)

#### `help_edit`
Text editing and editor controls.

**Covers:**
- `write` command usage
- Editor controls and shortcuts
- File creation and modification
- Editor features and capabilities

#### `help_system`
System utilities and administrative commands.

**Covers:**
- System info (`ver`, `clear`)
- Utilities (`echo`, `calc`, `random`, `sort`)
- History (`history`, `log`)
- Storage (`fdisk`, `format`, `lsata`)

#### `help_game`
Game engine and gaming commands.

**Covers:**
- `game` command usage
- Game file formats (.game)
- Game directory structure
- Example game commands

**Covers:**
- Assembly (`assemble`)
- Execution (`run`)
- Assembly language features
- Development workflow

### LS Sub-Commands (3 commands)

Extend directory listing with different visualization formats.

#### `ls_tree [depth]`
Display directory structure in a tree format with indentation.

**Features:**
- Visual tree connectors (├─)
- Color-coded directories (blue) and files (white)
- Configurable depth (default: 3, max: 10)
- Recursive directory traversal

**Examples:**
```bash
ls_tree              # Tree view with default depth (3)
ls_tree 5            # Tree view with depth 5
ls_tree 10           # Maximum depth tree view
```

#### `ls_size [depth]`
List files and directories with size information.

**Features:**
- Human-readable sizes (B, KB, MB)
- Directory markers ([DIR])
- Configurable depth (default: 1, max: 5)
- Recursive size display

**Examples:**
```bash
ls_size              # Size listing with default depth
ls_size 3            # Size listing with depth 3
```

#### `ls_detail [depth]`
Detailed listing with comprehensive file information.

**Features:**
- File type indicators ([FILE], [DIR])
- Block numbers for directories
- Formatted sizes with block information
- Configurable depth (default: 1, max: 3)

**Output Format:**
```
[DIR] folder/ (block: 7)
[FILE] file.txt (size: 1.2 KB, block: 8)
```

### Filesystem Utility Sub-Commands (7 commands)

Advanced filesystem management and debugging tools.

#### `fsstat`
Display comprehensive filesystem status and statistics.

**Information:**
- Filesystem type and version
- Total blocks and capacity
- Free/used space calculation
- Usage percentage
- Block size and layout

#### `cache_stats`
Show cache performance statistics.

**Statistics:**
- Block cache hits and misses
- Cache hit rate percentage
- Performance metrics
- Cache management tips

#### `cache_clear`
Clear all filesystem caches.

**Purpose:**
- Free memory from block cache
- Reset directory cache
- Clear free block cache
- Force fresh data reads

#### `cache_reset`
Reset cache performance statistics.

**Purpose:**
- Clear hit/miss counters
- Reset performance metrics
- Prepare for new measurements

#### `blockmap`
Visualize block usage with color-coded map.

**Features:**
- Grid-based block display
- Color coding: Green (free), Red (used)
- 64 blocks per line
- Spacing for readability
- Total block count

#### `debug_superblock`
Display detailed superblock information.

**Information:**
- Magic number and version
- Block layout and sizes
- Filesystem parameters
- Bitmap hex dump (first 32 bytes)

#### `debug_directory <path>`
Analyze directory structure and entries.

**Features:**
- Directory block information
- Entry-by-entry analysis
- File/directory type identification
- Block number and size details

**Examples:**
```bash
debug_directory /           # Debug root directory
debug_directory /games      # Debug games directory
```

## Technical Implementation

### File Structure
```
src/utilities/shell/
├── subcommands.c          # Unified implementation
└── shell.c               # Command registration

include/
└── subcommands.h         # Function declarations
```

### Code Organization
```c
// ============================================================================
// SEARCH SUB-COMMANDS
// ============================================================================
// 4 search functions with recursive traversal

// ============================================================================
// HELP SUB-COMMANDS  
// ============================================================================
// 7 help functions with detailed documentation

// ============================================================================
// LS SUB-COMMANDS
// ============================================================================
// 3 ls functions with different display formats

// ============================================================================
// FILESYSTEM UTILITY SUB-COMMANDS
// ============================================================================
// 7 utility functions for system administration
```

### Key Features

1. **Unified Architecture**
   - Single source file for all sub-commands
   - Consistent parameter parsing
   - Shared filesystem access patterns

2. **Recursive Traversal**
   - Depth-limited directory traversal
   - Configurable search depths
   - Memory-efficient implementation

3. **Error Handling**
   - Robust filesystem validation
   - User-friendly error messages
   - Graceful failure recovery

4. **Performance Optimizations**
   - Cached filesystem access
   - Efficient bitmap reading
   - Optimized string operations

## Usage Patterns

### File Management Workflow
```bash
# 1. Explore filesystem structure
ls_tree 3

# 2. Find specific files
search_size > 1000
search_type .txt

# 3. Get detailed information
ls_detail 2

# 4. Monitor system status
fsstat
cache_stats
```

### Debugging Workflow
```bash
# 1. Check filesystem health
fsstat

# 2. Analyze specific directories
debug_directory /games

# 3. Visualize block usage
blockmap

# 4. Examine superblock
debug_superblock
```

### Help System Usage
```bash
# Get help for specific command categories
help_fs          # Filesystem commands
help_search      # Search functionality
help_edit        # Text editing
help_system      # System utilities
```

## Benefits

1. **Enhanced Productivity**
   - Specialized tools for common tasks
   - Reduced command complexity
   - Intuitive command naming

2. **Better Organization**
   - Logical command grouping
   - Consistent interface design
   - Clear documentation structure

3. **Advanced Capabilities**
   - Filesystem debugging tools
   - Performance monitoring
   - Visual data representation

4. **Developer Experience**
   - Comprehensive help system
   - Detailed error messages
   - Consistent command patterns

## Future Enhancements

Potential areas for expansion:

1. **Additional Sub-Commands**
   - `drive_` sub-commands for drive management
   - `system_` sub-commands for system administration
   - `debug_` sub-commands for advanced debugging

2. **Enhanced Features**
   - Command completion
   - Command aliases
   - Scripting capabilities

3. **Performance Improvements**
   - Advanced caching strategies
   - Parallel processing
   - Memory optimization

The sub-command system provides a powerful, extensible foundation for EYN-OS command-line functionality, offering both novice-friendly tools and advanced system administration capabilities. 