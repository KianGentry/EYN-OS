// NaN comparison behavior smoke test.
// Exercises: double compare codegen, unordered comparisons (NaN).

#include <stdio.h>

int main(void) {
    volatile double z = 0.0;
    double nan = z / z;

    if (nan == nan) {
        printf("fp_nan_cmp: nan == nan should be false\n");
        return 1;
    }
    if (!(nan != nan)) {
        printf("fp_nan_cmp: nan != nan should be true\n");
        return 1;
    }

    if (nan < 1.0) {
        printf("fp_nan_cmp: nan < 1 should be false\n");
        return 1;
    }
    if (nan > 1.0) {
        printf("fp_nan_cmp: nan > 1 should be false\n");
        return 1;
    }
    if (nan <= 1.0) {
        printf("fp_nan_cmp: nan <= 1 should be false\n");
        return 1;
    }
    if (nan >= 1.0) {
        printf("fp_nan_cmp: nan >= 1 should be false\n");
        return 1;
    }

    printf("fp_nan_cmp: ok\n");
    return 0;
}
