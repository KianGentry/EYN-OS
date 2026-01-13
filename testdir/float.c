// Float smoke test for chibicc i386 + EYN-OS ring3.
// Prints values using libc "%f" support.

#include <stdio.h>

int main(void) {
    double x = 1.5;
    double y = 2.25;

    double z = x + y;      // 3.75
    if (!(z > 3.74 && z < 3.76)) {
        printf("float: add failed\n");
        return 1;
    }
    z = z * 2.0;           // 7.5
    if (!(z > 7.49 && z < 7.51)) {
        printf("float: mul failed\n");
        return 1;
    }
    z = z / 3.0;           // 2.5
    if (!(z > 2.49 && z < 2.51)) {
        printf("float: div failed\n");
        return 1;
    }
    z = z - 1.0;           // 1.5

    // Validate via comparisons only.
    if (!(z > 1.49 && z < 1.51)) {
        printf("float: sub/range failed\n");
        return 1;
    }

    printf("float: x=%f y=%f\n", x, y);
    printf("float: z=%.6f\n", z);
    return 0;
}
