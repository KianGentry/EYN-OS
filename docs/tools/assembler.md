# EYN-OS Assembler

The EYN-OS assembler is a custom NASM-compatible assembler that converts assembly code into the EYN executable format. It supports the full i386 instruction set and provides comprehensive error reporting and memory management.

## Architecture

The assembler consists of four main components:

1. **Lexer** - Tokenizes the input source code
2. **Parser** - Builds an Abstract Syntax Tree (AST) from tokens
3. **Symbol Table** - Manages labels and their addresses
4. **Code Generator** - Emits machine code from the AST

## Supported Features

### Instruction Set
The assembler supports the full i386 instruction set with over 200 instructions across all categories:

- **Data Movement**: mov, push, pop, lea, xchg, movsx, movzx
- **Arithmetic**: add, sub, inc, dec, mul, div, imul, idiv, neg, cmp
- **Logical**: and, or, xor, not, test
- **Shifts**: shl, shr, sal, sar, rol, ror, rcl, rcr
- **Control Flow**: jmp, call, ret, jz, jnz, jg, jl, je, jne, ja, jb
- **String**: movs, lods, stos, cmps, scas
- **System**: int, cli, sti, hlt, in, out

### Registers
- **32-bit**: eax, ebx, ecx, edx, esi, edi, esp, ebp
- **16-bit**: ax, bx, cx, dx, si, di, sp, bp
- **8-bit**: al, cl, dl, bl, ah, ch, dh, bh
- **Segment**: es, cs, ss, ds, fs, gs

### Directives
- `section .text` - Code section
- `section .data` - Data section
- `global <symbol>` - Export symbol
- `db`, `dw`, `dd` - Data definitions
- `resb`, `resw`, `resd` - Reserve uninitialized bytes/words/dwords (emitted as zeroes in the final image)
- `align N` - Pad the data section with zeroes until the current offset is a multiple of `N`

### Addressing Modes
- Register: `mov eax, ebx`
- Immediate: `mov eax, 42`
- Memory (r/m32) with full i386 forms:
	- Displacement only: `mov eax, [label]`, `mov [0x1000], ebx`
	- Base + disp: `mov eax, [ebp+8]`
	- Base + index*scale + disp: `mov eax, [eax+edi*4+16]` (ESP is not used as an index)
- LEA: `lea ecx, [label]` or `lea edx, [eax+esi*2+8]`

Notes:
- The assembler emits proper ModR/M and SIB bytes and selects the shortest displacement (disp8 vs disp32) when possible.
- Indirect control flow is supported: `jmp r/m32`, `call r/m32`.
- Conditional jumps are auto-sized: short if the target fits in int8, otherwise near.

## Usage

### Basic Assembly
```bash
assemble input.asm output.eyn
```

### Running Programs
```bash
run output.eyn
```

## Error Reporting

The assembler provides comprehensive error reporting with colored output:

- **Red Errors**: Syntax errors, unknown instructions, memory allocation failures
- **Pink Warnings**: Unused labels, data definitions, potential issues

Error messages include:
- File name and line number
- Context information
- Specific error descriptions

## Memory Management

The assembler uses intelligent memory management:

- **Dynamic Sizing**: Estimates required buffer sizes based on AST content
- **Size Caps**: Maximum 16KB for code and data sections
- **Overflow Detection**: Automatic buffer overflow detection and reporting
- **Memory Safety**: Proper cleanup on allocation failures

## Safety Features

The EYN-OS executable loader includes security restrictions that block potentially dangerous instructions:

- `hlt` (halt) - 0xF4
- `cli` (clear interrupt flag) - 0xFA  
- `sti` (set interrupt flag) - 0xFB
- `int` (software interrupt) - 0xCD
- `in` (input from port) - 0xE4, 0xEC, 0xE5, 0xED
- `out` (output to port) - 0xE6, 0xEE, 0xE7, 0xEF

## Example Programs

### Hello World (testdir/hello_world.asm)
Demonstrates comprehensive assembly features:
- Register operations and arithmetic
- Logical operations (and, or, xor)
- Shift operations (shl, shr)
- Control flow with loops and conditionals
- Data section usage

### Simple Test (testdir/test_hello.asm)
Minimal program for testing basic assembly functionality.

### LEA + Data Directives Example
```asm
section .data
align 16
buf: resb 3
arr: resd 2
msg: db "Hello from LEA!", 0x0A

section .text
global _start
_start:
	; write(stdout, msg, len)
	mov eax, 1
	mov ebx, 1
	lea ecx, [msg]    ; also works: mov ecx, msg
	mov edx, 17
	int 0x80

	; memory/imm group-1 op
	add [arr], 1

	; exit(0)
	mov eax, 2
	xor ebx, ebx
	int 0x80
```

What it demonstrates:
- `align` pads `.data` to a 16-byte boundary before `buf`.
- `resb`/`resd` reserve zero-initialized storage (BSS-like) in the final image.
- `lea` computes the effective address of `msg` and places it in `ecx` for the write syscall.
- `add [arr], 1` uses the group-1 encoding with an optimized imm8 when possible.

## Current Limitations

- **Optimization**: No code optimization performed
- **Debugging**: Limited debug information
- **Libraries**: No standard library support
- **Advanced Features**: No MMX/SSE support
- **Assembler syntax**: Size keywords like `byte/word/dword` are not yet parsed in operands (write `add [arr], 1`, not `add [arr], dword 1`).
- **Native executor**: LEA is supported for typical forms (absolute disp32 and common SIB cases) in the built-in emulator; more exotic forms may behave as NOPs in the emulator but assemble correctly.

## Future Enhancements

- **Advanced Directives**: Include files, macros, more data directives
- **Optimization**: Basic code optimization
- **Debug Support**: Debug information generation
- **Extended Addressing**: More complex memory addressing modes
- **Library Support**: Standard library integration
- **Output Formats**: Listing files, symbol table dumps
- **Integration**: Shell autocomplete, return code propagation

## Development Tips
## Implementation Notes (for contributors)

Recent enhancements relevant to this repository revision:
- Memory operands: unified parsing of `[base + index*scale + disp]` with SIB emission.
- ALU ops: support for `r/m32, r32`, `r32, r/m32`, and `r/m32, imm` with automatic `0x83` (imm8) selection.
- Control flow: indirect `jmp`/`call` plus short/near auto-picking for all Jcc variants.
- LEA: `lea r32, [mem]` emits `0x8D` with full ModR/M/SIB addressing.
- Data directives: `resb/resw/resd` (zero-filled) and `align N` (zero padding) affect both label address computation and emission.
- Emulator: the native execution model recognizes `lea` and computes effective addresses for common encodings so syscalls using `lea` work as expected.

These changes are implemented primarily in:
- `src/utilities/assembler/assemble.c` (memory operand parsing, `emit_ea`, ALU, branches, LEA, directives)
- `src/utilities/assembler/instruction_set.c` (instruction metadata)
- `src/cpu/native_exec.c` (minimal `lea` execution in the emulator)

Style note: keep new function comments to 1–2 lines summarizing purpose, consistent with the codebase.

1. Start with simple programs to test the assembler
2. Use the `fscheck` command to verify filesystem integrity
3. Check for "unsupported instruction" errors and report them
4. Use `Ctrl+C` to interrupt running programs
5. Monitor memory usage with large programs
6. Use colored error messages to quickly identify issues
