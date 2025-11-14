This folder contains a 32-bit Linux C hello program.

- hello_c.c: simple program using write(1, ...) to stdout
- hello_c_static: statically linked i386 ELF built on host with: gcc -m32 -static hello_c.c -o hello_c_static

EYN-OS loader will execute it and the Linux syscall compatibility layer will handle write/exit.

You can re-run `make eynfsimg` to ensure this binary is copied into eynfs.img under the root.
