#include "multiobj_math.h"

int add3(int a, int b, int c) {
    return a + b + c;
}

int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int checksum_u32(unsigned int x) {
    // Simple mix (no 64-bit required)
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (int)(x & 0x7fffffffU);
}
