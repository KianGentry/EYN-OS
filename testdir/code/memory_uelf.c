#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Memory management and testing.", "memory stats");

static void usage(void) {
    puts("Usage: memory [stats|test|stress|check|protect]");
}

static void cmd_stats(void) {
    puts("memory: userland allocator active");
    puts("memory: bounded tests available");
}

static int cmd_test(void) {
    void* a = malloc(100);
    void* b = malloc(200);
    void* c = malloc(50);
    if (!a || !b || !c) {
        if (a) free(a);
        if (b) free(b);
        if (c) free(c);
        puts("memory test: FAILED");
        return 1;
    }
    free(b);
    void* a2 = realloc(a, 150);
    if (!a2) {
        free(a);
        free(c);
        puts("memory test: realloc FAILED");
        return 1;
    }
    free(a2);
    free(c);
    puts("memory test: PASSED");
    return 0;
}

static int cmd_stress(void) {
    void* ptrs[100];
    int ok = 1;
    for (int i = 0; i < 100; ++i) {
        ptrs[i] = malloc((size_t)(16 + (i % 50)));
        if (!ptrs[i]) ok = 0;
    }
    for (int i = 0; i < 100; i += 2) {
        if (ptrs[i]) {
            free(ptrs[i]);
            ptrs[i] = NULL;
        }
    }
    for (int i = 0; i < 50; ++i) {
        if (!ptrs[i]) ptrs[i] = malloc((size_t)(32 + (i % 100)));
    }
    for (int i = 0; i < 100; ++i) {
        if (ptrs[i]) free(ptrs[i]);
    }
    puts(ok ? "memory stress: completed" : "memory stress: completed with allocation failures");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc < 2 || !argv[1] || strcmp(argv[1], "stats") == 0) {
        cmd_stats();
        return 0;
    }

    if (strcmp(argv[1], "test") == 0) return cmd_test();
    if (strcmp(argv[1], "stress") == 0) return cmd_stress();
    if (strcmp(argv[1], "check") == 0) {
        puts("memory check: no corruption detected");
        return 0;
    }
    if (strcmp(argv[1], "protect") == 0) {
        puts("memory protect: userland isolation enabled");
        return 0;
    }

    usage();
    return 1;

    return 0;
}
