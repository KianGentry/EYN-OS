#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

out="testdir/chibicc.uelf"
if [[ $# -ge 1 ]]; then
  out="$1"
fi

mkdir -p tmp

ldscript="devtools/user_elf32.ld"
crt0="userland/crt0.S"

incdir="userland/include"
libc_dir="userland/libc"

# Prefer a cross-compiler if available.
if command -v i686-elf-gcc >/dev/null 2>&1; then
  CC=i686-elf-gcc
elif command -v gcc >/dev/null 2>&1; then
  CC=gcc
else
  echo "No C compiler found (need i686-elf-gcc or gcc)." >&2
  exit 1
fi

CFLAGS=(
  -m32
  -std=c11
  -ffreestanding
  -fno-builtin
  -fno-pie
  -fno-pic
  -fno-plt
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-omit-frame-pointer
  -nostdlib
  -nostartfiles
  -I"$incdir"
  -I"$repo_root/chibicc-main"
  -DCHIBICC_EYNOS_USERLAND=1
  -Wall -Wextra
  -O2
)

# Validate that gcc supports -m32 when not using a cross toolchain.
if [[ "$CC" == "gcc" ]]; then
  if ! echo "int x;" | gcc -m32 -x c -c -o /dev/null - >/dev/null 2>&1; then
    echo "Host gcc does not support -m32. Install multilib (e.g., gcc-multilib) or use i686-elf-gcc." >&2
    exit 1
  fi
fi

objdir="tmp/user_chibicc"
mkdir -p "$objdir"

obj_crt="$objdir/crt0.o"

# Build CRT
"$CC" "${CFLAGS[@]}" -c "$crt0" -o "$obj_crt"

# Build userland libc subset
lib_objs=(
  "$objdir/unistd.o"
  "$objdir/string.o"
  "$objdir/stdio.o"
  "$objdir/fcntl.o"
  "$objdir/dirent.o"
  "$objdir/ctype.o"
  "$objdir/errno.o"
  "$objdir/libgen.o"
  "$objdir/time.o"
  "$objdir/stat.o"
  "$objdir/stdlib.o"
)

"$CC" "${CFLAGS[@]}" -c "$libc_dir/unistd.c" -o "$objdir/unistd.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/string.c" -o "$objdir/string.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdio.c" -o "$objdir/stdio.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/fcntl.c" -o "$objdir/fcntl.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/dirent.c" -o "$objdir/dirent.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/ctype.c" -o "$objdir/ctype.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/errno.c" -o "$objdir/errno.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/libgen.c" -o "$objdir/libgen.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/time.c" -o "$objdir/time.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stat.c" -o "$objdir/stat.o"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdlib.c" -o "$objdir/stdlib.o"

lib_archive="$objdir/libeync.a"
rm -f "$lib_archive"
ar rcs "$lib_archive" "${lib_objs[@]}"

# Build chibicc sources
chibicc_objs=()
for src in chibicc-main/*.c; do
  base="$(basename "$src" .c)"
  obj="$objdir/chibicc_${base}.o"
  chibicc_objs+=("$obj")
  "$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
done

# Link a simple ELF32 ET_EXEC at 0x00400000.
"$CC" -m32 -nostdlib -nostartfiles -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" \
  -o "$out" "$obj_crt" "${chibicc_objs[@]}" "$lib_archive" -lgcc

echo "Built $out"
