#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Show or clear command history.", "history");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: history");
        puts("       history clear");
        return 0;
    }

    if (argc >= 2 && argv[1] && strcmp(argv[1], "clear") == 0) {
        if (eyn_sys_history_clear() != 0) {
            puts("history: failed to clear history");
            return 1;
        }
        puts("Command history cleared.");
        return 0;
    }

    int count = eyn_sys_history_count();
    if (count < 0) {
        puts("history: failed to read history");
        return 1;
    }

    if (count == 0) {
        puts("No command history.");
        return 0;
    }

    char entry[220];
    for (int i = 0; i < count; ++i) {
        memset(entry, 0, sizeof(entry));
        if (eyn_sys_history_entry(i, entry, sizeof(entry)) == 0) {
            printf("%d: %s\n", i + 1, entry);
        }
    }

    return 0;
}
