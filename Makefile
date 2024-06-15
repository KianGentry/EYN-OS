kernel_src := $(shell find src/imp/kernel -name *.c)
kernel_obj := $(patsubst src/imp/kernel/%.c, build/kernel/%.o, $(kernel_src))

x86_c_src := $(shell find src/imp/x86 -name *.c)
x86_c_obj := $(patsubst src/imp/x86/%.c, build/x86/%.o, $(x86_c_src))

x86_asm_src := $(shell find src/imp/x86 -name *.asm)
x86_asm_obj := $(patsubst src/imp/x86/%.asm, build/x86/%.o, $(x86_asm_src))

x86_obj := $(x86_c_obj) $(x86_asm_obj)

$(kernel_obj): build/kernel/%.o : src/imp/kernel/%.c
	mkdir -p $(dir $@) && \
	gcc -c -I src/int -ffreestanding $(patsubst build/kernel/%.o, src/imp/kernel/%.c, $@) -o $@

$(x86_c_obj): build/x86/%.o : src/imp/x86/%.c
	mkdir -p $(dir $@) && \
	gcc -c -I src/int -ffreestanding $(patsubst build/x86/%.o, src/imp/x86/%.c, $@) -o $@

$(x86_asm_obj): build/x86/%.o : src/imp/x86/%.asm
	mkdir -p $(dir $@) && \
	nasm -f elf64 $< -o $@

.PHONY: build-x86_64
build-x86_64: $(kernel_obj) $(x86_obj)
	mkdir -p dist/x86 && \
	ld -o dist/x86/kernel.bin -T targets/x86/linker.ld $(kernel_obj) $(x86_obj) && \
	cp dist/x86/kernel.bin targets/x86/iso/boot/kernel.bin && \
	grub-mkrescue /usr/lib/grub/i386-pc -o dist/x86/kernel.iso targets/x86/iso

