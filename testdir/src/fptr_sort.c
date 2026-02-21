// Function-pointer + loops + array smoke test.
// Exercises: indirect calls, comparisons, swapping, basic control flow.

#include <stdio.h>

typedef int (*cmp_fn)(int a, int b);

static int cmp_asc(int a, int b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static void sort_ints(int* a, int n, cmp_fn cmp) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j + 1 < n; j++) {
            if (cmp(a[j], a[j + 1]) > 0) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
}

static int check_sorted(const int* a, int n) {
    for (int i = 0; i + 1 < n; i++) {
        if (a[i] > a[i + 1]) return 0;
    }
    return 1;
}

int main(void) {
    int a[10] = { 7, 3, 9, 1, 5, 8, 2, 6, 4, 0 };

    sort_ints(a, 10, cmp_asc);

    if (!check_sorted(a, 10)) {
        printf("fptr_sort: not sorted\n");
        return 1;
    }

    printf("fptr_sort: ");
    for (int i = 0; i < 10; i++) {
        printf("%d ", a[i]);
    }
    printf("\n");

    return 0;
}
