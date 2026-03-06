#!/usr/bin/env bash
#
# build_doom.sh — cross-compile linuxdoom-1.10 for EYN-OS.
#
# Usage:
#   devtools/build_doom.sh [output_path]
#
# Default output: testdir/binaries/doom
#
# What this does:
#   1. Compiles all DOOM C source files (from testdir/DOOM/) with -m32
#      -ffreestanding and the EYN-OS userland include path.
#   2. Skips the original platform backends (i_video.c, i_sound.c,
#      i_system.c, i_net.c) and compiles our EYN-OS replacements instead.
#   3. Compiles the EYN-OS userland libc (libeync.a) from userland/libc/.
#   4. Links everything against the EYN-OS UELF linker script, producing a
#      32-bit ELF ET_EXEC at base address 0x00400000 that the kernel loader
#      understands.
#
# Prerequisites:
#   i686-elf-gcc (cross-compiler) OR host gcc with multilib (gcc-multilib).
#   DOOM1.WAD must be present in testdir/ for the game to run.
#
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

out="${1:-testdir/binaries/doom}"

doom_src="testdir/DOOM"
tmp_root="tmp_user"
mkdir -p "$tmp_root/doom"

ldscript="devtools/user_elf32.ld"
crt0="userland/crt0.S"
incdir="userland/include"
libc_dir="userland/libc"

lib_archive="$tmp_root/libeync.a"

# -------------------------------------------------------------------------
# Compiler selection
# -------------------------------------------------------------------------
if command -v i686-elf-gcc >/dev/null 2>&1; then
    CC=i686-elf-gcc
elif command -v gcc >/dev/null 2>&1; then
    CC=gcc
    # Verify -m32 is supported (requires gcc-multilib on Debian/Ubuntu).
    if ! echo "int x;" | gcc -m32 -x c -c -o /dev/null - >/dev/null 2>&1; then
        echo "Host gcc does not support -m32." >&2
        echo "Install gcc-multilib or use i686-elf-gcc." >&2
        exit 1
    fi
else
    echo "No C compiler found (need i686-elf-gcc or gcc)." >&2
    exit 1
fi

CFLAGS=(
    -m32
    -std=gnu11
    -ffreestanding
    -fno-builtin
    -fno-pie
    -fno-pic
    -fno-plt
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -nostdlib
    -nostartfiles
    -I"$incdir"
    # DOOM-source include path (for local headers like doomdef.h).
    -I"$doom_src"
    # Define the same pre-processor symbols as the original DOOM Makefile.
    -DNORMALUNIX
    -DLINUX
    # Disable the sound server (SNDSERV) path so i_eynos_sound.c stubs work.
    -USNDSERV
    # Suppress warnings that are common in legacy C code and not actionable.
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-prototypes
    # Needed for old DOOM code: 'static nexttic = 0' style implicit-int.
    -Wno-implicit-int
    -O2
)

# -------------------------------------------------------------------------
# Build libeync.a (userland libc)
# -------------------------------------------------------------------------
echo "==> Compiling EYN-OS userland libc…"

compile_libc() {
    local name="$1"
    local src="$2"
    "$CC" "${CFLAGS[@]}" -c "$src" -o "$tmp_root/user_libc_${name}.o"
}

"$CC" "${CFLAGS[@]}" -c "$crt0" -o "$tmp_root/user_crt0.o"

compile_libc unistd  "$libc_dir/unistd.c"
compile_libc string  "$libc_dir/string.c"
compile_libc stdio   "$libc_dir/stdio.c"
compile_libc fcntl   "$libc_dir/fcntl.c"
compile_libc dirent  "$libc_dir/dirent.c"
compile_libc gui     "$libc_dir/gui.c"
compile_libc time    "$libc_dir/time.c"
compile_libc stdlib  "$libc_dir/stdlib.c"
compile_libc errno   "$libc_dir/errno.c"
compile_libc x11     "$libc_dir/x11.c"
compile_libc setjmp  "$libc_dir/setjmp.c"
compile_libc stat    "$libc_dir/stat.c"
compile_libc ctype   "$libc_dir/ctype.c"
compile_libc libgen  "$libc_dir/libgen.c"

rm -f "$lib_archive"
ar rcs "$lib_archive" \
    "$tmp_root/user_libc_unistd.o" \
    "$tmp_root/user_libc_string.o" \
    "$tmp_root/user_libc_stdio.o" \
    "$tmp_root/user_libc_fcntl.o" \
    "$tmp_root/user_libc_dirent.o" \
    "$tmp_root/user_libc_gui.o" \
    "$tmp_root/user_libc_time.o" \
    "$tmp_root/user_libc_stdlib.o" \
    "$tmp_root/user_libc_errno.o" \
    "$tmp_root/user_libc_x11.o" \
    "$tmp_root/user_libc_setjmp.o" \
    "$tmp_root/user_libc_stat.o" \
    "$tmp_root/user_libc_ctype.o" \
    "$tmp_root/user_libc_libgen.o"

echo "==> libeync.a built."

# -------------------------------------------------------------------------
# Enumerate DOOM source files
# -------------------------------------------------------------------------
#
# The original platform backends are replaced by i_eynos_*.c equivalents;
# they are excluded from the compilation entirely.
#
DOOM_SKIP="i_video i_sound i_system i_net i_main"

doom_objects=()

for csrc in "$doom_src"/*.c; do
    base="$(basename "$csrc" .c)"

    # Skip original platform backends.
    skip=0
    for s in $DOOM_SKIP; do
        if [ "$base" = "$s" ]; then skip=1; break; fi
    done
    [ "$skip" -eq 1 ] && continue

    obj="$tmp_root/doom/${base}.o"
    "$CC" "${CFLAGS[@]}" -c "$csrc" -o "$obj"
    doom_objects+=("$obj")
done

echo "==> Compiled ${#doom_objects[@]} DOOM source files."

# -------------------------------------------------------------------------
# Compile i_main.c separately (entry point — lives in DOOM source tree).
# -------------------------------------------------------------------------
"$CC" "${CFLAGS[@]}" -c "$doom_src/i_main.c" -o "$tmp_root/doom/i_main.o"
doom_objects+=("$tmp_root/doom/i_main.o")

# -------------------------------------------------------------------------
# Link
# -------------------------------------------------------------------------
echo "==> Linking $out…"

"$CC" \
    -m32 \
    -nostdlib -nostartfiles \
    -Wl,-m,elf_i386 \
    -Wl,-nostdlib \
    -Wl,-e,_start \
    -Wl,-T,"$ldscript" \
    -o "$out" \
    "$tmp_root/user_crt0.o" \
    "${doom_objects[@]}" \
    -Wl,--start-group "$lib_archive" -lgcc -Wl,--end-group

echo "Built $out"
echo ""
echo "To run DOOM on EYN-OS:"
echo "  1. Run 'make eynfsimg' to pack the binary into the disk image."
echo "  2. In the EYN-OS shell, run:  doom -iwad /DOOM1.WAD"
echo ""
echo "  Note: DOOM searches for 'doom1.wad' (lowercase) by default in the"
echo "  current directory.  Override with -iwad to use the uppercase name."
