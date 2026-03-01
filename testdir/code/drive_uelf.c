#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Drive selection helper.", "drive 0");

static void usage(void) {
    puts("Usage: drive <n>");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    char* end = NULL;
    unsigned long logical = strtoul(argv[1], &end, 10);
    if (!end || *end != '\0') {
        puts("drive: invalid drive number");
        return 1;
    }

    int rc = eyn_sys_drive_set_logical((uint32_t)logical);
    if (rc < 0) {
        puts("drive: failed to switch drive");
        return 1;
    }

    int cur = eyn_sys_drive_get_logical();
    if (cur < 0) cur = rc;
    printf("Switched to drive %d\n", cur);
    return 0;
}
