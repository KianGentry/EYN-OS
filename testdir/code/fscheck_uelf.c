#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Check filesystem integrity.", "fscheck");

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    puts("Checking filesystem integrity...");
    int result = eyn_sys_fs_check_integrity();
    if (result == 0) {
        puts("Filesystem is healthy.");
        return 0;
    }

    puts("Filesystem corruption detected!");
    puts("Recommendation: Reboot and reformat if problems persist.");
    return 1;
}
