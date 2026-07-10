/* Minimal termios.h for EYN-OS userland
 * Provides a small subset of termios needed by nano/toybox
 */
#ifndef TERMIOS_H
#define TERMIOS_H

#include <stdint.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

/* c_lflag bits (subset) */
#define ICANON  0x0001
#define ECHO    0x0002
#define ISIG    0x0004

/* indices into c_cc */
#define VMIN  6
#define VTIME 5

/* tcsetattr actions */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
void cfmakeraw(struct termios *termios_p);

#endif
