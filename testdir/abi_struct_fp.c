// ABI + float/struct smoke test for chibicc i386 + EYN-OS ring3.
// Exercises: struct by-value args/returns, mixed float/double fields, bitwise ops.

#include <stdio.h>

typedef struct {
    int a;
    double x;
    float y;
    unsigned int b;
} S;

static S make_s(int a, double x, float y, unsigned int b) {
    S s;
    s.a = a;
    s.x = x;
    s.y = y;
    s.b = b;
    return s;
}

static S tweak(S s, int k) {
    s.a += k;
    s.x = s.x * 2.0 + (double)s.y;
    s.y = (float)(s.y + 0.25f);
    s.b ^= 0x55AA55AAu;
    return s;
}

static int in_range(double v, double lo, double hi) {
    return (v > lo) && (v < hi);
}

int main(void) {
    S s0 = make_s(10, 1.5, 2.0f, 0x12345678u);
    S s1 = tweak(s0, 3);

    if (s1.a != 13) {
        printf("abi_struct_fp: a bad (%d)\n", s1.a);
        return 1;
    }
    if (!in_range(s1.x, 4.999, 5.001)) {
        printf("abi_struct_fp: x bad (%f)\n", s1.x);
        return 1;
    }
    if (!in_range((double)s1.y, 2.249, 2.251)) {
        printf("abi_struct_fp: y bad (%f)\n", (double)s1.y);
        return 1;
    }
    if (s1.b != 0x479E03D2u) {
        printf("abi_struct_fp: b bad (0x%x)\n", s1.b);
        return 1;
    }

    // Print a summary for visual verification.
    printf("abi_struct_fp: a=%d x=%.6f y=%.6f b=0x%x\n", s1.a, s1.x, (double)s1.y, s1.b);
    return 0;
}
