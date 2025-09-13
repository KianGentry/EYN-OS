# EYN-OS Shell Scripts

EYN-OS supports shell script execution through `.shell` files, similar to how bash supports `.sh` files. Shell scripts allow you to automate tasks by combining multiple EYN-OS commands into executable files.

## Overview

Shell scripts in EYN-OS provide:
- **Command Automation**: Execute multiple commands in sequence
- **Comment Support**: Use `#` for comments
- **Error Handling**: Track successful and failed commands
- **Line-by-Line Execution**: Each line is executed as a separate command
- **Integration**: Works seamlessly with the existing `run` command

## File Format

### File Extension
Shell scripts must use the `.shell` extension to be recognized by the system.

### Syntax
- **Commands**: Each line contains a single EYN-OS command
- **Comments**: Lines starting with `#` are ignored
- **Empty Lines**: Blank lines are ignored
- **Whitespace**: Leading and trailing whitespace is trimmed

### Example Shell Script
```bash
# This is a comment
echo "Hello from EYN-OS shell script!"

# Show system information
ver
ls
memory stats

# Demonstrate calculator
calc 2+3
calc 10*5

echo "Script completed!"
```

## Usage

### Running Shell Scripts
Use the `run` command to execute shell scripts:

```bash
run demo.shell
run system_info.shell
run my_script.shell
```

### Creating Shell Scripts
1. Create a text file with `.shell` extension
2. Write EYN-OS commands, one per line
3. Add comments with `#` as needed
4. Save the file to your EYNFS filesystem
5. Execute with `run filename.shell`

## Supported Commands

Shell scripts can execute any available EYN-OS command, including:

### Essential Commands
- `init` - Initialize system services
- `ls` - List directory contents
- `clear` - Clear screen
- `help` - Show help information
- `memory` - Memory management
- `portable` - Portability information
- `status` - Command system status

### Filesystem Commands
- `cd` - Change directory
- `makedir` - Create directory
- `deldir` - Delete directory
- `copy` - Copy files
- `move` - Move files
- `del` - Delete files
- `read` - Read files
- `write` - Edit files

### Utility Commands
- `echo` - Print text
- `calc` - Calculator
- `search` - Search files
- `random` - Random number generator
- `sort` - Sort data
- `game` - Games

### System Commands
- `ver` - Version information
- `drive` - Change drive
- `lsata` - List ATA devices
- `format` - Format drive
- `fdisk` - Partition management

## Execution Flow

When a shell script is executed:

1. **File Validation**: System checks for `.shell` extension
2. **File Reading**: Script content is loaded from EYNFS
3. **Line Processing**: Each line is processed sequentially
4. **Command Execution**: Commands are executed using the standard command system
5. **Result Tracking**: Success and error counts are maintained
6. **Summary**: Execution summary is displayed

## Error Handling

### Command Errors
- Individual command failures don't stop script execution
- Error count is tracked and displayed
- Failed commands are logged with line numbers

### File Errors
- Missing files show clear error messages
- Invalid file extensions are rejected
- Memory allocation failures are handled gracefully

## Best Practices

### Script Organization
- Use comments to document script purpose
- Group related commands together
- Add descriptive echo statements for clarity

### Error Prevention
- Test commands individually before scripting
- Use simple, reliable commands
- Avoid complex command combinations

### Performance
- Keep scripts focused and concise
- Avoid unnecessary commands
- Use appropriate command types (essential vs streaming)

## Examples

### System Information Script
```bash
# System Information Script
echo "=== EYN-OS System Information ==="
ver
ls
memory stats
portable stats
status
echo "Information gathering complete!"
```

### File Management Script
```bash
# File Management Script
echo "Creating backup directory..."
makedir backup

echo "Copying important files..."
copy config.txt backup/
copy data.txt backup/

echo "Backup completed!"
ls backup/
```

### System Maintenance Script
```bash
# System Maintenance Script
echo "Starting system maintenance..."

# Check filesystem
fscheck

# Show memory status
memory stats

# Clean up temporary files
del temp_*.txt

echo "Maintenance completed!"
```

## Integration with Native Programs

Shell scripts complement native `.eyn` programs:

- **Shell Scripts**: For command automation and system administration
- **Native Programs**: For complex applications and system utilities
- **Both**: Can be executed with the same `run` command

## Limitations

### Current Limitations
- No variables or parameter passing
- No conditional execution (if/else)
- No loops or iteration
- No function definitions
- No input redirection within scripts

### Future Enhancements
- Variable support
- Conditional statements
- Loop constructs
- Function definitions
- Parameter passing
- Input/output redirection

## Troubleshooting

### Common Issues

**Script not found**
```
Error: Shell script file not found: myscript.shell
```
- Ensure file exists in current directory
- Check filename spelling
- Verify file has `.shell` extension

**Invalid file format**
```
Error: File must have .shell extension
```
- Rename file to use `.shell` extension
- Don't use other extensions like `.sh` or `.txt`

**Command not found**
```
Unknown command: invalidcmd
```
- Check command spelling
- Ensure command is available (may need `load` first)
- Verify command syntax

### Debug Tips
- Test commands individually before scripting
- Use `echo` statements to track execution progress
- Check command availability with `help`
- Verify file permissions and location

## Conclusion

EYN-OS shell scripts provide a powerful way to automate system tasks and create reusable command sequences. While currently focused on simple command execution, they form the foundation for more advanced scripting capabilities in future versions.

The integration with the existing `run` command makes shell scripts a natural extension of the EYN-OS command system, providing users with both interactive and automated ways to interact with the operating system.
