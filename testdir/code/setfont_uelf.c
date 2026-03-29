#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Set runtime system font (.hex/.otf/.ttf).", "setfont /fonts/unscii-16.otf");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: setfont <font_path|builtin>");
        return 0;
    }

    if (argc < 2 || !argv[1] || !argv[1][0]) {
        puts("setfont: missing font path");
        puts("Usage: setfont <font_path|builtin>");
        return 1;
    }

    if (eyn_sys_setfont_path(argv[1]) != 0) {
        printf("setfont: failed to apply font: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
