#include <termios.h>
#include <eynos_syscall.h>
#include <stddef.h>

int tcgetattr(int fd, struct termios *termios_p) {
    (void)fd; // EYN-OS provides per-vterm tty mode rather than per-fd
    if (!termios_p) return -1;
    int mode = eyn_sys_tty_get_mode();
    /* Populate a minimal termios structure based on raw flag */
    termios_p->c_iflag = 0;
    termios_p->c_oflag = 0;
    termios_p->c_cflag = 0;
    if (mode & EYN_TTY_MODE_RAW) {
        termios_p->c_lflag = 0; /* raw: no canonical, no echo, no signals */
        for (int i = 0; i < NCCS; ++i) termios_p->c_cc[i] = 0;
        termios_p->c_cc[VMIN] = 1;
        termios_p->c_cc[VTIME] = 0;
    } else {
        termios_p->c_lflag = ICANON | ECHO | ISIG;
        for (int i = 0; i < NCCS; ++i) termios_p->c_cc[i] = 0;
        termios_p->c_cc[VMIN] = 1;
        termios_p->c_cc[VTIME] = 0;
    }
    termios_p->c_ispeed = termios_p->c_ospeed = 0;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    (void)fd; (void)optional_actions;
    if (!termios_p) return -1;
    /* Decide raw vs cooked by checking ICANON/ECHO bits */
    int raw = 0;
    if ((termios_p->c_lflag & (ICANON | ECHO)) == 0) raw = 1;
    int mode = raw ? EYN_TTY_MODE_RAW : 0;
    int rc = eyn_sys_tty_set_mode(mode);
    return rc < 0 ? -1 : 0;
}

void cfmakeraw(struct termios *termios_p) {
    if (!termios_p) return;
    termios_p->c_iflag = 0;
    termios_p->c_oflag = 0;
    termios_p->c_cflag = 0;
    termios_p->c_lflag &= ~(ICANON | ECHO | ISIG);
    for (int i = 0; i < NCCS; ++i) termios_p->c_cc[i] = 0;
    termios_p->c_cc[VMIN] = 1;
    termios_p->c_cc[VTIME] = 0;
}
