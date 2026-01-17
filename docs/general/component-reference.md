# EYN-OS Component Implementation Reference

Quick reference for understanding and debugging each major OS component.

## Table of Contents
- [Memory Management](#memory-management)
- [Interrupt Handling](#interrupt-handling)
- [Filesystems](#filesystems)
- [Networking](#networking)
- [Tiling Manager](#tiling-manager)
- [Shell & Commands](#shell--commands)
- [Userspace](#userspace)

---

## Memory Management

### Heap (`src/mm/heap.c`)

**Implementation**: Best-fit allocator with block coalescing

**Key Structures**:
```c
typedef struct heap_block {
    uint32_t magic;       // 0xDEADBEEF
    uint32_t size;        // Block size (includes header)
    uint32_t checksum;    // Integrity check
    uint8_t free;         // 0=allocated, 1=free
    struct heap_block* next;
} heap_block_t;
```

**Public API**:
- `void* kmalloc(size_t size)` - Allocate memory
- `void kfree(void* ptr)` - Free memory
- `void* krealloc(void* ptr, size_t size)` - Resize allocation

**Debugging**:
- Check magic numbers: `block->magic == 0xDEADBEEF`
- Verify checksums before operations
- Use `memory stats` to check heap health
- Watch for fragmentation (many small free blocks)

**Common Issues**:
- **Corruption**: Buffer overflow writes past allocation
- **Double free**: Same pointer freed twice
- **Leak**: `kmalloc` without matching `kfree`

**Panic Codes**:
- Heap corruption detected → Check recent allocations
- Invalid free → Pointer not from `kmalloc` or already freed

---

### Paging (`src/cpu/paging.c`, `src/mm/vmm.c`)

**Implementation**: Optional page-based virtual memory

**Key Structures**:
```c
typedef struct page_directory_entry {
    uint32_t present : 1;
    uint32_t rw : 1;
    uint32_t user : 1;
    // ... (see mm/virtual-memory.md)
} pde_t;
```

**Memory Map** (paging enabled):
```
0x00000000 - 0x003FFFFF : User code/data
0x00400000 - 0xBFFFFFFF : User heap/stack
0xC0000000 - 0xFFFFFFFF : Kernel (identity mapped)
```

**Debugging**:
- Page fault: Check CR2 register (fault address)
- Access violations: U/S and R/W bits mismatch
- Check page directory/table entries in GDB

**Common Issues**:
- **Null dereference**: Fault at 0x00000000
- **Stack overflow**: Fault near 0xC0000000
- **Permission**: Kernel accessing user page (or vice versa)

---

## Interrupt Handling

### IDT (`src/cpu/idt.c`, `src/cpu/isr.c`)

**Implementation**: Interrupt Descriptor Table with 256 entries

**Key Functions**:
- `idt_init()` - Initialize IDT
- `idt_set_gate(num, handler, selector, flags)` - Configure entry
- ISR stubs in `src/cpu/isr_stubs.asm`

**Interrupt Categories**:
- **0-31**: CPU exceptions (fault, trap, abort)
- **32-47**: IRQ 0-15 (hardware interrupts)
- **0x80**: System call interrupt
- **48+**: Available for custom use

**Debugging**:
- Unhandled interrupt → Check IDT entry exists
- Double fault (INT 8) → Stack overflow or bad handler
- GPF (INT 13) → Segment violation, bad memory access

**Common Issues**:
- **Spurious IRQ**: Hardware glitch, send EOI and ignore
- **Missing EOI**: Interrupts stop firing, check `outb(0x20, 0x20)`
- **Wrong handler**: Verify IDT gate points to correct function

---

### IRQ (`src/cpu/irq.c`)

**Implementation**: PIC (8259) interrupt controller

**IRQ Mapping**:
```
IRQ 0  - Timer (PIT)
IRQ 1  - Keyboard
IRQ 2  - Cascade (IRQ 8-15)
IRQ 3  - COM2/COM4
IRQ 4  - COM1/COM3
IRQ 5  - LPT2
IRQ 6  - Floppy
IRQ 7  - LPT1
IRQ 8  - RTC
IRQ 9  - Available
IRQ 10 - Available
IRQ 11 - Available
IRQ 12 - Mouse
IRQ 13 - FPU
IRQ 14 - Primary ATA
IRQ 15 - Secondary ATA
```

**Debugging**:
- Check IRQ mask: `inb(0x21)` and `inb(0xA1)`
- Verify EOI sent to PIC
- Use serial log to trace IRQ handlers

---

### Watchdog (`src/misc/watchdog.c`)

**Implementation**: Ticker-based hang detection

**Configuration**:
```c
#define WATCHDOG_TIMEOUT_TICKS 250  // ~2.5 seconds at 100 Hz
```

**Usage**:
```c
watchdog_kick("context-string");
```

**How It Works**:
1. Each `watchdog_kick()` resets counter
2. Timer IRQ increments counter
3. If counter exceeds timeout → PANIC

**Debugging**:
- Panic shows "last" context where kick was called
- Add kicks to long loops to prevent false positives
- Increase timeout for legitimately slow operations

**Common Issues**:
- **False positive**: Operation takes longer than expected
- **No trigger**: Loop doesn't call `watchdog_kick()`

---

## Filesystems

### EYNFS (`src/fs/eynfs.c`)

**Implementation**: Simple flat filesystem with directories

**On-Disk Layout**:
```
LBA 0-2047:      MBR + reserved
LBA 2048:        Superblock
LBA 2049-2560:   Root directory entries
LBA 2561+:       File data blocks
```

**Superblock**:
```c
typedef struct {
    uint32_t magic;         // 0x454E5946 ('EYNF')
    uint32_t version;       // 1
    uint32_t total_blocks;
    uint32_t root_dir_lba;
    uint32_t root_dir_size;
} eynfs_superblock_t;
```

**Debugging**:
- Check magic: `python3 devtools/fsck_eynfs.py`
- Verify partition starts at LBA 2048
- Use `fdisk` to inspect partitions
- Run `fscheck` for consistency

**Common Issues**:
- **Magic mismatch**: Wrong partition or not formatted
- **Corruption**: Improper shutdown or write bug
- **Out of space**: No free blocks remaining

---

### VFS (`src/fs/vfs.c`)

**Implementation**: Virtual filesystem abstraction layer

**API**:
```c
int vfs_stat(const char* path, file_stat_t* stat);
int vfs_read_file(const char* path, void** buf, uint32* size);
int vfs_write_file(const char* path, void* data, uint32 size);
int vfs_list_dir(const char* path, /*...*/);
```

**Path Resolution**:
- Absolute: `/path/to/file`
- Relative: `file.txt` (uses shell CWD)
- Drive switching: `drive 0`, `drive 1`

**Debugging**:
- Log all VFS calls to serial
- Check return codes (negative = error)
- Verify path strings are null-terminated

---

## Networking

### E1000 Driver (`src/drivers/e1000.c`)

**Implementation**: Intel 82540EM NIC driver (MMIO-based)

**Key Structures**:
```c
typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;
```

**Descriptor Rings**:
- **RX**: 16 descriptors × 2048 bytes = 32 KB
- **TX**: 8 descriptors × 2048 bytes = 16 KB

**Debugging**:
- `e1000 regs` - Dump register values
- `e1000 test` - Run self-tests
- Check link: STATUS register bit 1
- Verify descriptors: DD bit set when done

**Common Issues**:
- **No link**: Cable unplugged or NIC disabled
- **Descriptor stall**: RDT/TDT not advancing
- **DMA failure**: Buffer addresses invalid

**Register Reference**:
```
CTRL (0x0000)  - Control register
STATUS (0x0008) - Device status (bit 1 = link up)
RDBAL (0x2800) - RX descriptor base (low)
RDLEN (0x2808) - RX ring length
RDH (0x2810)   - RX head (hardware)
RDT (0x2818)   - RX tail (software)
RCTL (0x0100)  - Receiver control
```

See [network/e1000-driver.md](../network/e1000-driver.md) for full details.

---

### Network Stack (`src/network/netstack.c`)

**Implementation**: UDP/IPv4/ARP stack

**Protocols**:
1. **Ethernet**: Frame parsing (14-byte header)
2. **ARP**: IP→MAC resolution (4-entry cache)
3. **IPv4**: Basic packet handling (20-byte header)
4. **UDP**: Datagram sockets (8-byte header)

**Timers and Aging**:
- Netstack uses scheduler ticks for lightweight timers
- ARP cache entries expire after 30 seconds
- Gateway MAC is refreshed periodically

**Packet Flow**:
```
Hardware → e1000_receive() → net_poll_rx() → 
  → parse Ethernet → dispatch by EtherType →
    → ARP handler / IPv4 handler → UDP handler → queue
```

**Debugging**:
- Enable `#define NET_DEBUG 1`
- Use `e1000 udp-stats` for counters
- Check ARP cache: modify code to print entries
- Verify checksums: IPv4 header checksum
- `netstat` shows IPv4 fragment and ICMP error counters
- RX interrupt assist: `e1000_irq_rx_pending` helps spot bursty traffic

**Common Issues**:
- **Queue full**: RX overrun, call `e1000 udp-drain`
- **ARP timeout**: Bad IP or network config
- **Checksum error**: Corrupted packet or bug
- **Fragment drop**: Sender is fragmenting (DF set on TX; RX drops fragments)

See [network/network-stack.md](../network/network-stack.md) for protocol details.

---

## Tiling Manager

### Implementation (`src/utilities/tui/tiling_manager.c`)

**Tile System**: Fixed grid of tiles for windows/applications

**Key Structures**:
```c
typedef struct {
    int active;
    int x, y, width, height;
    uint8_t* framebuffer;
    // ... callbacks for input/rendering
} tile_t;
```

**Debugging**:
- Tile not drawing: Check `invalidate` flag
- Input not working: Verify callback registered
- Crash during render: Check framebuffer bounds

**Common Issues**:
- **Tile overlap**: Incorrect x/y/width/height
- **Memory leak**: Framebuffer not freed on close
- **Render flicker**: Not using double buffering

See [ui/tiling-manager.md](../ui/tiling-manager.md) for API details.

---

## Shell & Commands

### Shell (`src/utilities/shell/shell.c`)

**Command Registration**:
```c
REGISTER_SHELL_COMMAND(cmd_info, "mycommand", handler_func, 
                       CMD_STREAMING, "Description");
```

**Types**:
- `CMD_ESSENTIAL`: Always loaded (ls, cd, exit)
- `CMD_STREAMING`: Loaded on-demand (format, write)
- `CMD_DIAGNOSTIC`: Debug commands (e1000, panic)

**Debugging**:
- Commands not found: Not registered or not loaded (`load`)
- Crashes in handler: Check argument parsing
- Wrong behavior: Verify command reads correct args

**Common Issues**:
- **Parse error**: Space handling in arguments
- **Buffer overflow**: `string arg` too long
- **CWD issues**: Use `resolve_path()` for files

---

## Userspace

### UELF Loader (`src/cpu/user_elf.c`)

**Implementation**: ELF32 loader for ring-3 programs

**Loading Steps**:
1. Parse ELF header (magic, entry point)
2. Map program segments to 0x00400000
3. Allocate stack at high address
4. Set up page tables (user-accessible)
5. Switch to ring 3 and jump to entry

**Debugging**:
- Invalid ELF: Check magic `0x7F 'E' 'L' 'F'`
- Crash on entry: Verify entry point alignment
- Segfault: Check segment permissions (R/W/X)

**Common Issues**:
- **Linker mismatch**: Use `devtools/user_elf32.ld`
- **Stack overflow**: Increase stack allocation
- **Bad syscall**: Check syscall number and args

See [api/userland-uelf-abi.md](../api/userland-uelf-abi.md) for format specification.

---

## Quick Debugging Reference

| Component | Debug Command | Key File | Common Panic |
|-----------|---------------|----------|--------------|
| Heap | `memory stats` | `mm/heap.c` | Heap corruption |
| Paging | GDB `info registers` | `mm/vmm.c` | Page fault |
| E1000 | `e1000 regs` | `drivers/e1000.c` | Descriptor error |
| Network | `e1000 udp-stats` | `network/netstack.c` | Queue full |
| Filesystem | `fscheck` | `fs/eynfs.c` | Magic mismatch |
| Watchdog | Check serial log | `misc/watchdog.c` | Hang timeout |
| UELF | `run` simple prog | `cpu/user_elf.c` | Invalid ELF |

## See Also

- [stop-codes.md](../stop-codes.md) - Panic code reference
- [debugging.md](debugging.md) - General debugging guide
- [network/e1000-driver.md](../network/e1000-driver.md) - NIC implementation
- [network/network-stack.md](../network/network-stack.md) - Protocol details
- [api/userland-uelf-abi.md](../api/userland-uelf-abi.md) - Userspace ABI
