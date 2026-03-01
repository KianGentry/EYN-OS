#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Integer calculator supporting + - * /.", "calc 2+3*4");

static void usage(void) {
    puts("Usage: calc <expr>\n"
         "Examples: calc 2+3  |  calc 9/3");
}

static int parse_int(const char* s, long* out) {
    if (!s || !s[0] || !out) return -1;
    char* end = 0;
    long v = (long)strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = v;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) { usage(); return 1; }
    if (strcmp(argv[1], "-h") == 0) { usage(); return 0; }

    const char* e = argv[1];
    char left[32] = {0}, right[32] = {0};
    char op = 0;
    int i = 0, j = 0, k = 0;
    for (; e[i]; ++i) {
        if (e[i] == '+' || e[i] == '-' || e[i] == '*' || e[i] == '/') { op = e[i++]; break; }
        if (j < 31) left[j++] = e[i];
    }
    for (; e[i]; ++i) if (k < 31) right[k++] = e[i];

    long a = 0, b = 0;
    if (!op || parse_int(left, &a) != 0 || parse_int(right, &b) != 0) {
        puts("calc: invalid expression");
        return 1;
    }

    long r = 0;
    if (op == '+') r = a + b;
    else if (op == '-') r = a - b;
    else if (op == '*') r = a * b;
    else if (op == '/') {
        if (b == 0) { puts("calc: division by zero"); return 1; }
        r = a / b;
    }

    printf("%ld\n", r);
    return 0;
}
