#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Repair FAT32 directory entry flags.", "fatfix /");

int main(int argc, char** argv) {
    const char* path = (argc >= 2 && argv[1] && argv[1][0]) ? argv[1] : "/";
    int repaired = eyn_sys_fs_fatfix(path);
    if (repaired < 0) {
        printf("fatfix failed on %s (err %d)\n", path, repaired);
        return 1;
    }

    printf("fatfix: repaired %d entries under %s\n", repaired, path);
    return 0;
}
