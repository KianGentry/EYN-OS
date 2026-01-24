# EYN-OS Command Reference

This document is auto-generated from the source code. Last updated: 2026-01-24 12:18:08

**Total Commands:** 66

## Table of Contents

- [Essential Commands](#essential-commands)
- [Streaming Commands](#streaming-commands)
- [Filesystem Commands](#filesystem-commands)
- [System Commands](#system-commands)
- [Utility Commands](#utility-commands)
- [Development Commands](#development-commands)

## Essential Commands

### clear

**Handler:** `clear_cmd`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Clears the screen and resets the shell display.
Usage: clear

**Example:**
```bash
clear
```

---

### exit

**Handler:** `handler_exit`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Exits the kernel and shuts down the system.
Usage: exit

**Example:**
```bash
exit
```

---

### help

**Handler:** `help_cmd`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Display this message and show all available commands with descriptions and examples.
Usage: help

**Example:**
```bash
help
```

---

### init

**Handler:** `init_cmd`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Initialize full system services (ATA drives, etc.).
Usage: init

**Example:**
```bash
init
```

---

### memory

**Handler:** `memory_cmd`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Memory management and testing.
Usage: memory stats | test | stress

**Example:**
```bash
memory stats
```

---

### portable

**Handler:** `portable_cmd`

**Type:** CMD_ESSENTIAL

**File:** `shell_commands.c`

**Description:**
Display portability optimizations and memory usage.
Usage: portable [stats|optimize]

**Example:**
```bash
portable
```

---

## Streaming Commands

### assertfail

**Handler:** `assertfail_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Trigger an assertion failure (ASSERT).
Usage: assertfail yes

**Example:**
```bash
assertfail yes
```

---

### catram

**Handler:** `catram_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Display contents of a file from RAM disk (FAT32).
Usage: catram <filename>

**Example:**
```bash
catram test.txt
```

---

### clearbg

**Handler:** `clearbg_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Clear background image for the focused tile.

**Example:**
```bash
clearbg
```

---

### e1000

**Handler:** `e1000_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Intel e1000 utilities (probe + bring-up helpers).
Usage: e1000 probe | e1000 init | e1000 regs | e1000 test [--expect-link up|down] [--expect-mac xx:xx:xx:xx:xx:xx] | e1000 udp-send | e1000 tcp-send | e1000 tcp-listen | e1000 tcp-recv | e1000 tcp-sendcur | e1000 tcp-close

**Example:**
```bash
e1000 init
```

---

### e1000probe

**Handler:** `e1000probe_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Probe the Intel e1000 NIC (read-only MMIO sanity check).
Usage: e1000probe

**Example:**
```bash
e1000probe
```

---

### error

**Handler:** `error_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Display system error statistics and status.
Usage: error [clear|details]

**Example:**
```bash
error
```

---

### fatfix

**Handler:** `fatfix_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Scan and repair FAT32 entries incorrectly marked as directories.
Usage: fatfix [path]

**Example:**
```bash
fatfix /
```

---

### lsram

**Handler:** `lsram_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
List files in the RAM disk (FAT32) with directory tree.
Usage: lsram

**Example:**
```bash
lsram
```

---

### memory_stats

**Handler:** `memory_stats_cmd`

**Type:** CMD_STREAMING

**File:** `predictive_commands.c`

**Description:**
Show predictive memory statistics

**Example:**
```bash
memory_stats
```

---

### mmap

**Handler:** `mmap_cmd`

**Type:** CMD_STREAMING

**File:** `predictive_commands.c`

**Description:**
Memory map a file for zero-copy access

**Example:**
```bash
mmap <filename>
```

---

### msync

**Handler:** `msync_cmd`

**Type:** CMD_STREAMING

**File:** `predictive_commands.c`

**Description:**
Synchronize memory-mapped file to disk

**Example:**
```bash
msync <address>
```

---

### munmap

**Handler:** `munmap_cmd`

**Type:** CMD_STREAMING

**File:** `predictive_commands.c`

**Description:**
Unmap a memory-mapped file

**Example:**
```bash
munmap <address>
```

---

### netcfg

**Handler:** `netcfg_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Network configuration (defaults match QEMU user-net).
Usage: netcfg show | netcfg verify | netcfg route <dst_ip> | netcfg defaults [--save] | netcfg set ip|gw|mask|dns <a.b.c.d> [--save] | netcfg save [path] | netcfg load [path]
Default path: /config/net.cfg

**Example:**
```bash
netcfg show
```

---

### netstat

**Handler:** `netstat_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Network status (netstack + ARP + UDP + ICMP).
Usage: netstat
Note: run 'e1000 init' first for full info.

**Example:**
```bash
netstat
```

---

### pagingguards

**Handler:** `pagingguards_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Install optional paging guards (null-page, .text/.rodata RO).
Usage: pagingguards

**Example:**
```bash
pagingguards
```

---

### panic

**Handler:** `panic_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Trigger a kernel panic to test diagnostics.
Usage: panic yes

**Example:**
```bash
panic yes
```

---

### pciscan

**Handler:** `pciscan_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Scan PCI devices and print vendor/device IDs and BAR0.
Usage: pciscan [net]
Tip: e1000 usually shows as 8086:100E.

**Example:**
```bash
pciscan net
```

---

### pf

**Handler:** `pf_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Intentionally trigger a page fault (read/write/exec a chosen address).
Usage: pf yes [addr] [r|w|x]

**Example:**
```bash
pf yes 0x0 r
```

---

### ping

**Handler:** `ping_cmd`

**Type:** CMD_DIAGNOSTIC

**File:** `shell_commands.c`

**Description:**
Send ICMP echo request(s).
Usage: ping <dst_ip> [count] [local_ip]
Example: ping 10.0.2.2
Note: run 'e1000 init' first.

**Example:**
```bash
ping 10.0.2.2
```

---

### predict

**Handler:** `predict_cmd`

**Type:** CMD_STREAMING

**File:** `predictive_commands.c`

**Description:**
Predictive memory management

**Example:**
```bash
predict [stats|reset|optimize]
```

---

### rect

**Handler:** `draw_cmd_handler`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Draw a rectangle.
Usage: rect <x> <y> <width> <height> <r> <g> <b>.
Example: rect 10 20 100 50 255 0 0 draws a red rectangle.

**Example:**
```bash
rect 10 20 100 50 255 0 0
```

---

### ring3

**Handler:** `ring3_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Switch to ring 3 and run a tiny user-mode stub (prints via int 0x80).
Usage: ring3 yes

**Example:**
```bash
ring3 yes
```

---

### serialtest

**Handler:** `serialtest_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Write a test line to COM1 to verify serial output.
Usage: serialtest

**Example:**
```bash
serialtest
```

---

### setbg

**Handler:** `setbg_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Set a REI image as background for the focused tile (shows Tile/Scale/Center chooser).
Usage: setbg <file.rei>

**Example:**
```bash
setbg eynos.rei
```

---

### setfont

**Handler:** `setfont_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Set the system font at runtime (loads .hex from disk into RAM).
Usage: setfont <file.hex> | setfont builtin

**Example:**
```bash
setfont /fonts/unscii-16.hex
```

---

### size

**Handler:** `size`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Show the size of a file in bytes.
Usage: size <filename>

**Example:**
```bash
size myfile.txt
```

---

### stats

**Handler:** `stats_cmd`

**Type:** CMD_STREAMING

**File:** `stats_gui.c`

**Description:**
Graphical system performance monitor with CPU, memory, disk pies and sortable table

**Example:**
```bash
stats
```

---

### theme

**Handler:** `theme_cmd`

**Type:** CMD_STREAMING

**File:** `theme_cmd.c`

**Description:**
Open a GUI theme editor (colors + font).

**Example:**
```bash
theme
```

---

### tiling

**Handler:** `tiling_cmd`

**Type:** CMD_STREAMING

**File:** `tiling_cmd.c`

**Description:**
Launch the tiling front-end manager.

**Example:**
```bash
tiling
```

---

### userrun

**Handler:** `userrun_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Load a raw user-mode code blob from VFS into ring 3 and run it at 0x00400000.
The program should use int 0x80 with EYN-OS syscall numbers (write=1, exit=2).
Usage: userrun <path>

**Example:**
```bash
userrun /testdir/user_hello.bin
```

---

### validate

**Handler:** `validate_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Display input validation statistics and test validation.
Usage: validate [test|stats]

**Example:**
```bash
validate
```

---

### view

**Handler:** `view_cmd`

**Type:** CMD_STREAMING

**File:** `image_viewer_gui.c`

**Description:**
Open a REI image in a GUI viewer.
Usage: view <file.rei>

**Example:**
```bash
view eynos.rei
```

---

### vieww

**Handler:** `vieww_cmd`

**Type:** CMD_STREAMING

**File:** `image_viewer_gui.c`

**Description:**
Open a REI image in a floating window.
Usage: vieww <file.rei>

**Example:**
```bash
vieww eynos.rei
```

---

### win_test

**Handler:** `win_test_cmd`

**Type:** CMD_STREAMING

**File:** `window_test.c`

**Description:**
Open a sample floating window to test compositor performance.

**Example:**
```bash
win_test
```

---

## Filesystem Commands

### cd

**Handler:** `cd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Change the current directory.
Usage: cd <directory>

**Example:**
```bash
cd myfolder
```

---

### copy

**Handler:** `copy_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Copy a file from source to destination.
Usage: copy <source> <destination>

**Example:**
```bash
copy file1.txt file2.txt
```

---

### del

**Handler:** `del`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Delete a file from the filesystem.
Usage: del <filename>

**Example:**
```bash
del myfile.txt
```

---

### deldir

**Handler:** `deldir`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Delete an empty directory.
Usage: deldir <directory>

**Example:**
```bash
deldir myfolder
```

---

### fdisk

**Handler:** `fdisk_cmd_handler`

**Type:** CMD_STREAMING

**File:** `fdisk_commands.c`

**Description:**
List partition table or create partitions.
Usage: fdisk [create <start_lba> <size> <type>]

**Example:**
```bash
fdisk create 2048 1024000 0x0C
```

---

### format

**Handler:** `format_cmd_handler`

**Type:** CMD_STREAMING

**File:** `format_command.c`

**Description:**
Format partition n (0-3) as FAT32 or EYNFS.
FAT32: widely supported, max 4GB files.
EYNFS: native, supports long filenames, fast directory access.
Usage: format <partition_num> <filesystem_type>

**Example:**
```bash
format 1 fat32
```

---

### fscheck

**Handler:** `fscheck`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Check filesystem integrity.
Usage: fscheck

**Example:**
```bash
fscheck
```

---

### ls

**Handler:** `ls_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
List files in the root directory of the selected drive.
Usage: ls

**Example:**
```bash
ls
```

---

### makedir

**Handler:** `makedir`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Create a new directory.
Usage: makedir <directory>

**Example:**
```bash
makedir myfolder
```

---

### move

**Handler:** `move_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Move a file from source to destination.
Usage: move <source> <destination>

**Example:**
```bash
move file1.txt /backup/file1.txt
```

---

### read

**Handler:** `read_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Display text files (.txt) or render markdown (.md). For images, use 'view' or 'vieww'.
Usage: read <filename>

**Example:**
```bash
read myfile.txt
```

---

### write

**Handler:** `write_cmd`

**Type:** CMD_STREAMING

**File:** `fs_commands.c`

**Description:**
Open nano-like text editor for a file.
Usage: write <filename>

**Example:**
```bash
write myfile.txt
```

---

## System Commands

### drive

**Handler:** `drive_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Change between different drives (from lsata).
Usage: drive <n>

**Example:**
```bash
drive 0
```

---

### lsata

**Handler:** `lsata_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
List detected ATA drives and their details.
Usage: lsata

**Example:**
```bash
lsata
```

---

## Utility Commands

### calc

**Handler:** `calc_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
32-bit fixed-point calculator. Supports +, -, *, /.
Usage: calc <expression>

**Example:**
```bash
calc 2.5+3.7
```

---

### draw

**Handler:** `draw_cmd`

**Type:** CMD_STREAMING

**File:** `draw_gui.c`

**Description:**
Create or edit a REI image with a GUI. Usage: draw <filename.rei>

**Example:**
```bash
draw test.rei
```

---

### echo

**Handler:** `echo_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Reprints a given text to the screen.
Usage: echo <text>

**Example:**
```bash
echo Hello, world!
```

---

### history

**Handler:** `history_cmd`

**Type:** CMD_STREAMING

**File:** `history.c`

**Description:**
Show or clear command history.
Usage: history [clear]
Example: history | history clear

**Example:**
```bash
history
```

---

### log

**Handler:** `log_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Enable or disable shell logging.
Usage: log on|off

**Example:**
```bash
log on
```

---

### random

**Handler:** `random_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Generate random numbers.
Usage: random [count] | random [min] [max]
Example: random 5 | random 10 20

**Example:**
```bash
random 5
```

---

### search

**Handler:** `search_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Search for text in filenames and file contents using Boyer-Moore algorithm.
Usage: search <pattern> [-f|-c|-a]
Example: search hello -a

**Example:**
```bash
search hello -a
```

---

### sort

**Handler:** `sort_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Sort strings alphabetically.
Usage: sort <string1> <string2> <string3> ...
Example: sort zebra apple banana

**Example:**
```bash
sort zebra apple banana
```

---

### spam

**Handler:** `spam_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Spam 'EYN-OS' to the shell 100 times for fun.
Usage: spam

**Example:**
```bash
spam
```

---

### ver

**Handler:** `ver_cmd`

**Type:** CMD_STREAMING

**File:** `shell_commands.c`

**Description:**
Shows the current system version and release information.
Usage: ver

**Example:**
```bash
ver
```

---

## Development Commands

### assemble

**Handler:** `handler_assemble`

**Type:** CMD_STREAMING

**File:** `assemble.c`

**Description:**
Converts assembly code into machine code.
Supports NASM syntax.
Usage: assemble <input file> <output file>

**Example:**
```bash
assemble example.asm example.eyn
```

---

### run

**Handler:** `run_cmd`

**Type:** CMD_STREAMING

**File:** `run_command.c`

**Description:**
Run a native program, ring3 ELF, or a shell script.
Usage: run <program.eyn|program.bin|program.flat|program.uelf|script.shell>

**Example:**
```bash
run user_hello.uelf
```

---

## Command Statistics

| Category | Count |
|----------|-------|
| Essential Commands | 6 |
| Streaming Commands | 34 |
| Filesystem Commands | 12 |
| System Commands | 2 |
| Utility Commands | 10 |
| Development Commands | 2 |

