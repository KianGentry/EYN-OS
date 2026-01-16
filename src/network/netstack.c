#include <network/netstack.h>

#include <drivers/e1000.h>
#include <string.h>
#include <vga.h>
#include <system.h>
#include <utilities/tile_manager.h>
#include <utilities/util.h>
#include <watchdog.h>
#include <misc/sched.h>

// The shell command blocks the normal input pump, so we must poll Ctrl-C
// ourselves while listening.
extern void poll_keyboard_for_ctrl_c(void);

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

typedef struct __attribute__((packed)) icmp_echo_hdr {
    uint8 type;
    uint8 code;
    uint16 checksum_be;
    uint16 id_be;
    uint16 seq_be;
} icmp_echo_hdr;

typedef struct arp_cache_entry {
    uint8 ip[4];
    uint8 mac[6];
    uint8 valid;
} arp_cache_entry;

typedef struct udp_rx_slot {
    uint8 valid;
    net_udp_rx_packet pkt;
} udp_rx_slot;

typedef struct icmp_echo_reply_slot {
    uint8 valid;
    uint8 src_ip[4];
    uint16 id;
    uint16 seq;
    uint32 payload_len;
} icmp_echo_reply_slot;

static struct {
    net_config cfg;
    int inited;
    uint8 mac[6];
    const netdev* dev;
    arp_cache_entry arp_cache[4];

    udp_rx_slot udp_rxq[8];
    net_udp_stats udp_stats;

    icmp_echo_reply_slot icmp_rxq[4];
    net_icmp_stats icmp_stats;
} g_net = {
    .cfg = {
        .local_ip = {10, 0, 2, 15},
        .gateway_ip = {10, 0, 2, 2},
        .netmask = {255, 255, 255, 0},
        .dns_ip = {10, 0, 2, 3},
    },
};

void net_config_get(net_config* out)
{
    if (!out) return;
    *out = g_net.cfg;
}

int net_config_set(const net_config* in)
{
    if (!in) return -1;
    g_net.cfg = *in;
    return 0;
}

void net_config_set_defaults(void)
{
    g_net.cfg.local_ip[0] = 10; g_net.cfg.local_ip[1] = 0; g_net.cfg.local_ip[2] = 2; g_net.cfg.local_ip[3] = 15;
    g_net.cfg.gateway_ip[0] = 10; g_net.cfg.gateway_ip[1] = 0; g_net.cfg.gateway_ip[2] = 2; g_net.cfg.gateway_ip[3] = 2;
    g_net.cfg.netmask[0] = 255; g_net.cfg.netmask[1] = 255; g_net.cfg.netmask[2] = 255; g_net.cfg.netmask[3] = 0;
    g_net.cfg.dns_ip[0] = 10; g_net.cfg.dns_ip[1] = 0; g_net.cfg.dns_ip[2] = 2; g_net.cfg.dns_ip[3] = 3;
}

void net_get_local_ip(uint8 out_ip[4])
{
    if (!out_ip) return;
    out_ip[0] = g_net.cfg.local_ip[0];
    out_ip[1] = g_net.cfg.local_ip[1];
    out_ip[2] = g_net.cfg.local_ip[2];
    out_ip[3] = g_net.cfg.local_ip[3];
}

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

static int arp_send_reply_silent(const uint8 target_mac[6],
                                const uint8 sender_ip[4], const uint8 sender_mac[6],
                                const uint8 target_ip[4]);

static int arp_resolve(const uint8 sender_ip[4], const uint8 target_ip[4], uint8 out_mac[6], int arp_spins);

static void udp_rxq_clear(void)
{
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        g_net.udp_rxq[i].valid = 0;
    }
    g_net.udp_stats.udp_rx_enqueued = 0;
    g_net.udp_stats.udp_rx_dropped = 0;
    g_net.udp_stats.udp_rx_truncated = 0;
}

static void icmp_rxq_clear(void)
{
    for (int i = 0; i < (int)(sizeof(g_net.icmp_rxq) / sizeof(g_net.icmp_rxq[0])); i++) {
        g_net.icmp_rxq[i].valid = 0;
    }
    g_net.icmp_stats.echo_req_rx = 0;
    g_net.icmp_stats.echo_rep_rx = 0;
    g_net.icmp_stats.echo_rep_tx = 0;
    g_net.icmp_stats.echo_rep_dropped = 0;
}

static void icmp_rxq_enqueue_reply(const uint8 src_ip[4], uint16 id, uint16 seq, uint32 payload_len)
{
    for (int i = 0; i < (int)(sizeof(g_net.icmp_rxq) / sizeof(g_net.icmp_rxq[0])); i++) {
        if (!g_net.icmp_rxq[i].valid) {
            g_net.icmp_rxq[i].valid = 1;
            for (int j = 0; j < 4; j++) g_net.icmp_rxq[i].src_ip[j] = src_ip[j];
            g_net.icmp_rxq[i].id = id;
            g_net.icmp_rxq[i].seq = seq;
            g_net.icmp_rxq[i].payload_len = payload_len;
            return;
        }
    }
    g_net.icmp_stats.echo_rep_dropped++;
}

static int icmp_rxq_dequeue_match(uint16 id, uint16 seq, uint8 out_src_ip[4], uint32* out_payload_len)
{
    for (int i = 0; i < (int)(sizeof(g_net.icmp_rxq) / sizeof(g_net.icmp_rxq[0])); i++) {
        if (g_net.icmp_rxq[i].valid && g_net.icmp_rxq[i].id == id && g_net.icmp_rxq[i].seq == seq) {
            if (out_src_ip) {
                for (int j = 0; j < 4; j++) out_src_ip[j] = g_net.icmp_rxq[i].src_ip[j];
            }
            if (out_payload_len) {
                *out_payload_len = g_net.icmp_rxq[i].payload_len;
            }
            g_net.icmp_rxq[i].valid = 0;
            return 1;
        }
    }
    return 0;
}

static int udp_rxq_enqueue(const uint8 src_ip[4], uint16 src_port,
                           const uint8 dst_ip[4], uint16 dst_port,
                           const uint8* payload, uint32 payload_len)
{
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        if (!g_net.udp_rxq[i].valid) {
            udp_rx_slot* s = &g_net.udp_rxq[i];
            s->valid = 1;
            for (int j = 0; j < 4; j++) s->pkt.src_ip[j] = src_ip[j];
            for (int j = 0; j < 4; j++) s->pkt.dst_ip[j] = dst_ip[j];
            s->pkt.src_port = src_port;
            s->pkt.dst_port = dst_port;

            uint32 copy_len = payload_len;
            if (copy_len > NET_UDP_MAX_PAYLOAD) {
                copy_len = NET_UDP_MAX_PAYLOAD;
                g_net.udp_stats.udp_rx_truncated++;
            }
            s->pkt.payload_len = copy_len;
            if (copy_len != 0u) {
                memcpy(s->pkt.payload, payload, copy_len);
            }
            g_net.udp_stats.udp_rx_enqueued++;
            return 0;
        }
    }

    g_net.udp_stats.udp_rx_dropped++;
    return -1;
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
    udp_rxq_clear();
    icmp_rxq_clear();
    g_net.inited = 1;
    return 0;
}

int net_is_inited(void)
{
    return g_net.inited ? 1 : 0;
}

net_udp_stats net_udp_get_stats(void)
{
    return g_net.udp_stats;
}

net_icmp_stats net_icmp_get_stats(void)
{
    return g_net.icmp_stats;
}

int net_get_mac(uint8 out_mac[6])
{
    if (!out_mac) return -1;
    if (!g_net.inited) return -2;
    for (int i = 0; i < 6; i++) out_mac[i] = g_net.mac[i];
    return 0;
}

uint32 net_get_arp_cache(net_arp_entry* out, uint32 out_cap)
{
    if (!out || out_cap == 0u) return 0;
    uint32 written = 0;
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (written >= out_cap) break;
        out[written].valid = g_net.arp_cache[i].valid;
        for (int j = 0; j < 4; j++) out[written].ip[j] = g_net.arp_cache[i].ip[j];
        for (int j = 0; j < 6; j++) out[written].mac[j] = g_net.arp_cache[i].mac[j];
        written++;
    }
    return written;
}

uint32 net_udp_queue_total(void)
{
    if (!g_net.inited) return 0;
    uint32 count = 0;
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        if (g_net.udp_rxq[i].valid) count++;
    }
    return count;
}

static int icmp_send_echo_reply_direct(const uint8 dst_mac[6],
                                      const uint8 local_ip[4], const uint8 dst_ip[4],
                                      uint16 id, uint16 seq,
                                      const uint8* payload, uint32 payload_len)
{
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
    ip->proto = 1;
    for (int i = 0; i < 4; i++) ip->src[i] = local_ip[i];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];

    uint16 ip_total_len = (uint16)(sizeof(ipv4_hdr) + sizeof(icmp_echo_hdr) + payload_len);
    ip->total_len_be = be16(ip_total_len);
    static uint16 ip_id = 0x9000;
    ip->id_be = be16(ip_id++);
    ip->flags_frag_off_be = be16(0u);
    ip->hdr_checksum_be = 0;
    ip->hdr_checksum_be = be16(ipv4_checksum16(ip, (uint32)sizeof(*ip)));
    off += (uint32)sizeof(ipv4_hdr);

    icmp_echo_hdr* ic = (icmp_echo_hdr*)(frame + off);
    ic->type = 0;
    ic->code = 0;
    ic->checksum_be = 0;
    ic->id_be = be16(id);
    ic->seq_be = be16(seq);
    off += (uint32)sizeof(icmp_echo_hdr);

    if (payload_len != 0u) {
        memcpy(frame + off, payload, payload_len);
        off += payload_len;
    }

    uint16 csum = ipv4_checksum16((const void*)(frame + sizeof(eth_hdr) + sizeof(ipv4_hdr)),
                                 (uint32)sizeof(icmp_echo_hdr) + payload_len);
    ic->checksum_be = be16(csum);

    if (off < 60u) {
        memset(frame + off, 0, 60u - off);
        off = 60u;
    }
    return g_net.dev->send_frame(frame, off);
}

static int icmp_send_echo_request(const uint8 local_ip[4], const uint8 dst_ip[4], const uint8 dst_mac[6],
                                 uint16 id, uint16 seq)
{
    // Small fixed payload (helps validate checksums/length).
    uint8 payload[32];
    for (uint32 i = 0; i < (uint32)sizeof(payload); i++) payload[i] = (uint8)('A' + (i % 26u));

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
    ip->proto = 1;
    for (int i = 0; i < 4; i++) ip->src[i] = local_ip[i];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];

    uint16 ip_total_len = (uint16)(sizeof(ipv4_hdr) + sizeof(icmp_echo_hdr) + sizeof(payload));
    ip->total_len_be = be16(ip_total_len);
    static uint16 ip_id = 0x1000;
    ip->id_be = be16(ip_id++);
    ip->flags_frag_off_be = be16(0u);
    ip->hdr_checksum_be = 0;
    ip->hdr_checksum_be = be16(ipv4_checksum16(ip, (uint32)sizeof(*ip)));
    off += (uint32)sizeof(ipv4_hdr);

    icmp_echo_hdr* ic = (icmp_echo_hdr*)(frame + off);
    ic->type = 8;
    ic->code = 0;
    ic->checksum_be = 0;
    ic->id_be = be16(id);
    ic->seq_be = be16(seq);
    off += (uint32)sizeof(icmp_echo_hdr);

    memcpy(frame + off, payload, (uint32)sizeof(payload));
    off += (uint32)sizeof(payload);

    uint16 csum = ipv4_checksum16((const void*)(frame + sizeof(eth_hdr) + sizeof(ipv4_hdr)),
                                 (uint32)sizeof(icmp_echo_hdr) + (uint32)sizeof(payload));
    ic->checksum_be = be16(csum);

    if (off < 60u) {
        memset(frame + off, 0, 60u - off);
        off = 60u;
    }
    return g_net.dev->send_frame(frame, off);
}

uint32 net_udp_queue_count(uint16 local_port)
{
    if (!g_net.inited) return 0;
    if (local_port == 0) return 0;
    uint32 count = 0;
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        if (g_net.udp_rxq[i].valid && g_net.udp_rxq[i].pkt.dst_port == local_port) {
            count++;
        }
    }
    return count;
}

uint32 net_udp_queue_clear(uint16 local_port)
{
    if (!g_net.inited) return 0;
    if (local_port == 0) return 0;
    uint32 cleared = 0;
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        if (g_net.udp_rxq[i].valid && g_net.udp_rxq[i].pkt.dst_port == local_port) {
            g_net.udp_rxq[i].valid = 0;
            cleared++;
        }
    }
    return cleared;
}

int net_udp_recv(uint16 local_port, net_udp_rx_packet* out)
{
    if (!g_net.inited || !g_net.dev) return -1;
    if (!out) return -2;
    if (local_port == 0) return -3;

    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        if (g_net.udp_rxq[i].valid && g_net.udp_rxq[i].pkt.dst_port == local_port) {
            *out = g_net.udp_rxq[i].pkt;
            g_net.udp_rxq[i].valid = 0;
            return 1;
        }
    }
    return 0;
}

int net_poll(const uint8 local_ip[4], uint32 budget_frames)
{
    if (!local_ip) return -1;
    if (!g_net.inited || !g_net.dev) {
        if (net_init_e1000_default() != 0) return -2;
    }

    if (budget_frames == 0u) budget_frames = 64u;

    uint8 frame[1600];
    uint32 len = 0;
    uint32 processed = 0;

    for (uint32 i = 0; i < budget_frames; i++) {
        int got = g_net.dev->rx_poll_frame(frame, (uint32)sizeof(frame), &len, 1);
        if (got < 0) return -3;
        if (got == 0) break;
        processed++;

        // ARP: learn and respond to requests for our IP.
        if (len >= (uint32)(sizeof(eth_hdr) + sizeof(arp_pkt))) {
            eth_hdr* eh0 = (eth_hdr*)frame;
            if (be16(eh0->ethertype_be) == 0x0806u) {
                arp_pkt* a0 = (arp_pkt*)(frame + sizeof(eth_hdr));
                uint16 oper0 = be16(a0->oper_be);
                if (oper0 == 1u) {
                    arp_cache_update(a0->spa, a0->sha);
                    if (ipv4_eq(a0->tpa, local_ip)) {
                        (void)arp_send_reply_silent(a0->sha, local_ip, g_net.mac, a0->spa);
                    }
                } else if (oper0 == 2u) {
                    arp_cache_update(a0->spa, a0->sha);
                }
                continue;
            }
        }

        // IPv4: basic handling (UDP enqueue + ICMP echo).
        if (len < (uint32)(sizeof(eth_hdr) + sizeof(ipv4_hdr))) continue;
        eth_hdr* eh = (eth_hdr*)frame;
        if (be16(eh->ethertype_be) != 0x0800u) continue;

        ipv4_hdr* ip = (ipv4_hdr*)(frame + sizeof(eth_hdr));
        uint8 ver = (uint8)(ip->ver_ihl >> 4);
        uint8 ihl_words = (uint8)(ip->ver_ihl & 0x0Fu);
        if (ver != 4 || ihl_words < 5) continue;

        uint32 ip_hdr_len = (uint32)ihl_words * 4u;
        if (!ipv4_eq(ip->dst, local_ip)) continue;

        // Opportunistically learn IP->MAC mapping from IPv4 frames.
        arp_cache_update(ip->src, eh->src);

        if (ip->proto == 1) {
            // ICMP echo support for debugging (ping).
            if (len < (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(icmp_echo_hdr)) continue;

            icmp_echo_hdr* ic = (icmp_echo_hdr*)(frame + sizeof(eth_hdr) + ip_hdr_len);
            uint16 id = be16(ic->id_be);
            uint16 seq = be16(ic->seq_be);

            // Use IPv4 total length to compute payload length (bounds-checked against frame len).
            uint32 ip_total_len = (uint32)be16(ip->total_len_be);
            if (ip_total_len < ip_hdr_len + (uint32)sizeof(icmp_echo_hdr)) continue;
            uint32 icmp_total_len = ip_total_len - ip_hdr_len;
            uint32 icmp_payload_len = icmp_total_len - (uint32)sizeof(icmp_echo_hdr);
            uint32 icmp_payload_off = (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(icmp_echo_hdr);
            if (icmp_payload_off > len) continue;
            if (icmp_payload_off + icmp_payload_len > len) {
                if (len > icmp_payload_off) icmp_payload_len = len - icmp_payload_off;
                else icmp_payload_len = 0;
            }

            if (ic->type == 8 && ic->code == 0) {
                // Echo request to our IP: reply directly back to sender MAC.
                g_net.icmp_stats.echo_req_rx++;
                int rc = icmp_send_echo_reply_direct(eh->src, ip->dst, ip->src, id, seq,
                                                     frame + icmp_payload_off, icmp_payload_len);
                if (rc == 0) g_net.icmp_stats.echo_rep_tx++;
            } else if (ic->type == 0 && ic->code == 0) {
                // Echo reply: enqueue for ping.
                g_net.icmp_stats.echo_rep_rx++;
                icmp_rxq_enqueue_reply(ip->src, id, seq, icmp_payload_len);
            }
            continue;
        }

        // UDP receive enqueue.
        if (ip->proto != 17) continue;
        if (len < (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(udp_hdr)) continue;

        udp_hdr* udp = (udp_hdr*)(frame + sizeof(eth_hdr) + ip_hdr_len);
        uint16 src_port = be16(udp->src_port_be);
        uint16 dst_port = be16(udp->dst_port_be);
        if (dst_port == 0 || src_port == 0) continue;

        uint32 udp_total_len = (uint32)be16(udp->len_be);
        if (udp_total_len < (uint32)sizeof(udp_hdr)) continue;

        uint32 payload_off = (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(udp_hdr);
        uint32 payload_len = udp_total_len - (uint32)sizeof(udp_hdr);
        if (payload_off > len) continue;
        if (payload_off + payload_len > len) {
            if (len > payload_off) payload_len = len - payload_off;
            else payload_len = 0;
        }

        (void)udp_rxq_enqueue(ip->src, src_port, ip->dst, dst_port, frame + payload_off, payload_len);
    }

    return (int)processed;
}

int net_icmp_ping(const uint8 local_ip[4], const uint8 dst_ip[4], int count, int timeout_spins)
{
    if (!local_ip || !dst_ip) return -1;
    if (count <= 0) count = 4;
    if (timeout_spins <= 0) timeout_spins = 8000000;

    if (net_init_e1000_default() != 0) return -2;

    // Stable ID so we can match replies.
    const uint16 id = 0xE100;
    int replies = 0;

    g_user_interrupt = 0;

    for (int i = 0; i < count; i++) {
        watchdog_kick("net-ping");
        poll_keyboard_for_ctrl_c();
        if (g_user_interrupt) {
            g_user_interrupt = 0;
            break;
        }

        uint16 seq = (uint16)(i + 1);

        uint8 dst_mac[6];
        int rc = arp_resolve(local_ip, dst_ip, dst_mac, 800000);
        if (rc != 0) {
            printf("%cPING%c ", 255, 255, 255, 255, 255, 255);
            print_ipv4_bytes(dst_ip);
            printf(": arp failed (%d)\n", rc);
            continue;
        }

        uint32 send_tick = sched_get_tick_count();
        rc = icmp_send_echo_request(local_ip, dst_ip, dst_mac, id, seq);
        if (rc != 0) {
            printf("%cPING%c ", 255, 255, 255, 255, 255, 255);
            print_ipv4_bytes(dst_ip);
            printf(": tx failed (%d)\n", rc);
            continue;
        }

        int got_reply = 0;
        for (int spin = 0; spin < timeout_spins; spin++) {
            if ((spin & 0x3FFF) == 0) {
                watchdog_kick("net-ping-wait");
                poll_keyboard_for_ctrl_c();
                if (g_user_interrupt) {
                    g_user_interrupt = 0;
                    return replies;
                }
            }
            (void)net_poll(local_ip, 64);

            uint8 src_ip[4];
            uint32 payload_len = 0;
            int found = icmp_rxq_dequeue_match(id, seq, src_ip, &payload_len);
            if (found == 1) {
                uint32 now_tick = sched_get_tick_count();
                uint32 rtt_ticks = now_tick - send_tick;
                printf("%cPING%c ", 0, 255, 0, 255, 255, 255);
                print_ipv4_bytes(dst_ip);
                printf(": seq=%d len=%d time=%d ticks\n", (int)seq, (int)payload_len, (int)rtt_ticks);
                replies++;
                got_reply = 1;
                break;
            }
        }

        if (!got_reply) {
            printf("%cPING%c ", 255, 255, 255, 255, 255, 255);
            print_ipv4_bytes(dst_ip);
            printf(": seq=%d timeout\n", (int)seq);
        }

        // Small delay between pings.
        sleep(1);
    }

    return replies;
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
    if (!src_ip || !dst_ip) return -1;
    if (payload_len != 0u && !payload) return -1;
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

    g_user_interrupt = 0;

    // The shell command blocks the normal input pump, so we must poll Ctrl-C
    // ourselves while listening.

    const int unlimited_packets = (max_packets == 0);
    const int unlimited_spins = (spin_limit == 0);
    uint32 spin = 0;

    // UI refresh pacing: keep the tile display updating while we block.
    uint32 ui_ticks = 0;

    // Batch polling helps amortize overhead while still staying responsive.
    const uint32 poll_budget = 64u;

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

        spin++;
        int rc = net_poll(local_ip, poll_budget);
        if (rc < 0) return -4;
        if (rc > 0) did_work = 1;

        // Drain any queued UDP packets for our port.
        for (;;) {
            net_udp_rx_packet pkt;
            int gotp = net_udp_recv(local_port, &pkt);
            if (gotp < 0) return -5;
            if (gotp == 0) break;

            printf("%cUDP RX ", 0, 255, 0);
            print_ipv4_bytes(pkt.src_ip);
            printf(":%d -> ", (int)pkt.src_port);
            print_ipv4_bytes(pkt.dst_ip);
            printf(":%d (%d bytes): ", (int)local_port, (int)pkt.payload_len);

            char ascii[260];
            uint32 show_len = pkt.payload_len;
            if (show_len > 256u) show_len = 256u;
            for (uint32 i = 0; i < show_len; i++) {
                char c = (char)pkt.payload[i];
                if (c < 32 || c > 126) c = '.';
                ascii[i] = c;
            }
            ascii[show_len] = 0;
            printf("%s", ascii);
            if (pkt.payload_len > show_len) {
                printf("...");
            }
            printf("\n");
            if (tile_is_tiling_active()) {
                tile_render_once();
            }
            printed++;
            did_work = 1;

            if (!unlimited_packets && printed >= max_packets) break;
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

int net_udp_echo(const uint8 local_ip[4], uint16 local_port, int max_packets, int spin_limit)
{
    if (!local_ip) return -1;
    if (local_port == 0) return -2;
    // max_packets == 0 => unlimited
    // spin_limit  == 0 => unlimited
    if (max_packets < 0) max_packets = 1;
    if (spin_limit < 0) spin_limit = 12000000;

    if (net_init_e1000_default() != 0) return -3;

    int printed = 0;

    g_user_interrupt = 0;

    // The shell command blocks the normal input pump, so we must poll Ctrl-C
    // ourselves while listening.

    const int unlimited_packets = (max_packets == 0);
    const int unlimited_spins = (spin_limit == 0);
    uint32 spin = 0;

    // UI refresh pacing: keep the tile display updating while we block.
    uint32 ui_ticks = 0;

    const uint32 poll_budget = 64u;

    for (;;) {
        watchdog_kick("net-udp-echo");

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

        spin++;
        int rc = net_poll(local_ip, poll_budget);
        if (rc < 0) return -4;
        if (rc > 0) did_work = 1;

        // Drain any queued UDP packets for our port, print and echo.
        for (;;) {
            net_udp_rx_packet pkt;
            int gotp = net_udp_recv(local_port, &pkt);
            if (gotp < 0) return -5;
            if (gotp == 0) break;

            printf("%cUDP RX ", 0, 255, 0);
            print_ipv4_bytes(pkt.src_ip);
            printf(":%d -> ", (int)pkt.src_port);
            print_ipv4_bytes(pkt.dst_ip);
            printf(":%d (%d bytes): ", (int)local_port, (int)pkt.payload_len);

            char ascii[260];
            uint32 show_len = pkt.payload_len;
            if (show_len > 256u) show_len = 256u;
            for (uint32 i = 0; i < show_len; i++) {
                char c = (char)pkt.payload[i];
                if (c < 32 || c > 126) c = '.';
                ascii[i] = c;
            }
            ascii[show_len] = 0;
            printf("%s", ascii);
            if (pkt.payload_len > show_len) {
                printf("...");
            }
            printf("\n");
            if (tile_is_tiling_active()) {
                tile_render_once();
            }

            // Echo payload back to sender. Note payload was already truncated to NET_UDP_MAX_PAYLOAD.
            if (pkt.payload_len == 0u) {
                static const uint8 empty_msg[1] = { '\n' };
                (void)net_udp_send(local_ip, local_port, pkt.src_ip, pkt.src_port, empty_msg, 1u, 800000);
            } else {
                (void)net_udp_send(local_ip, local_port, pkt.src_ip, pkt.src_port, pkt.payload, pkt.payload_len, 800000);
            }

            printed++;
            did_work = 1;

            if (!unlimited_packets && printed >= max_packets) break;
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
