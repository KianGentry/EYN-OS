# EYN-OS Debugging Guide

Comprehensive guide for debugging EYN-OS kernel, drivers, and applications.

## Table of Contents
- [Quick Debug Checklist](#quick-debug-checklist)
- [Serial Port Debugging](#serial-port-debugging)
- [GDB Integration](#gdb-integration)
- [Memory Debugging](#memory-debugging)
- [Network Debugging](#network-debugging)
- [Filesystem Debugging](#filesystem-debugging)
- [Performance Profiling](#performance-profiling)
- [Common Issues](#common-issues)

## Quick Debug Checklist

When something goes wrong:

1. **Check serial output** (`make qemu-debug`)
2. **Run with GDB** (`make qemu-gdb`)
3. **Check system stats** (`memory stats`, `e1000 udp-stats`, `netstat`)
4. **Verify initialization** (`init` command)
5. **Test minimal case** (reduce to simplest reproduction)
6. **Check recent changes** (`git diff`)

## Serial Port Debugging

### Setup

**QEMU with stdio**:
```bash
make qemu-debug
# Serial output appears in terminal
```

**QEMU with file**:
```bash
qemu-system-i386 -kernel tmp/boot/kernel.bin -serial file:debug.log
```

**Real Hardware**:
- Connect null modem cable to COM1
- Use `minicom`, `screen`, or `putty` on host at 9600 baud

### What Gets Logged

- Kernel initialization messages
- Panic backtraces (most important!)
- Driver initialization status
- Watchdog kicks (if enabled)
- Custom debug messages via `serial_write()`

### Adding Debug Output

```c
#include <serial.h>

// Simple message
serial_write(SERIAL_COM1, "Debug checkpoint\n", 17);

// Formatted output
char buf[128];
snprintf(buf, sizeof(buf), "Value: %d\n", value);
serial_write(SERIAL_COM1, buf, strlen(buf));

// Only in debug builds
#ifdef DEBUG
serial_write(SERIAL_COM1, "Debug info\n", 11);
#endif
```

### Serial Debugging Best Practices

- Log **before** risky operations
- Include context: function name, variable values
- Use consistent prefixes: `"[E1000] "`, `"[NETSTACK] "`
- Don't log in tight loops (use counters instead)
- Flush critical messages immediately

## GDB Integration

### Starting GDB Session

```bash
# Terminal 1: Start QEMU paused
make qemu-gdb

# Terminal 2: Connect GDB
gdb tmp/boot/kernel.bin
(gdb) target remote :1234
(gdb) continue
```

### Useful GDB Commands

```gdb
# Breakpoints
(gdb) break main
(gdb) break e1000_init
(gdb) break panic

# Step through code
(gdb) step        # Step into functions
(gdb) next        # Step over functions
(gdb) finish      # Run until return

# Examine state
(gdb) backtrace   # Show call stack
(gdb) info registers
(gdb) print variable_name
(gdb) print *pointer
(gdb) print/x value    # Print in hex

# Memory examination
(gdb) x/10x $esp       # 10 words at stack pointer
(gdb) x/10i $eip       # 10 instructions at PC
(gdb) x/s 0x12345678   # String at address

# Continue execution
(gdb) continue
(gdb) until <line>

# Watchpoints (break on memory change)
(gdb) watch variable_name
(gdb) watch *(int*)0x12345678
```

### GDB Scripting

Create `.gdbinit` in project root:
```gdb
# Connect to QEMU
target remote :1234

# Useful breakpoints
break panic
break assert_fail

# Display commands
display/i $eip
display/x $esp

# Custom commands
define dump_heap
  x/100x 0x00200000
end
```

### Hardware Breakpoints

```gdb
# Set hardware breakpoint (useful for code in ROM)
(gdb) hbreak <location>

# Watch memory (max 4 watchpoints on x86)
(gdb) watch *(uint32_t*)0x12345678
```

## Memory Debugging

### Heap Statistics

```bash
# In EYN-OS shell
memory stats

# Output shows:
# - Total heap size
# - Used bytes
# - Free bytes
# - Largest free block
# - Allocation count
```

### Detecting Corruption

The heap uses **magic numbers** and **checksums** for integrity:

```c
// In mm/heap.c
typedef struct heap_block {
    uint32_t magic;      // 0xDEADBEEF
    uint32_t checksum;   // FNV-1a hash of metadata
    // ...
} heap_block_t;
```

**Signs of corruption**:
- `"Heap corruption detected"` panic
- `"Invalid magic number"` assertion
- Crashes in `kmalloc` or `kfree`

**Debugging steps**:
1. Enable heap debugging: `#define HEAP_DEBUG 1`
2. Check for:
   - Buffer overflows (write past allocation)
   - Double free (freeing same pointer twice)
   - Use after free (accessing freed memory)
3. Use GDB watchpoints on suspect addresses

### Memory Leak Detection

```c
// Track allocations
uint32_t alloc_count = 0;
uint32_t free_count = 0;

void* kmalloc(size_t size) {
    alloc_count++;
    // ... allocate ...
}

void kfree(void* ptr) {
    free_count++;
    // ... free ...
}

// Check periodically
printf("Allocs: %d, Frees: %d, Leaks: %d\n", 
       alloc_count, free_count, alloc_count - free_count);
```

### Stack Overflow Detection

**Symptoms**:
- Crashes in random locations
- Corrupted local variables
- Page faults near `0xBFFFFFFF`

**Check stack usage**:
```gdb
(gdb) print $esp
(gdb) print $ebp
# Stack grows down; check distance from base
```

**For ring-3 programs**: Increase stack size in linker script.

## Network Debugging

### E1000 Diagnostics

```bash
# Probe NIC
e1000probe

# Check registers
e1000 regs

# Test with expected values
e1000 test --expect-link up
e1000 test --expect-mac 52:54:00:12:34:56

# Initialize
e1000 init
```

### UDP Statistics

```bash
e1000 udp-stats

# Quick status dump
netstat

# Shows:
# - RX/TX packet counts
# - Dropped packets
# - Bad checksums
# - Queue usage
```

### Packet Capture

Enable debug tracing in `src/network/netstack.c`:
```c
#define NET_DEBUG 1
```

This logs:
- Every received frame (src/dst MAC, EtherType)
- ARP requests/replies
- IPv4 packet details
- UDP deliveries

### Common Network Issues

| Symptom | Check | Fix |
|---------|-------|-----|
| "No e1000 device" | `pciscan net` | Verify QEMU config |
| "ARP timeout" | Destination IP | Use 10.0.2.x in QEMU |
| "Queue full" | `e1000 udp-stats` | `e1000 udp-drain` |
| Packets not received | Link status | `e1000 regs` check bit 1 of STATUS |

## Filesystem Debugging

### EYNFS Validation

```bash
# In EYN-OS
fscheck

# On host (more comprehensive)
python3 devtools/fsck_eynfs.py eynfs.img
```

### Disk Image Issues

**Recreate disk image**:
```bash
make eynfsimg
make run
```

**Check partitioning**:
```bash
# In EYN-OS
fdisk
lsata
```

**Extract files for inspection**:
```bash
# On host
python3 devtools/extract_from_eynfs.py eynfs.img output_dir/
```

### FAT32 Debugging

```bash
# Scan for issues
fatfix

# List files on the current drive
ls
```

### File Operation Tracing

Add debug output to VFS operations:
```c
// In fs/vfs.c
int vfs_read_file(const char* path, void** buf, uint32* size) {
    serial_write(SERIAL_COM1, "VFS read: ", 10);
    serial_write(SERIAL_COM1, path, strlen(path));
    serial_write(SERIAL_COM1, "\n", 1);
    // ... continue ...
}
```

## Performance Profiling

### Timing Functions

```c
#include <system.h>  // For timer_ticks

uint32_t start = timer_ticks;
// ... code to measure ...
uint32_t elapsed = timer_ticks - start;
printf("Took %d ticks\n", elapsed);
```

### Watchdog as Profiler

The watchdog can identify slow operations:
```c
watchdog_kick("before-slow-op");
slow_operation();
watchdog_kick("after-slow-op");
// If timeout, you know it's in slow_operation()
```

### Packet Rate Measurement

```c
uint32_t start_ticks = timer_ticks;
uint32_t start_packets = tx_count;

// ... run test ...

uint32_t elapsed = timer_ticks - start_ticks;
uint32_t packets = tx_count - start_packets;
printf("Rate: %d packets/sec\n", packets * HZ / elapsed);
```

## Common Issues

### System Hangs

**Symptoms**: System stops responding, no panic

**Causes**:
- Infinite loop without watchdog kick
- Interrupts disabled (`cli`) and not restored
- Deadlock waiting for hardware
- Busy-wait on condition that never becomes true

**Debug**:
1. Check serial output - may have logged before hang
2. Use GDB: `Ctrl+C` to break, `backtrace` to see where stuck
3. Look for recent `cli` instructions without `sti`
4. Check if watchdog triggers (if not, loop isn't calling `watchdog_kick`)

### Random Crashes

**Symptoms**: Crashes at different locations, hard to reproduce

**Causes**:
- Memory corruption (buffer overflow, use-after-free)
- Uninitialized variables
- Race conditions (if multithreading added)
- Stack overflow

**Debug**:
1. Enable heap guards and checksums
2. Use `memory stats` frequently
3. Valgrind-like techniques (EYN-OS-friendly)
    - Use heap guard/check features already present in the kernel allocator (see memory management docs).
    - Prefer "small reproducer" programs under `testdir/` and bisect with `make eynfsimg`.
    - Add targeted assertions around invariants to catch the first bad write.
4. Binary search: disable features until crash stops

### Slow Performance

**Symptoms**: Operations take longer than expected

**Causes**:
- Inefficient algorithm (O(n²) instead of O(n))
- Excessive polling instead of interrupts
- Memory allocation churn
- Cache misses (less relevant on simple systems)

**Debug**:
1. Profile with timer ticks
2. Count operations (e.g., `malloc` calls)
3. Check if GCC optimizations enabled (`-O2` vs `-O0`)

### "Works in QEMU, fails on real hardware"

**Common reasons**:
- Timing differences (real hardware is faster/slower)
- QEMU tolerates incorrect I/O sequences
- Hardware doesn't match QEMU's emulation
- Interrupts behave differently

**Debug**:
1. Add delays: `for (int i=0; i<1000; i++);`
2. Test on different real hardware
3. Check datasheets for hardware timing requirements
4. Use PIO instead of MMIO (or vice versa)

## Debugging Tools Summary

| Tool | Use Case | Access |
|------|----------|--------|
| Serial output | Panic traces, logging | `make qemu-debug` |
| GDB | Live debugging, breakpoints | `make qemu-gdb` |
| `memory stats` | Heap analysis | In shell |
| `e1000 udp-stats` | Network profiling | In shell |
| `e1000 regs` | NIC diagnostics | In shell |
| `fscheck` | Filesystem validation | In shell |
| `panic` | Test panic system | In shell |
| `assertfail` | Test assertions | In shell |
| `pciscan` | Hardware enumeration | In shell |
| `fdisk` | Partition inspection | In shell |

## Best Practices

1. **Always log to serial** in addition to console
2. **Use assertions** for invariants, not error conditions
3. **Kick watchdog** in all long loops
4. **Validate pointers** before dereferencing
5. **Check bounds** before array access
6. **Return error codes** instead of panicking when possible
7. **Test incrementally** - don't write 1000 lines before testing
8. **Keep commits small** - easier to bisect bugs
9. **Document assumptions** in comments
10. **Write reproducible test cases** for bugs

## Next Steps

- [stop-codes.md](../stop-codes.md) - Panic code reference
- [general/watchdog.md](../general/watchdog.md) - Watchdog configuration
- [general/panic.md](../general/panic.md) - Panic system architecture
- Network debugging
    - Prefer `make qemu-debug` so you can correlate NIC logs with kernel state.
    - Use built-in shell commands (when available): `e1000 regs`, `e1000 udp-stats`, `e1000 init`.
    - If the stack is polling, add a periodic `watchdog_kick("net-poll")` inside long poll loops.
    - On the host, validate port forwards from `make run` and use host-side tools (e.g. `tcpdump`) to see traffic.
- GDB cheat sheet
    - Connect: `target remote :1234`
    - Break/step: `b <symbol>`, `si`, `ni`, `c`
    - Inspect: `bt`, `info reg`, `x/32wx $esp`, `x/i $eip`
    - Handy: `set disassembly-flavor intel`, `layout asm` (TUI)
