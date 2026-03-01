#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Trigger a kernel panic for diagnostics.", "panic yes");

static void usage(void) {
    puts("This will trigger a kernel panic and stop the system.");
    puts("Usage: panic yes");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || strcmp(argv[1], "yes") != 0) {
        usage();
        return 1;
    }

    (void)eyn_sys_trigger_panic(1);
    return 0;
}
