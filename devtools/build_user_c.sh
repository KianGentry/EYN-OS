#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

src="${1:-testdir/hello_c_uelf.c}"
out="${2:-testdir/hello_c_uelf.uelf}"

mkdir -p tmp

ldscript="devtools/user_elf32.ld"
crt0="userland/crt0.S"

incdir="userland/include"
libc_dir="userland/libc"

obj_app="tmp/user_app.o"
obj_crt="tmp/user_crt0.o"
obj_libc_unistd="tmp/user_libc_unistd.o"
obj_libc_string="tmp/user_libc_string.o"
obj_libc_stdio="tmp/user_libc_stdio.o"
obj_libc_fcntl="tmp/user_libc_fcntl.o"
obj_libc_dirent="tmp/user_libc_dirent.o"
lib_archive="tmp/libeync.a"

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

"$CC" "${CFLAGS[@]}" -c "$crt0" -o "$obj_crt"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/unistd.c" -o "$obj_libc_unistd"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/string.c" -o "$obj_libc_string"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdio.c" -o "$obj_libc_stdio"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/fcntl.c" -o "$obj_libc_fcntl"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/dirent.c" -o "$obj_libc_dirent"

rm -f "$lib_archive"
ar rcs "$lib_archive" "$obj_libc_unistd" "$obj_libc_string" "$obj_libc_stdio" "$obj_libc_fcntl" "$obj_libc_dirent"

"$CC" "${CFLAGS[@]}" -c "$src" -o "$obj_app"

# Link a simple ELF32 ET_EXEC at 0x00400000.
"$CC" -m32 -nostdlib -nostartfiles -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" -o "$out" "$obj_crt" "$obj_app" "$lib_archive"

echo "Built $out"
