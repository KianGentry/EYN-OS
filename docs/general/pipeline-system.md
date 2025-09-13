# Pipeline and Redirection System - EYN-OS

EYN-OS includes a Unix-like pipeline and redirection system that enables command chaining and I/O manipulation. **Note: This system is currently in development and not all features are fully functional yet.**

## Current Status

### **Working Features**
- **Basic Output Redirection**: Write to files with `>` operator
- **Append Redirection**: Append to files with `>>` operator  
- **Simple Command Piping**: Basic `|` operator for 2-command pipelines
- **Universal Search Command**: Enhanced `search` command that works in multiple modes

### **In Development**
- **Complex Pipelines**: Multi-command pipelines (3+ commands)
- **Input Redirection**: Read from files with `<` operator
- **Background Execution**: Run commands in background with `&`
- **Advanced File Descriptor Management**: Full Unix file descriptor support

### **Planned Features**
- **Process Substitution**: `<()` and `>()` operators
- **Here Documents**: `<<` operator
- **Signal Handling**: Process control and job management
- **Parallel Execution**: Concurrent pipeline command execution

## Overview

The current pipeline system provides:
- **Command Piping**: Chain commands with `|` operator (2 commands max)
- **Output Redirection**: Write to files with `>` operator
- **Append Redirection**: Append to files with `>>` operator
- **Universal Search**: Enhanced search command for filtering and filesystem search

## Architecture

### Command Structure

```c
typedef struct command {
    char* name;                // Command name
    char** args;               // Command arguments
    int argc;                  // Number of arguments
    command_type_t type;       // Command type
    file_descriptor_t* fds;    // File descriptors
    int fd_count;              // Number of file descriptors
    struct command* next;      // Next command in pipeline
    struct command* prev;      // Previous command in pipeline
    int background;            // Run in background
    int pid;                   // Process ID when running
} command_t;
```

### Pipeline Structure

```c
typedef struct {
    command_t* first;          // First command in pipeline
    command_t* last;           // Last command in pipeline
    int command_count;         // Number of commands
    int background;            // Run entire pipeline in background
} pipeline_t;
```

### File Descriptor Management

```c
typedef struct {
    int fd;                    // File descriptor number
    redirection_type_t type;   // Type of redirection
    char* filename;            // Filename for file redirection
    int pipe_fd;              // Pipe file descriptor
    int is_open;              // Is this descriptor open?
} file_descriptor_t;
```

## Key Features

### Universal Search Command

The `search` command has been enhanced to work as a universal filter and filesystem search tool:

```bash
# Filesystem search (default behavior)
search test.txt                    # Search entire filesystem for "test.txt"

# Command output search
search test.txt ls                 # Search ls output for "test.txt"
search test.txt echo "hello world" # Search echo output

# Specific drive/path search
search test.txt 0:/                # Search drive 0 from root
search test.txt 1:/home            # Search drive 1 from /home

# Pipeline mode (filtering)
ls | search test.txt               # Filter ls output for "test.txt"
echo "hello test.txt world" | search test.txt  # Show matching line
```

**Search Command Features:**
- **Smart Source Detection**: Automatically detects if source is a command or path
- **Drive Support**: Can search specific drives with `drive:path` syntax
- **Pipeline Compatibility**: Works seamlessly in pipelines with `|`
- **Consistent Output**: Shows matching lines in green
- **Error Handling**: Clear error messages for invalid paths/commands

### Command Piping

Pipe the output of one command to the input of another (currently limited to 2 commands):

```bash
# List files and filter by extension
ls | search test.txt

# Search for text in command output
echo "hello world test.txt goodbye" | search test.txt

# Filter command output
ls | search .txt
```

**Note**: Complex pipelines with 3+ commands are not yet supported.

### Input Redirection

Read input from files instead of keyboard:

```bash
# Sort lines from a file
sort < input.txt

# Search for pattern in file
grep "pattern" < data.txt

# Count words in file
wc -w < document.txt

# Process file with multiple commands
cat < input.txt | sort | uniq
```

### Output Redirection

Write command output to files:

```bash
# Save command output to file
ls > file_list.txt

# Append output to existing file
echo "new line" >> log.txt

# Redirect error output
gcc program.c 2> errors.txt

# Redirect both output and errors
command > output.txt 2>&1
```

### Append Redirection

Add output to existing files without overwriting:

```bash
# Add timestamp to log file
date >> log.txt

# Append search results
grep "error" *.log >> all_errors.txt

# Build a list incrementally
echo "item1" >> list.txt
echo "item2" >> list.txt
```

### Background Execution

Run commands in the background:

```bash
# Run command in background
long_running_command &

# Run pipeline in background
ls | grep .txt | sort &

# Multiple background commands
command1 & command2 & command3
```

## File Descriptor Management

### Standard File Descriptors

- **STDIN (0)**: Standard input (keyboard)
- **STDOUT (1)**: Standard output (screen)
- **STDERR (2)**: Standard error output

### File Descriptor Operations

```c
// Allocate a new file descriptor
int fd = allocate_file_descriptor();

// Close a file descriptor
close_fd(fd);

// Duplicate a file descriptor
int new_fd = dup_fd(old_fd);

// Create a pipe
int read_fd, write_fd;
create_pipe(&read_fd, &write_fd);
```

## Background Process Management

### Background Process Structure

```c
typedef struct {
    int pid;                   // Process ID
    char* command;             // Command string
    int status;                // Exit status
    int active;                // Is process active?
} background_process_t;
```

### Background Process Commands

```bash
# List background processes
jobs

# Bring process to foreground
fg <pid>

# Show background process help
bg

# Wait for all background processes
wait
```

## Usage Examples

### Current Working Examples

```bash
# Basic output redirection
echo "Hello World" > output.txt
ls > file_list.txt

# Append redirection
echo "Line 1" > test.txt
echo "Line 2" >> test.txt

# Simple pipeline (2 commands max)
ls | search test.txt
echo "hello world test.txt goodbye" | search test.txt

# Universal search examples
search test.txt                    # Search filesystem
search test.txt ls                 # Search ls output
search test.txt 0:/                # Search specific drive/path
```

### Examples That Don't Work Yet

```bash
# Complex pipelines (3+ commands) - NOT YET SUPPORTED
cat file.txt | sort | uniq | wc -l

# Input redirection - NOT YET SUPPORTED
sort < input.txt

# Background execution - NOT YET SUPPORTED
long_command &

# Multiple pipes with redirection - NOT YET SUPPORTED
ls | grep .txt | sort > text_files.txt
```

### File Processing Examples

```bash
# Process multiple files
cat file1.txt file2.txt | sort > combined.txt

# Filter and count
grep "error" *.log | wc -l

# Extract specific fields
cat data.csv | cut -d',' -f1,3 | sort

# Search and replace
sed 's/old/new/g' < input.txt > output.txt
```

### System Administration Examples

```bash
# Monitor system activity
ps aux | grep process_name

# Check disk usage
df -h | grep /dev/sda

# Monitor network connections
netstat -an | grep LISTEN

# System information
uname -a | tee system_info.txt
```

## Advanced Features

### Custom File Descriptors

```bash
# Use custom file descriptors
command 3< input.txt 4> output.txt

# Redirect specific descriptors
command 2>&1 | grep error
```

### Process Substitution

```bash
# Compare two files
diff <(sort file1.txt) <(sort file2.txt)

# Process multiple inputs
paste <(cut -f1 file1.txt) <(cut -f2 file2.txt)
```

### Conditional Execution

```bash
# Execute only if previous command succeeds
command1 && command2

# Execute only if previous command fails
command1 || command2

# Complex conditions
command1 && command2 | command3 || command4
```

## Integration with Existing Commands

### Enhanced Command Support

All existing EYN-OS commands work with the pipeline system:

```bash
# Filesystem commands
ls | grep .txt
find . -name "*.c" | xargs read

# System commands
memory stats | grep "Used"
portable stats | sort

# Utility commands
random 100 | sort | uniq
calc 2+2 | echo "Result:"
```

### New Pipeline Commands

```bash
# Show pipeline help
pipe

# List background jobs
jobs

# Background process management
bg

# Bring job to foreground
fg <pid>
```

## Performance Characteristics

- **Command Parsing**: O(n) where n is command length
- **Pipeline Execution**: Sequential execution (future: parallel)
- **Memory Usage**: Minimal overhead per command
- **File Descriptor Limits**: 32 descriptors per process
- **Background Process Limits**: 16 concurrent background processes

## Error Handling

### Common Error Scenarios

**File Not Found**
```bash
# Input file doesn't exist
sort < nonexistent.txt
# Error: File not found: nonexistent.txt
```

**Permission Denied**
```bash
# No write permission
ls > /readonly/file.txt
# Error: Permission denied
```

**Command Not Found**
```bash
# Invalid command in pipeline
ls | invalid_command
# Error: Command not found: invalid_command
```

### Error Recovery

- **Graceful Degradation**: Continue pipeline execution when possible
- **Error Propagation**: Pass error codes through pipeline
- **Resource Cleanup**: Automatic cleanup of file descriptors
- **Background Process Management**: Handle terminated background processes

## Future Enhancements

### Planned Features

**Parallel Execution**
- Execute pipeline commands concurrently
- Inter-process communication via pipes
- Process synchronization

**Advanced Redirection**
- Here documents (`<<`)
- Process substitution (`<()`)
- Named pipes (FIFOs)

**Enhanced Background Processing**
- Job control (suspend/resume)
- Signal handling
- Process groups

**Performance Optimizations**
- Command caching
- Smart parsing
- Memory pooling

## Troubleshooting

### Common Issues

**Pipeline Not Working**
```bash
# Check command syntax
pipe

# Verify command exists
help | grep command_name
```

**Background Process Issues**
```bash
# List background processes
jobs

# Check process status
process list
```

**File Descriptor Problems**
```bash
# Check open descriptors
process info

# Monitor system resources
memory stats
```

### Debugging Commands

```bash
# Show pipeline parsing
echo "ls | grep .txt" | pipe

# Check background processes
jobs

# Monitor system activity
status
```

## Conclusion

The pipeline and redirection system transforms EYN-OS into a powerful command-line environment, enabling complex data processing workflows and efficient system administration. It maintains the system's educational focus while providing professional-grade functionality.

The system is designed to be:
- **Intuitive**: Familiar Unix-like syntax
- **Efficient**: Minimal overhead and fast execution
- **Reliable**: Robust error handling and resource management
- **Extensible**: Easy to add new commands and features
