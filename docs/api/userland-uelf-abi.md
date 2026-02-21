# EYN-OS Userland ABI (".uelf")

This document describes the current, minimal ABI/format requirements for **ring3 user programs** on EYN-OS.

## 1) Executable format

- **File type**: ELF32, little-endian, i386 (`EM_386`)
- **Load model**: `PT_LOAD` program headers only
- **Typical output**: `ET_EXEC` linked at a fixed virtual address
- **Entry point**: `e_entry` is entered directly via `enter_user_mode(entry, user_stack_top)`

The kernel loader reads the entire ELF into memory, validates the ELF header, walks program headers, and maps a single contiguous region covering all `PT_LOAD` segments.

Implementation: [src/cpu/user_elf.c](src/cpu/user_elf.c)

## 2) Virtual address expectations

- **User code base**: `0x00400000` (`USER_CODE_BASE`)
- **User stack region**: `0xB0000000` – `0xBFFFFFFF`, with `USER_STACK_TOP = 0xC0000000`

The stock linker script starts at `0x00400000`:
- [devtools/user_elf32.ld](devtools/user_elf32.ld)

The loader currently rejects any ELF whose load range crosses into the user stack region.

## 3) Stack

- **Initial mapped stack**: 32 pages (128KB) below `USER_STACK_TOP`
- **Initial `esp`**: `USER_STACK_TOP - 0x10`
- **Growth**: The VMM can grow the stack downward on page faults.

Notes:
- Stack growth is controlled by `vmm_kernel_as.stack_bottom`, which is set when a user task starts.
- Cleanup unmaps stack pages from `stack_bottom` up to `USER_STACK_TOP`.

Related code:
- [src/cpu/user_elf.c](src/cpu/user_elf.c)
- [src/mm/vmm.c](src/mm/vmm.c)
- [src/utilities/util.c](src/utilities/util.c)

## 4) Startup contract

User programs are expected to provide `_start`.

Current CRT does:
1. `call main(0, NULL)`
2. `push %eax`
3. `call _exit`

File: [userland/crt0.S](userland/crt0.S)

Implications:
- `main` may be written as either `int main(void)` or `int main(int argc, char** argv)`.
	- If you use the 2-arg signature, you currently receive `argc=0` and `argv=NULL`.
- The process termination path goes through `_exit` in userland libc.

## 4.1) C runtime contract (minimal)

This section is the **contract that compilers/toolchains should target** for EYN-OS `.uelf` programs.

- **Architecture**: 32-bit i386
- **C ABI**: SysV i386 **cdecl**
	- Args pushed right-to-left on the stack
	- Caller cleans the stack
	- Return value in `EAX` (and `EDX:EAX` for 64-bit integers)
- **Stack alignment**:
	- Kernel enters user mode with a 16-byte aligned `ESP`.
	- The CRT ensures `main()` is entered with **16-byte stack alignment**.
		(This reduces surprises for code that uses SSE or assumes modern compiler alignment.)
- **Required symbols**:
	- `_start` (entrypoint)
	- `_exit(int)` (noreturn)
	- `main(...)` (called by CRT)
- **Not provided (yet)**:
	- `argc/argv/envp` setup by the kernel (CRT currently passes `(0, NULL)`)
	- TLS (`__thread`), `errno` as TLS, thread runtime
	- C++ exceptions/RTTI, dynamic loader, shared libraries
	- Global constructors/destructors (`.init_array` / `.fini_array`) execution

## 4.2) Upgrade path (planned)

As EYN-OS moves toward hosting a compiler and richer user programs, these are the expected next contract expansions:

1. **Kernel-provided initial stack frame**: real `argc/argv` (and maybe `envp`) at process start.
2. **Constructor support**: run `.init_array` entries before `main` (and `.fini_array` on exit).
3. **More syscalls / libc**: memory management (`brk`/`mmap`), `stat`, `pipe`, process primitives.
4. **Linker expectations**: support relocations and/or a PIE model (only if/when a dynamic loader exists).

## 5) Toolchain expectations (today)

Current host-side build uses a freestanding i386 compile and then links with the EYN-OS linker script.

Reference script: [devtools/build_user_c.sh](devtools/build_user_c.sh)

Key constraints:
- `-m32 -ffreestanding -fno-pie -fno-pic -nostdlib -nostartfiles`
- Link with `-Wl,-m,elf_i386 -Wl,-e,_start -Wl,-T,devtools/user_elf32.ld`
- Provide `userland/crt0.S` and `userland/libc` objects/archive

## 6) Syscalls / libc surface

- Syscalls are invoked via `int 0x80` (see ring3 stub test code).
- The userland libc lives under [userland/libc/](userland/libc/) and headers in [userland/include/](userland/include/).

Syscall documentation: [docs/api/syscalls.md](docs/api/syscalls.md)

## 7) Current loader limits

- Max ELF file size: 2MB (loader reads the whole file into RAM)
- Max mapped `PT_LOAD` span: 1024 pages (4MB) per current safety check

These are pragmatic limits for small-memory QEMU configurations and may be raised later when we stream ELF loading and/or improve memory accounting.
