// Multi-object link test.
// Exercises: multiple .c compilation, extern calls across objects, header include.

#include <stdio.h>
#include "multiobj_math.h"

static int local_fn(int x) {
    return clampi(x * 3 + 1, -10, 10);
}

int main(void) {
    int a = add3(1, 2, 3);
    int b = local_fn(4); // 4*3+1=13 => clamp to 10
    int c = checksum_u32(0x12345678u);

    if (a != 6 || b != 10 || c == 0) {
        printf("multiobj: bad a=%d b=%d c=%d\n", a, b, c);
        return 1;
    }

    printf("multiobj: ok a=%d b=%d c=%d\n", a, b, c);
    return 0;
}
