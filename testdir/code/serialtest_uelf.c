#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Serial output test (userland).", "serialtest");

int main(int argc, char** argv) {
    const char* msg = "[serialtest] Hello from EYN-OS shell via COM1!\n";
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: serialtest");
        return 0;
    }

    int written = eyn_sys_serial_write_com1(msg, (int)strlen(msg));
    if (written < 0) {
        puts("serialtest: serial write failed");
        return 1;
    }
    printf("Wrote %d bytes to COM1\n", written);
    return 0;
}
