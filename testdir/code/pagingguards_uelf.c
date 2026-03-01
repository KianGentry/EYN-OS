#include <stdio.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Install optional paging guards.", "pagingguards");

int main(void) {
    if (eyn_sys_paging_guards() != 0) {
        puts("pagingguards: failed");
        return 1;
    }

    puts("Requested paging guards: null-page NX and .text/.rodata RO.");
    return 0;
}
