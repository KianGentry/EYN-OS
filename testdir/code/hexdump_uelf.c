#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Hex dump file bytes.", "hexdump /test.txt 256");

/*
 * SECURITY-INVARIANT: Fixed line width for predictable memory use.
 *
 * Why: Keeps per-line formatting bounded and stable in low-RAM environments.
 * Invariant: Formatting logic assumes exactly HEXDUMP_LINE_BYTES bytes per row.
 * Breakage if changed:
 *   - Larger values increase stack usage and widen output unexpectedly.
 *   - Smaller values reduce readability and may break downstream parsers.
 * ABI-sensitive: No.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (resource bounding).
 */
#define HEXDUMP_LINE_BYTES 16

static void usage(void) {
    puts("Usage: hexdump <file> [max_bytes]");
}

static int is_printable(unsigned char c) {
    return (c >= 32 && c <= 126);
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    size_t max_bytes = (size_t)-1;
    if (argc >= 3 && argv[2] && argv[2][0]) {
        char* end = NULL;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (end != argv[2]) max_bytes = (size_t)v;
    }

    int fd = open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        printf("hexdump: cannot open %s\n", argv[1]);
        return 1;
    }

    unsigned char buf[HEXDUMP_LINE_BYTES];
    size_t off = 0;
    while (off < max_bytes) {
        size_t want = HEXDUMP_LINE_BYTES;
        if (max_bytes - off < want) want = max_bytes - off;
        ssize_t rc = read(fd, buf, want);
        if (rc <= 0) break;

        int n = (int)rc;
        printf("%08x  ", (unsigned)off);
        for (int i = 0; i < HEXDUMP_LINE_BYTES; ++i) {
            if (i < n) printf("%02x ", (unsigned)buf[i]);
            else printf("   ");
            if (i == 7) putchar(' ');
        }

        putchar(' ');
        putchar('|');
        for (int i = 0; i < n; ++i) putchar(is_printable(buf[i]) ? buf[i] : '.');
        putchar('|');
        putchar('\n');

        off += (size_t)n;
    }

    (void)close(fd);
    return 0;
}
