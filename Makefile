COMPILER = gcc
LINKER = ld
ASSEMBLER = nasm

CONFIG_FILE ?= .eynosconfig
-include $(CONFIG_FILE)

# Target architecture selection for portability work.
ARCH ?= i386
SUPPORTED_ARCHES := i386 amd64
OBJDIR := obj/$(ARCH)

ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH '$(ARCH)'. Supported values: $(SUPPORTED_ARCHES))
endif

# Prefer grub2-mkrescue if available; fall back to grub-mkrescue
# Path is resolved at parse time; if neither exists, we'll stop in the build rule with a friendly message
GRUB_MKRESCUE := $(shell command -v grub2-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null)

# Kernel (freestanding) compiler flags
# Note: keep frame pointers for stack traces; avoid stack protector & fortify in freestanding kernel
CPU_HAS_INVLPG ?= 0
CONFIG_SCHED_MLFQ ?= 1
CONFIG_SCHED_MLFQ_IRQ_PREEMPT ?= 0
CONFIG_SCHED_MLFQ_Q0_MS ?= 10
CONFIG_SCHED_MLFQ_Q1_MS ?= 25
CONFIG_SCHED_MLFQ_Q2_MS ?= 50
CONFIG_SCHED_MLFQ_BOOST_MS ?= 1000

ARCH_KERNEL_CFLAGS_i386 = -m32 -march=i386 -mtune=i386 -DEYNOS_ARCH_I386=1
ARCH_KERNEL_CFLAGS_amd64 = -m64 -march=x86-64 -mtune=generic -mno-red-zone -DEYNOS_ARCH_AMD64=1

KERNEL_CFLAGS = $(ARCH_KERNEL_CFLAGS_$(ARCH)) -c -ffreestanding -fno-builtin -fno-omit-frame-pointer -fno-common -MMD -MP \
		 -Os -fno-strict-overflow -fwrapv \
		 -DCONFIG_CPU_HAS_INVLPG=$(CPU_HAS_INVLPG) \
		 -DCONFIG_SCHED_MLFQ=$(CONFIG_SCHED_MLFQ) \
		 -DCONFIG_SCHED_MLFQ_IRQ_PREEMPT=$(CONFIG_SCHED_MLFQ_IRQ_PREEMPT) \
		 -DCONFIG_SCHED_MLFQ_Q0_MS=$(CONFIG_SCHED_MLFQ_Q0_MS) \
		 -DCONFIG_SCHED_MLFQ_Q1_MS=$(CONFIG_SCHED_MLFQ_Q1_MS) \
		 -DCONFIG_SCHED_MLFQ_Q2_MS=$(CONFIG_SCHED_MLFQ_Q2_MS) \
		 -DCONFIG_SCHED_MLFQ_BOOST_MS=$(CONFIG_SCHED_MLFQ_BOOST_MS) \
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
ARCH_ASFLAGS_i386 = -f elf32
ARCH_ASFLAGS_amd64 = -f elf64
ASFLAGS = $(ARCH_ASFLAGS_$(ARCH))
## Linker flags: target i386 linker script, enable section GC, strip symbols, and emit a map
TMPDIR ?= tmp_user
BOOTDIR = $(TMPDIR)/boot
ARCH_LDFORMAT_i386 = elf_i386
ARCH_LDFORMAT_amd64 = elf_x86_64
ARCH_LINK_SCRIPT_i386 = src/boot/link.ld
ARCH_LINK_SCRIPT_amd64 = src/boot/link_amd64.ld
LDFLAGS = -m $(ARCH_LDFORMAT_$(ARCH)) -T $(ARCH_LINK_SCRIPT_$(ARCH)) --gc-sections -Map $(BOOTDIR)/kernel.map -s

ARCH_IDT_SRC_i386 = src/cpu/idt.c
ARCH_IDT_SRC_amd64 = src/cpu/idt_amd64_stub.c
ARCH_FPU_SRC_i386 = src/cpu/fpu.c
ARCH_FPU_SRC_amd64 = src/cpu/fpu_amd64.c
ARCH_CPU_ARCH_SRC_i386 = src/cpu/arch.c
ARCH_CPU_ARCH_SRC_amd64 = src/cpu/arch_amd64.c
ARCH_ISR_STUB_SRC_i386 = src/cpu/isr.asm
ARCH_ISR_STUB_SRC_amd64 = src/cpu/isr_amd64.asm
ARCH_IRQ_STUB_SRC_i386 = src/cpu/irq.asm
ARCH_IRQ_STUB_SRC_amd64 = src/cpu/irq_amd64.asm
ARCH_SYSCALL_STUB_SRC_i386 = src/cpu/syscall.asm
ARCH_SYSCALL_STUB_SRC_amd64 = src/cpu/syscall_amd64.asm
ARCH_BOOT_SRC_i386 = src/boot/kernel.asm
ARCH_BOOT_SRC_amd64 = src/boot/kernel_amd64.asm
ARCH_MEM_ASM_SRC_i386 = src/misc/mem386.asm
ARCH_MEM_ASM_SRC_amd64 = src/misc/mem_amd64.asm
ARCH_GDT_SRC_i386 = src/cpu/gdt.c
ARCH_GDT_SRC_amd64 = src/cpu/gdt_amd64.c
ARCH_GDT_ASM_SRC_i386 = src/cpu/gdt.asm
ARCH_GDT_ASM_SRC_amd64 = src/cpu/gdt_amd64.asm

ARCH_EMULATOR_i386 = qemu-system-i386
ARCH_EMULATOR_amd64 = qemu-system-x86_64
EMULATOR = $(ARCH_EMULATOR_$(ARCH))

# QEMU display backend.
QEMU_DISPLAY ?= gtk,grab-on-hover=on

# QEMU environment prefix.
# Default forces GTK to use X11/XWayland for reliable mouse grab under Wayland.
# Override at invocation time if needed, e.g.:
#   make run QEMU_ENV=
#   make run QEMU_ENV=GDK_BACKEND=wayland
QEMU_ENV ?= GDK_BACKEND=x11
EMULATOR_FLAGS = -kernel

OBJS = $(OBJDIR)/kasm.o $(OBJDIR)/kc.o $(OBJDIR)/gdt.o $(OBJDIR)/gdt_asm.o $(OBJDIR)/idt.o $(OBJDIR)/isr.o $(OBJDIR)/isr_stubs.o $(OBJDIR)/syscall.o $(OBJDIR)/fpu.o $(OBJDIR)/kb.o $(OBJDIR)/string.o $(OBJDIR)/system.o $(OBJDIR)/arch.o $(OBJDIR)/util.o $(OBJDIR)/mem386.o $(OBJDIR)/slab.o $(OBJDIR)/shell.o $(OBJDIR)/shell_args.o $(OBJDIR)/math.o $(OBJDIR)/vga.o $(OBJDIR)/serial.o $(OBJDIR)/fat32.o $(OBJDIR)/ata.o $(OBJDIR)/eynfs.o $(OBJDIR)/rei.o $(OBJDIR)/reiv.o $(OBJDIR)/tui.o $(OBJDIR)/run_command.o $(OBJDIR)/history.o $(OBJDIR)/alias.o $(OBJDIR)/predictive_memory.o $(OBJDIR)/zero_copy.o $(OBJDIR)/vmm.o $(OBJDIR)/paging_compat.o $(OBJDIR)/user_access.o $(OBJDIR)/pipeline.o $(OBJDIR)/kernel_api.o $(OBJDIR)/native_exec.o $(OBJDIR)/user_elf.o $(OBJDIR)/sched.o $(OBJDIR)/irq.o $(OBJDIR)/irq_stubs.o $(OBJDIR)/mouse.o $(OBJDIR)/vfs.o $(OBJDIR)/panic.o $(OBJDIR)/watchdog.o $(OBJDIR)/capabilities.o $(OBJDIR)/segdom.o $(OBJDIR)/crashlog.o $(OBJDIR)/context.o $(OBJDIR)/fs_txn.o $(OBJDIR)/linux_syscalls.o

OBJS += $(OBJDIR)/tiling_manager.o $(OBJDIR)/ui_prefs.o
OBJS += $(OBJDIR)/terminals.o
OBJS += $(OBJDIR)/partition.o
OBJS += $(OBJDIR)/pci.o
OBJS += $(OBJDIR)/e1000.o
OBJS += $(OBJDIR)/netstack.o
OBJS += $(OBJDIR)/shell_script.o
OBJS += $(OBJDIR)/ac97.o
OBJS += $(OBJDIR)/reis.o
OBJS += $(OBJDIR)/otf_font.o

ifeq ($(ARCH),i386)
OBJS += $(OBJDIR)/vbe_bios.o
endif
OUTPUT = $(BOOTDIR)/kernel.bin

# Source files to object files

all: $(OBJS)
	mkdir $(TMPDIR)/ -p
	mkdir $(BOOTDIR)/ -p
	$(LINKER) $(LDFLAGS) -o $(OUTPUT) $(OBJS)

$(OBJS): | preflight

.PHONY: preflight
preflight:
	@mkdir -p $(OBJDIR)/

.PHONY: preflight-arch
preflight-arch:
	@echo "[preflight] ARCH=$(ARCH)"
	@echo "[preflight] CFLAGS: $(ARCH_KERNEL_CFLAGS_$(ARCH))"
	@echo "[preflight] ASFLAGS: $(ARCH_ASFLAGS_$(ARCH))"
	@echo "[preflight] LDFORMAT: $(ARCH_LDFORMAT_$(ARCH))"
	@echo "[preflight] LINK_SCRIPT: $(ARCH_LINK_SCRIPT_$(ARCH))"
	@if [ ! -f "$(ARCH_LINK_SCRIPT_$(ARCH))" ]; then \
		echo "[preflight] ERROR: missing linker script $(ARCH_LINK_SCRIPT_$(ARCH))"; \
		exit 1; \
	fi
	@if [ "$(ARCH)" = "amd64" ]; then \
		echo "[preflight] amd64 compile/link scaffolding: ready"; \
		echo "[preflight] amd64 runtime note: long-mode interrupt/syscall/user ABI paths are still in progress"; \
	fi

docs: all
	python3 devtools/generate_command_docs.py src/

$(OBJDIR)/kasm.o:$(ARCH_BOOT_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/kasm.o $(ARCH_BOOT_SRC_$(ARCH))

$(OBJDIR)/syscall.o:$(ARCH_SYSCALL_STUB_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/syscall.o $(ARCH_SYSCALL_STUB_SRC_$(ARCH))

$(OBJDIR)/isr_stubs.o:$(ARCH_ISR_STUB_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/isr_stubs.o $(ARCH_ISR_STUB_SRC_$(ARCH))

$(OBJDIR)/irq_stubs.o:$(ARCH_IRQ_STUB_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/irq_stubs.o $(ARCH_IRQ_STUB_SRC_$(ARCH))

$(OBJDIR)/mem386.o:$(ARCH_MEM_ASM_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/mem386.o $(ARCH_MEM_ASM_SRC_$(ARCH))

ifeq ($(ARCH),i386)
$(OBJDIR)/vbe_bios.o:src/drivers/vbe_bios_i386.asm
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/vbe_bios.o src/drivers/vbe_bios_i386.asm
endif
	
$(OBJDIR)/kc.o:src/entry/kernel.c
	$(COMPILER) $(CFLAGS) src/entry/kernel.c -o $(OBJDIR)/kc.o 
	
$(OBJDIR)/idt.o:$(ARCH_IDT_SRC_$(ARCH))
	$(COMPILER) $(CFLAGS) $(ARCH_IDT_SRC_$(ARCH)) -o $(OBJDIR)/idt.o 

$(OBJDIR)/fpu.o:$(ARCH_FPU_SRC_$(ARCH))
	$(COMPILER) $(CFLAGS) $(ARCH_FPU_SRC_$(ARCH)) -o $(OBJDIR)/fpu.o

$(OBJDIR)/gdt.o:$(ARCH_GDT_SRC_$(ARCH))
	$(COMPILER) $(CFLAGS) $(ARCH_GDT_SRC_$(ARCH)) -o $(OBJDIR)/gdt.o

$(OBJDIR)/gdt_asm.o:$(ARCH_GDT_ASM_SRC_$(ARCH))
	mkdir $(OBJDIR)/ -p
	$(ASSEMBLER) $(ASFLAGS) -o $(OBJDIR)/gdt_asm.o $(ARCH_GDT_ASM_SRC_$(ARCH))

$(OBJDIR)/kb.o:src/drivers/kb.c
	$(COMPILER) $(CFLAGS) src/drivers/kb.c -o $(OBJDIR)/kb.o

$(OBJDIR)/isr.o:src/cpu/isr.c
	$(COMPILER) $(CFLAGS) src/cpu/isr.c -o $(OBJDIR)/isr.o

$(OBJDIR)/user_elf.o:src/cpu/user_elf.c
	$(COMPILER) $(CFLAGS) src/cpu/user_elf.c -o $(OBJDIR)/user_elf.o

$(OBJDIR)/string.o:src/utilities/shell/string.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/string.c -o $(OBJDIR)/string.o

$(OBJDIR)/shell_args.o:src/utilities/shell/shell_args.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_args.c -o $(OBJDIR)/shell_args.o

$(OBJDIR)/system.o:src/cpu/system.c
	$(COMPILER) $(CFLAGS) src/cpu/system.c -o $(OBJDIR)/system.o

$(OBJDIR)/arch.o:$(ARCH_CPU_ARCH_SRC_$(ARCH))
	$(COMPILER) $(CFLAGS) $(ARCH_CPU_ARCH_SRC_$(ARCH)) -o $(OBJDIR)/arch.o

$(OBJDIR)/util.o:src/utilities/util.c
	$(COMPILER) $(CFLAGS) src/utilities/util.c -o $(OBJDIR)/util.o

$(OBJDIR)/slab.o:src/mm/slab.c
	$(COMPILER) $(CFLAGS) src/mm/slab.c -o $(OBJDIR)/slab.o
	
$(OBJDIR)/shell.o:src/utilities/shell/shell.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell.c -o $(OBJDIR)/shell.o

$(OBJDIR)/shell_script.o:src/utilities/shell/shell_script.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/shell_script.c -o $(OBJDIR)/shell_script.o

$(OBJDIR)/math.o:src/utilities/basic/math.c
	$(COMPILER) $(CFLAGS) src/utilities/basic/math.c -o $(OBJDIR)/math.o

$(OBJDIR)/vga.o:src/drivers/vga.c
	$(COMPILER) $(CFLAGS) src/drivers/vga.c -o $(OBJDIR)/vga.o
$(OBJDIR)/mouse.o:src/drivers/mouse.c
	$(COMPILER) $(CFLAGS) src/drivers/mouse.c -o $(OBJDIR)/mouse.o

$(OBJDIR)/serial.o:src/drivers/serial.c
	$(COMPILER) $(CFLAGS) src/drivers/serial.c -o $(OBJDIR)/serial.o

$(OBJDIR)/pci.o:src/drivers/pci.c
	$(COMPILER) $(CFLAGS) src/drivers/pci.c -o $(OBJDIR)/pci.o

$(OBJDIR)/e1000.o:src/drivers/e1000.c
	$(COMPILER) $(CFLAGS) src/drivers/e1000.c -o $(OBJDIR)/e1000.o

$(OBJDIR)/netstack.o:src/network/netstack.c
	$(COMPILER) $(CFLAGS) src/network/netstack.c -o $(OBJDIR)/netstack.o

$(OBJDIR)/panic.o:src/misc/panic.c
	$(COMPILER) $(CFLAGS) src/misc/panic.c -o $(OBJDIR)/panic.o

$(OBJDIR)/watchdog.o:src/misc/watchdog.c
	$(COMPILER) $(CFLAGS) src/misc/watchdog.c -o $(OBJDIR)/watchdog.o

$(OBJDIR)/capabilities.o:src/misc/capabilities.c
	$(COMPILER) $(CFLAGS) src/misc/capabilities.c -o $(OBJDIR)/capabilities.o

$(OBJDIR)/segdom.o:src/cpu/segdom.c
	$(COMPILER) $(CFLAGS) src/cpu/segdom.c -o $(OBJDIR)/segdom.o

$(OBJDIR)/crashlog.o:src/misc/crashlog.c
	$(COMPILER) $(CFLAGS) src/misc/crashlog.c -o $(OBJDIR)/crashlog.o

$(OBJDIR)/context.o:src/misc/context.c
	$(COMPILER) $(CFLAGS) src/misc/context.c -o $(OBJDIR)/context.o

$(OBJDIR)/fs_txn.o:src/fs/fs_txn.c
	$(COMPILER) $(CFLAGS) src/fs/fs_txn.c -o $(OBJDIR)/fs_txn.o

## QR renderer disabled (no longer used by panic screen)

$(OBJDIR)/fat32.o:src/drivers/fat32.c
	$(COMPILER) $(CFLAGS) src/drivers/fat32.c -o $(OBJDIR)/fat32.o

$(OBJDIR)/ata.o:src/drivers/ata.c
	$(COMPILER) $(CFLAGS) src/drivers/ata.c -o $(OBJDIR)/ata.o

$(OBJDIR)/eynfs.o:src/drivers/eynfs.c
	$(COMPILER) $(CFLAGS) src/drivers/eynfs.c -o $(OBJDIR)/eynfs.o

$(OBJDIR)/partition.o:src/drivers/partition.c
	$(COMPILER) $(CFLAGS) src/drivers/partition.c -o $(OBJDIR)/partition.o

$(OBJDIR)/rei.o:src/drivers/rei.c
	$(COMPILER) $(CFLAGS) src/drivers/rei.c -o $(OBJDIR)/rei.o

$(OBJDIR)/reiv.o:src/drivers/reiv.c
	$(COMPILER) $(CFLAGS) src/drivers/reiv.c -o $(OBJDIR)/reiv.o

$(OBJDIR)/reis.o:src/drivers/reis.c
	$(COMPILER) $(CFLAGS) src/drivers/reis.c -o $(OBJDIR)/reis.o

OTF_CFLAGS = $(KERNEL_CFLAGS) -Wno-shadow -Wno-switch-enum -Wno-switch-default -Wno-old-style-definition -Wno-strict-prototypes -Wno-missing-prototypes
$(OBJDIR)/otf_font.o:src/drivers/otf_font.c include/drivers/otf_font.h include/third_party/stb_truetype.h
	$(COMPILER) $(OTF_CFLAGS) src/drivers/otf_font.c -o $(OBJDIR)/otf_font.o

$(OBJDIR)/ac97.o:src/drivers/ac97.c
	$(COMPILER) $(CFLAGS) src/drivers/ac97.c -o $(OBJDIR)/ac97.o

$(OBJDIR)/run_command.o:src/utilities/shell/run_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/run_command.c -o $(OBJDIR)/run_command.o

$(OBJDIR)/alias.o:src/utilities/shell/alias.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/alias.c -o $(OBJDIR)/alias.o

$(OBJDIR)/history.o:src/utilities/shell/history.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/history.c -o $(OBJDIR)/history.o

$(OBJDIR)/compile_command.o:src/utilities/shell/compile_command.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/compile_command.c -o $(OBJDIR)/compile_command.o

$(OBJDIR)/tui.o:src/utilities/tui/tui.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/tui.c -o $(OBJDIR)/tui.o
$(OBJDIR)/tiling_manager.o:src/gui/tiling_manager.c src/gui/gui_state.c src/gui/gui_wm.c src/gui/gui_tiles.c src/gui/gui_taskbar.c src/gui/gui_input.c
	$(COMPILER) $(GUI_CFLAGS) src/gui/tiling_manager.c -o $(OBJDIR)/tiling_manager.o

$(OBJDIR)/terminals.o:src/utilities/tui/terminals.c
	$(COMPILER) $(GUI_CFLAGS) src/utilities/tui/terminals.c -o $(OBJDIR)/terminals.o

$(OBJDIR)/ui_prefs.o:src/utilities/tui/ui_prefs.c
	$(COMPILER) $(CFLAGS) src/utilities/tui/ui_prefs.c -o $(OBJDIR)/ui_prefs.o

$(OBJDIR)/vfs.o:src/fs/vfs.c
	$(COMPILER) $(CFLAGS) src/fs/vfs.c -o $(OBJDIR)/vfs.o

$(OBJDIR)/linux_syscalls.o:src/cpu/linux_syscalls.c
	$(COMPILER) $(CFLAGS) src/cpu/linux_syscalls.c -o $(OBJDIR)/linux_syscalls.o

$(OBJDIR)/predictive_memory.o:src/utilities/predictive_memory.c
	$(COMPILER) $(CFLAGS) src/utilities/predictive_memory.c -o $(OBJDIR)/predictive_memory.o

$(OBJDIR)/zero_copy.o:src/utilities/zero_copy.c
	$(COMPILER) $(CFLAGS) src/utilities/zero_copy.c -o $(OBJDIR)/zero_copy.o

$(OBJDIR)/vmm.o:src/mm/vmm.c
	$(COMPILER) $(CFLAGS) -I include/mm src/mm/vmm.c -o $(OBJDIR)/vmm.o

$(OBJDIR)/paging_compat.o:src/mm/paging_compat.c
	$(COMPILER) $(CFLAGS) -I include/mm src/mm/paging_compat.c -o $(OBJDIR)/paging_compat.o

$(OBJDIR)/user_access.o:src/mm/user_access.c include/mm/user_access.h
	$(COMPILER) $(CFLAGS) src/mm/user_access.c -o $(OBJDIR)/user_access.o

$(OBJDIR)/pipeline.o:src/utilities/shell/pipeline.c
	$(COMPILER) $(CFLAGS) src/utilities/shell/pipeline.c -o $(OBJDIR)/pipeline.o

$(OBJDIR)/kernel_api.o:src/cpu/kernel_api.c
	$(COMPILER) $(CFLAGS) src/cpu/kernel_api.c -o $(OBJDIR)/kernel_api.o

$(OBJDIR)/native_exec.o:src/cpu/native_exec.c
	$(COMPILER) $(CFLAGS) src/cpu/native_exec.c -o $(OBJDIR)/native_exec.o

$(OBJDIR)/sched.o:src/cpu/sched.c include/misc/sched.h
	$(COMPILER) $(CFLAGS) src/cpu/sched.c -o $(OBJDIR)/sched.o

$(OBJDIR)/irq.o:src/cpu/irq.c include/cpu/irq.h
	$(COMPILER) $(CFLAGS) src/cpu/irq.c -o $(OBJDIR)/irq.o

# Actually building the OS (The stuff you should actually run, i.e. make run, make build, etc.)

build: all installer_ramdisk eynfsimg docs
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "grub-mkrescue not found. Install grub2 (grub2-mkrescue) or grub-pc-bin (grub-mkrescue)."; \
		exit 1; \
	fi
	bash devtools/build_iso.sh "$(GRUB_MKRESCUE)"

.PHONY: installer_userland installer_ramdisk
menuconfig:
	python3 devtools/menuconfig.py $(CONFIG_FILE)

.PHONY: installer_userland installer_ramdisk menuconfig
installer_userland:
	bash EYN-packages/devtools/build_user_c.sh packages/installer/installer_uelf.c ../testdir/binaries/installer
	bash EYN-packages/devtools/build_user_c.sh packages/install/install_uelf.c ../testdir/binaries/install
	bash EYN-packages/devtools/build_user_c.sh packages/extract/extract_uelf.c ../testdir/binaries/extract

installer_ramdisk: installer_userland
	mkdir -p tmp_user/boot
	python3 devtools/build_installer_ramdisk.py tmp_user/boot/installer_ramdisk.img

clean:
	rm -rf obj tmp/boot/kernel.bin *.img eynfs_format EYNOS.iso
	@rm -rf tmp/grub_minimal tmp/grub_ultra_minimal tmp/grub_ultra_minimal.* tmp/grub.* tmp/iso_edit.* tmp/iso_clean.* 2>/dev/null || true
	rm -f userland/*.o userland/*.bin
	rm -rf tmp tmp_user

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

# Rebuild all userland C programs from EYN-packages package sources.
# Run this after editing package *_uelf.c files, then run 'make build'.
# Usage: make userland
.PHONY: userland
userland:
	$(MAKE) -C EYN-packages userland OUT_DIR=../testdir/binaries

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
	dd if=/dev/zero of=testimg.img bs=1M count=30
	$(COMPILER) $(HOST_CFLAGS) -o eynfs_format eynfs_format.c $(HOST_LDFLAGS)
	./eynfs_format testimg.img 20480

# Rebuilds and runs the OS

run: build
	$(QEMU_ENV) $(EMULATOR) -cdrom EYNOS.iso \
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
	$(QEMU_ENV) $(EMULATOR) -cdrom EYNOS.iso \
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

.PHONY: installer-debug
installer-debug: build
	@mkdir -p tmp
	$(QEMU_ENV) $(EMULATOR) -cdrom EYNOS.iso \
	-drive file=testimg.img,format=raw,if=ide,index=0,media=disk \
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

.PHONY: installer-debug
installer-nobuild:
	@mkdir -p tmp
	$(QEMU_ENV) $(EMULATOR) -cdrom EYNOS.iso \
	-drive file=testimg.img,format=raw,if=ide,index=0,media=disk \
	-boot c \
	-display $(QEMU_DISPLAY) \
	-netdev user,id=net0,hostfwd=udp::10000-:9999,hostfwd=tcp::10000-:9999 \
	-device e1000,netdev=net0 \
	-audiodev pipewire,id=audio0 \
	-device ac97,audiodev=audio0 \
	-serial stdio \
	-d int,cpu_reset -D tmp/qemu-debug.log \
	-m 64M

# Halt at start for GDB attach on tcp:1234 (target remote :1234)
.PHONY: qemu-gdb
qemu-gdb: build
	$(EMULATOR) -cdrom EYNOS.iso \
	-hda eynfs.img \
	-boot d \
	-S -s \
	-serial stdio \
	-m 64M
# Just runs the OS, no rebuilding.

test: testimg
	$(QEMU_ENV) $(EMULATOR) -cdrom EYNOS.iso \
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
	$(EMULATOR) -cdrom EYNOS.iso \
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
-include $(wildcard $(OBJDIR)/*.d)
