#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Intel e1000 utilities.", "e1000 init");

static void usage(void) {
    puts("Usage: e1000 probe | e1000 init | e1000 test [--expect-link up|down]");
}

static int parse_link_expect(int argc, char** argv, int* out_expect) {
    *out_expect = -1;
    for (int i = 2; i + 1 < argc; ++i) {
        if (argv[i] && strcmp(argv[i], "--expect-link") == 0 && argv[i + 1]) {
            if (strcmp(argv[i + 1], "up") == 0) {
                *out_expect = 1;
                return 0;
            }
            if (strcmp(argv[i + 1], "down") == 0) {
                *out_expect = 0;
                return 0;
            }
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char* sub = (argc >= 2 && argv[1]) ? argv[1] : "probe";

    if (strcmp(sub, "init") == 0) {
        int rc = eyn_sys_e1000_init();
        if (rc == 0) {
            puts("e1000 init ok (netstack ready)");
            return 0;
        }
        printf("e1000 init failed (%d)\n", rc);
        return 1;
    }

    eyn_e1000_probe_info_t info;
    if (eyn_sys_e1000_probe(&info) != 0) {
        puts("e1000: probe failed");
        return 1;
    }

    if (strcmp(sub, "probe") == 0) {
        printf("e1000: %02x:%02x.%d link=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               (unsigned)info.bus, (unsigned)info.device, (int)info.function,
               info.link_up ? "up" : "down",
               (unsigned)info.mac[0], (unsigned)info.mac[1], (unsigned)info.mac[2],
               (unsigned)info.mac[3], (unsigned)info.mac[4], (unsigned)info.mac[5]);
        return 0;
    }

    if (strcmp(sub, "test") == 0) {
        int expect_link = -1;
        if (parse_link_expect(argc, argv, &expect_link) != 0) {
            puts("e1000: --expect-link must be up|down");
            return 1;
        }
        if (expect_link != -1 && info.link_up != expect_link) {
            printf("FAIL: link is %s (expected %s)\n", info.link_up ? "up" : "down", expect_link ? "up" : "down");
            return 1;
        }
        puts("PASS: e1000 probe looks good");
        return 0;
    }

    usage();
    return 1;

    return 0;
}
