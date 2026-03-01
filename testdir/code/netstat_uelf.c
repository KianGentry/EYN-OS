#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Show network status.", "netstat");

static void print_mac(const uint8_t mac[6]) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

static void print_ip(const uint8_t ip[4]) {
    printf("%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int inited = eyn_sys_net_is_inited();
    printf("netstack: %s\n", inited ? "initialized" : "not initialized");

    uint8_t mac[6];
    if (eyn_sys_net_get_mac(mac) == 0) {
        printf("mac: ");
        print_mac(mac);
        putchar('\n');
    } else {
        puts("mac: unavailable");
    }

    eyn_net_udp_stats_t udp;
    if (eyn_sys_net_get_udp_stats(&udp) == 0) {
        printf("udp: rx_enqueued=%u rx_dropped=%u rx_truncated=%u rx_bad_csum=%u tx_checksums=%u\n",
               (unsigned)udp.udp_rx_enqueued,
               (unsigned)udp.udp_rx_dropped,
               (unsigned)udp.udp_rx_truncated,
               (unsigned)udp.udp_rx_bad_checksum,
               (unsigned)udp.udp_tx_checksums);
    } else {
        puts("udp: unavailable");
    }

    eyn_net_ip_stats_t ip;
    if (eyn_sys_net_get_ip_stats(&ip) == 0) {
        printf("ip: rx_fragments=%u rx_frag_dropped=%u\n",
               (unsigned)ip.ipv4_rx_fragments,
               (unsigned)ip.ipv4_rx_frag_dropped);
    } else {
        puts("ip: unavailable");
    }

    eyn_net_arp_entry_t arp[32];
    int arp_count = eyn_sys_net_get_arp(arp, 32);
    if (arp_count < 0) {
        puts("arp: unavailable");
    } else {
        printf("arp cache (%d):\n", arp_count);
        for (int i = 0; i < arp_count; ++i) {
            printf("  ");
            print_ip(arp[i].ip);
            printf(" -> ");
            print_mac(arp[i].mac);
            putchar('\n');
        }
    }

    eyn_net_socket_info_t socks[32];
    int sock_count = eyn_sys_net_get_sockets(socks, 32);
    if (sock_count < 0) {
        puts("sockets: unavailable");
        return 1;
    }
    printf("sockets (%d):\n", sock_count);
    for (int i = 0; i < sock_count; ++i) {
        printf("  port=%u bound=%u queued=%u dropped=%u\n",
               (unsigned)socks[i].port,
               (unsigned)socks[i].bound,
               (unsigned)socks[i].queued,
               (unsigned)socks[i].dropped);
    }
    return 0;
}
