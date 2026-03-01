#include <stdio.h>

#include <eynos_cmdmeta.h>

// Optional help metadata consumed by the kernel's `help` command.
// This is stored in an ELF section named `.eynos.cmdmeta`.
EYN_CMDMETA_V1("Print Hello World.", "hello");

int main() {
    printf("Hello World!\n");
}
