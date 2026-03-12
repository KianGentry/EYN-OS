# DOOM Port

EYN-OS now ships with a native userland port of **linuxdoom-1.10** and two
practical ways to build or run it:

1. **Host-built DOOM** -- cross-compiled on the host and packed into the disk
   image as the `doom` binary.
2. **In-OS DOOM build** -- compiled from source inside EYN-OS using chibicc via
   the `build_doom` shell script, producing `doom_chibicc`.

This makes the port useful both as a playable application and as a regression
test for the userland compiler, paging, filesystem, and GUI stack.

## What is included

- `doom` -- the host-built DOOM binary in `testdir/programs/doom`
- `build_doom` -- an in-OS shell script that drives chibicc
- `doom_chibicc` -- generated at runtime by `build_doom`
- `doom1.wad` -- bundled test IWAD in `testdir/`

The host-side helper script lives at `devtools/build_doom.sh` and cross-builds
the port into a normal EYN-OS userland executable.

## Run the prebuilt port

After booting EYN-OS:

```bash
doom -iwad /DOOM1.WAD
```

The explicit `-iwad` argument is the most reliable way to launch the bundled
game data regardless of the current working directory or filename casing.

## Build DOOM inside EYN-OS

The `build_doom` script demonstrates the full in-OS compilation workflow:

```bash
build_doom
doom_chibicc
```

Internally, the script does the following:

1. Runs chibicc over `/DOOM/doom_unity.c`.
2. Emits assembly to `/doom_compiled.s`.
3. Invokes chibicc's assembler/linker mode.
4. Writes the result to `/binaries/doom_chibicc`.

This path is intentionally ambitious.  It exercises:

- the shell script interpreter
- large C preprocessing and parsing workloads
- EYNFS file reads and writes
- the UELF toolchain
- demand paging and swap under low-memory pressure

## Why this port matters

DOOM is a good systems milestone because it is much larger than the usual
hello-world style demos.  Getting it to run validates several recent EYN-OS
improvements at once:

- larger practical chibicc workloads
- more capable userland libc coverage
- better GUI/application compatibility plumbing
- improved stability under memory pressure
- end-to-end userland development inside the OS

## Build paths

### Host-side cross-build

From the repository root on Linux:

```bash
bash devtools/build_doom.sh
make eynfsimg
make run
```

This produces the prebuilt `doom` binary that is packed into the EYNFS image.

### In-OS build

Inside the EYN-OS shell:

```bash
build_doom
```

The in-OS path is slower, but it is the most representative demonstration of
EYN-OS as a self-hosting-style development environment.

## Current constraints

- The in-OS `build_doom` path is the more demanding path and is sensitive to
  low-memory pressure.
- The X11 compatibility layer is separate from the DOOM port; DOOM uses native
  EYN-OS userland APIs and compatibility shims tailored for the port.
- Audio/networking behavior follows the constraints of the current EYN-OS
  userland/runtime environment rather than a full Unix environment.

## Related documentation

- `docs/tools/chibicc.md` -- compiler and in-OS build workflow
- `docs/ui/shell-scripts.md` -- `build_doom` scripting model
- `docs/api/userland-uelf-abi.md` -- executable format and runtime ABI
- `docs/api/x11-compat.md` -- source-level GUI compatibility layer for Xlib apps