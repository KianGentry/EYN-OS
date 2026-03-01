#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Initialize core services.", "init");

int main(int argc, char** argv) {
    (void)argc;
    if (argv && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: init");
        return 0;
    }
    puts("Initializing full system services...");
    if (eyn_sys_init_services() != 0) {
        puts("init: failed to initialize services");
        return 1;
    }
    puts("System initialization complete!");
    return 0;
}
