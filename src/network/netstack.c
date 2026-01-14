#include <network/netstack.h>

#include <drivers/e1000.h>
#include <string.h>
#include <vga.h>
#include <system.h>
#include <utilities/tile_manager.h>
#include <watchdog.h>

typedef struct __attribute__((packed)) eth_hdr {
    uint8 dst[6];
    uint8 src[6];
    uint16 ethertype_be;
} eth_hdr;

typedef struct __attribute__((packed)) arp_pkt {
    uint16 htype_be;
    uint16 ptype_be;
    uint8 hlen;
    uint8 plen;
    uint16 oper_be;
    uint8 sha[6];
    uint8 spa[4];
    uint8 tha[6];
    uint8 tpa[4];
} arp_pkt;

typedef struct __attribute__((packed)) ipv4_hdr {
    uint8 ver_ihl;
    uint8 dscp_ecn;
    uint16 total_len_be;
    uint16 id_be;
    uint16 flags_frag_off_be;
    uint8 ttl;
    uint8 proto;
    uint16 hdr_checksum_be;
    uint8 src[4];
    uint8 dst[4];
} ipv4_hdr;

typedef struct __attribute__((packed)) udp_hdr {
    uint16 src_port_be;
    uint16 dst_port_be;
    uint16 len_be;
    uint16 checksum_be;
} udp_hdr;

typedef struct arp_cache_entry {
    uint8 ip[4];
    uint8 mac[6];
    uint8 valid;
} arp_cache_entry;

static struct {
    int inited;
    uint8 mac[6];
    const netdev* dev;
    arp_cache_entry arp_cache[4];
} g_net;

static uint16 be16(uint16 x)
{
    return (uint16)((x >> 8) | (x << 8));
}

static int ipv4_eq(const uint8 a[4], const uint8 b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void print_ipv4_bytes(const uint8 ip[4])
{
    printf("%d.%d.%d.%d", (int)ip[0], (int)ip[1], (int)ip[2], (int)ip[3]);
}

static uint16 ipv4_checksum16(const void* data, uint32 len)
{
    const uint8* b = (const uint8*)data;
    uint32 sum = 0;
    for (uint32 i = 0; i + 1 < len; i += 2) {
        sum += ((uint16)b[i] << 8) | (uint16)b[i + 1];
    }
    if (len & 1u) {
        sum += ((uint16)b[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16)(~sum);
}

static int arp_cache_lookup(const uint8 ip[4], uint8 out_mac[6])
{
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (g_net.arp_cache[i].valid && ipv4_eq(g_net.arp_cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) out_mac[j] = g_net.arp_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

static void arp_cache_update(const uint8 ip[4], const uint8 mac[6])
{
    // Update in place if present.
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (g_net.arp_cache[i].valid && ipv4_eq(g_net.arp_cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) g_net.arp_cache[i].mac[j] = mac[j];
            return;
        }
    }

    // Insert into first free slot.
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (!g_net.arp_cache[i].valid) {
            for (int j = 0; j < 4; j++) g_net.arp_cache[i].ip[j] = ip[j];
            for (int j = 0; j < 6; j++) g_net.arp_cache[i].mac[j] = mac[j];
            g_net.arp_cache[i].valid = 1;
            return;
        }
    }

    // Cache full: overwrite slot 0.
    for (int j = 0; j < 4; j++) g_net.arp_cache[0].ip[j] = ip[j];
    for (int j = 0; j < 6; j++) g_net.arp_cache[0].mac[j] = mac[j];
    g_net.arp_cache[0].valid = 1;
}

int net_init_e1000_default(void)
{
    static netdev e1000_dev;
    e1000_dev.send_frame = e1000_send_frame;
    e1000_dev.rx_poll_frame = e1000_rx_poll_frame;
    e1000_dev.get_mac = e1000_get_mac;

    if (e1000_init() != 0) return -10;
    return net_init(&e1000_dev);
}

int net_init(const netdev* dev)
{
    if (g_net.inited) return 0;
    if (!dev || !dev->send_frame || !dev->rx_poll_frame || !dev->get_mac) return -1;

    if (dev->get_mac(g_net.mac) != 0) return -2;

    g_net.dev = dev;
    g_net.inited = 1;
    return 0;
}

static int arp_send_request_silent(const uint8 sender_ip[4], const uint8 target_ip[4])
{
    uint8 frame[64];
    memset(frame, 0, sizeof(frame));

    // Ethernet header
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    for (int i = 0; i < 6; i++) frame[6 + i] = g_net.mac[i];
    frame[12] = 0x08;
    frame[13] = 0x06;

    arp_pkt* a = (arp_pkt*)(frame + sizeof(eth_hdr));
    a->htype_be = be16(1u);
    a->ptype_be = be16(0x0800u);
    a->hlen = 6;
    a->plen = 4;
    a->oper_be = be16(1u);
    for (int i = 0; i < 6; i++) a->sha[i] = g_net.mac[i];
    for (int i = 0; i < 4; i++) a->spa[i] = sender_ip[i];
    for (int i = 0; i < 6; i++) a->tha[i] = 0;
    for (int i = 0; i < 4; i++) a->tpa[i] = target_ip[i];

    uint32 total_len = (uint32)(sizeof(eth_hdr) + sizeof(arp_pkt));
    if (total_len < 60u) total_len = 60u;

    return g_net.dev->send_frame(frame, total_len);
}

static int arp_poll_reply_mac(const uint8 target_ip[4], uint8 out_mac[6], int spin_limit)
{
    if (spin_limit <= 0) spin_limit = 12000000;

    uint8 frame[1600];
    uint32 len = 0;

    for (int spin = 0; spin < spin_limit; spin++) {
        if ((spin & 0x3FFF) == 0) {
            watchdog_kick("net-arp-wait");
        }
        int got = g_net.dev->rx_poll_frame(frame, (uint32)sizeof(frame), &len, 1);
        if (got < 0) return -2;
        if (got == 0) continue;

        if (len >= (uint32)(sizeof(eth_hdr) + sizeof(arp_pkt))) {
            eth_hdr* eh = (eth_hdr*)frame;
            uint16 et = be16(eh->ethertype_be);
            if (et == 0x0806u) {
                arp_pkt* a = (arp_pkt*)(frame + sizeof(eth_hdr));
                uint16 oper = be16(a->oper_be);
                if (oper == 2u) {
                    arp_cache_update(a->spa, a->sha);
                    if (ipv4_eq(a->spa, target_ip)) {
                        for (int i = 0; i < 6; i++) out_mac[i] = a->sha[i];
                        return 0;
                    }
                }
            }
        }
    }

    return -3;
}

static int arp_resolve(const uint8 sender_ip[4], const uint8 target_ip[4], uint8 out_mac[6], int arp_spins)
{
    if (arp_cache_lookup(target_ip, out_mac)) return 0;

    int rc = arp_send_request_silent(sender_ip, target_ip);
    if (rc != 0) return -100 + rc;

    rc = arp_poll_reply_mac(target_ip, out_mac, arp_spins);
    if (rc != 0) return -200 + rc;

    arp_cache_update(target_ip, out_mac);
    return 0;
}

static int arp_send_reply_silent(const uint8 target_mac[6],
                                const uint8 sender_ip[4], const uint8 sender_mac[6],
                                const uint8 target_ip[4])
{
    uint8 frame[64];
    memset(frame, 0, sizeof(frame));

    // Ethernet header
    for (int i = 0; i < 6; i++) frame[i] = target_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = sender_mac[i];
    frame[12] = 0x08;
    frame[13] = 0x06;

    arp_pkt* a = (arp_pkt*)(frame + sizeof(eth_hdr));
    a->htype_be = be16(1u);
    a->ptype_be = be16(0x0800u);
    a->hlen = 6;
    a->plen = 4;
    a->oper_be = be16(2u);

    // Reply: "sender" is us; "target" is requester
    for (int i = 0; i < 6; i++) a->sha[i] = sender_mac[i];
    for (int i = 0; i < 4; i++) a->spa[i] = sender_ip[i];
    for (int i = 0; i < 6; i++) a->tha[i] = target_mac[i];
    for (int i = 0; i < 4; i++) a->tpa[i] = target_ip[i];

    uint32 total_len = (uint32)(sizeof(eth_hdr) + sizeof(arp_pkt));
    if (total_len < 60u) total_len = 60u;
    return g_net.dev->send_frame(frame, total_len);
}

int net_arp_test_send(const uint8 sender_ip[4], const uint8 target_ip[4], int rx_spins)
{
    if (!sender_ip || !target_ip) return -1;
    if (net_init_e1000_default() != 0) return -2;

    int rc = arp_send_request_silent(sender_ip, target_ip);
    if (rc != 0) return rc;

    printf("ARP who-has ");
    print_ipv4_bytes(target_ip);
    printf(" tell ");
    print_ipv4_bytes(sender_ip);
    printf(" (TX ok)\n");

    uint8 mac[6];
    rc = arp_poll_reply_mac(target_ip, mac, rx_spins);
    if (rc == 0) {
        printf("%cARP reply received.\n", 0, 255, 0);
        return 0;
    }

    return rc;
}

int net_udp_send(const uint8 src_ip[4], uint16 src_port,
                 const uint8 dst_ip[4], uint16 dst_port,
                 const uint8* payload, uint32 payload_len,
                 int arp_spins)
{
    if (!src_ip || !dst_ip || !payload) return -1;
    if (payload_len > 1400u) return -2;
    if (src_port == 0 || dst_port == 0) return -3;

    if (net_init_e1000_default() != 0) return -4;

    uint8 dst_mac[6];
    int rc = arp_resolve(src_ip, dst_ip, dst_mac, arp_spins);
    if (rc != 0) return rc;

    uint8 frame[1600];
    uint32 off = 0;

    eth_hdr* eh = (eth_hdr*)(frame + off);
    for (int i = 0; i < 6; i++) eh->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eh->src[i] = g_net.mac[i];
    eh->ethertype_be = be16(0x0800u);
    off += (uint32)sizeof(eth_hdr);

    ipv4_hdr* ip = (ipv4_hdr*)(frame + off);
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = 17;
    for (int i = 0; i < 4; i++) ip->src[i] = src_ip[i];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];

    uint16 ip_total_len = (uint16)(sizeof(ipv4_hdr) + sizeof(udp_hdr) + payload_len);
    ip->total_len_be = be16(ip_total_len);
    static uint16 ip_id = 1;
    ip->id_be = be16(ip_id++);
    ip->flags_frag_off_be = be16(0u);
    ip->hdr_checksum_be = 0;
    ip->hdr_checksum_be = be16(ipv4_checksum16(ip, (uint32)sizeof(*ip)));
    off += (uint32)sizeof(ipv4_hdr);

    udp_hdr* udp = (udp_hdr*)(frame + off);
    udp->src_port_be = be16(src_port);
    udp->dst_port_be = be16(dst_port);
    udp->len_be = be16((uint16)(sizeof(udp_hdr) + payload_len));
    udp->checksum_be = 0;
    off += (uint32)sizeof(udp_hdr);

    memcpy(frame + off, payload, payload_len);
    off += payload_len;

    if (off < 60u) {
        memset(frame + off, 0, 60u - off);
        off = 60u;
    }

    rc = g_net.dev->send_frame(frame, off);
    if (rc != 0) return -300 + rc;

    return 0;
}

int net_udp_listen(const uint8 local_ip[4], uint16 local_port, int max_packets, int spin_limit)
{
    if (!local_ip) return -1;
    if (local_port == 0) return -2;
    // max_packets == 0 => unlimited
    // spin_limit  == 0 => unlimited
    if (max_packets < 0) max_packets = 1;
    if (spin_limit < 0) spin_limit = 12000000;

    if (net_init_e1000_default() != 0) return -3;

    int printed = 0;
    uint8 frame[1600];
    uint32 len = 0;

    extern volatile int g_user_interrupt;
    g_user_interrupt = 0;

    // The shell command blocks the normal input pump, so we must poll Ctrl-C
    // ourselves while listening.
    extern void poll_keyboard_for_ctrl_c(void);

    const int unlimited_packets = (max_packets == 0);
    const int unlimited_spins = (spin_limit == 0);
    uint32 spin = 0;

    // UI refresh pacing: keep the tile display updating while we block.
    uint32 ui_ticks = 0;

    // Batch polling helps amortize overhead while still staying responsive.
    const int poll_batch = 64;

    for (;;) {
        watchdog_kick("net-udp-listen");

        // Keep UI rendering while shell is blocked.
        if (tile_is_tiling_active()) {
            if ((ui_ticks++ & 0x1F) == 0) {
                tile_render_once();
            }
        }

        // Detect Ctrl-C while the normal UI loop is blocked.
        poll_keyboard_for_ctrl_c();
        if (g_user_interrupt) {
            g_user_interrupt = 0;
            break;
        }
        if (!unlimited_packets && printed >= max_packets) break;
        if (!unlimited_spins && spin >= (uint32)spin_limit) break;

        int did_work = 0;

        for (int b = 0; b < poll_batch; b++) {
            if (!unlimited_packets && printed >= max_packets) break;
            if (!unlimited_spins && spin >= (uint32)spin_limit) break;
            poll_keyboard_for_ctrl_c();
            if (g_user_interrupt) break;

            spin++;
            int got = g_net.dev->rx_poll_frame(frame, (uint32)sizeof(frame), &len, 1);
            if (got < 0) return -4;
            if (got == 0) continue;
            did_work = 1;

            // Reply to ARP requests for our IP so QEMU's router can deliver inbound UDP.
            if (len >= (uint32)(sizeof(eth_hdr) + sizeof(arp_pkt))) {
                eth_hdr* eh0 = (eth_hdr*)frame;
                if (be16(eh0->ethertype_be) == 0x0806u) {
                    arp_pkt* a0 = (arp_pkt*)(frame + sizeof(eth_hdr));
                    uint16 oper0 = be16(a0->oper_be);
                    if (oper0 == 1u) {
                        // Cache the requester.
                        arp_cache_update(a0->spa, a0->sha);
                        if (ipv4_eq(a0->tpa, local_ip)) {
                            (void)arp_send_reply_silent(a0->sha, local_ip, g_net.mac, a0->spa);
                        }
                    } else if (oper0 == 2u) {
                        // Cache any replies we see too.
                        arp_cache_update(a0->spa, a0->sha);
                    }
                    continue;
                }
            }

            if (len < (uint32)(sizeof(eth_hdr) + sizeof(ipv4_hdr))) continue;

            eth_hdr* eh = (eth_hdr*)frame;
            if (be16(eh->ethertype_be) != 0x0800u) continue;

            ipv4_hdr* ip = (ipv4_hdr*)(frame + sizeof(eth_hdr));
            uint8 ver = (uint8)(ip->ver_ihl >> 4);
            uint8 ihl_words = (uint8)(ip->ver_ihl & 0x0Fu);
            if (ver != 4 || ihl_words < 5) continue;

            uint32 ip_hdr_len = (uint32)ihl_words * 4u;
            if (len < (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(udp_hdr)) continue;
            if (ip->proto != 17) continue; // UDP
            if (!ipv4_eq(ip->dst, local_ip)) continue;

            udp_hdr* udp = (udp_hdr*)(frame + sizeof(eth_hdr) + ip_hdr_len);
            uint16 dstp = be16(udp->dst_port_be);
            if (dstp != local_port) continue;

            uint32 udp_total_len = (uint32)be16(udp->len_be);
            if (udp_total_len < (uint32)sizeof(udp_hdr)) continue;

            uint32 payload_off = (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(udp_hdr);
            uint32 payload_len = udp_total_len - (uint32)sizeof(udp_hdr);
            if (payload_off > len) continue;
            if (payload_off + payload_len > len) {
                // Truncated RX buffer or inconsistent lengths; clamp.
                if (len > payload_off) payload_len = len - payload_off;
                else payload_len = 0;
            }

            printf("%cUDP RX ", 0, 255, 0);
            print_ipv4_bytes(ip->src);
            printf(":%d -> ", (int)be16(udp->src_port_be));
            print_ipv4_bytes(ip->dst);
            printf(":%d (%d bytes): ", (int)local_port, (int)payload_len);

            // Print payload as best-effort ASCII.
            // NOTE: Do NOT print byte-by-byte with printf("%c", ...): this kernel
            // uses "%c" as a color-prefix convention and expects RGB args.
            char ascii[260];
            uint32 show_len = payload_len;
            if (show_len > 256u) show_len = 256u;
            for (uint32 i = 0; i < show_len; i++) {
                char c = (char)frame[payload_off + i];
                if (c < 32 || c > 126) c = '.';
                ascii[i] = c;
            }
            ascii[show_len] = 0;
            printf("%s", ascii);
            if (payload_len > show_len) {
                printf("...");
            }
            printf("\n");
            if (tile_is_tiling_active()) {
                tile_render_once();
            }

            printed++;
        }

        // If we saw nothing, sleep briefly to keep the host/VM responsive.
        if (!did_work) {
            // Use a simple busy delay here (not HLT-based) because some shell
            // contexts may have interrupts temporarily disabled.
            sleep(1);
        }
    }

    return printed;
}
