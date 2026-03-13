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
- EBX: File descriptor (stdout is `1`; open FDs and pipe ends are also supported)
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
- EBX: File descriptor (stdin is `0`; open FDs and pipe ends are also supported)
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

Notes:
- Paths created by `mkfifo` resolve to persistent FIFO metadata files (`VFS_NODE_FIFO`) and runtime FIFO objects.
- For FIFO paths, `O_RDONLY` opens the read end, `O_WRONLY`/`O_RDWR` open the write end.
- `O_NONBLOCK` is honored for pipe/FIFO endpoints.

#### Close (syscall 5)
Close an open file descriptor.

**Arguments:**
- EBX: fd

#### Anonymous pipe (syscall 124)
Create an anonymous unidirectional byte-stream pipe.

**Arguments:**
- EBX: `int pipefd[2]` user pointer

**Returns:**
- EAX: `0` on success, `-1` on failure

On success, `pipefd[0]` is the read end and `pipefd[1]` is the write end.

#### Named pipe create (syscall 125)
Create a named FIFO endpoint and persist its metadata in the filesystem.

**Arguments:**
- EBX: `const char* path`

**Returns:**
- EAX: `0` on success, `-1` on failure

Notes:
- FIFO names are opened with `open(path, O_RDONLY|O_WRONLY|O_RDWR, ...)`.
- FIFO metadata is persisted as a marker file and surfaced as `VFS_NODE_FIFO` by `vfs_stat`.

#### Duplicate file descriptor (syscall 126)
Create a duplicate of an open file descriptor.

**Arguments:**
- EBX: `oldfd`

**Returns:**
- EAX: `newfd` on success, `-1` on failure

#### Duplicate to target descriptor (syscall 127)
Duplicate `oldfd` into a caller-selected descriptor.

**Arguments:**
- EBX: `oldfd`
- ECX: `newfd`

**Returns:**
- EAX: `newfd` on success, `-1` on failure

Notes:
- `newfd` values `0`, `1`, `2` remap stdin/stdout/stderr to the duplicated endpoint.

#### Set FD inheritance mode (syscall 128)
Control whether ring3 FD table entries survive `exit`/`run` transitions.

**Arguments:**
- EBX: `enabled` (`0` or `1`)

**Returns:**
- EAX: previous mode (`0` or `1`)

#### Set stdio FD mapping (syscall 129)
Explicitly set the active stdin/stdout/stderr backing descriptors.

**Arguments:**
- EBX: stdin fd
- ECX: stdout fd
- EDX: stderr fd

**Returns:**
- EAX: `0` on success, `-1` on failure

#### Set FD non-blocking mode (syscall 130)
Toggle non-blocking behavior on an open FD.

**Arguments:**
- EBX: fd
- ECX: enabled (`0` or `1`)

**Returns:**
- EAX: `0` on success, `-1` on failure

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

#### DNS + TCP networking (syscalls 119–123)
Minimal userland access to DNS resolution and TCP streams.

**DNS resolve (syscall 119)**
- EBX: `const char* name`
- ECX: `uint8_t out_ip[4]`
- Returns: 0 on success, <0 on error

**TCP connect (syscall 120)**
- EBX: `const uint8_t dst_ip[4]`
- ECX: `uint16_t dst_port`
- EDX: `uint16_t local_port` (0 = ephemeral)
- Returns: 0 on success, <0 on error

**TCP send (syscall 121)**
- EBX: `const void* buf`
- ECX: `uint32_t len` (<= 512)
- Returns: bytes sent or <0 on error

**TCP recv (syscall 122)**
- EBX: `void* buf`
- ECX: `uint32_t buflen`
- Returns: bytes received, 0 if none, -2 if closed, or <0 on error

**TCP close (syscall 123)**
- Returns: 0 on success, <0 on error

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

## Command parity syscalls (56–68)

These syscalls support userland ports of shell commands that need kernel-owned state.

| # | Name | Description |
|---|---|---|
| 56 | `DRIVE_SET_LOGICAL` | Set current logical drive |
| 57 | `DRIVE_GET_LOGICAL` | Get current logical drive |
| 58 | `DRIVE_GET_COUNT` | Number of logical drives |
| 59 | `DRIVE_IS_PRESENT` | Test whether a logical drive is present |
| 60 | `INIT_SERVICES` | Run core init services (ATA init + netcfg autoload) |
| 61 | `SERIAL_WRITE_COM1` | Write bytes to COM1 |
| 62 | `SHELL_LOG_SET` | Enable/disable shell logging |
| 63 | `SHELL_LOG_GET` | Query shell logging state |
| 64 | `CRASHLOG_COUNT` | Get crashlog record count |
| 65 | `CRASHLOG_INFO` | Get record metadata by index |
| 66 | `CRASHLOG_DATA` | Get record payload by index |
| 67 | `CRASHLOG_CLEAR` | Clear all crashlog records |
| 68 | `SHELL_MIGRATED_DISPATCH` | Run a migrated shell command handler from userland |

## Networking and system utility syscalls (69–104)

| # | Name | Description |
|---|---|---|
| 69 | `PCI_GET_COUNT` | Number of PCI devices (optionally network-only) |
| 70 | `PCI_GET_ENTRY` | Get PCI entry by index |
| 71 | `E1000_PROBE` | Probe for an e1000 NIC and return info |
| 72 | `E1000_INIT` | Initialise the e1000 NIC |
| 73 | `NETCFG_GET` | Get current network config (IP, gateway, …) |
| 74 | `NETCFG_SET` | Set network config |
| 75 | `NETCFG_DEFAULTS` | Reset network config to defaults |
| 76 | `NET_IS_INITED` | Returns 1 if the network stack is running |
| 77 | `NET_GET_MAC` | Get the current MAC address |
| 78 | `NET_GET_ARP` | Copy ARP cache entries into user buffer |
| 79 | `NET_GET_UDP_STATS` | UDP packet/drop counters |
| 80 | `NET_GET_IP_STATS` | IPv4 fragment and ICMP counters |
| 81 | `NET_GET_SOCKETS` | Enumerate open sockets |
| 82 | `NET_PING` | Send ICMP echo requests |
| 83 | `FS_CHECK_INTEGRITY` | Run filesystem integrity check |
| 84 | `FS_FATFIX` | Fix FAT-style directory entries at path |
| 85 | `VTERM_CLEAR` | Clear the active virtual terminal |
| 86 | `HISTORY_COUNT` | Number of history entries |
| 87 | `HISTORY_ENTRY` | Read a history entry by index |
| 88 | `HISTORY_CLEAR` | Clear all history entries |
| 89 | `BG_JOB_COUNT` | Number of background jobs |
| 90 | `BG_JOB_INFO` | Get info for a background job by index |
| 91 | `TILING_START` | Start the tiling manager (GUI entry point) |
| 92 | `SETBG_PATH` | Set tile background from a REI file path |
| 93 | `CLEARBG_FOCUSED` | Clear background for the focused tile |
| 94 | `SETFONT_PATH` | Load a `.hex` bitmap font system-wide |
| 95 | `CHDIR` | Change working directory |
| 96 | `RUN` | Execute a UELF binary by path |
| 97 | `WRITE_EDITOR` | Open the text editor on a file |
| 98 | `MMAP` | Map a region of memory |
| 99 | `MUNMAP` | Unmap a region |
| 100 | `MSYNC` | Sync a mapped region to disk |
| 101 | `PAGING_GUARDS` | Enable/disable paging guard pages |
| 102 | `PANIC` | Trigger a kernel panic (diagnostic/test) |
| 103 | `PF` | Trigger a test page fault |
| 104 | `RING3` | Run code in ring 3 (internal loader path) |

## Additional GUI draw syscalls (105–108)

These extend the immediate-mode drawing API with per-character and icon rendering.

#### GUI draw icon (syscall 105)
Draw a named icon from the kernel icon cache at pixel position `(x, y)`.

**Arguments:**
- EBX: GUI handle
- ECX: Pointer to `gui_icon_t { int x, y; const char* icon_name; }`

Icons are loaded from `/icons16/` with `/icons/` as fallback. If the icon is not found, nothing is drawn.

#### GUI outline rectangle (syscall 106)
Draw a 1-pixel border rectangle with no fill. Uses the same `gui_rect_t` layout as `gui_fill_rect`.

**Arguments:**
- EBX: GUI handle
- ECX: Pointer to `gui_rect_t`

#### GUI draw character (syscall 107)
Draw a single character at a pixel position. More efficient than `gui_draw_text` for per-character rendering (e.g. text editors).

**Arguments:**
- EBX: GUI handle
- ECX: Pointer to `gui_char_t { int x, y, ch; unsigned char r, g, b, _pad; }`

#### GUI font metrics (syscall 108)
Query the character cell size for the active font on a GUI handle.

**Arguments:**
- EBX: GUI handle
- ECX: Pointer to output `gui_font_metrics_t { int char_w, char_h; }`

**Returns:**
- EAX: 0 on success, -1 on invalid handle

## Capability-based GUI syscalls (28–46)

The capability syscalls mirror the plain GUI calls but accept a capability token instead of a raw handle. They are intended for programs that have been given access to a GUI surface through a capability rather than creating one themselves.

| # | Name | Mirrors |
|---|---|---|
| 28 | `CAP_MINT_FD` | -- (mint a file-descriptor capability) |
| 29–41 | `CAP_GUI_*` | `GUI_BEGIN` … `GUI_BLIT_RGB565` |
| 42 | `CAP_GUI_CLOSE` | `wm_close_window` |
| 43 | `CAP_FD_WRITE` | file write via capability |
| 44 | `CAP_FD_SEEK` | file seek via capability |
| 45 | `CAP_GUI_CREATE` | `gui_create` (returns capability) |
| 46 | `CAP_GUI_ATTACH` | `gui_attach` (returns capability) |

## Deterministic execution mode (47–48)

| # | Name | Description |
|---|---|---|
| 47 | `DET_ENABLE` | Enable/disable deterministic scheduling for the current task |
| 48 | `DET_STEP` | Advance by at most N scheduler events while in deterministic mode |

## Audio syscalls (113–117)

Audio playback is provided through the AC97 driver. The output format is fixed at 48 kHz stereo 16-bit signed little-endian. User programs must resample their data to this format before writing.

### Audio Probe (syscall 113)

Scans the PCI bus for an Intel AC97 audio controller (vendor 0x8086, device 0x2415).

**Arguments:** none

**Returns:** EAX: 0 if AC97 hardware found, -1 otherwise

**Capability:** `CAP_DEV_AUDIO`

### Audio Init (syscall 114)

Initializes the AC97 hardware: performs cold reset, configures codec registers, allocates DMA buffer descriptor list and double-buffered DMA buffers, and registers the IRQ handler.

**Arguments:** none

**Returns:** EAX: 0 on success, -1 on failure

**Capability:** `CAP_DEV_AUDIO`, `CAP_ALLOC_MEMORY`

### Audio Write (syscall 115)

Submits a buffer of PCM audio data (48 kHz stereo s16le) for playback. The data is copied from userspace into a kernel buffer and then forwarded to the AC97 DMA engine.

**Arguments:**
- EBX: Pointer to PCM data buffer (userspace)
- ECX: Size in bytes (max 4096 per call)

**Returns:** EAX: number of bytes queued, or -1 on error

**Capability:** `CAP_DEV_AUDIO`

### Audio Stop (syscall 116)

Stops audio playback and resets the DMA engine.

**Arguments:** none

**Returns:** EAX: 0

**Capability:** none

### Audio Is Available (syscall 117)

Checks whether the AC97 driver has been initialized and is ready for playback.

**Arguments:** none

**Returns:** EAX: 1 if available, 0 if not

**Capability:** none

Deterministic mode is used by test harnesses that need reproducible event ordering.and wrappers.