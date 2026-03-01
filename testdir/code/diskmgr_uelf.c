#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Manage logical drives from userland.", "diskmgr status");

static void usage(void) {
    puts("Usage: diskmgr <help|status|list|use|init>");
    puts("  status          show active logical drive and total count");
    puts("  list            list available logical drives");
    puts("  use <logical>   switch active drive");
    puts("  init            probe/initialize storage and network defaults");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        int cur = eyn_sys_drive_get_logical();
        int cnt = eyn_sys_drive_get_count();
        if (cur < 0 || cnt < 0) {
            puts("diskmgr: failed to query drive status");
            return 1;
        }
        printf("Active logical drive: %d\n", cur);
        printf("Logical drive count: %d\n", cnt);
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        int cnt = eyn_sys_drive_get_count();
        if (cnt < 0) {
            puts("diskmgr: failed to query drive count");
            return 1;
        }
        for (int i = 0; i < cnt; ++i) {
            int present = eyn_sys_drive_is_present((uint32_t)i);
            printf("drive %d: %s\n", i, (present > 0) ? "present" : "absent");
        }
        return 0;
    }

    if (strcmp(argv[1], "use") == 0) {
        if (argc < 3 || !argv[2] || !argv[2][0]) {
            puts("diskmgr: use requires <logical>");
            return 1;
        }
        int id = 0;
        for (int i = 0; argv[2][i]; ++i) {
            char c = argv[2][i];
            if (c < '0' || c > '9') {
                puts("diskmgr: logical drive must be numeric");
                return 1;
            }
            id = (id * 10) + (c - '0');
        }
        int rc = eyn_sys_drive_set_logical((uint32_t)id);
        if (rc < 0) {
            puts("diskmgr: failed to switch drive");
            return 1;
        }
        printf("diskmgr: active logical drive set to %d\n", rc);
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        int rc = eyn_sys_init_services();
        if (rc < 0) {
            puts("diskmgr: init failed");
            return 1;
        }
        puts("diskmgr: services initialized");
        return 0;
    }

    usage();
    return 1;
}
