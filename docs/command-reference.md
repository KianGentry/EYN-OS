# EYN-OS Command Reference

This document is auto-generated from userland command metadata and binaries. Last updated: 2026-06-06 00:53:05

**Total Commands:** 106

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

**Metadata Source:** `EYN-packages/packages/cd/cd_uelf.c`

**Description:**
Change the current directory.

**Example:**
```bash
cd <directory>
```

---

### copy

**Binary:** `testdir/binaries/copy`

**Metadata Source:** `EYN-packages/packages/copy/copy_uelf.c`

**Description:**
Copy a file from source to destination.

**Example:**
```bash
copy file1.txt file2.txt
```

---

### create

**Binary:** `testdir/binaries/create`

**Metadata Source:** `EYN-packages/packages/create/create_uelf.c`

**Description:**
Create a file or directory.

**Example:**
```bash
create test/
```

---

### del

**Binary:** `testdir/binaries/del`

**Metadata Source:** `EYN-packages/packages/del/del_uelf.c`

**Description:**
Delete a file from the filesystem.

**Example:**
```bash
del myfile.txt
```

---

### deldir

**Binary:** `testdir/binaries/deldir`

**Metadata Source:** `EYN-packages/packages/deldir/deldir_uelf.c`

**Description:**
Delete an empty directory.

**Example:**
```bash
deldir myfolder
```

---

### delete

**Binary:** `testdir/binaries/delete`

**Metadata Source:** `EYN-packages/packages/delete/delete_uelf.c`

**Description:**
Delete a file or directory.

**Example:**
```bash
delete test.txt
```

---

### fatfix

**Binary:** `testdir/binaries/fatfix`

**Metadata Source:** `EYN-packages/packages/fatfix/fatfix_uelf.c`

**Description:**
Repair FAT32 directory entry flags.

**Example:**
```bash
fatfix /
```

---

### fscheck

**Binary:** `testdir/binaries/fscheck`

**Metadata Source:** `EYN-packages/packages/fscheck/fscheck_uelf.c`

**Description:**
Check filesystem integrity.

**Example:**
```bash
fscheck
```

---

### head

**Binary:** `testdir/binaries/head`

**Metadata Source:** `EYN-packages/packages/head/head_uelf.c`

**Description:**
Print the first lines of a file.

**Example:**
```bash
head -n 10 /test.txt
```

---

### ls

**Binary:** `testdir/binaries/ls`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the ls command.

**Example:**
```bash
ls
```

---

### makedir

**Binary:** `testdir/binaries/makedir`

**Metadata Source:** `EYN-packages/packages/makedir/makedir_uelf.c`

**Description:**
Create a new directory.

**Example:**
```bash
makedir myfolder
```

---

### move

**Binary:** `testdir/binaries/move`

**Metadata Source:** `EYN-packages/packages/move/move_uelf.c`

**Description:**
Move a file from source to destination.

**Example:**
```bash
move file1.txt /backup/file1.txt
```

---

### pwd

**Binary:** `testdir/binaries/pwd`

**Metadata Source:** `EYN-packages/packages/pwd/pwd_uelf.c`

**Description:**
Print the current working directory.

**Example:**
```bash
pwd
```

---

### read

**Binary:** `testdir/binaries/read`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the read command.

**Example:**
```bash
read
```

---

### size

**Binary:** `testdir/binaries/size`

**Metadata Source:** `EYN-packages/packages/size/size_uelf.c`

**Description:**
Show the size of a file in bytes.

**Example:**
```bash
size myfile.txt
```

---

### tail

**Binary:** `testdir/binaries/tail`

**Metadata Source:** `EYN-packages/packages/tail/tail_uelf.c`

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

**Metadata Source:** `EYN-packages/packages/clear/clear_uelf.c`

**Description:**
Clear terminal output.

**Example:**
```bash
clear
```

---

### drive

**Binary:** `testdir/binaries/drive`

**Metadata Source:** `EYN-packages/packages/drive/drive_uelf.c`

**Description:**
Drive selection helper.

**Example:**
```bash
drive 0
```

---

### exit

**Binary:** `testdir/binaries/exit`

**Metadata Source:** `EYN-packages/packages/exit/exit_uelf.c`

**Description:**
Exits the kernel and shuts down the system.

**Example:**
```bash
exit
```

---

### help

**Binary:** `testdir/binaries/help`

**Metadata Source:** `EYN-packages/packages/help/help_uelf.c`

**Description:**
Display command help.

**Example:**
```bash
help
```

---

### init

**Binary:** `testdir/binaries/init`

**Metadata Source:** `EYN-packages/packages/init/init_uelf.c`

**Description:**
Initialize core services.

**Example:**
```bash
init
```

---

### lsata

**Binary:** `testdir/binaries/lsata`

**Metadata Source:** `EYN-packages/packages/lsata/lsata_uelf.c`

**Description:**
List detected logical drives.

**Example:**
```bash
lsata
```

---

### portable

**Binary:** `testdir/binaries/portable`

**Metadata Source:** `EYN-packages/packages/portable/portable_uelf.c`

**Description:**
Show portability optimization status.

**Example:**
```bash
portable stats
```

---

### serialtest

**Binary:** `testdir/binaries/serialtest`

**Metadata Source:** `EYN-packages/packages/serialtest/serialtest_uelf.c`

**Description:**
Serial output test (userland).

**Example:**
```bash
serialtest
```

---

### ver

**Binary:** `testdir/binaries/ver`

**Metadata Source:** `EYN-packages/packages/ver/ver_uelf.c`

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

**Metadata Source:** `EYN-packages/packages/e1000/e1000_uelf.c`

**Description:**
Intel e1000 utilities.

**Example:**
```bash
e1000 init
```

---

### e1000probe

**Binary:** `testdir/binaries/e1000probe`

**Metadata Source:** `EYN-packages/packages/e1000probe/e1000probe_uelf.c`

**Description:**
Probe Intel e1000 NIC.

**Example:**
```bash
e1000probe
```

---

### netcfg

**Binary:** `testdir/binaries/netcfg`

**Metadata Source:** `EYN-packages/packages/netcfg/netcfg_uelf.c`

**Description:**
Network configuration command.

**Example:**
```bash
netcfg show
```

---

### netstat

**Binary:** `testdir/binaries/netstat`

**Metadata Source:** `EYN-packages/packages/netstat/netstat_uelf.c`

**Description:**
Show network status.

**Example:**
```bash
netstat
```

---

### pciscan

**Binary:** `testdir/binaries/pciscan`

**Metadata Source:** `EYN-packages/packages/pciscan/pciscan_uelf.c`

**Description:**
Scan PCI devices.

**Example:**
```bash
pciscan net
```

---

### ping

**Binary:** `testdir/binaries/ping`

**Metadata Source:** `EYN-packages/packages/ping/ping_uelf.c`

**Description:**
Send ICMP echo requests (IPv4 or hostname).

**Example:**
```bash
ping g.co
```

---

## Memory Commands

### memory

**Binary:** `testdir/binaries/memory`

**Metadata Source:** `EYN-packages/packages/memory/memory_uelf.c`

**Description:**
Memory management and testing.

**Example:**
```bash
memory stats
```

---

### memory_stats

**Binary:** `testdir/binaries/memory_stats`

**Metadata Source:** `EYN-packages/packages/memory_stats/memory_stats_uelf.c`

**Description:**
Show predictive memory statistics

**Example:**
```bash
memory_stats
```

---

### mmap

**Binary:** `testdir/binaries/mmap`

**Metadata Source:** `EYN-packages/packages/mmap/mmap_uelf.c`

**Description:**
Memory map a file for zero-copy access.

**Example:**
```bash
mmap <filename> [readonly]
```

---

### msync

**Binary:** `testdir/binaries/msync`

**Metadata Source:** `EYN-packages/packages/msync/msync_uelf.c`

**Description:**
Synchronize memory-mapped file to disk.

**Example:**
```bash
msync <address>
```

---

### munmap

**Binary:** `testdir/binaries/munmap`

**Metadata Source:** `EYN-packages/packages/munmap/munmap_uelf.c`

**Description:**
Unmap a memory-mapped file.

**Example:**
```bash
munmap <address>
```

---

### pagingguards

**Binary:** `testdir/binaries/pagingguards`

**Metadata Source:** `EYN-packages/packages/pagingguards/pagingguards_uelf.c`

**Description:**
Install optional paging guards.

**Example:**
```bash
pagingguards
```

---

### predict

**Binary:** `testdir/binaries/predict`

**Metadata Source:** `EYN-packages/packages/predict/predict_uelf.c`

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

**Metadata Source:** `EYN-packages/packages/clearbg/clearbg_uelf.c`

**Description:**
Clear background image for focused tile.

**Example:**
```bash
clearbg
```

---

### draw

**Binary:** `testdir/binaries/draw`

**Metadata Source:** `EYN-packages/packages/draw/draw_uelf.c`

**Description:**
Open the draw canvas editor.

**Example:**
```bash
draw /images/sketch.rei
```

---

### kstats

**Binary:** `testdir/binaries/kstats`

**Metadata Source:** `EYN-packages/packages/kstats/kstats_uelf.c`

**Description:**
Legacy kernel stats GUI (migrated default is userland 'stats').

**Example:**
```bash
kstats
```

---

### kwin_test

**Binary:** `testdir/binaries/kwin_test`

**Metadata Source:** `EYN-packages/packages/kwin_test/kwin_test_uelf.c`

**Description:**
Legacy kernel window-test command (migrated default is userland 'win_test').

**Example:**
```bash
kwin_test
```

---

### rect

**Binary:** `testdir/binaries/rect`

**Metadata Source:** `EYN-packages/packages/rect/rect_uelf.c`

**Description:**
Draw a rectangle.

**Example:**
```bash
rect 10 20 100 50 255 0 0
```

---

### setbg

**Binary:** `testdir/binaries/setbg`

**Metadata Source:** `EYN-packages/packages/setbg/setbg_uelf.c`

**Description:**
Set background image for focused tile.

**Example:**
```bash
setbg /images/eynos.rei
```

---

### setfont

**Binary:** `testdir/binaries/setfont`

**Metadata Source:** `EYN-packages/packages/setfont/setfont_uelf.c`

**Description:**
Set runtime system font (.hex/.otf/.ttf).

**Example:**
```bash
setfont /fonts/unscii-16.otf
```

---

### stats

**Binary:** `testdir/binaries/stats`

**Metadata Source:** `EYN-packages/packages/stats/stats_uelf.c`

**Description:**
Open the stats GUI.

**Example:**
```bash
stats
```

---

### theme

**Binary:** `testdir/binaries/theme`

**Metadata Source:** `EYN-packages/packages/theme/theme_uelf.c`

**Description:**
Open the theme editor.

**Example:**
```bash
theme /fonts/unscii-16.hex
```

---

### title

**Binary:** `testdir/binaries/title`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the title command.

**Example:**
```bash
title
```

---

### view

**Binary:** `testdir/binaries/view`

**Metadata Source:** `EYN-packages/packages/view/view_uelf.c`

**Description:**
Dispatch a file to a view backend from /.view.

**Example:**
```bash
view /images/picture.rei
```

---

### win_test

**Binary:** `testdir/binaries/win_test`

**Metadata Source:** `EYN-packages/packages/win_test/win_test_uelf.c`

**Description:**
Open compositor window test.

**Example:**
```bash
win_test
```

---

## Development Commands

### assertfail

**Binary:** `testdir/binaries/assertfail`

**Metadata Source:** `EYN-packages/packages/assertfail/assertfail_uelf.c`

**Description:**
Trigger an assertion failure (ASSERT).

**Example:**
```bash
assertfail yes
```

---

### crashlog

**Binary:** `testdir/binaries/crashlog`

**Metadata Source:** `EYN-packages/packages/crashlog/crashlog_uelf.c`

**Description:**
Inspect and clear crashlog records.

**Example:**
```bash
crashlog dump
```

---

### hexdump

**Binary:** `testdir/binaries/hexdump`

**Metadata Source:** `EYN-packages/packages/hexdump/hexdump_uelf.c`

**Description:**
Hex dump file bytes.

**Example:**
```bash
hexdump /test.txt 256
```

---

### log

**Binary:** `testdir/binaries/log`

**Metadata Source:** `EYN-packages/packages/log/log_uelf.c`

**Description:**
Enable or disable shell logging.

**Example:**
```bash
log on
```

---

### panic

**Binary:** `testdir/binaries/panic`

**Metadata Source:** `EYN-packages/packages/panic/panic_uelf.c`

**Description:**
Trigger a kernel panic for diagnostics.

**Example:**
```bash
panic yes
```

---

### pf

**Binary:** `testdir/binaries/pf`

**Metadata Source:** `EYN-packages/packages/pf/pf_uelf.c`

**Description:**
Intentionally trigger a page fault.

**Example:**
```bash
pf yes [addr] [r|w|x]
```

---

### ring3

**Binary:** `testdir/binaries/ring3`

**Metadata Source:** `EYN-packages/packages/ring3/ring3_uelf.c`

**Description:**
Switch to ring 3 and run a tiny user-mode stub.

**Example:**
```bash
ring3 yes
```

---

### run

**Binary:** `testdir/binaries/run`

**Metadata Source:** `EYN-packages/packages/run/run_uelf.c`

**Description:**
Run a native program, .uelf, or script.

**Example:**
```bash
run <program> [args...]
```

---

### validate

**Binary:** `testdir/binaries/validate`

**Metadata Source:** `EYN-packages/packages/validate/validate_uelf.c`

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

**Metadata Source:** `EYN-packages/packages/alias/alias_uelf.c`

**Description:**
Create or remove command aliases.

**Example:**
```bash
alias ll ls -l
```

---

### breakout

**Binary:** `testdir/binaries/breakout`

**Metadata Source:** `EYN-packages/packages/breakout/breakout_uelf.c`

**Description:**
Arcade brick-breaker with paddle controls.

**Example:**
```bash
breakout
```

---

### calc

**Binary:** `testdir/binaries/calc`

**Metadata Source:** `EYN-packages/packages/calc/calc_uelf.c`

**Description:**
Integer calculator supporting + - * /.

**Example:**
```bash
calc 2+3*4
```

---

### cat

**Binary:** `testdir/binaries/cat`

**Metadata Source:** `EYN-packages/packages/cat/cat_uelf.c`

**Description:**
Print a file to stdout.

**Example:**
```bash
read /test.txt
```

---

### diskmgr

**Binary:** `testdir/binaries/diskmgr`

**Metadata Source:** `EYN-packages/packages/diskmgr/diskmgr_uelf.c`

**Description:**
Manage logical drives from userland.

**Example:**
```bash
diskmgr status
```

---

### download

**Binary:** `testdir/binaries/download`

**Metadata Source:** `EYN-packages/packages/download/download_uelf.c`

**Description:**
Download a file over HTTP/1.1 or HTTPS/TLS (GET only) with DNS support.

**Example:**
```bash
download https://example.com/index.html
```

---

### echo

**Binary:** `testdir/binaries/echo`

**Metadata Source:** `EYN-packages/packages/echo/echo_uelf.c`

**Description:**
Print arguments to stdout.

**Example:**
```bash
echo hello world
```

---

### edit

**Binary:** `testdir/binaries/edit`

**Metadata Source:** `EYN-packages/packages/edit/edit_uelf.c`

**Description:**
Graphical text editor.

**Example:**
```bash
edit [/path/to/file]
```

---

### extract

**Binary:** `testdir/binaries/extract`

**Metadata Source:** `EYN-packages/packages/extract/extract_uelf.c`

**Description:**
Extract a TAR or TAR.GZ archive into a directory.

**Example:**
```bash
extract /archive.tar.gz /out
```

---

### eynfetch

**Binary:** `testdir/binaries/eynfetch`

**Metadata Source:** `EYN-packages/packages/eynfetch/eynfetch_uelf.c`

**Description:**
Display system information with ASCII art logo.

**Example:**
```bash
eynfetch
```

---

### fdisk

**Binary:** `testdir/binaries/fdisk`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the fdisk command.

**Example:**
```bash
fdisk
```

---

### files

**Binary:** `testdir/binaries/files`

**Metadata Source:** `EYN-packages/packages/files/files_uelf.c`

**Description:**
Graphical file explorer.

**Example:**
```bash
files [/path]
```

---

### fontpreview

**Binary:** `testdir/binaries/fontpreview`

**Metadata Source:** `EYN-packages/packages/fontpreview/fontpreview_uelf.c`

**Description:**
Preview an OTF/TTF font in multiple sizes.

**Example:**
```bash
fontpreview /fonts/unscii-16.otf
```

---

### format

**Binary:** `testdir/binaries/format`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the format command.

**Example:**
```bash
format
```

---

### gui_demo

**Binary:** `testdir/binaries/gui_demo`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the gui_demo command.

**Example:**
```bash
gui_demo
```

---

### hello

**Binary:** `testdir/binaries/hello`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the hello command.

**Example:**
```bash
hello
```

---

### hello_c

**Binary:** `testdir/binaries/hello_c`

**Metadata Source:** `EYN-packages/packages/hello_c/hello_c_uelf.c`

**Description:**
Print Hello World.

**Example:**
```bash
hello
```

---

### history

**Binary:** `testdir/binaries/history`

**Metadata Source:** `EYN-packages/packages/history/history_uelf.c`

**Description:**
Show or clear command history.

**Example:**
```bash
history
```

---

### install

**Binary:** `testdir/binaries/install`

**Metadata Source:** `EYN-packages/packages/install/install_uelf.c`

**Description:**
Install, update, remove, and search packages.

**Example:**
```bash
install --update-all
```

---

### installer

**Binary:** `testdir/binaries/installer`

**Metadata Source:** `EYN-packages/packages/installer/installer_uelf.c`

**Description:**
EYN-OS graphical installer.

**Example:**
```bash
installer
```

---

### jobs

**Binary:** `testdir/binaries/jobs`

**Metadata Source:** `EYN-packages/packages/jobs/jobs_uelf.c`

**Description:**
List background jobs.

**Example:**
```bash
jobs
```

---

### jsonparse

**Binary:** `testdir/binaries/jsonparse`

**Metadata Source:** `EYN-packages/packages/jsonparse/jsonparse_uelf.c`

**Description:**
Parse and normalize JSON files.

**Example:**
```bash
jsonparse --pretty /config/settings.json
```

---

### ldso

**Binary:** `testdir/binaries/ldso`

**Metadata Source:** `EYN-packages/packages/ldso/ldso_uelf.c`

**Description:**
Minimal ELF dynamic loader for EYN-OS.

**Example:**
```bash
ldso <program> [args...]
```

---

### ldso_debug

**Binary:** `testdir/binaries/ldso_debug`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the ldso_debug command.

**Example:**
```bash
ldso_debug
```

---

### list

**Binary:** `testdir/binaries/list`

**Metadata Source:** `EYN-packages/packages/list/list_uelf.c`

**Description:**
List directory entries with terminal icons.

**Example:**
```bash
list [path]
```

---

### metadata

**Binary:** `testdir/binaries/metadata`

**Metadata Source:** `EYN-packages/packages/metadata/metadata_uelf.c`

**Description:**
Show metadata for a file or directory.

**Example:**
```bash
metadata /binaries/ping
```

---

### notify

**Binary:** `testdir/binaries/notify`

**Metadata Source:** `EYN-packages/packages/notify/notify_uelf.c`

**Description:**
Show a graphical notification popup.

**Example:**
```bash
notify --title Update --message 'Updates available'
```

---

### pipe

**Binary:** `testdir/binaries/pipe`

**Metadata Source:** `EYN-packages/packages/pipe/pipe_uelf.c`

**Description:**
Run a command pipeline from userspace command launcher.

**Example:**
```bash
pipe files 'search test -a'
```

---

### ptyspawn

**Binary:** `testdir/binaries/ptyspawn`

**Metadata Source:** `EYN-packages/packages/ptyspawn/ptyspawn_uelf.c`

**Description:**
Spawn a child process on a PTY slave and relay its output.

**Example:**
```bash
ptyspawn /binaries/help
```

---

### random

**Binary:** `testdir/binaries/random`

**Metadata Source:** `EYN-packages/packages/random/random_uelf.c`

**Description:**
Generate random numbers.

**Example:**
```bash
random 5
```

---

### search

**Binary:** `testdir/binaries/search`

**Metadata Source:** `EYN-packages/packages/search/search_uelf.c`

**Description:**
Search for text in filenames and file contents.

**Example:**
```bash
search hello -a
```

---

### settings

**Binary:** `testdir/binaries/settings`

**Metadata Source:** `EYN-packages/packages/settings/settings_uelf.c`

**Description:**
Open system settings (video + customization).

**Example:**
```bash
settings
```

---

### sha256

**Binary:** `testdir/binaries/sha256`

**Metadata Source:** `EYN-packages/packages/sha256/sha256_uelf.c`

**Description:**
Compute SHA-256 digests for files or strings.

**Example:**
```bash
sha256 /test.txt
```

---

### snake

**Binary:** `testdir/binaries/snake`

**Metadata Source:** `EYN-packages/packages/snake/snake_uelf.c`

**Description:**
Classic snake game with keyboard controls.

**Example:**
```bash
snake
```

---

### sort

**Binary:** `testdir/binaries/sort`

**Metadata Source:** `EYN-packages/packages/sort/sort_uelf.c`

**Description:**
Sort strings alphabetically.

**Example:**
```bash
sort zebra apple banana
```

---

### spam

**Binary:** `testdir/binaries/spam`

**Metadata Source:** `EYN-packages/packages/spam/spam_uelf.c`

**Description:**
Spam 'EYN-OS' to stdout 100 times.

**Example:**
```bash
spam
```

---

### ssh

**Binary:** `testdir/binaries/ssh`

**Metadata Source:** `EYN-packages/packages/ssh/ssh_uelf.c`

**Description:**
SSH client with password auth and interactive shell relay.

**Example:**
```bash
ssh user@192.168.1.10
```

---

### tetris

**Binary:** `testdir/binaries/tetris`

**Metadata Source:** `EYN-packages/packages/tetris/tetris_uelf.c`

**Description:**
Play classic falling-block puzzle game.

**Example:**
```bash
tetris
```

---

### tiling

**Binary:** `testdir/binaries/tiling`

**Metadata Source:** `EYN-packages/packages/tiling/tiling_uelf.c`

**Description:**
Launch the tiling manager.

**Example:**
```bash
tiling
```

---

### view_backend_bmp

**Binary:** `testdir/binaries/view_backend_bmp`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the view_backend_bmp command.

**Example:**
```bash
view_backend_bmp
```

---

### view_backend_builtin

**Binary:** `testdir/binaries/view_backend_builtin`

**Metadata Source:** `EYN-packages/packages/view_backend_builtin/view_backend_builtin_uelf.c`

**Description:**
Open an REI, BMP image, REIV video, REIS audio, or WAV audio viewer.

**Example:**
```bash
view /images/picture.rei
```

---

### view_backend_rei

**Binary:** `testdir/binaries/view_backend_rei`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the view_backend_rei command.

**Example:**
```bash
view_backend_rei
```

---

### view_backend_reis

**Binary:** `testdir/binaries/view_backend_reis`

**Metadata Source:** `EYN-packages/packages/view_backend_reis/view_backend_reis_uelf.c`

**Description:**
Play a REIS audio file in a dedicated backend.

**Example:**
```bash
view_backend_reis /audio/track.reis
```

---

### view_backend_reiv

**Binary:** `testdir/binaries/view_backend_reiv`

**Metadata Source:** `EYN-packages/packages/view_backend_reiv/view_backend_reiv_uelf.c`

**Description:**
Play a REIV video file in a dedicated backend.

**Example:**
```bash
view_backend_reiv /videos/demo.reiv
```

---

### xeyes

**Binary:** `testdir/binaries/xeyes`

**Metadata Source:** `EYN-packages/packages/xeyes/xeyes_uelf.c`

**Description:**
X11 xeyes - eyes that follow the mouse pointer

**Example:**
```bash
xeyes
```

---

### xeyes_local

**Binary:** `testdir/binaries/xeyes_local`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the xeyes_local command.

**Example:**
```bash
xeyes_local
```

---

### zsnes_1_51

**Binary:** `testdir/binaries/zsnes_1_51`

**Metadata Source:** `(metadata not found)`

**Description:**
Run the zsnes_1_51 command.

**Example:**
```bash
zsnes_1_51
```

---

## Command Statistics

| Category | Count |
|----------|-------|
| Filesystem Commands | 16 |
| System Commands | 9 |
| Network Commands | 6 |
| Memory Commands | 7 |
| GUI/Window Commands | 12 |
| Development Commands | 9 |
| Utility Commands | 47 |

