#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Intentionally trigger a page fault.", "pf yes [addr] [r|w|x]");

static void usage(void) {
    puts("Triggers a deliberate page fault.");
    puts("Usage: pf yes [addr] [r|w|x]");
    puts("Examples: pf yes | pf yes 0x0 r | pf yes 0xDEADBEEF w");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || strcmp(argv[1], "yes") != 0) {
        usage();
        return 1;
    }

    uint32_t addr = 0;
    if (argc >= 3 && argv[2] && argv[2][0]) {
        addr = (uint32_t)strtoul(argv[2], NULL, 0);
    }

    int mode = 0;
    if (argc >= 4 && argv[3] && argv[3][0]) {
        if (argv[3][0] == 'w') mode = 1;
        else if (argv[3][0] == 'x') mode = 2;
    }

    (void)eyn_sys_trigger_pf(addr, mode, 1);
    return 0;
}
