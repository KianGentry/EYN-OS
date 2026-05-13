#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

out_dir="testdir/lib"
mkdir -p "$out_dir"
out="$out_dir/libx11compat.so.6"

src="userland/x11compat/x11compat.c"
incdir="userland/include"

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

if [[ "$CC" == "gcc" ]]; then
  if ! echo "int x;" | gcc -m32 -x c -c -o /dev/null - >/dev/null 2>&1; then
    echo "Host gcc does not support -m32. Install multilib (e.g., gcc-multilib) or use i686-elf-gcc." >&2
    exit 1
  fi
fi

obj="tmp_shared_x11compat.o"
"$CC" "${CFLAGS[@]}" -c "$src" -o "$obj"

"$CC" -m32 -shared -Wl,-soname,libx11compat.so.6 -o "$out" "$obj" -Ltestdir/lib -lX11 -lc
chmod 644 "$out"

# A few legacy X11 SONAMEs are expected by the packaged xeyes binary.
# They all resolve to the same compatibility shim.
for alias in libXmu.so.6 libXt.so.6 libXext.so.6 libXrender.so.1 libX11-xcb.so.1 libxcb.so.1 libxcb-present.so.0 libxcb-damage.so.0 libxcb-xfixes.so.0; do
  cp -f "$out_dir/libx11compat.so.6" "$out_dir/$alias"
  chmod 644 "$out_dir/$alias"
done

echo "Built compatibility shim: $out"