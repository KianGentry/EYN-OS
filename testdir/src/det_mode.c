// Deterministic mode smoke test.

#include <stdio.h>
#include <eynos_syscall.h>

int main(void) {
    printf("det_mode: enabling deterministic mode\n");
    (void)eyn_sys_det_enable(1);

    for (int i = 0; i < 5; ++i) {
        int processed = eyn_sys_det_step(4);
        printf("det_mode: step %d processed=%d\n", i, processed);
    }

    printf("det_mode: disabling deterministic mode\n");
    (void)eyn_sys_det_enable(0);
    return 0;
}
