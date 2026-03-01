#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Switch to ring 3 and run a tiny user-mode stub.", "ring3 yes");

static void usage(void) {
    puts("This will switch the CPU to ring 3 and run a tiny user stub.");
    puts("Usage: ring3 yes");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || strcmp(argv[1], "yes") != 0) {
        usage();
        return 1;
    }

    if (eyn_sys_ring3_test(1) != 0) {
        puts("ring3: failed");
        return 1;
    }

    return 0;
}
