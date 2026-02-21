#!/usr/bin/env python3

import argparse
import bisect
import re
import sys


def parse_kernel_map(map_path: str):
    # Support either:
    # - nm-style: "00105cfa T slab_alloc"
    # - ld map-style: "0x00105cfa                slab_alloc"
    pat_nm = re.compile(r"^([0-9a-fA-F]+)\s+\w\s+(\S+)$")
    # Avoid matching lines that include a second hex column (size) + object file.
    pat_map = re.compile(r"^\s*0x([0-9a-fA-F]+)\s+([A-Za-z_][A-Za-z0-9_.$]*)\s*$")
    syms = []
    with open(map_path, "r", errors="ignore") as f:
        for line in f:
            s = line.strip("\n")
            m = pat_nm.match(s.strip())
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
                continue

            m = pat_map.match(s)
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def sym_for(syms, addr: int):
    if not syms:
        return None
    addrs = [a for a, _ in syms]
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return None
    sym_addr, name = syms[i]
    return (sym_addr, name, addr - sym_addr)


def parse_addr(s: str) -> int:
    s = s.strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    # allow raw hex (e.g. 00105DEC)
    if re.fullmatch(r"[0-9a-f]+", s):
        return int(s, 16)
    return int(s, 10)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Symbolicate kernel addresses using a kernel.map file")
    ap.add_argument("map", help="Path to kernel.map (e.g. tmp_user/boot/kernel.map)")
    ap.add_argument("addrs", nargs="+", help="Address(es) to symbolicate (hex like 0x00105DEC)")
    args = ap.parse_args(argv)

    syms = parse_kernel_map(args.map)
    if not syms:
        print(f"No symbols parsed from {args.map}", file=sys.stderr)
        return 2

    for s in args.addrs:
        a = parse_addr(s)
        sym = sym_for(syms, a)
        if not sym:
            print(f"0x{a:08X}: <no symbol>")
            continue
        sym_addr, name, off = sym
        print(f"0x{a:08X}: {name}+0x{off:X} (0x{sym_addr:08X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
