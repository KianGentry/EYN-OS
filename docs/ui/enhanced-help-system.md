# Enhanced Help System

## Overview

The EYN-OS help system provides interactive documentation for all commands. It features **collapsible sub-command exploration** and integrates with the Tiling Manager.

## Features

### Visual Indicators
- **Asterisk (*)** marks commands with sub-commands
- **Colour-coded** interface for easy navigation
- **Status bar** with navigation instructions

### Tiling Integration
- **Runs in a Tile**: When the Tiling Manager is active, Help opens in its own tile or window.
- **Mouse Support**: Click to select commands, scroll wheel to navigate list.
- **Resizing**: Adapts layout to the tile size.

### Collapsible Exploration
- **Press Enter** on commands with asterisks to expand/collapse
- **Sub-commands appear** directly below parent command
- **Indented and gray** sub-command display
- **Toggle functionality** - expand and collapse as needed

### User Interface
- **Dual-pane layout**: Commands list + Description
- **Window-based design** with borders and titles
- **Inline expansion** - no separate windows
- **Full-width status bar** at the very bottom of the screen
- **Clean separation** between content and navigation
- **Proper positioning** - status bar below window borders

## How to Use

### 1. Launch Help System
```bash
help
```
- **Tiling Mode**: Opens in a new tile or floating window.
- **Text Mode**: Opens full-screen legacy TUI.

### 2. Navigate Commands
- **Arrow keys** (↑/↓): Move through command list and sub-commands.
- **Mouse**: Click to select, scroll to move.
- **Enter**: Toggle sub-command expansion for selected command.
- **Ctrl+X**: Close the help window/tile.

### 3. Explore Sub-Commands
When you press **Enter** on a command with an asterisk (*), the sub-commands appear directly below:

```
┌─────────────────────┬─────────────────────────────────────────────────────┐
│ Commands            │ Description                                         │
├─────────────────────┼─────────────────────────────────────────────────────┤
│ cd                  │ Change directory                                    │
│ clear               │ Clear screen                                        │
│ copy                │ Copy file                                           │
│ del                 │ Delete file                                         │
│ echo                │ Print text                                          │
│ fsstat *            │ Filesystem status                                   │
│   fsstat            │ Filesystem status                                   │
│   cache_stats       │ Cache statistics                                    │
│   cache_clear       │ Clear all caches                                    │
│   cache_reset       │ Reset cache statistics                              │
│   blockmap          │ Visual block map                                    │
│   debug_superblock  │ Superblock debug info                               │
│   debug_directory   │ Directory debug                                     │
│ help *              │ Show help                                           │
│ ls *                │ List directory contents                             │
│ search *            │ Search for text in files                            │
│ write               │ Edit file                                           │
│                     │                                                     │
│                     │ Sub-commands are expanded.                          │
│                     │ Press Enter to collapse.                            │
└─────────────────────┴─────────────────────────────────────────────────────┘
┌───────────────────────────────────────────────────────────────────────────┐
│ ^/v : Move | Enter: Toggle | Ctrl+X: Exit                                 │
└───────────────────────────────────────────────────────────────────────────┘
```

**Note**: You can now navigate through sub-commands using the arrow keys. The cursor (!) will appear before both main commands and sub-commands when selected. When you select a sub-command, its description and usage information will appear in the right pane.

## Command Categories with Sub-Commands

### Search Commands (*)
- `search_size` - Find files by size
- `search_type` - Find files by extension  
- `search_empty` - Find empty files/directories
- `search_depth` - Search with depth limitation

### LS Commands (*)
- `ls_tree` - Tree view listing
- `ls_size` - Size-based listing
- `ls_detail` - Detailed listing

### Filesystem Commands (*)
- `fsstat` - Filesystem status
- `cache_stats` - Cache statistics
- `cache_clear` - Clear all caches
- `cache_reset` - Reset cache statistics
- `blockmap` - Visual block map
- `debug_superblock` - Superblock debug info
- `debug_directory` - Directory debug

## Interface Design

### Main Help Window (Collapsed)
```
┌─────────────────────┬─────────────────────────────────────────────────────┐
│ Commands            │ Description                                         │
├─────────────────────┼─────────────────────────────────────────────────────┤
│ cd                  │ Change directory                                    │
│ clear               │ Clear screen                                        │
│ copy                │ Copy file                                           │
│ del                 │ Delete file                                         │
│ echo                │ Print text                                          │
│ fsstat *            │ Filesystem status                                   │
│ help *              │ Show help                                           │
│ ls *                │ List directory contents                             │
│ search *            │ Search for text in files                            │
│ write               │ Edit file                                           │
│                     │                                                     │
│                     │ This command has sub-commands.                      │
│                     │ Press Enter to expand.                              │
└─────────────────────┴─────────────────────────────────────────────────────┘
┌───────────────────────────────────────────────────────────────────────────┐
│ ^/v: Move | Enter: Toggle | Ctrl+X: Exit                                  │
└───────────────────────────────────────────────────────────────────────────┘
```

### Main Help Window (Expanded)
```
┌─────────────────────┬─────────────────────────────────────────────────────┐
│ Commands            │ Description                                         │
├─────────────────────┼─────────────────────────────────────────────────────┤
│ cd                  │ Change directory                                    │
│ clear               │ Clear screen                                        │
│ copy                │ Copy file                                           │
│ del                 │ Delete file                                         │
│ echo                │ Print text                                          │
│ fsstat *            │ Filesystem status                                   │
│   fsstat            │ Filesystem status                                   │
│   cache_stats       │ Cache statistics                                    │
│   cache_clear       │ Clear all caches                                    │
│   cache_reset       │ Reset cache statistics                              │
│   blockmap          │ Visual block map                                    │
│   debug_superblock  │ Superblock debug info                               │
│   debug_directory   │ Directory debug                                     │
│ help *              │ Show help                                           │
│ ls *                │ List directory contents                             │
│ search *            │ Search for text in files                            │
│ write               │ Edit file                                           │
│                     │                                                     │
│                     │ Sub-commands are expanded.                          │
│                     │ Press Enter to collapse.                            │
└─────────────────────┴─────────────────────────────────────────────────────┘
┌───────────────────────────────────────────────────────────────────────────┐
│ ^/v: Move | Enter: Toggle | Ctrl+X: Exit                                  │
└───────────────────────────────────────────────────────────────────────────┘
```

## Colour Scheme

- **Yellow**: Selected items and titles
- **White**: Normal text and main commands
- **Gray**: Sub-commands (indented and gray)
- **Black**: Background

## Navigation Controls

### Main Help Window
- **↑/↓**: Navigate through command list and sub-commands
- **Enter**: Toggle sub-command expansion (if available)
- **Ctrl+X**: Exit help system

### Sub-Command Display
- **Inline expansion**: Sub-commands appear directly below parent
- **Indented display**: Sub-commands are indented with spaces
- **Gray colour**: Sub-commands use gray text to distinguish them
- **Cursor support**: Sub-commands can be selected with arrow keys
- **Descriptions**: Sub-command descriptions appear in the right pane when selected
- **Toggle functionality**: Press Enter again to collapse
- **Smart scrolling**: Proper scroll handling for expanded sub-commands

## Technical Implementation

### Data Structures
```c
typedef struct {
    const char* name;
    const char* description;
    const char* usage;
} subcommand_info_t;
```

### Sub-Command Detection
```c
static int has_subcommands(const char* cmd_name) {
    return (strcmp(cmd_name, "search") == 0 ||
            strcmp(cmd_name, "help") == 0 ||
            strcmp(cmd_name, "ls") == 0 ||
            strcmp(cmd_name, "fs") == 0);
}
```

### Expansion Tracking
```c
// Track which commands are expanded
static int expanded_commands[128] = {0};

// Toggle expansion
if (has_subcommands(sorted_cmds[selected]->name)) {
    expanded_commands[selected] = !expanded_commands[selected];
}
```

### Visual Indicators
- Commands with sub-commands are marked with `" *"`
- Asterisk is appended to command names in the display
- Description area shows expansion state
- Sub-commands are indented and coloured gray

## Benefits

### 1. **Intuitive Navigation**
- Commands and sub-commands in the same view
- No separate windows or popups
- Clear visual hierarchy with indentation

### 2. **Space Efficient**
- Uses existing window space
- No additional window management
- Clean, uncluttered interface

### 3. **User Experience**
- Familiar tree-like expansion
- Toggle functionality for easy exploration
- Visual feedback with colours and indentation

### 4. **Extensibility**
- Easy to add new sub-command categories
- Modular design for future enhancements
- Consistent API for sub-command definitions

## Future Enhancements

### 1. **Multi-Level Expansion**
- Support for nested sub-commands
- Deeper indentation levels
- Complex command hierarchies

### 2. **Search and Filter**
- Search within expanded sub-commands
- Filter by category or function
- Quick access to specific commands

### 3. **Contextual Help**
- Show related commands
- Cross-reference between commands
- Usage patterns and workflows

### 4. **Customization**
- User-defined command categories
- Custom help text and examples
- Personalized command shortcuts

## Usage Examples

### Exploring Search Commands
1. Type `help` to open help system
2. Navigate to `search *` using arrow keys
3. Press **Enter** to expand search sub-commands
4. See `search_size`, `search_type`, etc. indented below
5. Press **Enter** again to collapse

### Exploring Filesystem Commands
1. Navigate to `fsstat *` in help system
2. Press **Enter** to see filesystem utility commands
3. View all sub-commands indented and in gray
4. Press **Enter** again to collapse the list

### Learning New Commands
1. Use help system to discover available commands
2. Look for asterisks (*) to find commands with sub-commands
3. Expand sub-commands to understand full functionality
4. Use descriptions and examples to learn proper usage

The enhanced help system provides a **clean, intuitive way** to explore and understand EYN-OS commands and their sub-command capabilities with inline expansion! 