#ifndef KB_H
#define KB_H
#include <misc/types.h>
#include <multiboot.h>

string readStr();
// Non-blocking: returns 0 if no key available, else ASCII character.
int kb_getchar_nonblocking();

#endif
