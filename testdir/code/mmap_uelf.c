#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Memory map a file for zero-copy access.", "mmap <filename> [readonly]");

static void usage(void) {
    puts("Usage: mmap <filename> [readonly]");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    int read_only = 0;
    if (argc >= 3 && argv[2] && strcmp(argv[2], "readonly") == 0) {
        read_only = 1;
    }

    size_t size = 0;
    void* addr = eyn_sys_mmap(argv[1], &size, read_only);
    if (!addr) {
        printf("mmap: failed to map %s\n", argv[1]);
        return 1;
    }

    printf("Address: 0x%X\n", (unsigned int)(unsigned long)addr);
    printf("Size: %u bytes\n", (unsigned int)size);
    printf("Mode: %s\n", read_only ? "read-only" : "read-write");
    return 0;
}
