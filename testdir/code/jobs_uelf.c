#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("List background jobs.", "jobs");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: jobs");
        return 0;
    }

    int count = eyn_sys_bg_job_count();
    if (count < 0) {
        puts("jobs: failed to query background jobs");
        return 1;
    }

    if (count == 0) {
        puts("No background jobs.");
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        eyn_bg_job_info_t info;
        memset(&info, 0, sizeof(info));
        if (eyn_sys_bg_job_info(i, &info) != 0) continue;
        printf("[%d] %-7s %s\n", info.pid, info.active ? "Running" : "Done", info.command);
    }

    return 0;
}
