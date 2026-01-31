#include <misc/printf.h>

// Some shared shell/FS code expects a `putchar()` symbol.
// On AArch64 we route this through the kernel printf implementation.
void putchar(char c)
{
    printf("%c", c);
}
