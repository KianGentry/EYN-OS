#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Generate random numbers.", "random 5");

static int parse_u32(const char* s, unsigned int* out) {
    if (!s || !s[0] || !out) return -1;
    char* end = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (unsigned int)v;
    return 0;
}

static void usage(void) {
    puts("Usage: random [-s seed] [count] | random [-s seed] [min] [max] [count]\n"
         "Examples:\n"
         "  random\n"
         "  random 5\n"
         "  random 10 20\n"
         "  random 10 20 5\n"
         "  random -s 12345 10 20 5");
}

static unsigned int rand_u32(void) {
    unsigned int hi = (unsigned int)(rand() & RAND_MAX);
    unsigned int lo = (unsigned int)(rand() & RAND_MAX);
    return (hi << 16) ^ lo;
}

int main(int argc, char** argv) {
    int argi = 1;

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc >= 4 && argv[1] && strcmp(argv[1], "-s") == 0) {
        unsigned int seed = 0;
        if (parse_u32(argv[2], &seed) != 0) {
            puts("random: invalid seed");
            return 1;
        }
        srand(seed);
        argi = 3;
    }

    if (argi >= argc) {
        printf("%u\n", rand_u32());
        return 0;
    }

    unsigned int a = 0;
    if (parse_u32(argv[argi], &a) != 0) {
        puts("random: invalid number");
        return 1;
    }

    if (argi + 1 < argc) {
        unsigned int b = 0;
        if (parse_u32(argv[argi + 1], &b) != 0 || a > b) {
            puts("random: invalid range");
            return 1;
        }

        unsigned int count = 1;
        if (argi + 2 < argc) {
            if (parse_u32(argv[argi + 2], &count) != 0 || count == 0) {
                puts("random: invalid count");
                return 1;
            }
        }
        if (count > 1000u) {
            puts("random: count too large (max 1000)");
            return 1;
        }

        unsigned int span = b - a + 1u;
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int n = a + (rand_u32() % span);
            printf("%u", n);
            if (i + 1u < count) putchar(' ');
        }
        putchar('\n');
        return 0;
    }

    if (a > 1000u) {
        puts("random: count too large (max 1000)");
        return 1;
    }

    for (unsigned int i = 0; i < a; ++i) {
        printf("%u", rand_u32());
        if (i + 1u < a) putchar(' ');
    }
    putchar('\n');
    return 0;
}
