# Contributing to EYN-OS

Welcome to EYN-OS! This guide will help you get started with contributing to our freestanding kernel operating system.

## Quick Start

### Prerequisites
- GCC with 32-bit support (`gcc-multilib` on Ubuntu/Debian)
- NASM assembler
- QEMU for testing
- Python 3 (for development tools)

### Building EYN-OS
```bash
make clean
make
make run  # Runs in QEMU
```

## Adding New Commands

EYN-OS uses a unified command registration system. Adding a new command is simple:

### 1. Create Your Command Handler

Create a new file in `src/utilities/shell/` or add to an existing file:

```c
// In your .c file
#include <shell_command_info.h>
#include <vga.h>
#include <string.h>

void custom_command_handler(string arg) {
    // Parse arguments
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: custom_command <argument>\n", 255, 255, 255);
        return;
    }
    
    // Your command logic here
    printf("%cExecuting custom_command with: %s\n", 0, 255, 0, arg + i);
}

// Register the command
REGISTER_SHELL_COMMAND(custom_command, "custom_command", custom_command_handler, CMD_STREAMING, 
    "My custom command that does something useful.\nUsage: custom_command <argument>", 
    "custom_command example");
```

### 2. Add to Makefile (if new file)

If you created a new `.c` file, add it to the `OBJS` list in `Makefile`:

```makefile
OBJS = ... obj/my_new_command.o
```

### 3. Build and Test

```bash
make clean && make
make run
```

Your command will automatically appear in the help system and be available in the shell!

## Command Types

- **CMD_ESSENTIAL**: Always loaded, core system commands (init, exit, help, etc.)
- **CMD_STREAMING**: Loaded on demand, regular commands
- **CMD_DISABLED**: Not available (for future use)

## Code Style Guidelines

### C Code
- Use `uint8`, `uint32`, etc. from `types.h` instead of standard types
- Include headers with `#include <header.h>` (not relative paths)
- Functions should have descriptive names: `custom_command_handler()`
- Use `printf("%c", r, g, b, ...)` for coloured output
- Keep functions focused and under 100 lines when possible

### Command Handlers
- Always validate input arguments
- Provide helpful usage messages
- Use consistent error handling
- Return early on invalid input

### Example Command Structure
```c
void example_cmd(string arg) {
    // 1. Parse arguments
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: example <required_arg>\n", 255, 255, 255);
        return;
    }
    
    // 2. Extract arguments
    char param[64];
    uint8 j = 0;
    while (arg[i] && arg[i] != ' ' && j < 63) {
        param[j++] = arg[i++];
    }
    param[j] = '\0';
    
    // 3. Validate and execute
    if (strlen(param) == 0) {
        printf("%cError: Invalid parameter\n", 255, 0, 0);
        return;
    }
    
    // 4. Your logic here
    printf("%cProcessing: %s\n", 0, 255, 0, param);
}
```

## File Organization

### Source Files
- `src/entry/`: Kernel entry point
- `src/cpu/`: CPU-related code (IDT, ISR, system calls)
- `src/drivers/`: Hardware drivers (VGA, keyboard, disk)
- `src/utilities/shell/`: Shell commands and utilities
- `src/utilities/games/`: Game engine and games
- `src/utilities/tui/`: Text UI components

### Headers
- `include/`: All public headers
- Use `#include <header.h>`

## Testing Your Changes

### Basic Testing
1. Build: `make clean && make`
2. Run: `make run`
3. Test your command in the shell
4. Check help system: `help`

### Debugging
- Use `printf()` for debugging (avoid debug prints in final code)
- Test edge cases (empty args, invalid input, etc.)
- Verify command appears in help system

## Common Issues

### Build Errors
- **"undefined reference"**: Check if your `.c` file is in the `OBJS` list
- **"undeclared function"**: Add forward declaration for your handler
- **"redefinition"**: Check for duplicate includes or definitions

### Runtime Issues
- **Command not found**: Verify `REGISTER_SHELL_COMMAND` macro is correct
- **Crash**: Check argument parsing and validation
- **Wrong output**: Verify `printf()` colour parameters

## Getting Help

- Check existing commands in `src/utilities/shell/` for examples
- Look at similar commands for patterns
- Test thoroughly before submitting

## Submitting Changes

1. Test your changes thoroughly
2. Ensure your command follows the style guidelines
3. Update documentation if needed
4. Submit a pull request

## Development Tips

- Start with simple commands to learn the system
- Use existing commands as templates
- Test with various input types (empty, long, special characters)
- Keep commands focused and well-documented
- Consider error cases and edge conditions

---

Thank you for contributing to EYN-OS! Your contributions help make this operating system better for everyone. 
