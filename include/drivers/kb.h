#ifndef KB_H
#define KB_H
#include <misc/types.h>
#include <multiboot.h>

// Line input reader (legacy); returns pointer to a static buffer.
string readStr(void);

// Non-blocking: returns 0 if no key available, else ASCII character.
int kb_getchar_nonblocking(void);

// Used by long-running loops to allow Ctrl+C to interrupt.
void poll_keyboard_for_ctrl_c(void);

#endif
