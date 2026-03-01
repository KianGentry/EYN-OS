#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Send ICMP echo requests.", "ping 10.0.2.2");

static int parse_ipv4(const char* s, uint8_t out[4]) {
    if (!s || !out) return -1;
    for (int part = 0; part < 4; ++part) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = (v * 10) + (*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part != 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    return (*s == '\0') ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0] || strcmp(argv[1], "-h") == 0) {
        puts("Usage: ping <dst_ip> [count] [local_ip]");
        return (argc >= 2) ? 0 : 1;
    }

    uint8_t dst_ip[4];
    if (parse_ipv4(argv[1], dst_ip) != 0) {
        puts("ping: invalid dst_ip (expected a.b.c.d)");
        return 1;
    }

    eyn_net_config_t cfg;
    if (eyn_sys_netcfg_get(&cfg) != 0) {
        puts("ping: failed to read net config");
        return 1;
    }

    uint8_t local_ip[4] = {cfg.local_ip[0], cfg.local_ip[1], cfg.local_ip[2], cfg.local_ip[3]};
    int count = 4;

    if (argc >= 3 && argv[2] && argv[2][0]) {
        char* end = NULL;
        unsigned long v = strtoul(argv[2], &end, 10);
        if (!end || *end != '\0' || v == 0 || v > 64) {
            puts("ping: invalid count (1..64)");
            return 1;
        }
        count = (int)v;
    }

    if (argc >= 4 && argv[3] && argv[3][0]) {
        if (parse_ipv4(argv[3], local_ip) != 0) {
            puts("ping: invalid local_ip (expected a.b.c.d)");
            return 1;
        }
    }

    if (eyn_sys_net_is_inited() == 0) {
        puts("Note: run 'e1000 init' first if ping fails.");
    }

    printf("PING %d.%d.%d.%d from %d.%d.%d.%d (%d request(s))\n",
           (int)dst_ip[0], (int)dst_ip[1], (int)dst_ip[2], (int)dst_ip[3],
           (int)local_ip[0], (int)local_ip[1], (int)local_ip[2], (int)local_ip[3],
           count);

    int replies = eyn_sys_net_ping(dst_ip, local_ip, count);
    if (replies < 0) {
        printf("PING failed (%d).\n", replies);
        return 1;
    }

    printf("PING done: %d/%d replies\n", replies, count);
    return 0;
}
