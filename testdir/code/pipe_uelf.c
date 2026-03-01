#include <stdio.h>
#include <string.h>
#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Run a command pipeline from userspace command launcher.", "pipe files 'search test -a'");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: pipe <left_cmd> <right_cmd>");
        return 0;
    }

    puts("Pipeline support is shell-native.");
    puts("Use syntax directly: cmd1 | cmd2");
    if (argc >= 3) {
        printf("Example equivalent: %s | %s\n", argv[1], argv[2]);
    }

    return 0;
}
