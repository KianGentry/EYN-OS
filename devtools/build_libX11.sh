#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

out_dir="testdir/lib"
mkdir -p "$out_dir"
out="$out_dir/libX11.so.6"

src="userland/libc/x11.c"
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

obj="tmp_shared_x11.o"
echo "$CC ${CFLAGS[*]} -c $src -o $obj"
"$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"

# Link libX11 against the shared libc we built
echo "Linking libX11 to $out"
"$CC" -m32 -shared -Wl,-soname,libX11.so.6 -o "$out" "$obj" -Ltestdir/lib -lc

chmod 644 "$out"

echo "Built libX11: $out"
