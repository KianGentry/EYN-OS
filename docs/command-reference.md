# EYN-OS Command Reference

This document is auto-generated from userland command metadata and binaries. Last updated: 2026-04-04 14:06:31

**Total Commands:** 84

## Table of Contents

- [Filesystem Commands](#filesystem-commands)
- [System Commands](#system-commands)
- [Network Commands](#network-commands)
- [Memory Commands](#memory-commands)
- [GUI/Window Commands](#gui/window-commands)
- [Development Commands](#development-commands)
- [Utility Commands](#utility-commands)

## Filesystem Commands

### cd

**Binary:** `testdir/binaries/cd`

**Metadata Source:** `testdir/code/cd_uelf.c`

**Description:**
Change the current directory.

**Example:**
```bash
cd <directory>
```

---

### copy

**Binary:** `testdir/binaries/copy`

**Metadata Source:** `testdir/code/copy_uelf.c`

**Description:**
Copy a file from source to destination.

**Example:**
```bash
copy file1.txt file2.txt
```

---

### create

**Binary:** `testdir/binaries/create`

**Metadata Source:** `testdir/code/create_uelf.c`

**Description:**
Create a file or directory.

**Example:**
```bash
create test/
```

---

### del

**Binary:** `testdir/binaries/del`

**Metadata Source:** `testdir/code/del_uelf.c`

**Description:**
Delete a file from the filesystem.

**Example:**
```bash
del myfile.txt
```

---

### deldir

**Binary:** `testdir/binaries/deldir`

**Metadata Source:** `testdir/code/deldir_uelf.c`

**Description:**
Delete an empty directory.

**Example:**
```bash
deldir myfolder
```

---

### delete

**Binary:** `testdir/binaries/delete`

**Metadata Source:** `testdir/code/delete_uelf.c`

**Description:**
Delete a file or directory.

**Example:**
```bash
delete test.txt
```

---

### fatfix

**Binary:** `testdir/binaries/fatfix`

**Metadata Source:** `testdir/code/fatfix_uelf.c`

**Description:**
Repair FAT32 directory entry flags.

**Example:**
```bash
fatfix /
```

---

### fscheck

**Binary:** `testdir/binaries/fscheck`

**Metadata Source:** `testdir/code/fscheck_uelf.c`

**Description:**
Check filesystem integrity.

**Example:**
```bash
fscheck
```

---

### head

**Binary:** `testdir/binaries/head`

**Metadata Source:** `testdir/code/head_uelf.c`

**Description:**
Print the first lines of a file.

**Example:**
```bash
head -n 10 /test.txt
```

---

### ls

**Binary:** `testdir/binaries/ls`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the ls command.

**Example:**
```bash
ls
```

---

### makedir

**Binary:** `testdir/binaries/makedir`

**Metadata Source:** `testdir/code/makedir_uelf.c`

**Description:**
Create a new directory.

**Example:**
```bash
makedir myfolder
```

---

### move

**Binary:** `testdir/binaries/move`

**Metadata Source:** `testdir/code/move_uelf.c`

**Description:**
Move a file from source to destination.

**Example:**
```bash
move file1.txt /backup/file1.txt
```

---

### pwd

**Binary:** `testdir/binaries/pwd`

**Metadata Source:** `testdir/code/pwd_uelf.c`

**Description:**
Print the current working directory.

**Example:**
```bash
pwd
```

---

### read

**Binary:** `testdir/binaries/read`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the read command.

**Example:**
```bash
read
```

---

### size

**Binary:** `testdir/binaries/size`

**Metadata Source:** `testdir/code/size_uelf.c`

**Description:**
Show the size of a file in bytes.

**Example:**
```bash
size myfile.txt
```

---

### tail

**Binary:** `testdir/binaries/tail`

**Metadata Source:** `testdir/code/tail_uelf.c`

**Description:**
Print the last lines of a file.

**Example:**
```bash
tail -n 10 /test.txt
```

---

## System Commands

### clear

**Binary:** `testdir/binaries/clear`

**Metadata Source:** `testdir/code/clear_uelf.c`

**Description:**
Clear terminal output.

**Example:**
```bash
clear
```

---

### drive

**Binary:** `testdir/binaries/drive`

**Metadata Source:** `testdir/code/drive_uelf.c`

**Description:**
Drive selection helper.

**Example:**
```bash
drive 0
```

---

### exit

**Binary:** `testdir/binaries/exit`

**Metadata Source:** `testdir/code/exit_uelf.c`

**Description:**
Exits the kernel and shuts down the system.

**Example:**
```bash
exit
```

---

### help

**Binary:** `testdir/binaries/help`

**Metadata Source:** `testdir/code/help_uelf.c`

**Description:**
Display command help.

**Example:**
```bash
help
```

---

### init

**Binary:** `testdir/binaries/init`

**Metadata Source:** `testdir/code/init_uelf.c`

**Description:**
Initialize core services.

**Example:**
```bash
init
```

---

### lsata

**Binary:** `testdir/binaries/lsata`

**Metadata Source:** `testdir/code/lsata_uelf.c`

**Description:**
List detected logical drives.

**Example:**
```bash
lsata
```

---

### portable

**Binary:** `testdir/binaries/portable`

**Metadata Source:** `testdir/code/portable_uelf.c`

**Description:**
Show portability optimization status.

**Example:**
```bash
portable stats
```

---

### serialtest

**Binary:** `testdir/binaries/serialtest`

**Metadata Source:** `testdir/code/serialtest_uelf.c`

**Description:**
Serial output test (userland).

**Example:**
```bash
serialtest
```

---

### ver

**Binary:** `testdir/binaries/ver`

**Metadata Source:** `testdir/code/ver_uelf.c`

**Description:**
Show system version information.

**Example:**
```bash
ver
```

---

## Network Commands

### e1000

**Binary:** `testdir/binaries/e1000`

**Metadata Source:** `testdir/code/e1000_uelf.c`

**Description:**
Intel e1000 utilities.

**Example:**
```bash
e1000 init
```

---

### e1000probe

**Binary:** `testdir/binaries/e1000probe`

**Metadata Source:** `testdir/code/e1000probe_uelf.c`

**Description:**
Probe Intel e1000 NIC.

**Example:**
```bash
e1000probe
```

---

### netcfg

**Binary:** `testdir/binaries/netcfg`

**Metadata Source:** `testdir/code/netcfg_uelf.c`

**Description:**
Network configuration command.

**Example:**
```bash
netcfg show
```

---

### netstat

**Binary:** `testdir/binaries/netstat`

**Metadata Source:** `testdir/code/netstat_uelf.c`

**Description:**
Show network status.

**Example:**
```bash
netstat
```

---

### pciscan

**Binary:** `testdir/binaries/pciscan`

**Metadata Source:** `testdir/code/pciscan_uelf.c`

**Description:**
Scan PCI devices.

**Example:**
```bash
pciscan net
```

---

### ping

**Binary:** `testdir/binaries/ping`

**Metadata Source:** `testdir/code/ping_uelf.c`

**Description:**
Send ICMP echo requests.

**Example:**
```bash
ping 10.0.2.2
```

---

## Memory Commands

### memory

**Binary:** `testdir/binaries/memory`

**Metadata Source:** `testdir/code/memory_uelf.c`

**Description:**
Memory management and testing.

**Example:**
```bash
memory stats
```

---

### memory_stats

**Binary:** `testdir/binaries/memory_stats`

**Metadata Source:** `testdir/code/memory_stats_uelf.c`

**Description:**
Show predictive memory statistics

**Example:**
```bash
memory_stats
```

---

### mmap

**Binary:** `testdir/binaries/mmap`

**Metadata Source:** `testdir/code/mmap_uelf.c`

**Description:**
Memory map a file for zero-copy access.

**Example:**
```bash
mmap <filename> [readonly]
```

---

### msync

**Binary:** `testdir/binaries/msync`

**Metadata Source:** `testdir/code/msync_uelf.c`

**Description:**
Synchronize memory-mapped file to disk.

**Example:**
```bash
msync <address>
```

---

### munmap

**Binary:** `testdir/binaries/munmap`

**Metadata Source:** `testdir/code/munmap_uelf.c`

**Description:**
Unmap a memory-mapped file.

**Example:**
```bash
munmap <address>
```

---

### pagingguards

**Binary:** `testdir/binaries/pagingguards`

**Metadata Source:** `testdir/code/pagingguards_uelf.c`

**Description:**
Install optional paging guards.

**Example:**
```bash
pagingguards
```

---

### predict

**Binary:** `testdir/binaries/predict`

**Metadata Source:** `testdir/code/predict_uelf.c`

**Description:**
Predictive memory management

**Example:**
```bash
predict [stats|reset|optimize]
```

---

## GUI/Window Commands

### clearbg

**Binary:** `testdir/binaries/clearbg`

**Metadata Source:** `testdir/code/clearbg_uelf.c`

**Description:**
Clear background image for focused tile.

**Example:**
```bash
clearbg
```

---

### draw

**Binary:** `testdir/binaries/draw`

**Metadata Source:** `testdir/code/draw_uelf.c`

**Description:**
Open the draw canvas editor.

**Example:**
```bash
draw /images/sketch.rei
```

---

### kstats

**Binary:** `testdir/binaries/kstats`

**Metadata Source:** `testdir/code/kstats_uelf.c`

**Description:**
Legacy kernel stats GUI (migrated default is userland 'stats').

**Example:**
```bash
kstats
```

---

### kwin_test

**Binary:** `testdir/binaries/kwin_test`

**Metadata Source:** `testdir/code/kwin_test_uelf.c`

**Description:**
Legacy kernel window-test command (migrated default is userland 'win_test').

**Example:**
```bash
kwin_test
```

---

### rect

**Binary:** `testdir/binaries/rect`

**Metadata Source:** `testdir/code/rect_uelf.c`

**Description:**
Draw a rectangle.

**Example:**
```bash
rect 10 20 100 50 255 0 0
```

---

### setbg

**Binary:** `testdir/binaries/setbg`

**Metadata Source:** `testdir/code/setbg_uelf.c`

**Description:**
Set background image for focused tile.

**Example:**
```bash
setbg /images/eynos.rei
```

---

### setfont

**Binary:** `testdir/binaries/setfont`

**Metadata Source:** `testdir/code/setfont_uelf.c`

**Description:**
Set runtime system font (.hex/.otf/.ttf).

**Example:**
```bash
setfont /fonts/unscii-16.otf
```

---

### stats

**Binary:** `testdir/binaries/stats`

**Metadata Source:** `testdir/code/stats_uelf.c`

**Description:**
Open the stats GUI.

**Example:**
```bash
stats
```

---

### theme

**Binary:** `testdir/binaries/theme`

**Metadata Source:** `testdir/code/theme_uelf.c`

**Description:**
Open the theme editor.

**Example:**
```bash
theme /fonts/unscii-16.hex
```

---

### view

**Binary:** `testdir/binaries/view`

**Metadata Source:** `testdir/code/view_uelf.c`

**Description:**
Dispatch a file to a view backend from /.view.

**Example:**
```bash
view /images/picture.rei
```

---

## Development Commands

### assertfail

**Binary:** `testdir/binaries/assertfail`

**Metadata Source:** `testdir/code/assertfail_uelf.c`

**Description:**
Trigger an assertion failure (ASSERT).

**Example:**
```bash
assertfail yes
```

---

### crashlog

**Binary:** `testdir/binaries/crashlog`

**Metadata Source:** `testdir/code/crashlog_uelf.c`

**Description:**
Inspect and clear crashlog records.

**Example:**
```bash
crashlog dump
```

---

### error

**Binary:** `testdir/binaries/error`

**Metadata Source:** `testdir/code/error_uelf.c`

**Description:**
Display command error status.

**Example:**
```bash
error details
```

---

### hexdump

**Binary:** `testdir/binaries/hexdump`

**Metadata Source:** `testdir/code/hexdump_uelf.c`

**Description:**
Hex dump file bytes.

**Example:**
```bash
hexdump /test.txt 256
```

---

### log

**Binary:** `testdir/binaries/log`

**Metadata Source:** `testdir/code/log_uelf.c`

**Description:**
Enable or disable shell logging.

**Example:**
```bash
log on
```

---

### panic

**Binary:** `testdir/binaries/panic`

**Metadata Source:** `testdir/code/panic_uelf.c`

**Description:**
Trigger a kernel panic for diagnostics.

**Example:**
```bash
panic yes
```

---

### pf

**Binary:** `testdir/binaries/pf`

**Metadata Source:** `testdir/code/pf_uelf.c`

**Description:**
Intentionally trigger a page fault.

**Example:**
```bash
pf yes [addr] [r|w|x]
```

---

### ring3

**Binary:** `testdir/binaries/ring3`

**Metadata Source:** `testdir/code/ring3_uelf.c`

**Description:**
Switch to ring 3 and run a tiny user-mode stub.

**Example:**
```bash
ring3 yes
```

---

### run

**Binary:** `testdir/binaries/run`

**Metadata Source:** `testdir/code/run_uelf.c`

**Description:**
Run a native program, .uelf, or script.

**Example:**
```bash
run <program> [args...]
```

---

### validate

**Binary:** `testdir/binaries/validate`

**Metadata Source:** `testdir/code/validate_uelf.c`

**Description:**
Show input validation status and tests.

**Example:**
```bash
validate test
```

---

## Utility Commands

### alias

**Binary:** `testdir/binaries/alias`

**Metadata Source:** `testdir/code/alias_uelf.c`

**Description:**
Create or remove command aliases.

**Example:**
```bash
alias ll ls -l
```

---

### calc

**Binary:** `testdir/binaries/calc`

**Metadata Source:** `testdir/code/calc_uelf.c`

**Description:**
Integer calculator supporting + - * /.

**Example:**
```bash
calc 2+3*4
```

---

### diskmgr

**Binary:** `testdir/binaries/diskmgr`

**Metadata Source:** `testdir/code/diskmgr_uelf.c`

**Description:**
Manage logical drives from userland.

**Example:**
```bash
diskmgr status
```

---

### download

**Binary:** `testdir/binaries/download`

**Metadata Source:** `testdir/code/download_uelf.c`

**Description:**
Download a file over HTTP/1.1 (GET only) with DNS support.

**Example:**
```bash
download http://example.com/index.html
```

---

### echo

**Binary:** `testdir/binaries/echo`

**Metadata Source:** `testdir/code/echo_uelf.c`

**Description:**
Print arguments to stdout.

**Example:**
```bash
echo hello world
```

---

### edit

**Binary:** `testdir/binaries/edit`

**Metadata Source:** `testdir/code/edit_uelf.c`

**Description:**
Graphical text editor.

**Example:**
```bash
edit [/path/to/file]
```

---

### fdisk

**Binary:** `testdir/binaries/fdisk`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the fdisk command.

**Example:**
```bash
fdisk
```

---

### files

**Binary:** `testdir/binaries/files`

**Metadata Source:** `testdir/code/files_uelf.c`

**Description:**
Graphical file explorer.

**Example:**
```bash
files [/path]
```

---

### fontpreview

**Binary:** `testdir/binaries/fontpreview`

**Metadata Source:** `testdir/code/fontpreview_uelf.c`

**Description:**
Preview an OTF/TTF font in multiple sizes.

**Example:**
```bash
fontpreview /fonts/unscii-16.otf
```

---

### format

**Binary:** `testdir/binaries/format`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the format command.

**Example:**
```bash
format
```

---

### gui_demo

**Binary:** `testdir/binaries/gui_demo`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the gui_demo command.

**Example:**
```bash
gui_demo
```

---

### hello

**Binary:** `testdir/binaries/hello`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the hello command.

**Example:**
```bash
hello
```

---

### history

**Binary:** `testdir/binaries/history`

**Metadata Source:** `testdir/code/history_uelf.c`

**Description:**
Show or clear command history.

**Example:**
```bash
history
```

---

### installer

**Binary:** `testdir/binaries/installer`

**Metadata Source:** `testdir/code/installer_uelf.c`

**Description:**
EYN-OS graphical installer.

**Example:**
```bash
installer
```

---

### jobs

**Binary:** `testdir/binaries/jobs`

**Metadata Source:** `testdir/code/jobs_uelf.c`

**Description:**
List background jobs.

**Example:**
```bash
jobs
```

---

### list

**Binary:** `testdir/binaries/list`

**Metadata Source:** `testdir/code/list_uelf.c`

**Description:**
List directory entries with terminal icons.

**Example:**
```bash
list [path]
```

---

### metadata

**Binary:** `testdir/binaries/metadata`

**Metadata Source:** `testdir/code/metadata_uelf.c`

**Description:**
Show metadata for a file or directory.

**Example:**
```bash
metadata /binaries/ping
```

---

### pipe

**Binary:** `testdir/binaries/pipe`

**Metadata Source:** `testdir/code/pipe_uelf.c`

**Description:**
Run a command pipeline from userspace command launcher.

**Example:**
```bash
pipe files 'search test -a'
```

---

### random

**Binary:** `testdir/binaries/random`

**Metadata Source:** `testdir/code/random_uelf.c`

**Description:**
Generate random numbers.

**Example:**
```bash
random 5
```

---

### search

**Binary:** `testdir/binaries/search`

**Metadata Source:** `testdir/code/search_uelf.c`

**Description:**
Search for text in filenames and file contents.

**Example:**
```bash
search hello -a
```

---

### sort

**Binary:** `testdir/binaries/sort`

**Metadata Source:** `testdir/code/sort_uelf.c`

**Description:**
Sort strings alphabetically.

**Example:**
```bash
sort zebra apple banana
```

---

### spam

**Binary:** `testdir/binaries/spam`

**Metadata Source:** `testdir/code/spam_uelf.c`

**Description:**
Spam 'EYN-OS' to stdout 100 times.

**Example:**
```bash
spam
```

---

### tiling

**Binary:** `testdir/binaries/tiling`

**Metadata Source:** `testdir/code/tiling_uelf.c`

**Description:**
Launch the tiling manager.

**Example:**
```bash
tiling
```

---

### view_backend_bmp

**Binary:** `testdir/binaries/view_backend_bmp`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the view_backend_bmp command.

**Example:**
```bash
view_backend_bmp
```

---

### view_backend_rei

**Binary:** `testdir/binaries/view_backend_rei`

**Metadata Source:** `testdir/code/(metadata not found)`

**Description:**
Run the view_backend_rei command.

**Example:**
```bash
view_backend_rei
```

---

### xeyes

**Binary:** `testdir/binaries/xeyes`

**Metadata Source:** `testdir/code/xeyes_uelf.c`

**Description:**
X11 xeyes - eyes that follow the mouse pointer

**Example:**
```bash
xeyes
```

---

## Command Statistics

| Category | Count |
|----------|-------|
| Filesystem Commands | 16 |
| System Commands | 9 |
| Network Commands | 6 |
| Memory Commands | 7 |
| GUI/Window Commands | 10 |
| Development Commands | 10 |
| Utility Commands | 26 |

