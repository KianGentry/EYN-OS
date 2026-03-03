#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# Usage:
#   build_user_c_chibicc.sh <src.c> [out.uelf]
#   build_user_c_chibicc.sh <src1.c> <src2.c> ... <out.uelf>
# If the last argument ends with .uelf, it is treated as the output path.

default_src="testdir/hello_c_uelf.c"
default_out="testdir/hello_c_uelf.uelf"

declare -a sources=()
out="$default_out"

if [[ $# -eq 0 ]]; then
  sources+=("$default_src")
else
  last="${!#}"
  if [[ "$last" == *.uelf ]]; then
    out="$last"
    for ((i=1; i<$#; i++)); do
      sources+=("${!i}")
    done
  else
    sources+=("$1")
    if [[ $# -ge 2 ]]; then
      out="$2"
    fi
  fi
fi

if [[ ${#sources[@]} -eq 0 ]]; then
  echo "No input sources provided." >&2
  exit 1
fi

tmp_root="tmp_user"
mkdir -p "$tmp_root"

ldscript="devtools/user_elf32.ld"
crt0="userland/crt0.S"

incdir="userland/include"
libc_dir="userland/libc"

chibicc_bin="chibicc-main/chibicc"

obj_app_prefix="$tmp_root/user_app_chibicc"
obj_crt="$tmp_root/user_crt0.o"
obj_libc_unistd="$tmp_root/user_libc_unistd.o"
obj_libc_string="$tmp_root/user_libc_string.o"
obj_libc_stdio="$tmp_root/user_libc_stdio.o"
obj_libc_fcntl="$tmp_root/user_libc_fcntl.o"
obj_libc_dirent="$tmp_root/user_libc_dirent.o"
obj_libc_gui="$tmp_root/user_libc_gui.o"
obj_libc_time="$tmp_root/user_libc_time.o"
obj_libc_stdlib="$tmp_root/user_libc_stdlib.o"
obj_libc_errno="$tmp_root/user_libc_errno.o"
obj_libc_x11="$tmp_root/user_libc_x11.o"
lib_archive="$tmp_root/libeync.a"

if [[ ! -x "$chibicc_bin" ]]; then
  echo "Missing $chibicc_bin (build it with: (cd chibicc-main && make))" >&2
  exit 1
fi

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

# Build CRT + libc with GCC (known-good toolchain).
"$CC" "${CFLAGS[@]}" -c "$crt0" -o "$obj_crt"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/unistd.c" -o "$obj_libc_unistd"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/string.c" -o "$obj_libc_string"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdio.c" -o "$obj_libc_stdio"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/fcntl.c" -o "$obj_libc_fcntl"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/dirent.c" -o "$obj_libc_dirent"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/gui.c" -o "$obj_libc_gui"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/time.c" -o "$obj_libc_time"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/stdlib.c" -o "$obj_libc_stdlib"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/errno.c" -o "$obj_libc_errno"
"$CC" "${CFLAGS[@]}" -c "$libc_dir/x11.c" -o "$obj_libc_x11"

rm -f "$lib_archive"
ar rcs "$lib_archive" \
  "$obj_libc_x11" \
  "$obj_libc_unistd" \
  "$obj_libc_string" \
  "$obj_libc_stdio" \
  "$obj_libc_fcntl" \
  "$obj_libc_dirent" \
  "$obj_libc_gui" \
  "$obj_libc_time" \
  "$obj_libc_stdlib" \
  "$obj_libc_errno"


# Compile the user program(s) with chibicc i386 backend.
declare -a obj_apps=()
idx=0
for src in "${sources[@]}"; do
  obj_app="${obj_app_prefix}_${idx}.o"
  obj_apps+=("$obj_app")
  "$chibicc_bin" -target eynos --sysroot "$repo_root/userland" -c -o "$obj_app" "$src"
  idx=$((idx + 1))
done

# Link a simple ELF32 ET_EXEC at 0x00400000.
"$CC" -m32 -nostdlib -nostartfiles -Wl,-m,elf_i386 -Wl,-nostdlib -Wl,-e,_start -Wl,-T,"$ldscript" \
  -o "$out" "$obj_crt" "${obj_apps[@]}" -Wl,--start-group "$lib_archive" -lgcc -Wl,--end-group

echo "Built $out"
