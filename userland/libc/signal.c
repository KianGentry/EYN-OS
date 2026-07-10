#include <signal.h>
#include <eynos_syscall.h>
#include <stdint.h>

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (act) {
        /* Kernel syscall takes (sig, handler) */
        int rc = eyn_syscall3_iii(EYN_SYSCALL_SIGNAL, signum, (int)(uintptr_t)act->sa_handler, 0);
        if (rc < 0) return -1;
    } else {
        /* Removing handler: set to default */
        int rc = eyn_syscall3_iii(EYN_SYSCALL_SIGNAL, signum, (int)(uintptr_t)SIG_DFL, 0);
        if (rc < 0) return -1;
    }

    if (oldact) {
        /* Kernel does not provide previous handler via syscall; report default */
        oldact->sa_handler = SIG_DFL;
        oldact->sa_mask = 0;
        oldact->sa_flags = 0;
    }
    return 0;
}

int sigemptyset(sigset_t *set) {
    if (!set) return -1;
    *set = 0;
    return 0;
}

int sigaddset(sigset_t *set, int signo) {
    if (!set || signo <= 0) return -1;
    *set |= (1UL << (signo % (8 * sizeof(unsigned long))));
    return 0;
}
