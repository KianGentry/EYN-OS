#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

out_dir="testdir/lib"
mkdir -p "$out_dir"
out="$out_dir/libc.so.6"

libc_dir="userland/libc"
incdir="userland/include"

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
  -fPIC
  -fno-plt
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-omit-frame-pointer
  -nostdlib
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

# Compile the same subset of libc sources used by the project (exclude math.c)
libs=(
  unistd.c
  string.c
  stdio.c
  fcntl.c
  dirent.c
  gui.c
  time.c
  stdlib.c
  errno.c
  # x11.c is built into libX11 instead of libc to satisfy DT_NEEDED
  setjmp.c
  stat.c
  ctype.c
  libgen.c
  notify.c
)
objs=()
for f in "${libs[@]}"; do
  src="$libc_dir/$f"
  if [ ! -f "$src" ]; then
    echo "warning: missing $src, skipping"
    continue
  fi
  base=$(basename "$src")
  obj="tmp_shared_${base%.c}.o"
  echo "$CC ${CFLAGS[*]} -c $src -o $obj"
  "$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done

# Link shared object
echo "Linking shared libc to $out"
"$CC" -m32 -shared -Wl,-soname,libc.so.6 -o "$out" "${objs[@]}"

# Set permissions
chmod 644 "$out"

echo "Built shared libc: $out"
