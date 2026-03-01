#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("List detected logical drives.", "lsata");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: lsata");
        return 0;
    }

    int count = eyn_sys_drive_get_count();
    if (count < 0) {
        puts("lsata: failed to query drives");
        return 1;
    }

    int current = eyn_sys_drive_get_logical();
    printf("Detected logical drives: %d\n", count);
    for (int i = 0; i < count; ++i) {
        int present = eyn_sys_drive_is_present((uint32_t)i);
        if (present < 0) {
            printf("Drive %d: query error\n", i);
            continue;
        }
        if (present) {
            if (i == current) printf("Drive %d: Present (current)\n", i);
            else printf("Drive %d: Present\n", i);
        } else {
            printf("Drive %d: Not present\n", i);
        }
    }
    return 0;
}
