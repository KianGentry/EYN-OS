# EYN-OS Shell System

The EYN-OS shell provides a command-line interface for interacting with the operating system. In the modern EYN-OS environment, the shell runs inside **Virtual Terminals** managed by the **Tiling Manager**.

## Shell Architecture

### Core Components
- **Virtual Terminal Integration**: Runs inside a tile or window, supporting standard I/O.
- **Command Parser**: Processes user input and dispatches commands.
- **History System**: Stores and retrieves previous commands.
- **Streaming Command System**: Dynamic command loading for memory efficiency.
- **Command Registry**: Maintains list of available commands.

### Design Philosophy
- **Integrated**: Works seamlessly with the GUI/Tiling environment.
- **Simple and Fast**: Minimal overhead for quick response.
- **User-Friendly**: Clear error messages and helpful feedback.
- **Extensible**: Easy to add new commands.
- **Memory Efficient**: Streaming architecture for low-end systems.

## Streaming Command Architecture

### Essential Commands
Always available in RAM for core functionality:
- **System**: `init`, `exit`, `clear`, `help`
- **Filesystem**: `ls`
- **Memory Management**: `memory`, `portable`, `load`, `unload`, `status`

### Streaming Commands
Loaded on-demand to conserve memory:
- **Filesystem**: `format`, `fdisk`, `fscheck`, `copy`, `move`, `del`, `cd`, `makedir`, `deldir`
- **File Operations**: `read`, `write`, `read_raw`, `read_md`
- **Basic Commands**: `echo`, `ver`, `calc`, `search`, `drive`, `run`
- **Advanced**: `random`, `history`, `sort`, `game`, `draw`, `spam`
- **Development**: `assemble`, `hexdump`, `log`
- **Subcommands**: Various specialized command variants

### Command Loading
```bash
load            # Load all streaming commands into RAM
unload          # Unload streaming commands to free memory
status          # Show which commands are currently loaded
```

## User Interface

### Tiling Environment
The shell typically runs in one of the 4 tiles managed by the Tiling Manager.
- **Focus**: Click a tile to focus it. Keyboard input goes to the focused tile.
- **Scrolling**: Use the mouse wheel to scroll the terminal history.
- **Selection**: You can select text on the input line for editing.

### Prompt Format
```
<drive>:/<path>! 
```
- **Drive**: Current disk drive (0, 1, 2, etc.)
- **Path**: Current working directory
- **!**: Command prompt indicator

### Examples
```
0:/!           # Root directory on drive 0
1:/games!      # Games directory on drive 1
RAM:/!         # RAM disk (special drive)
```

### Special Keys
- **Arrow Keys**: Navigate command history.
- **Backspace**: Delete character.
- **Enter**: Execute command.
- **Escape**: Clear current input.
- **Ctrl+C**: Interrupt current operation.
- **Ctrl+L**: Clear screen (in some contexts).

## Built-in Commands

### System Commands

#### `init`
Initialize full system services (ATA drives, etc.).
```bash
init            # Initialize all system services
```

#### `exit`
Exit the shell (or close the current tile/window).
```bash
exit            # Exit shell session
```

#### `clear`
Clear the terminal screen.
```bash
clear           # Clear terminal screen
```

#### `help`
Launch the interactive Help Viewer.
- In Tiling Mode: Opens a graphical help viewer in a new tile/window.
- In Text Mode: Opens the legacy TUI help browser.
```bash
help            # Launch Help Viewer
```

### Memory Management

#### `load`
Load streaming commands into RAM.
```bash
load            # Load all streaming commands
```

#### `unload`
Unload streaming commands to free memory.
```bash
unload          # Free memory by unloading commands
```

#### `status`
Show command loading status.
```bash
status          # Check which commands are loaded
```

#### `memory [stats|test|stress]`
Memory management and testing.
```bash
memory stats    # Show memory statistics
memory test     # Run memory tests
memory stress   # Stress test memory system
```

#### `portable [stats|optimize]`
Display portability optimizations.
```bash
portable stats  # Show portability statistics
portable optimize # Optimize for current system
```

### Filesystem Commands

#### `ls [path]`
List directory contents.
```bash
ls              # List current directory
ls /games       # List games directory
ls -l           # Long format (if supported)
```

#### `cd <directory>`
Change current working directory.
```bash
cd /games       # Navigate to games directory
cd ..           # Go to parent directory
cd /            # Go to root directory
```

#### `del <filename>`
Delete a file.
```bash
del test.txt    # Delete test.txt
del *.tmp       # Delete all .tmp files (if supported)
```

#### `deldir <directory>`
Delete a directory.
```bash
deldir old_dir  # Delete old_dir directory
```

#### `makedir <directory>`
Create a new directory.
```bash
makedir new_dir # Create new directory
```

### File Operations

#### `read <filename>`
Display text files (.txt) or render markdown (.md). For images, use the GUI viewer commands `view` or `vieww`.
```bash
read test.txt   # Display text file
read image.rei  # Display REI image
read doc.md     # Display markdown with formatting
```

#### `read_raw <filename>`
Display raw file contents.
```bash
read_raw test.bin # Display binary file as text
```

#### `read_md <filename>`
Display markdown files with formatting.
```bash
read_md doc.md  # Display markdown with bold/italic formatting
```

> Note: Image viewing moved to GUI commands:
```bash
view logo.rei   # Viewer in a tile
vieww logo.rei  # Viewer in a floating window
```

#### `write <filename>`
Open text editor for file.
```bash
write test.txt  # Edit test.txt in write editor
```

#### `run <filename>`
Execute an EYN executable file.
```bash
run program.eyn # Execute program.eyn
```

### Development Tools

#### `assemble <input> <output>`
Assemble NASM-compatible assembly code.
```bash
assemble test.asm test.eyn  # Assemble test.asm to test.eyn
```

#### `calc <expression>`
Simple calculator.
```bash
calc 2+2        # Calculate 2+2
calc 10*5       # Calculate 10*5
```

#### `hexdump <filename>`
Display file contents in hexadecimal.
```bash
hexdump file.bin # Show hex dump of file
```

### System Commands

#### `drive <number>`
Switch to different disk drive.
```bash
drive 0         # Switch to drive 0
drive 1         # Switch to drive 1
drive ram       # Switch to RAM disk
```

#### `lsata`
List detected ATA drives.
```bash
lsata           # Show all ATA drives
```

#### `ver`
Show version information.
```bash
ver             # Display EYN-OS version
```

### Utility Commands

#### `echo <text>`
Echo text to screen.
```bash
echo Hello      # Display "Hello"
echo "Hello World" # Display "Hello World"
```

#### `random [max]`
Generate random number.
```bash
random          # Random number 0-32767
random 100      # Random number 0-99
```

#### `sort <filename>`
Sort lines in a file.
```bash
sort data.txt   # Sort data.txt alphabetically
```

#### `search <pattern> [options]`
Search for text in files.
```bash
search "hello"              # Search for "hello" in all files
search "test" -f            # Search filenames only
search "pattern" -c         # Search file contents only
```

#### `game <filename>`
Launch a game.
```bash
game snake      # Launch snake game
game snake.dat  # Launch snake game (with extension)
```

#### `draw <x> <y> <width> <height> <r> <g> <b>`
Draw a rectangle.
```bash
draw 10 20 100 50 255 0 0  # Draw red rectangle
```

#### `spam`
Spam "EYN-OS" 100 times for fun.
```bash
spam            # Display EYN-OS 100 times
```

### Filesystem Management

#### `format <drive>`
Format a drive with EYNFS.
```bash
format 0        # Format drive 0
```

#### `fdisk`
Disk partitioning tool.
```bash
fdisk           # Interactive disk partitioning
```

#### `fscheck`
Check filesystem integrity.
```bash
fscheck         # Check current filesystem
```

### Error and Debug Commands

#### `error [clear|details]`
Display system error statistics.
```bash
error           # Show error statistics
error clear     # Clear error counters
error details   # Show detailed error information
```

#### `validate [test|stats]`
Display input validation statistics.
```bash
validate        # Show validation statistics
validate test   # Run validation tests
validate stats  # Show validation stats
```

#### `process [list|info|kill]`
Display process isolation statistics.
```bash
process         # Show process statistics
process list    # List running processes
process info    # Show process information
```

## Command System

### Command Registration
Commands are registered using the `REGISTER_SHELL_COMMAND` macro:
```c
REGISTER_SHELL_COMMAND("ls", "List directory contents", ls_cmd);
REGISTER_SHELL_COMMAND("cd", "Change directory", cd_cmd);
```

### Command Function Signature
```c
void command_name(string args);
```
- **args**: Command arguments as single string
- **Return**: void (use printf for output)

### Error Handling
Commands should provide clear error messages:
```c
void example_cmd(string args) {
    if (!args || strlen(args) == 0) {
        printf("Error: Missing argument\n");
        printf("Usage: example <argument>\n");
        return;
    }
    // Command logic here
}
```

## Command History

### Features
- **Persistent Storage**: Commands saved between sessions
- **Navigation**: Arrow keys to browse history
- **Search**: Quick access to previous commands
- **Size Limit**: Configurable history size (default: 100 commands)

### Implementation
```c
// Add command to history
add_to_history("ls /games");

// Retrieve from history
const char* cmd = get_history_entry(index);

// Clear history
clear_history();
```

## TUI Integration

### Help Command
The `help` command uses the TUI system to display:
- **Command List**: All available commands
- **Descriptions**: Brief explanation of each command
- **Usage Examples**: How to use commands
- **Navigation**: Arrow keys to browse commands
- **Subcommands**: Expandable sections for command variants

### Game Integration
Games launched via the `game` command:
- **Full Screen**: Take over entire terminal
- **TUI Framework**: Use TUI for game interface
- **Return to Shell**: Clean return after game ends

## Input Processing

### Key Handling
```c
int key = tui_read_key();
switch (key) {
    case 0x1001: // Up arrow
        // Navigate history up
        break;
    case 0x1002: // Down arrow
        // Navigate history down
        break;
    case '\n':   // Enter
        // Execute command
        break;
}
```

### Command Parsing
```c
// Split command and arguments
char* cmd = strtok(input, " ");
char* args = strtok(NULL, "");

// Find and execute command
for (int i = 0; i < command_count; i++) {
    if (strcmp(cmd, commands[i].name) == 0) {
        commands[i].function(args);
        return;
    }
}
printf("Error: Unknown command '%s'\n", cmd);
```

## Performance Features

### Optimizations
- **Command Caching**: Frequently used commands cached
- **Input Buffering**: Efficient keyboard input handling
- **Memory Management**: Minimal memory usage
- **Fast Dispatch**: Quick command lookup and execution
- **Streaming Architecture**: On-demand command loading

### Memory Usage
- **Shell Process**: ~2KB base memory
- **Command History**: ~1KB per 10 commands
- **Input Buffer**: 256 bytes
- **Essential Commands**: ~4KB for core commands
- **Streaming Commands**: Loaded on-demand (~8KB when loaded)

## Future Enhancements

### Planned Features
- **Tab Completion**: Auto-complete filenames and commands
- **Aliases**: Command shortcuts and aliases
- **Scripting**: Basic shell scripting support
- **Job Control**: Background process management
- **Environment Variables**: System and user variables

### Extensibility
- **Plugin System**: Loadable command modules
- **Custom Prompts**: User-configurable prompt format
- **Key Bindings**: Customizable keyboard shortcuts
- **Themes**: Visual customization options

## Development

### Adding New Commands
1. **Implement Function**: Create command function
2. **Register Command**: Use `REGISTER_SHELL_COMMAND` macro
3. **Add to Streaming System**: Include in streaming commands array
4. **Add to Help**: Update help system documentation
5. **Test**: Verify command works correctly

### Example New Command
```c
void custom_cmd(string args) {
    if (!args) {
        printf("Usage: custom_cmd <argument>\n");
        return;
    }
    printf("Custom command executed with: %s\n", args);
}

// In shell initialization
REGISTER_SHELL_COMMAND("custom_cmd", "My custom command", custom_cmd);

// Add to streaming commands array
{"custom_cmd", custom_cmd, CMD_STREAMING, "My custom command", "custom_cmd arg"},
```

---
