# EYN-OS Syscall System

EYN-OS implements a basic syscall system that allows user programs to interact with the kernel through software interrupts.

## Syscall Interface

Syscalls are made using the `int 0x80` instruction with the following register convention:

- **EAX**: Syscall number
- **EBX**: First argument
- **ECX**: Second argument  
- **EDX**: Third argument
- **Return value**: In EAX

## Supported Syscalls

### 1. Write (syscall 1)
Writes data to a file descriptor.

**Arguments:**
- EBX: File descriptor (1 for stdout)
- ECX: Buffer address
- EDX: Number of bytes to write

**Example:**
```assembly
mov eax, 1          ; syscall number for write
mov ebx, 1          ; file descriptor (stdout)
mov ecx, message    ; buffer address
mov edx, 13         ; length
int 0x80            ; make syscall
```

### 2. Exit (syscall 2)
Terminates the current process.

**Arguments:**
- EBX: Exit code

**Example:**
```assembly
mov eax, 2          ; syscall number for exit
mov ebx, 0          ; exit code
int 0x80            ; make syscall
```

### 3. Read (syscall 3)
Reads data from a file descriptor.

**Arguments:**
- EBX: File descriptor (0 for stdin)
- ECX: Buffer address
- EDX: Maximum bytes to read

**Returns:**
- EAX: Number of bytes read (0 for now)

**Example:**
```assembly
mov eax, 3          ; syscall number for read
mov ebx, 0          ; file descriptor (stdin)
mov ecx, buffer     ; buffer address
mov edx, 10         ; max bytes
int 0x80            ; make syscall
```

### Selected additional syscalls

#### Sleep in microseconds (syscall 22)
Cooperatively yields and sleeps for at least the specified duration.

**Arguments:**
- EBX: Microseconds to sleep

#### GUI continuous redraw (syscall 23)
Enables or disables continuous redraw for a GUI tile.

**Arguments:**
- EBX: GUI handle
- ECX: 0 to disable, non-zero to enable

#### GUI RGB565 blit (syscall 24)
Copies a RGB565LE framebuffer into the GUI content area.

**Arguments:**
- EBX: GUI handle
- ECX: Pointer to a `gui_blit_rgb565_t`

**Structure:**
```c
typedef struct {
    int src_w, src_h;
    const uint16_t* pixels; // RGB565LE
    int dst_w, dst_h;       // <=0 means use content size
} gui_blit_rgb565_t;
```

## Implementation Details

### Syscall Handler
The syscall handler is implemented in `src/cpu/isr.c` and registered as interrupt 0x80 in the IDT.

### Process Isolation
User programs run in a protected environment with:
- Separate code, stack, and heap memory regions
- Memory access validation
- Process isolation between programs

### Security Features
- Dangerous opcode detection (hlt, cli, sti, int, in, out)
- Memory bounds checking
- Process isolation

## Example Programs

### Simple Hello World
```assembly
section .text
global _start

_start:
    ; Write "Hello, World!"
    mov eax, 1
    mov ebx, 1
    mov ecx, message
    mov edx, 14
    int 0x80
    
    ; Exit
    mov eax, 2
    mov ebx, 0
    int 0x80

section .data
message: db "Hello, World!", 0x0A
```

### Assembly and Execution
```bash
# Assemble the program
assemble hello.asm hello.eyn

# Run the program
run hello.eyn
```

## Future Enhancements

- Additional syscalls (open, close, fork, exec)
- File system syscalls
- Process management syscalls
- Memory management syscalls
- Network syscalls 