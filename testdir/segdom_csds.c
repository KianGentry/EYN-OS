// Ring3 diagnostic: print current CS/DS selectors.

#include <stdio.h>
#include <eynos_syscall.h>

int main(void) {
    uint16_t cs = 0;
    uint16_t ds = 0;

    eyn_user_read_segments(&cs, &ds);
    printf("segdom: cs=0x%04x ds=0x%04x\n", (unsigned)cs, (unsigned)ds);
    return 0;
}
