COMPILER = gcc
LINKER = ld
ASSEMBLER = nasm

# Prefer grub2-mkrescue if available; fall back to grub-mkrescue
# Path is resolved at parse time; if neither exists, we'll stop in the build rule with a friendly message
GRUB_MKRESCUE := $(shell command -v grub2-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null)

# Kernel (freestanding) compiler flags
# Note: keep frame pointers for stack traces; avoid stack protector & fortify in freestanding kernel
CPU_HAS_INVLPG ?= 0

KERNEL_CFLAGS = -m32 -march=i386 -mtune=i386 -c -ffreestanding -fno-builtin -fno-omit-frame-pointer -fno-common -MMD -MP \
		 -Os -fno-strict-overflow -fwrapv \
		 -DCONFIG_CPU_HAS_INVLPG=$(CPU_HAS_INVLPG) \
		 -fdata-sections -ffunction-sections \
		 -I include/ -I include/cpu -I include/drivers -I include/misc -I include/graphics -I include/network -I include/utilities -I include/utilities/shell \
		 -Wall -Wextra -Werror=implicit-function-declaration -Wformat=2 -Wformat-security \
		 -Wno-unused-parameter -Wno-unused-variable \
		 -Wnull-dereference -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
		 -Wpointer-arith -Wshadow -Wundef -Wredundant-decls -Wswitch-enum -Wswitch-default

# Map legacy CFLAGS to kernel flags to avoid touching all compile rules below
CFLAGS = $(KERNEL_CFLAGS)

# Use a more aggressive optimization level for GUI-heavy compilation units
# Flip -Os to -O2 only for these files to speed up inner pixel loops
GUI_CFLAGS = $(KERNEL_CFLAGS:-Os=-O2)

# Host (tooling) compiler/linker flags (for eynfs_format, tests, etc.)
HOST_CFLAGS = -O2 -g -Wall -Wextra -Wformat=2 -Wformat-security -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
		 -I include -I include/misc -I include/drivers -I include/cpu -I include/utilities -I include/graphics -I include/network
HOST_LDFLAGS = -Wl,-z,relro,-z,now
		

# Optional toggles (not wired to rules by default)
DEBUG_CFLAGS = $(KERNEL_CFLAGS) -g -O0 -DDEBUG -D_DEBUG
RELEASE_CFLAGS = $(KERNEL_CFLAGS) -O2 -DNDEBUG
ASFLAGS = -f elf32
## Linker flags: target i386 linker script, enable section GC, strip symbols, and emit a map
TMPDIR ?= tmp_user
BOOTDIR = $(TMPDIR)/boot
LDFLAGS = -m elf_i386 -T src/boot/link.ld --gc-sections -Map $(BOOTDIR)/kernel.map -s
EMULATOR = qemu-system-i386

# QEMU display backend.
QEMU_DISPLAY ?= gtk,grab-on-hover=on

# QEMU environment prefix.
# Default forces GTK to use X11/XWayland for reliable mouse grab under Wayland.
# Override at invocation time if needed, e.g.:
#   make run QEMU_ENV=
#   make run QEMU_ENV=GDK_BACKEND=wayland
QEMU_ENV ?= GDK_BACKEND=x11
EMULATOR_FLAGS = -kernel

OBJS = obj/kasm.o obj/kc.o obj/gdt.o obj/gdt_asm.o obj/idt.o obj/isr.o obj/isr_stubs.o obj/syscall.o obj/fpu.o obj/kb.o obj/string.o obj/system.o obj/arch.o obj/util.o obj/mem386.o obj/slab.o obj/shell.o obj/shell_args.o obj/math.o obj/vga.o obj/serial.o obj/fat32.o obj/ata.o obj/eynfs.o obj/rei.o obj/reiv.o obj/tui.o obj/run_command.o obj/history.o obj/alias.o obj/predictive_memory.o obj/zero_copy.o obj/vmm.o obj/paging_compat.o obj/user_access.o obj/pipeline.o obj/kernel_api.o obj/native_exec.o obj/user_elf.o obj/sched.o obj/irq.o obj/irq_stubs.o obj/mouse.o obj/vfs.o obj/panic.o obj/watchdog.o obj/capabilities.o obj/segdom.o obj/crashlog.o obj/context.o obj/fs_txn.o obj/linux_syscalls.o

OBJS += obj/tiling_manager.o obj/ui_prefs.o
OBJS += obj/terminals.o
OBJS += obj/partition.o
OBJS += obj/pci.o
OBJS += obj/e1000.o
OBJS += obj/netstack.o
OBJS += obj/shell_script.o
OBJS += obj/ac97.o
OBJS += obj/reis.o
OBJS += obj/otf_font.o
OUTPUT = $(BOOTDIR)/kernel.bin

# Source files to object files

all:$(OBJS)
	mkdir $(TMPDIR)/ -p
	mkdir $(BOOTDIR)/ -p
	$(LINKER) $(LDFLAGS) -o $(OUTPUT) $(OBJS)

docs: all
	python3 devtools/generate_command_docs.py src/

obj/kasm.o:src/boot/kernel.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/kasm.o src/boot/kernel.asm

obj/syscall.o:src/cpu/syscall.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/syscall.o src/cpu/syscall.asm

obj/isr_stubs.o:src/cpu/isr.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/isr_stubs.o src/cpu/isr.asm

obj/irq_stubs.o:src/cpu/irq.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/irq_stubs.o src/cpu/irq.asm

obj/mem386.o:src/misc/mem386.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/mem386.o src/misc/mem386.asm
	
obj/kc.o:src/entry/kernel.c
	$(COMPILER) $(CFLAGS) src/entry/kernel.c -o obj/kc.o 
	
obj/idt.o:src/cpu/idt.c
	$(COMPILER) $(CFLAGS) src/cpu/idt.c -o obj/idt.o 

obj/fpu.o:src/cpu/fpu.c
	$(COMPILER) $(CFLAGS) src/cpu/fpu.c -o obj/fpu.o

obj/gdt.o:src/cpu/gdt.c
	$(COMPILER) $(CFLAGS) src/cpu/gdt.c -o obj/gdt.o

obj/gdt_asm.o:src/cpu/gdt.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/gdt_asm.o src/cpu/gdt.asm

obj/kb.o:src/drivers/kb.c
	$(COMPILER) $(CFLAGS) src/drivers/kb.c -o obj/kb.o

obj/isr.o:src/cpu/isr.c
	$(COMPILER) $(CFLAGS) src/cpu/isr.c -o obj/isr.o

obj/user_elf.o:src/cpu/user_elf.c
	$(COMPILER) $(CFLAGS) src/cpu/user_elf.c -o obj/user_elf.o

obj/string.o:src/utilities/shell/string.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/string.c -o obj/string.o

obj/shell_args.o:src/utilities/shell/shell_args.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_args.c -o obj/shell_args.o

obj/system.o:src/cpu/system.c
	$(COMPILER) $(CFLAGS) src/cpu/system.c -o obj/system.o

obj/arch.o:src/cpu/arch.c
	$(COMPILER) $(CFLAGS) src/cpu/arch.c -o obj/arch.o

obj/util.o:src/utilities/util.c
	$(COMPILER) $(CFLAGS) src/utilities/util.c -o obj/util.o

obj/slab.o:src/mm/slab.c
	$(COMPILER) $(CFLAGS) src/mm/slab.c -o obj/slab.o
	
obj/shell.o:src/utilities/shell/shell.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell.c -o obj/shell.o

obj/shell_script.o:src/utilities/shell/shell_script.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_script.c -o obj/shell_script.o

obj/math.o:src/utilities/basic/math.c
	$(COMPILER) $(CFLAGS) src/utilities/basic/math.c -o obj/math.o

obj/vga.o:src/drivers/vga.c
	$(COMPILER) $(CFLAGS) src/drivers/vga.c -o obj/vga.o
obj/mouse.o:src/drivers/mouse.c
	$(COMPILER) $(CFLAGS) src/drivers/mouse.c -o obj/mouse.o

obj/serial.o:src/drivers/serial.c
	$(COMPILER) $(CFLAGS) src/drivers/serial.c -o obj/serial.o

obj/pci.o:src/drivers/pci.c
	$(COMPILER) $(CFLAGS) src/drivers/pci.c -o obj/pci.o

obj/e1000.o:src/drivers/e1000.c
	$(COMPILER) $(CFLAGS) src/drivers/e1000.c -o obj/e1000.o

obj/netstack.o:src/network/netstack.c
	$(COMPILER) $(CFLAGS) src/network/netstack.c -o obj/netstack.o

obj/panic.o:src/misc/panic.c
	$(COMPILER) $(CFLAGS) src/misc/panic.c -o obj/panic.o

obj/watchdog.o:src/misc/watchdog.c
	$(COMPILER) $(CFLAGS) src/misc/watchdog.c -o obj/watchdog.o

obj/capabilities.o:src/misc/capabilities.c
	$(COMPILER) $(CFLAGS) src/misc/capabilities.c -o obj/capabilities.o

obj/segdom.o:src/cpu/segdom.c
	$(COMPILER) $(CFLAGS) src/cpu/segdom.c -o obj/segdom.o

obj/crashlog.o:src/misc/crashlog.c
	$(COMPILER) $(CFLAGS) src/misc/crashlog.c -o obj/crashlog.o

obj/context.o:src/misc/context.c
	$(COMPILER) $(CFLAGS) src/misc/context.c -o obj/context.o

obj/fs_txn.o:src/fs/fs_txn.c
	$(COMPILER) $(CFLAGS) src/fs/fs_txn.c -o obj/fs_txn.o

## QR renderer disabled (no longer used by panic screen)

obj/fat32.o:src/drivers/fat32.c
	$(COMPILER) $(CFLAGS) src/drivers/fat32.c -o obj/fat32.o

obj/ata.o:src/drivers/ata.c
	$(COMPILER) $(CFLAGS) src/drivers/ata.c -o obj/ata.o

obj/eynfs.o:src/drivers/eynfs.c
	$(COMPILER) $(CFLAGS) src/drivers/eynfs.c -o obj/eynfs.o

obj/partition.o:src/drivers/partition.c
	$(COMPILER) $(CFLAGS) src/drivers/partition.c -o obj/partition.o

obj/rei.o:src/drivers/rei.c
	$(COMPILER) $(CFLAGS) src/drivers/rei.c -o obj/rei.o

obj/reiv.o:src/drivers/reiv.c
	$(COMPILER) $(CFLAGS) src/drivers/reiv.c -o obj/reiv.o

obj/reis.o:src/drivers/reis.c
	$(COMPILER) $(CFLAGS) src/drivers/reis.c -o obj/reis.o

OTF_CFLAGS = $(KERNEL_CFLAGS) -Wno-shadow -Wno-switch-enum -Wno-switch-default -Wno-old-style-definition -Wno-strict-prototypes -Wno-missing-prototypes
obj/otf_font.o:src/drivers/otf_font.c include/drivers/otf_font.h include/third_party/stb_truetype.h
	$(COMPILER) $(OTF_CFLAGS) src/drivers/otf_font.c -o obj/otf_font.o

obj/ac97.o:src/drivers/ac97.c
	$(COMPILER) $(CFLAGS) src/drivers/ac97.c -o obj/ac97.o

obj/run_command.o:src/utilities/shell/run_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/run_command.c -o obj/run_command.o

obj/alias.o:src/utilities/shell/alias.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/alias.c -o obj/alias.o

obj/history.o:src/utilities/shell/history.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/history.c -o obj/history.o

obj/compile_command.o:src/utilities/shell/compile_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/compile_command.c -o obj/compile_command.o

obj/tui.o:src/utilities/tui/tui.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/tui.c -o obj/tui.o
obj/tiling_manager.o:src/gui/tiling_manager.c src/gui/gui_state.c src/gui/gui_wm.c src/gui/gui_tiles.c src/gui/gui_taskbar.c src/gui/gui_input.c
	$(COMPILER) $(GUI_CFLAGS) src/gui/tiling_manager.c -o obj/tiling_manager.o

obj/terminals.o:src/utilities/tui/terminals.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/terminals.c -o obj/terminals.o

obj/ui_prefs.o:src/utilities/tui/ui_prefs.c
	$(COMPILER) $(CFLAGS) src/utilities/tui/ui_prefs.c -o obj/ui_prefs.o

obj/vfs.o:src/fs/vfs.c
	$(COMPILER) $(CFLAGS) src/fs/vfs.c -o obj/vfs.o

obj/linux_syscalls.o:src/cpu/linux_syscalls.c
	$(COMPILER) $(CFLAGS) src/cpu/linux_syscalls.c -o obj/linux_syscalls.o

obj/predictive_memory.o:src/utilities/predictive_memory.c
	$(COMPILER) $(CFLAGS) src/utilities/predictive_memory.c -o obj/predictive_memory.o

obj/zero_copy.o:src/utilities/zero_copy.c
	$(COMPILER) $(CFLAGS) src/utilities/zero_copy.c -o obj/zero_copy.o

obj/vmm.o:src/mm/vmm.c
	$(COMPILER) $(CFLAGS) -I include/mm src/mm/vmm.c -o obj/vmm.o

obj/paging_compat.o:src/mm/paging_compat.c
	$(COMPILER) $(CFLAGS) -I include/mm src/mm/paging_compat.c -o obj/paging_compat.o

obj/user_access.o:src/mm/user_access.c include/mm/user_access.h
	$(COMPILER) $(CFLAGS) src/mm/user_access.c -o obj/user_access.o

obj/pipeline.o:src/utilities/shell/pipeline.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/pipeline.c -o obj/pipeline.o

obj/kernel_api.o:src/cpu/kernel_api.c
	$(COMPILER) $(CFLAGS) src/cpu/kernel_api.c -o obj/kernel_api.o

obj/native_exec.o:src/cpu/native_exec.c
	$(COMPILER) $(CFLAGS) src/cpu/native_exec.c -o obj/native_exec.o

obj/sched.o:src/cpu/sched.c include/misc/sched.h
	$(COMPILER) $(CFLAGS) src/cpu/sched.c -o obj/sched.o

obj/irq.o:src/cpu/irq.c include/cpu/irq.h
	$(COMPILER) $(CFLAGS) src/cpu/irq.c -o obj/irq.o

# Actually building the OS (The stuff you should actually run, i.e. make run, make build, etc.)

build: all installer_ramdisk eynfsimg docs
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "grub-mkrescue not found. Install grub2 (grub2-mkrescue) or grub-pc-bin (grub-mkrescue)."; \
		exit 1; \
	fi
	bash devtools/build_iso.sh "$(GRUB_MKRESCUE)"

.PHONY: installer_userland installer_ramdisk
installer_userland:
	bash devtools/build_user_c.sh testdir/code/installer_uelf.c testdir/binaries/installer

installer_ramdisk: installer_userland
	mkdir -p tmp_user/boot
	python3 devtools/build_installer_ramdisk.py tmp_user/boot/installer_ramdisk.img

clean:
	rm -rf obj/*.o tmp/boot/kernel.bin *.img eynfs_format EYNOS.iso
	@rm -rf tmp/grub_minimal tmp/grub_ultra_minimal tmp/grub_ultra_minimal.* tmp/grub.* tmp/iso_edit.* tmp/iso_clean.* 2>/dev/null || true
	rm -f userland/*.o userland/*.bin

clear: clean

# Build the userland EYNFS format tool
eynfs_format: eynfs_format.c
	$(COMPILER) $(HOST_CFLAGS) -o eynfs_format eynfs_format.c $(HOST_LDFLAGS)

# Create and format a 10MB partitioned EYNFS disk image
# Layout: 5MB EYNFS (user files) + 5MB Swap
eynfsimg:
	rm -f eynfs.img
	python3 devtools/create_partitioned_disk.py eynfs.img
	python3 devtools/copy_testdir_to_eynfs.py testdir/

# Rebuild all userland C programs from source in testdir/code/.
# Run this after editing any *_uelf.c file, then run 'make build'.
# Usage: make userland
.PHONY: userland
userland:
	@for src in testdir/code/*_uelf.c; do \
		name=$$(basename "$$src" _uelf.c); \
		out="testdir/binaries/$$name"; \
		echo "Building $$name ..."; \
		bash devtools/build_user_c.sh "$$src" "$$out" || true; \
	done

# Legacy non-partitioned disk image (for testing/compatibility)
eynfsimg-legacy:
	rm -f eynfs.img
	# Create a 10MB image (20,480 sectors at 512 bytes)
	dd if=/dev/zero of=eynfs.img bs=1M count=10
	$(COMPILER) $(HOST_CFLAGS) -o eynfs_format eynfs_format.c $(HOST_LDFLAGS)
	# Pass explicit sector count to avoid defaulting to 500MB in the formatter
	./eynfs_format eynfs.img 20480
	python3 devtools/copy_testdir_to_eynfs.py testdir/

# Create blank drive for testing
testimg: eynfs_format
	rm -f testimg.img
	dd if=/dev/zero of=testimg.img bs=1M count=10
	$(COMPILER) $(HOST_CFLAGS) -o eynfs_format eynfs_format.c $(HOST_LDFLAGS)
	./eynfs_format testimg.img 20480

# Rebuilds and runs the OS

run: build
	$(QEMU_ENV) qemu-system-i386 -cdrom EYNOS.iso \
	-drive file=eynfs.img,format=raw,if=ide,index=0,media=disk \
	-boot d \
	-display $(QEMU_DISPLAY) \
	-netdev user,id=net0,hostfwd=udp::10000-:9999,hostfwd=tcp::10000-:9999 \
	-device e1000,netdev=net0 \
	-audiodev pipewire,id=audio0 \
	-device ac97,audiodev=audio0 \
	-m 128M

# Debug run with serial logging and detailed CPU/interrupt logs
.PHONY: qemu-debug
qemu-debug: build
	@mkdir -p tmp
	$(QEMU_ENV) qemu-system-i386 -cdrom EYNOS.iso \
	-drive file=eynfs.img,format=raw,if=ide,index=0,media=disk \
	-boot d \
	-display $(QEMU_DISPLAY) \
	-netdev user,id=net0,hostfwd=udp::10000-:9999,hostfwd=tcp::10000-:9999 \
	-device e1000,netdev=net0 \
	-audiodev pipewire,id=audio0 \
	-device ac97,audiodev=audio0 \
	-serial stdio \
	-d int,cpu_reset -D tmp/qemu-debug.log \
	-no-reboot -no-shutdown \
	-m 64M

# Halt at start for GDB attach on tcp:1234 (target remote :1234)
.PHONY: qemu-gdb
qemu-gdb: build
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-boot d \
	-S -s \
	-serial stdio \
	-m 64M
# Just runs the OS, no rebuilding.

test: testimg
	$(QEMU_ENV) qemu-system-i386 -cdrom EYNOS.iso \
	-drive file=eynfs.img,format=raw,if=ide,index=0,media=disk \
	-boot d \
	-display $(QEMU_DISPLAY) \
	-netdev user,id=net0,hostfwd=udp::10000-:9999,hostfwd=tcp::10000-:9999 \
	-device e1000,netdev=net0 \
	-serial stdio \
	-d int,cpu_reset -D tmp/qemu-debug.log \
	-no-reboot -no-shutdown \
	-m 24M

# Create a FAT32 disk image for testing (requires mkfs.vfat from dosfstools)
fat32img:
	rm -f fat32.img
	dd if=/dev/zero of=fat32.img bs=1M count=64
	@if command -v mkfs.vfat >/dev/null 2>&1; then \
		mkfs.vfat -F 32 -n EYNOS fat32.img; \
		echo "FAT32 image created: fat32.img"; \
	else \
		echo "mkfs.vfat not found. Please install 'dosfstools' (e.g., sudo apt install dosfstools)."; \
		exit 1; \
	fi

# Optionally populate fat32.img with testdir contents if mtools is installed
fat32img-populate: fat32img
	@if command -v mcopy >/dev/null 2>&1; then \
		mcopy -i fat32.img -s testdir/* ::/ 2>/dev/null || true; \
		echo "Copied testdir/ into fat32.img"; \
	else \
		echo "mtools not found; skipping population. Install 'mtools' to auto-copy files."; \
	fi

# Run with FAT32 drive attached as primary disk
runfat32: build fat32img
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-hdb fat32.img \
	-boot d \
	-m 64M

# Static analysis (GCC -fanalyzer) over all kernel sources
.PHONY: analyze
analyze:
	@echo "Running GCC static analyzer over src/**/*.c ..."
	@find src -name "*.c" -print0 | xargs -0 -I{} sh -c 'echo Analyzing {}; $(COMPILER) $(KERNEL_CFLAGS) -fanalyzer -c {} -o /dev/null' || true
# Auto-generated header dependency files (produced by -MMD -MP in KERNEL_CFLAGS).
-include $(wildcard obj/*.d)
