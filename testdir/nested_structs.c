// Nested struct/pointer/array test.
// Exercises: pointer chasing, nested arrays, address-of, field access.

#include <stdio.h>

typedef struct {
    int v[3];
} Vec3;

typedef struct {
    Vec3 a;
    Vec3 b;
    int tag;
} Pair;

static int dot(const Vec3* p, const Vec3* q) {
    return p->v[0] * q->v[0] + p->v[1] * q->v[1] + p->v[2] * q->v[2];
}

static void fill(Vec3* p, int x, int y, int z) {
    p->v[0] = x;
    p->v[1] = y;
    p->v[2] = z;
}

int main(void) {
    Pair p;
    fill(&p.a, 1, 2, 3);
    fill(&p.b, 4, 5, 6);
    p.tag = 42;

    int d = dot(&p.a, &p.b); // 1*4 + 2*5 + 3*6 = 32
    if (d != 32 || p.tag != 42) {
        printf("nested_structs: bad d=%d tag=%d\n", d, p.tag);
        return 1;
    }

    // Mutate through pointers.
    Vec3* pa = &p.a;
    pa->v[1] = 99;
    d = dot(&p.a, &p.b); // 1*4 + 99*5 + 3*6 = 517
    if (d != 517) {
        printf("nested_structs: bad d2=%d\n", d);
        return 1;
    }

    printf("nested_structs: ok d=%d d2=%d\n", 32, d);
    return 0;
}
