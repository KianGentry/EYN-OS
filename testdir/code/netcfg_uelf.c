#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Network configuration command.", "netcfg show");

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

static uint32_t ipv4_to_u32(const uint8_t ip[4]) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

static int append_char(char* out, int cap, int* idx, char ch) {
    if (!out || !idx || cap <= 0) return -1;
    if (*idx >= cap - 1) return -1;
    out[*idx] = ch;
    (*idx)++;
    out[*idx] = '\0';
    return 0;
}

static int append_uint_dec(char* out, int cap, int* idx, unsigned int v) {
    char tmp[4];
    int n = 0;
    if (v >= 100) {
        tmp[n++] = (char)('0' + (v / 100));
        v %= 100;
        tmp[n++] = (char)('0' + (v / 10));
        tmp[n++] = (char)('0' + (v % 10));
    } else if (v >= 10) {
        tmp[n++] = (char)('0' + (v / 10));
        tmp[n++] = (char)('0' + (v % 10));
    } else {
        tmp[n++] = (char)('0' + v);
    }
    for (int i = 0; i < n; ++i) {
        if (append_char(out, cap, idx, tmp[i]) != 0) return -1;
    }
    return 0;
}

static int append_ip_line(char* out, int cap, int* idx, const char* key, const uint8_t ip[4]) {
    if (!key || !ip) return -1;
    for (int i = 0; key[i]; ++i) {
        if (append_char(out, cap, idx, key[i]) != 0) return -1;
    }
    if (append_char(out, cap, idx, '=') != 0) return -1;
    for (int part = 0; part < 4; ++part) {
        if (append_uint_dec(out, cap, idx, (unsigned int)ip[part]) != 0) return -1;
        if (part != 3 && append_char(out, cap, idx, '.') != 0) return -1;
    }
    if (append_char(out, cap, idx, '\n') != 0) return -1;
    return 0;
}

static int netmask_is_contiguous(const uint8_t mask[4]) {
    uint32_t m = ipv4_to_u32(mask);
    uint32_t neg = ~m;
    return (neg == 0) || (((neg + 1u) & neg) == 0u);
}

static int save_cfg_to_path(const char* path, const eyn_net_config_t* cfg) {
    char out[160];
    int idx = 0;
    out[0] = '\0';
    if (append_ip_line(out, (int)sizeof(out), &idx, "ip", cfg->local_ip) != 0) return -1;
    if (append_ip_line(out, (int)sizeof(out), &idx, "gw", cfg->gateway_ip) != 0) return -1;
    if (append_ip_line(out, (int)sizeof(out), &idx, "mask", cfg->netmask) != 0) return -1;
    if (append_ip_line(out, (int)sizeof(out), &idx, "dns", cfg->dns_ip) != 0) return -1;
    return (writefile(path, out, (size_t)idx) >= 0) ? 0 : -1;
}

static int load_cfg_from_path(const char* path, eyn_net_config_t* cfg) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char* key = line;
            const char* val = eq + 1;
            uint8_t ip[4];
            if (parse_ipv4(val, ip) == 0) {
                if (strcmp(key, "ip") == 0) memcpy(cfg->local_ip, ip, 4);
                else if (strcmp(key, "gw") == 0) memcpy(cfg->gateway_ip, ip, 4);
                else if (strcmp(key, "mask") == 0) memcpy(cfg->netmask, ip, 4);
                else if (strcmp(key, "dns") == 0) memcpy(cfg->dns_ip, ip, 4);
            }
        }
        line = next;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: netcfg show | netcfg verify | netcfg route <dst_ip> | netcfg defaults [--save] | netcfg set ip|gw|mask|dns <a.b.c.d> [--save] | netcfg save [path] | netcfg load [path]");
        return 0;
    }

    eyn_net_config_t cfg;
    if (eyn_sys_netcfg_get(&cfg) != 0) {
        puts("netcfg: failed to read config");
        return 1;
    }

    const char* sub = (argc >= 2 && argv[1]) ? argv[1] : "show";
    if (strcmp(sub, "show") == 0) {
        printf("netcfg:\n");
        printf("  ip=%u.%u.%u.%u\n", (unsigned)cfg.local_ip[0], (unsigned)cfg.local_ip[1], (unsigned)cfg.local_ip[2], (unsigned)cfg.local_ip[3]);
        printf("  gw=%u.%u.%u.%u\n", (unsigned)cfg.gateway_ip[0], (unsigned)cfg.gateway_ip[1], (unsigned)cfg.gateway_ip[2], (unsigned)cfg.gateway_ip[3]);
        printf("  mask=%u.%u.%u.%u\n", (unsigned)cfg.netmask[0], (unsigned)cfg.netmask[1], (unsigned)cfg.netmask[2], (unsigned)cfg.netmask[3]);
        printf("  dns=%u.%u.%u.%u\n", (unsigned)cfg.dns_ip[0], (unsigned)cfg.dns_ip[1], (unsigned)cfg.dns_ip[2], (unsigned)cfg.dns_ip[3]);
        return 0;
    }

    if (strcmp(sub, "verify") == 0) {
        int ok = 1;
        if (cfg.local_ip[0] == 0 && cfg.local_ip[1] == 0 && cfg.local_ip[2] == 0 && cfg.local_ip[3] == 0) {
            puts("netcfg verify: ip is 0.0.0.0");
            ok = 0;
        }
        if (!netmask_is_contiguous(cfg.netmask)) {
            puts("netcfg verify: netmask is not contiguous");
            ok = 0;
        }
        if (ok) puts("netcfg verify: ok");
        return ok ? 0 : 1;
    }

    if (strcmp(sub, "route") == 0) {
        if (argc < 3 || !argv[2]) {
            puts("Usage: netcfg route <dst_ip>");
            return 1;
        }
        uint8_t dst[4];
        if (parse_ipv4(argv[2], dst) != 0) {
            puts("netcfg route: invalid dst_ip");
            return 1;
        }
        uint32_t dst_u = ipv4_to_u32(dst);
        uint32_t ip_u = ipv4_to_u32(cfg.local_ip);
        uint32_t mask_u = ipv4_to_u32(cfg.netmask);
        int on_link = ((dst_u & mask_u) == (ip_u & mask_u));
        printf("route: dst=%u.%u.%u.%u ", (unsigned)dst[0], (unsigned)dst[1], (unsigned)dst[2], (unsigned)dst[3]);
        if (!on_link && !(cfg.gateway_ip[0] == 0 && cfg.gateway_ip[1] == 0 && cfg.gateway_ip[2] == 0 && cfg.gateway_ip[3] == 0)) {
            printf("via gw=%u.%u.%u.%u\n", (unsigned)cfg.gateway_ip[0], (unsigned)cfg.gateway_ip[1], (unsigned)cfg.gateway_ip[2], (unsigned)cfg.gateway_ip[3]);
        } else {
            puts("direct");
        }
        return 0;
    }

    if (strcmp(sub, "defaults") == 0) {
        if (eyn_sys_netcfg_defaults() != 0) {
            puts("netcfg: failed to restore defaults");
            return 1;
        }
        if (argc >= 3 && argv[2] && strcmp(argv[2], "--save") == 0) {
            if (eyn_sys_netcfg_get(&cfg) != 0 || save_cfg_to_path("/config/net.cfg", &cfg) != 0) {
                puts("netcfg: defaults restored, save failed");
                return 1;
            }
            puts("netcfg: defaults restored and saved to /config/net.cfg");
            return 0;
        }
        puts("netcfg: defaults restored");
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (argc < 4 || !argv[2] || !argv[3]) {
            puts("Usage: netcfg set ip|gw|mask|dns <a.b.c.d> [--save]");
            return 1;
        }
        uint8_t ip[4];
        if (parse_ipv4(argv[3], ip) != 0) {
            puts("netcfg set: invalid IPv4 value");
            return 1;
        }
        if (strcmp(argv[2], "ip") == 0) memcpy(cfg.local_ip, ip, 4);
        else if (strcmp(argv[2], "gw") == 0) memcpy(cfg.gateway_ip, ip, 4);
        else if (strcmp(argv[2], "mask") == 0) memcpy(cfg.netmask, ip, 4);
        else if (strcmp(argv[2], "dns") == 0) memcpy(cfg.dns_ip, ip, 4);
        else {
            puts("netcfg set: unknown key (ip|gw|mask|dns)");
            return 1;
        }
        if (eyn_sys_netcfg_set(&cfg) != 0) {
            puts("netcfg set: kernel rejected config");
            return 1;
        }
        if (argc >= 5 && argv[4] && strcmp(argv[4], "--save") == 0) {
            if (save_cfg_to_path("/config/net.cfg", &cfg) != 0) {
                puts("netcfg: updated, save failed");
                return 1;
            }
            puts("netcfg: updated and saved to /config/net.cfg");
            return 0;
        }
        puts("netcfg: updated");
        return 0;
    }

    if (strcmp(sub, "save") == 0) {
        const char* path = (argc >= 3 && argv[2] && argv[2][0]) ? argv[2] : "/config/net.cfg";
        if (save_cfg_to_path(path, &cfg) != 0) {
            puts("netcfg save: failed");
            return 1;
        }
        printf("netcfg: saved to %s\n", path);
        return 0;
    }

    if (strcmp(sub, "load") == 0) {
        const char* path = (argc >= 3 && argv[2] && argv[2][0]) ? argv[2] : "/config/net.cfg";
        if (load_cfg_from_path(path, &cfg) != 0) {
            puts("netcfg load: failed");
            return 1;
        }
        if (eyn_sys_netcfg_set(&cfg) != 0) {
            puts("netcfg load: kernel rejected config");
            return 1;
        }
        printf("netcfg: loaded from %s\n", path);
        return 0;
    }

    puts("Usage: netcfg show | netcfg verify | netcfg route <dst_ip> | netcfg defaults [--save] | netcfg set ip|gw|mask|dns <a.b.c.d> [--save] | netcfg save [path] | netcfg load [path]");
        return 1;

    return 0;
}
