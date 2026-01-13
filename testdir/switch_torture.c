// Switch/case stress test.
// Exercises: dense cases, fallthrough, default, arithmetic in cases.

#include <stdio.h>

static int f(int x) {
    int acc = 0;
    switch (x) {
        case 0: acc += 1; break;
        case 1: acc += 3; break;
        case 2: acc += 5; break;
        case 3: acc += 7; break;
        case 4: acc += 11; break;
        case 5: acc += 13; break;
        case 6: acc += 17; break;
        case 7: acc += 19; break;
        case 8: acc += 23; break;
        case 9: acc += 29; break;
        case 10: acc += 31; break;
        case 11:
            acc += 37;
            // fallthrough
        case 12:
            acc += 41;
            break;
        case 100: acc += 97; break;
        default:
            acc -= 123;
            break;
    }
    return acc;
}

int main(void) {
    int sum = 0;
    for (int i = -2; i <= 12; i++) sum += f(i);
    sum += f(100);

    // Expected:
    // i=-2,-1 => -123 each => -246
    // 0..10 => 1+3+5+7+11+13+17+19+23+29+31 = 159
    // 11 => 37+41 = 78
    // 12 => 41
    // + f(100)=97
    // Total = -246 + 159 + 78 + 41 + 97 = 129
    if (sum != 129) {
        printf("switch_torture: bad sum=%d\n", sum);
        return 1;
    }

    printf("switch_torture: ok sum=%d\n", sum);
    return 0;
}
