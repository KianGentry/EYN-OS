# EYN-OS Quick Reference

A quick reference guide for EYN-OS commands, features, and common operations.

## Getting Started

### Boot Process
1. **GRUB Menu**: Select EYN-OS from boot menu
2. **Kernel Load**: System initializes drivers and filesystem with dynamic memory detection
3. **Shell Ready**: Command prompt appears: `0:/!`

### Basic Navigation
```bash
ls              # List files and directories
cd <dir>        # Change directory
cd ..           # Go to parent directory
cd /            # Go to root directory
clear           # Clear screen
```

## Streaming Command System

### Essential Commands (Always Available)
```bash
init            # Initialize full system services
ls              # List directory contents
exit            # Exit EYN-OS
clear           # Clear screen
help            # Show interactive help
memory          # Memory management
portable        # Portability information
load            # Load streaming commands
unload          # Unload streaming commands
status          # Show command loading status
```

### Loading Streaming Commands
```bash
load            # Load all streaming commands into RAM
unload          # Unload commands to free memory
status          # Check which commands are loaded
```

## Filesystem Commands

| Command | Description | Example |
|---------|-------------|---------|
| `ls` | List directory contents | `ls /games` |
| `cd` | Change directory | `cd /games` |
| `del` | Delete file | `del test.txt` |
| `deldir` | Delete directory | `deldir old_dir` |
| `makedir` | Create directory | `makedir new_dir` |
| `read` | Display text or markdown | `read config.txt` |
| `read_raw` | Raw file display | `read_raw data.bin` |
| `read_md` | Markdown display | `read_md doc.md` |
| `view`/`vieww` | REI image viewer | `view logo.rei` / `vieww logo.rei` |
| `write` | Edit file | `write document.txt` |
| `copy` | Copy file | `copy source.txt dest.txt` |
| `move` | Move file | `move file.txt /backup/` |
| `format` | Format drive | `format 0` |
| `fdisk` | Disk partitioning | `fdisk` |
| `fscheck` | Check filesystem | `fscheck` |

## System Commands

| Command | Description | Example |
|---------|-------------|---------|
| `drive` | Switch disk drive | `drive 1` |
| `lsata` | List ATA drives | `lsata` |
| `ver` | Show version | `ver` |
| `help` | Show help | `help` |
| `history` | Show command history | `history` |
| `exit` | Exit EYN-OS | `exit` |

## Development Tools

| Command | Description | Example |
|---------|-------------|---------|
| `assemble` | Assemble code | `assemble test.asm test.eyn` |
| `run` | Execute program | `run program.eyn` |
| `calc` | Calculator | `calc 2+2` |
| `hexdump` | Hex dump | `hexdump file.bin` |

## Games & Applications

| Command | Description | Example |
|---------|-------------|---------|
| `game` | Launch game | `game snake` |
| `write` | Text editor | `write file.txt` |
| `draw` | Draw rectangle | `draw 10 20 100 50 255 0 0` |

## Utility Commands

| Command | Description | Example |
|---------|-------------|---------|
| `echo` | Echo text | `echo Hello` |
| `random` | Generate random number | `random 100` |
| `sort` | Sort file lines | `sort data.txt` |
| `search` | Universal search (filesystem/command/pipeline) | `search test.txt` |
| `spam` | Spam EYN-OS | `spam` |

## Pipeline and Redirection (In Development)

**Note**: These features are currently in development. Only basic functionality is available.

| Operator | Description | Example |
|----------|-------------|---------|
| `\|` | Pipe output (2 commands max) | `ls \| search test.txt` |
| `>` | Redirect output (overwrite) | `echo "Hello" > file.txt` |
| `>>` | Redirect output (append) | `echo "World" >> file.txt` |

### Search Command Examples
```bash
search test.txt                    # Search filesystem
search test.txt ls                 # Search ls output  
search test.txt 0:/                # Search drive 0 from root
ls | search test.txt               # Pipeline filtering
```

## Error and Debug Commands

| Command | Description | Example |
|---------|-------------|---------|
| `error` | Error statistics | `error` |
| `validate` | Validation stats | `validate` |
| `process` | Process info | `process` |

## File Operations

### Creating Files
```bash
write newfile.txt    # Create and edit file
```

### Reading Files
```bash
read filename.txt    # Display text or markdown
read_raw data.bin   # Raw file display
read_md doc.md      # Markdown with formatting
view logo.rei        # REI image (tile viewer)
vieww logo.rei       # REI image (window viewer)
```

### Copying Files
```bash
copy source.txt dest.txt  # Copy file
read source.txt > dest.txt  # Alternative copy method
```

### Moving Files
```bash
move file.txt /backup/     # Move file to directory
move old.txt new.txt       # Rename file
```

### Deleting Files
```bash
del filename.txt     # Delete file
deldir directory     # Delete directory
```

## Image Handling

### REI Image Format
EYN-OS supports the custom REI (Raw EYN Image) format for pixel-perfect image display.

```bash
view image.rei              # Display REI image in a tile
vieww image.rei             # Display REI image in a window
```

### Supported Formats
- **REI** (`.rei`) - Native EYN-OS format with pixel-perfect rendering
- **PNG** (`.png`) - Placeholder support (conversion recommended)
- **JPEG** (`.jpg`, `.jpeg`) - Placeholder support (conversion recommended)

### Converting Images
Use the conversion tool to create REI files:
```bash
python3 devtools/png_to_rei.py image.png -o image.rei
python3 devtools/png_to_rei.py --test -o test.rei
```

## Memory Management

### Checking Memory Status
```bash
memory stats    # Show memory statistics
memory test     # Run memory tests
memory stress   # Stress test memory
```

### Portability Information
```bash
portable stats  # Show portability statistics
portable optimize # Optimize for current system
```

## Common Tasks

### Setting Up Development
```bash
makedir projects     # Create projects directory
cd projects          # Navigate to projects
write hello.asm      # Create assembly file
assemble hello.asm hello.eyn  # Assemble code
run hello.eyn        # Execute program
```

### File Management
```bash
ls                   # See what's in current directory
makedir backup       # Create backup directory
copy important.txt backup/important.txt  # Backup file
```

### System Maintenance
```bash
init                 # Initialize full system services
drive 0              # Switch to main drive
ls                   # Check filesystem
format 1             # Format secondary drive
fscheck              # Check filesystem integrity
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| **Arrow Keys** | Navigate command history |
| **Backspace** | Delete character |
| **Enter** | Execute command |
| **Escape** | Clear current input |
| **Ctrl+C** | Interrupt operation |

## Game Controls

### Snake Game
- **WASD**: Move snake
- **Q**: Quit game
- **P**: Pause/unpause

### Write Editor
- **Arrow Keys**: Move cursor
- **Ctrl+O**: Save file
- **Ctrl+X**: Exit editor

## Configuration

### Drive Selection
- **Numeric drives** (0, 1, 2...): Permanent storage
- **RAM drive**: Special memory-based storage

### Path Resolution
- **Absolute paths**: Start with `/` (e.g., `/games/snake.dat`)
- **Relative paths**: No leading `/` (e.g., `games/snake.dat`)

## Troubleshooting

### Common Issues

**"No supported filesystem found"**
- Drive not formatted with EYNFS
- Use `format <drive>` to create filesystem

**"File not found"**
- Check path with `ls`
- Verify filename spelling
- Use `cd` to navigate to correct directory

**"Unknown command"**
- Use `load` to load streaming commands
- Use `help` to see available commands
- Check command spelling
- Use `history` to see previous commands

**"Memory allocation failed"**
- System running low on memory
- Use `unload` to free command memory
- Use `memory stats` to check memory status
- Restart system if necessary

**"Command not found"**
- Command may be in streaming system
- Use `load` to load all commands
- Use `status` to check loaded commands

### Getting Help
```bash
help                # Show interactive help system
load                # Load all streaming commands
status              # Check command loading status
```

## System Information

### Memory Layout
- **Kernel**: 0x00100000 - 0x001FFFFF
- **Available**: 0x00200000 - 0x007FFFFF (adaptive heap)
- **High Memory**: 0x00800000+ (if available)

### Memory Requirements
- **Minimum**: 3MB RAM (with GRUB bootloader), 1MB RAM (with direct boot)
- **Recommended**: 8MB+ RAM (for comfortable usage with all features)
- **Optimal**: 16MB+ RAM (for full performance and multitasking)
- **Note**: GRUB bootloader requires additional memory for its internal scripting system

### Filesystem
- **EYNFS**: Native filesystem
- **FAT32**: Compatible filesystem
- **Block Size**: 512 bytes
- **Superblock**: LBA 2048

### Hardware Support
- **VGA**: 80x25 text mode
- **PS/2 Keyboard**: Full keyboard support
- **ATA/IDE**: Hard disk access
- **Serial**: Basic serial communication

## Advanced Features

### Streaming Command System
- **Essential Commands**: Always available in RAM
- **Streaming Commands**: Loaded on-demand to conserve memory
- **Dynamic Loading**: Use `load`/`unload` to manage memory usage
- **Status Tracking**: Use `status` to see loaded commands

### Command History
- **Navigation**: Arrow keys to browse history
- **Search**: Quick access to previous commands
- **Persistence**: History saved between sessions

### TUI System
- **Consistent Interface**: All TUI apps use same framework
- **Color Support**: Multiple colors for different elements
- **Keyboard Handling**: Unified input processing
- **Dual-Pane Layout**: Interactive help system

### File Format Support
- **File Display**: `read` shows text/markdown. Use `view`/`vieww` for images
- **REI Images**: Native image format with pixel-perfect rendering
- **Markdown**: Formatted text display with bold/italic support
- **Raw Data**: Binary file display with hex dump support