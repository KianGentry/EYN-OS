# EYN-OS Command Reference

This document is auto-generated from the source code. Last updated: 2026-02-07 03:37:33

**Total Commands:** 26

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

## Streaming Commands

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

## Filesystem Commands

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
| Essential Commands | 4 |
| Streaming Commands | 8 |
| Filesystem Commands | 2 |
| System Commands | 2 |
| Utility Commands | 8 |
| Development Commands | 2 |

