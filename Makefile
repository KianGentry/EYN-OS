COMPILER = gcc
LINKER = ld
ASSEMBLER = nasm

# Prefer grub2-mkrescue if available; fall back to grub-mkrescue
# Path is resolved at parse time; if neither exists, we'll stop in the build rule with a friendly message
GRUB_MKRESCUE := $(shell command -v grub2-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null)

# Kernel (freestanding) compiler flags
# Note: keep frame pointers for stack traces; avoid stack protector & fortify in freestanding kernel
KERNEL_CFLAGS = -m32 -c -ffreestanding -fno-builtin -fno-omit-frame-pointer -fno-common \
		 -Os -fno-strict-overflow -fwrapv \
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
LDFLAGS = -m elf_i386 -T src/boot/link.ld --gc-sections -Map tmp/boot/kernel.map -s
EMULATOR = qemu-system-i386
EMULATOR_FLAGS = -kernel

OBJS = obj/kasm.o obj/kc.o obj/idt.o obj/isr.o obj/syscall.o obj/kb.o obj/string.o obj/system.o obj/util.o obj/shell.o obj/math.o obj/vga.o obj/serial.o obj/fat32.o obj/ata.o obj/eynfs.o obj/rei.o obj/shell_commands.o obj/fs_commands.o obj/fdisk_commands.o obj/format_command.o obj/write_editor.o obj/tui.o obj/help_tui.o obj/assemble.o obj/instruction_set.o obj/run_command.o obj/shell_script.o obj/history.o obj/subcommands.o obj/predictive_memory.o obj/predictive_commands.o obj/zero_copy.o obj/zero_copy_commands.o obj/paging.o obj/pipeline.o obj/kernel_api.o obj/native_exec.o obj/native_run.o obj/sched.o obj/irq.o obj/irq_stubs.o obj/mouse.o obj/draw_gui.o obj/image_viewer_gui.o obj/window_test.o obj/vfs.o obj/stats_gui.o obj/panic.o obj/watchdog.o

OBJS += obj/tiling_manager.o obj/tiling_cmd.o
OBJS += obj/terminals.o
OUTPUT = tmp/boot/kernel.bin

# Source files to object files

all:$(OBJS)
	mkdir tmp/ -p
	mkdir tmp/boot/ -p
	$(LINKER) $(LDFLAGS) -o $(OUTPUT) $(OBJS)

docs: all
	python3 devtools/generate_command_docs.py src/

obj/kasm.o:src/boot/kernel.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/kasm.o src/boot/kernel.asm

obj/syscall.o:src/cpu/syscall.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/syscall.o src/cpu/syscall.asm

obj/irq_stubs.o:src/cpu/irq.asm
	mkdir obj/ -p
	$(ASSEMBLER) $(ASFLAGS) -o obj/irq_stubs.o src/cpu/irq.asm
	
obj/kc.o:src/entry/kernel.c
	$(COMPILER) $(CFLAGS) src/entry/kernel.c -o obj/kc.o 
	
obj/idt.o:src/cpu/idt.c
	$(COMPILER) $(CFLAGS) src/cpu/idt.c -o obj/idt.o 

obj/kb.o:src/drivers/kb.c
	$(COMPILER) $(CFLAGS) src/drivers/kb.c -o obj/kb.o

obj/isr.o:src/cpu/isr.c
	$(COMPILER) $(CFLAGS) src/cpu/isr.c -o obj/isr.o

obj/string.o:src/utilities/shell/string.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/string.c -o obj/string.o

obj/system.o:src/cpu/system.c
	$(COMPILER) $(CFLAGS) src/cpu/system.c -o obj/system.o

obj/util.o:src/utilities/util.c
	$(COMPILER) $(CFLAGS) src/utilities/util.c -o obj/util.o
	
obj/shell.o:src/utilities/shell/shell.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell.c -o obj/shell.o

obj/math.o:src/utilities/basic/math.c
	$(COMPILER) $(CFLAGS) src/utilities/basic/math.c -o obj/math.o

obj/vga.o:src/drivers/vga.c
	$(COMPILER) $(CFLAGS) src/drivers/vga.c -o obj/vga.o
obj/mouse.o:src/drivers/mouse.c
	$(COMPILER) $(CFLAGS) src/drivers/mouse.c -o obj/mouse.o

obj/serial.o:src/drivers/serial.c
	$(COMPILER) $(CFLAGS) src/drivers/serial.c -o obj/serial.o

obj/panic.o:src/misc/panic.c
	$(COMPILER) $(CFLAGS) src/misc/panic.c -o obj/panic.o

obj/watchdog.o:src/misc/watchdog.c
	$(COMPILER) $(CFLAGS) src/misc/watchdog.c -o obj/watchdog.o

## QR renderer disabled (no longer used by panic screen)

obj/fat32.o:src/drivers/fat32.c
	$(COMPILER) $(CFLAGS) src/drivers/fat32.c -o obj/fat32.o

obj/ata.o:src/drivers/ata.c
	$(COMPILER) $(CFLAGS) src/drivers/ata.c -o obj/ata.o

obj/eynfs.o:src/drivers/eynfs.c
	$(COMPILER) $(CFLAGS) src/drivers/eynfs.c -o obj/eynfs.o

obj/rei.o:src/drivers/rei.c
	$(COMPILER) $(CFLAGS) src/drivers/rei.c -o obj/rei.o

obj/shell_commands.o:src/utilities/shell/shell_commands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_commands.c -o obj/shell_commands.o

obj/fs_commands.o:src/utilities/shell/fs_commands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/fs_commands.c -o obj/fs_commands.o

obj/fdisk_commands.o:src/utilities/shell/fdisk_commands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/fdisk_commands.c -o obj/fdisk_commands.o

obj/format_command.o:src/utilities/shell/format_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/format_command.c -o obj/format_command.o

obj/write_editor.o:src/utilities/shell/write_editor.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/write_editor.c -o obj/write_editor.o

obj/run_command.o:src/utilities/shell/run_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/run_command.c -o obj/run_command.o

obj/shell_script.o:src/utilities/shell/shell_script.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_script.c -o obj/shell_script.o

obj/history.o:src/utilities/shell/history.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/history.c -o obj/history.o

obj/compile_command.o:src/utilities/shell/compile_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/compile_command.c -o obj/compile_command.o

obj/tui.o:src/utilities/tui/tui.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/tui.c -o obj/tui.o
obj/tiling_manager.o:src/utilities/tui/tiling_manager.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/tiling_manager.c -o obj/tiling_manager.o

obj/terminals.o:src/utilities/tui/terminals.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/terminals.c -o obj/terminals.o

obj/tiling_cmd.o:src/utilities/shell/tiling_cmd.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/tiling_cmd.c -o obj/tiling_cmd.o

obj/help_tui.o:src/utilities/shell/help_tui.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/help_tui.c -o obj/help_tui.o

obj/draw_gui.o:src/utilities/shell/draw_gui.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/shell/draw_gui.c -o obj/draw_gui.o

obj/image_viewer_gui.o:src/utilities/shell/image_viewer_gui.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/shell/image_viewer_gui.c -o obj/image_viewer_gui.o

obj/window_test.o:src/utilities/shell/window_test.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/window_test.c -o obj/window_test.o

obj/stats_gui.o:src/utilities/shell/stats_gui.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/stats_gui.c -o obj/stats_gui.o

obj/vfs.o:src/fs/vfs.c
	$(COMPILER) $(CFLAGS) src/fs/vfs.c -o obj/vfs.o

obj/assemble.o: src/utilities/assembler/assemble.c obj/instruction_set.o
	$(COMPILER) $(CFLAGS) src/utilities/assembler/assemble.c -o obj/assemble.o

# Provide an explicit rule so parallel builds can make this target independently
obj/instruction_set.o:src/utilities/assembler/instruction_set.c
	$(COMPILER) $(CFLAGS) src/utilities/assembler/instruction_set.c -o obj/instruction_set.o 

obj/subcommands.o:src/utilities/shell/subcommands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/subcommands.c -o obj/subcommands.o

obj/predictive_memory.o:src/utilities/predictive_memory.c
	$(COMPILER) $(CFLAGS) src/utilities/predictive_memory.c -o obj/predictive_memory.o

obj/predictive_commands.o:src/utilities/shell/predictive_commands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/predictive_commands.c -o obj/predictive_commands.o

obj/zero_copy.o:src/utilities/zero_copy.c
	$(COMPILER) $(CFLAGS) src/utilities/zero_copy.c -o obj/zero_copy.o

obj/zero_copy_commands.o:src/utilities/shell/zero_copy_commands.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/zero_copy_commands.c -o obj/zero_copy_commands.o

obj/paging.o:src/cpu/paging.c
	$(COMPILER) $(CFLAGS) src/cpu/paging.c -o obj/paging.o

obj/pipeline.o:src/utilities/shell/pipeline.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/pipeline.c -o obj/pipeline.o

obj/kernel_api.o:src/cpu/kernel_api.c
	$(COMPILER) $(CFLAGS) src/cpu/kernel_api.c -o obj/kernel_api.o

obj/native_exec.o:src/cpu/native_exec.c
	$(COMPILER) $(CFLAGS) src/cpu/native_exec.c -o obj/native_exec.o

obj/native_run.o:src/utilities/shell/native_run.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/native_run.c -o obj/native_run.o

obj/sched.o:src/cpu/sched.c include/misc/sched.h
	$(COMPILER) $(CFLAGS) src/cpu/sched.c -o obj/sched.o

obj/irq.o:src/cpu/irq.c include/cpu/irq.h
	$(COMPILER) $(CFLAGS) src/cpu/irq.c -o obj/irq.o

# Actually building the OS (The stuff you should actually run, i.e. make run, make build, etc.)

build: all eynfsimg docs
	# Clean staging dir to avoid leftover permissions/ownership from prior runs
	-chmod -R u+w tmp/grub_ultra_minimal 2>/dev/null || true
	-rm -rf tmp/grub_ultra_minimal || true
	# Recreate with sane permissions
	install -d -m 0755 tmp/grub_ultra_minimal/boot/grub
	cp tmp/boot/kernel.bin tmp/grub_ultra_minimal/boot/
	@echo 'set default=0' > tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'set timeout=0' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'set gfxmode=text' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'set gfxpayload=text' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'set color_normal=white/black' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'set color_highlight=black/white' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo '' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo 'menuentry "EYN-OS" {' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo '    multiboot /boot/kernel.bin' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo '    boot' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@echo '}' >> tmp/grub_ultra_minimal/boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "grub-mkrescue not found. Install grub2 (grub2-mkrescue) or grub-pc-bin (grub-mkrescue)."; \
		exit 1; \
	fi
	$(GRUB_MKRESCUE) --modules="multiboot" --locales="" --themes="" --fonts="" --compress=xz -o EYNOS.iso tmp/grub_ultra_minimal/
	@echo "Ultra-minimal ISO created: EYNOS.iso"
	@ls -lh EYNOS.iso
	@echo "Attempting to strip EFI content (optional)..."
	@# Skip EFI cleanup if sudo isn't available non-interactively; the ISO from grub2-mkrescue works fine for QEMU.
	@if sudo -n true 2>/dev/null; then \
		echo "Cleaning ISO EFI content with sudo..."; \
		mkdir -p /tmp/iso_edit; \
		sudo mount -o loop EYNOS.iso /tmp/iso_edit; \
		mkdir -p /tmp/iso_clean; \
		cp -r /tmp/iso_edit/* /tmp/iso_clean/; \
		rm -rf /tmp/iso_clean/efi*; \
		rm -rf /tmp/iso_clean/boot/grub/i386-efi; \
		rm -rf /tmp/iso_clean/boot/grub/x86_64-efi; \
		sudo umount /tmp/iso_edit; \
		xorriso -as mkisofs -o EYNOS.iso -b boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img -r -V "EYN-OS" -iso-level 3 -joliet-long /tmp/iso_clean; \
		rm -rf /tmp/iso_clean; \
		echo "EFI content stripped from ISO."; \
	else \
		echo "Skipping EFI cleanup (no sudo available). Using original grub2-mkrescue ISO."; \
	fi
	@echo "ISO ready: EYNOS.iso"
	@ls -lh EYNOS.iso

clean:
	rm -rf obj/*.o tmp/boot/kernel.bin *.img eynfs_format EYNOS.iso
	rm -rf tmp/grub_minimal tmp/grub_ultra_minimal
	rm -f userland/*.o userland/*.bin

clear: clean

# Build the userland EYNFS format tool
eynfs_format: eynfs_format.c
	$(COMPILER) $(HOST_CFLAGS) -o eynfs_format eynfs_format.c $(HOST_LDFLAGS)

# Create and format a 10MB EYNFS disk image
eynfsimg:
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
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-boot d \
	-m 64M

# Debug run with serial logging and detailed CPU/interrupt logs
.PHONY: qemu-debug
qemu-debug: build
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-boot d \
	-serial stdio \
	-d int,cpu_reset \
	-no-reboot -no-shutdown \
	-m 9M

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
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-hdb testimg.img \
	-boot d \
	-m 64M

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

.PHONY: fsck_eynfs
fsck_eynfs: eynfsimg
	python3 devtools/fsck_eynfs.py eynfs.img || true

.PHONY: checkfs
checkfs: fsck_eynfs

# Static analysis (GCC -fanalyzer) over all kernel sources
.PHONY: analyze
analyze:
	@echo "Running GCC static analyzer over src/**/*.c ..."
	@find src -name "*.c" -print0 | xargs -0 -n1 -I{} sh -c 'echo Analyzing {}; $(COMPILER) $(KERNEL_CFLAGS) -fanalyzer -c {} -o /dev/null' || true