#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Synchronize memory-mapped file to disk.", "msync <address>");

static void usage(void) {
    puts("Usage: msync <address>");
    puts("Example: msync 0x12345678");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    unsigned long value = strtoul(argv[1], NULL, 0);
    void* addr = (void*)(uintptr_t)value;

    if (eyn_sys_msync(addr) != 0) {
        printf("msync: failed for 0x%X\n", (unsigned int)value);
        return 1;
    }

    puts("File synchronized successfully");
    return 0;
}
