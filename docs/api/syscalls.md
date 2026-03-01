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

#### Open (syscall 4)
Open a file or directory for reading.

**Arguments:**
- EBX: `const char* path`
- ECX: flags (currently minimal)
- EDX: mode (unused today)

#### Close (syscall 5)
Close an open file descriptor.

**Arguments:**
- EBX: fd

#### Get directory entries (syscall 7)
Read directory entries from an open directory fd.

**Arguments:**
- EBX: fd
- ECX: output buffer (`eyn_dirent_t[]`)
- EDX: buffer size in bytes

#### Write entire file (syscall 21)
Create/overwrite a file with the given contents.

**Arguments:**
- EBX: `const char* path`
- ECX: `const void* buf`
- EDX: length in bytes

Notes:
- A length of 0 creates/truncates the file to empty.

#### Make directory (syscall 49)
Create a directory.

**Arguments:**
- EBX: `const char* path`

#### Unlink file (syscall 50)
Delete a file.

**Arguments:**
- EBX: `const char* path`

#### Remove directory (syscall 51)
Delete an empty directory.

**Arguments:**
- EBX: `const char* path`

#### Get current working directory (syscall 52)
Query the current working directory as tracked by the active vterm.

**Arguments:**
- EBX: `char* buf`
- ECX: buffer size in bytes

**Returns:**
- EAX: bytes written excluding the terminating NUL (or -1 on error)

#### EYNFS streaming writer (syscalls 53–55)
Low-memory streaming writer for creating/overwriting a file without buffering the full contents.

Notes:
- Currently supported only for EYNFS.

**Begin (syscall 53)**
- EBX: `const char* path`
- Returns EAX: stream handle (>=0) or -1

**Write (syscall 54)**
- EBX: stream handle
- ECX: `const void* buf`
- EDX: length in bytes
- Returns EAX: bytes written or -1

**End (syscall 55)**
- EBX: stream handle
- Returns EAX: 0 on success or -1

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

- Additional syscalls (fork, exec)
- Process management syscalls
- Memory management syscalls
- Network syscalls 

## Command parity syscalls (56-68)

These syscalls support no-compromise userland ports for shell commands that
need kernel-owned state.

- `56` `DRIVE_SET_LOGICAL`: set current logical drive.
- `57` `DRIVE_GET_LOGICAL`: get current logical drive.
- `58` `DRIVE_GET_COUNT`: get number of logical drives.
- `59` `DRIVE_IS_PRESENT`: test whether a logical drive is present.
- `60` `INIT_SERVICES`: run core init services (ATA init + netcfg autoload).
- `61` `SERIAL_WRITE_COM1`: write bytes to COM1.
- `62` `SHELL_LOG_SET`: enable/disable shell logging.
- `63` `SHELL_LOG_GET`: query shell logging state.
- `64` `CRASHLOG_COUNT`: get crashlog record count.
- `65` `CRASHLOG_INFO`: get record metadata by index.
- `66` `CRASHLOG_DATA`: get record payload by index.
- `67` `CRASHLOG_CLEAR`: clear all crashlog records.
- `68` `SHELL_MIGRATED_DISPATCH`: run migrated shell command handlers from userland wrappers.