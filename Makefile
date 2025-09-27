COMPILER = gcc
LINKER = ld
ASSEMBLER = nasm
CFLAGS = -m32 -c -ffreestanding -fcommon -Oz -fno-stack-protector -I include/ \
		 -I include/cpu -I include/drivers -I include/misc -I include/graphics -I include/network -I include/utilities -I include/utilities/shell \
         -Wall -Wextra -Werror=implicit-function-declaration \
         -Wno-unused-parameter -Wno-unused-variable \
         -fno-strict-overflow -fwrapv \
         -D_FORTIFY_SOURCE=0 -fno-builtin \
		 -fstack-protector-strong -D_FORTIFY_SOURCE=1
		

# Debug flags for development
DEBUG_CFLAGS = $(CFLAGS) -g -O0 -DDEBUG -D_DEBUG
RELEASE_CFLAGS = $(CFLAGS) -O2 -DNDEBUG
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T src/boot/link.ld
EMULATOR = qemu-system-i386
EMULATOR_FLAGS = -kernel

OBJS = obj/kasm.o obj/kc.o obj/idt.o obj/isr.o obj/syscall.o obj/kb.o obj/string.o obj/system.o obj/util.o obj/shell.o obj/math.o obj/vga.o obj/fat32.o obj/ata.o obj/eynfs.o obj/rei.o obj/shell_commands.o obj/fs_commands.o obj/fdisk_commands.o obj/format_command.o obj/write_editor.o obj/tui.o obj/help_tui.o obj/assemble.o obj/instruction_set.o obj/run_command.o obj/shell_script.o obj/history.o obj/subcommands.o obj/predictive_memory.o obj/predictive_commands.o obj/zero_copy.o obj/zero_copy_commands.o obj/paging.o obj/pipeline.o obj/kernel_api.o obj/native_exec.o obj/native_run.o obj/sched.o obj/irq.o obj/irq_stubs.o obj/mouse.o obj/draw_gui.o obj/image_viewer_gui.o obj/window_test.o

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
	$(COMPILER) $(CFLAGS) src/utilities/tui/tui.c -o obj/tui.o
obj/tiling_manager.o:src/utilities/tui/tiling_manager.c
	$(COMPILER) $(CFLAGS) src/utilities/tui/tiling_manager.c -o obj/tiling_manager.o

obj/terminals.o:src/utilities/tui/terminals.c
	$(COMPILER) $(CFLAGS) src/utilities/tui/terminals.c -o obj/terminals.o

obj/tiling_cmd.o:src/utilities/shell/tiling_cmd.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/tiling_cmd.c -o obj/tiling_cmd.o

obj/help_tui.o:src/utilities/shell/help_tui.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/help_tui.c -o obj/help_tui.o

obj/draw_gui.o:src/utilities/shell/draw_gui.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/draw_gui.c -o obj/draw_gui.o

obj/image_viewer_gui.o:src/utilities/shell/image_viewer_gui.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/image_viewer_gui.c -o obj/image_viewer_gui.o

obj/window_test.o:src/utilities/shell/window_test.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/window_test.c -o obj/window_test.o

obj/assemble.o:src/utilities/assembler/assemble.c src/utilities/assembler/instruction_set.c
	$(COMPILER) $(CFLAGS) src/utilities/assembler/assemble.c -o obj/assemble.o 
	$(COMPILER) $(CFLAGS) src/utilities/assembler/instruction_set.c -o obj/instruction_set.o 

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
	mkdir -p tmp/grub_ultra_minimal/boot/grub
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
	grub2-mkrescue --modules="multiboot" --locales="" --themes="" --fonts="" --compress=xz -o EYNOS.iso tmp/grub_ultra_minimal/
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
	$(COMPILER) -I include -I include/misc -I include/drivers -I include/cpu -I include/utilities -I include/graphics -I include/network -o eynfs_format eynfs_format.c

# Create and format a 10MB EYNFS disk image
eynfsimg:
	rm -f eynfs.img
	dd if=/dev/zero of=eynfs.img bs=1M count=10
	$(COMPILER) -I include -I include/misc -I include/drivers -I include/cpu -I include/utilities -I include/graphics -I include/network -o eynfs_format eynfs_format.c
	./eynfs_format eynfs.img
	python3 devtools/copy_testdir_to_eynfs.py testdir/

# Create source code drive for testing
sourceimg: eynfs_format
	rm -f source.img
	dd if=/dev/zero of=source.img bs=1M count=10
	$(COMPILER) -I include -I include/misc -I include/drivers -I include/cpu -I include/utilities -I include/graphics -I include/network -o eynfs_format eynfs_format.c
	./eynfs_format source.img
	mkdir -p temp_source_structure
	cp -r src temp_source_structure/
	cp -r include temp_source_structure/
	cp -r docs temp_source_structure/
	python3 devtools/copy_testdir_to_eynfs.py temp_source_structure/ source.img
	rm -rf temp_source_structure

# Rebuilds and runs the OS

run: build
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-boot d \
	-m 32M
# Just runs the OS, no rebuilding.

test: sourceimg
	qemu-system-i386 -cdrom EYNOS.iso \
	-hda eynfs.img \
	-hdb source.img \
	-boot d \
	-m 64M