#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Exits the kernel and shuts down the system.", "exit");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: exit");
        return 0;
    }

    puts("exit: terminating current user task");
    _exit(0);
}
