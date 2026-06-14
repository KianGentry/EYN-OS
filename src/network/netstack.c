#include <network/netstack.h>

#include <drivers/e100.h>
#include <drivers/e1000.h>
#include <string.h>
#include <vga.h>
#include <system.h>
#include <utilities/tile_manager.h>
#include <utilities/util.h>
#include <watchdog.h>
#include <misc/sched.h>
#include <drivers/serial.h>
#include <fs/vfs.h>

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

typedef struct __attribute__((packed)) tcp_hdr {
    uint16 src_port_be;
    uint16 dst_port_be;
    uint32 seq_be;
    uint32 ack_be;
    uint8 data_off; // high 4 bits = data offset
    uint8 flags;
    uint16 window_be;
    uint16 checksum_be;
    uint16 urg_ptr_be;
} tcp_hdr;

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
    uint32 last_seen_tick;
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

typedef enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT
} tcp_state;

typedef struct tcp_conn {
    tcp_state state;
    uint8 local_ip[4];
    uint16 local_port;
    uint8 remote_ip[4];
    uint16 remote_port;
    uint32 seq;
    uint32 ack;
    uint16 listen_port;
    uint8 listening;
    int retx_timer_id;
    uint8 retx_active;
    uint8 retx_count;
    uint8 last_flags;
    uint32 last_seq;
    uint32 last_ack;
    uint32 last_end_seq;
    uint32 last_payload_len;
    uint8 last_payload[NET_TCP_MAX_PAYLOAD];
} tcp_conn;

typedef struct tcp_rx_slot {
    uint8 valid;
    net_tcp_rx_packet pkt;
} tcp_rx_slot;

typedef struct net_timer_slot {
    uint8 active;
    uint32 expiry_tick;
    uint32 interval_ticks;
    net_timer_cb cb;
    void* ctx;
} net_timer_slot;

// UDP socket: binds a port to a dedicated RX queue
#define SOCKET_RXQ_SIZE 4
#define MAX_SOCKETS 8

// ARP cache timing (ms)
#define ARP_CACHE_TTL_MS        30000u
#define ARP_CACHE_REFRESH_MS     8000u
#define ARP_RETRY_BACKOFF_MS     200u

// TCP flags
#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

// TCP retransmit policy (minimal reliability)
#define TCP_RETX_INTERVAL_MS 400u
#define TCP_RETX_MAX_ATTEMPTS 4u

/*
 * ABI-INVARIANT: DNS UDP payload cap for the resolver.
 *
 * Why: RFC 1035 limits UDP DNS messages to 512 bytes without EDNS.
 * Invariant: The resolver uses fixed buffers sized to this limit.
 * Breakage if changed:
 *   - Increasing: raises stack usage in low-RAM paths.
 *   - Decreasing: rejects valid replies and can break common lookups.
 * ABI-sensitive: No (internal kernel API).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (bounds all DNS parse operations).
 */
#define NET_DNS_MAX_MESSAGE 512u

/*
 * ABI-INVARIANT: Maximum DNS name length (RFC 1035).
 *
 * Why: DNS names are limited to 253 characters (labels + dots).
 * Invariant: Encoded queries must fit within NET_DNS_MAX_MESSAGE.
 * Breakage if changed: May allow malformed names to overflow buffers.
 * ABI-sensitive: No (internal kernel API).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (input validation boundary).
 */
#define NET_DNS_MAX_NAME 253u

/*
 * ABI-INVARIANT: Maximum DNS label length (RFC 1035).
 *
 * Why: A single label is limited to 63 bytes.
 * Invariant: Resolver rejects labels longer than this.
 * Breakage if changed: Risks encoding invalid queries.
 * ABI-sensitive: No (internal kernel API).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (input validation boundary).
 */
#define NET_DNS_MAX_LABEL 63u

/*
 * ABI-INVARIANT: DNS service port (UDP).
 *
 * Why: Standard DNS service port defined by RFC 1035.
 * Invariant: Resolver emits queries to this port on the configured nameserver.
 * Breakage if changed: Queries will target the wrong service.
 * ABI-sensitive: No (internal kernel API).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (limits protocol expectations).
 */
#define NET_DNS_PORT 53u
#define NET_DHCP_CLIENT_PORT 68u
#define NET_DHCP_SERVER_PORT 67u
#define NET_DHCP_MAGIC_COOKIE 0x63825363u

#define DHCP_OPT_PAD 0u
#define DHCP_OPT_SUBNET_MASK 1u
#define DHCP_OPT_ROUTER 3u
#define DHCP_OPT_DNS 6u
#define DHCP_OPT_REQUESTED_IP 50u
#define DHCP_OPT_MSG_TYPE 53u
#define DHCP_OPT_SERVER_ID 54u
#define DHCP_OPT_PARAM_REQ_LIST 55u
#define DHCP_OPT_END 255u

#define DHCP_MSG_DISCOVER 1u
#define DHCP_MSG_OFFER 2u
#define DHCP_MSG_REQUEST 3u
#define DHCP_MSG_ACK 5u

typedef struct udp_socket {
    uint8 bound;
    uint16 port;
    udp_rx_slot rxq[SOCKET_RXQ_SIZE];
    uint32 rxq_head;
    uint32 rxq_tail;
    uint32 dropped_count;
} udp_socket;

static struct {
    net_config cfg;
    int inited;
    uint8 mac[6];
    const netdev* dev;
    arp_cache_entry arp_cache[4];

    net_ip_stats ip_stats;

    udp_rx_slot udp_rxq[8];
    net_udp_stats udp_stats;

    icmp_echo_reply_slot icmp_rxq[4];
    net_icmp_stats icmp_stats;

    tcp_conn tcp;
    net_tcp_stats tcp_stats;
    tcp_rx_slot tcp_rxq[8];
    uint32 tcp_rxq_head;
    uint32 tcp_rxq_tail;

    udp_socket sockets[MAX_SOCKETS];

    net_timer_slot timers[8];
    uint32 last_timer_tick;
    uint32 last_arp_age_tick;
} g_net = {
    .cfg = {
        .local_ip = {10, 0, 2, 15},
        .gateway_ip = {10, 0, 2, 2},
        .netmask = {255, 255, 255, 0},
        .dns_ip = {10, 0, 2, 3},
    },
};

static uint16 checksum16_add_bytes(uint32 sum, const uint8* data, uint32 len);
static int tcp_send_segment(const uint8 src_ip[4], uint16 src_port,
                            const uint8 dst_ip[4], uint16 dst_port,
                            uint32 seq, uint32 ack, uint8 flags,
                            const uint8* payload, uint32 payload_len,
                            int arp_spins);
static void tcp_retx_timer_cb(void* ctx);
static void net_log(const char* msg);
static int dns_get_server_ip(uint8 out_ip[4]);
static int dns_encode_name(const char* name, uint8* out, uint32 out_cap, uint32* out_len);
static int dns_skip_name(const uint8* msg, uint32 len, uint32* io_off);

void net_config_get(net_config* out)
{
    if (!out) return;
    *out = g_net.cfg;
}
static uint16 tcp_checksum16_ex(const uint8 src_ip[4], const uint8 dst_ip[4], const uint8* tcp_bytes,
                                uint32 tcp_hdr_len, const uint8* payload, uint32 payload_len)
{
    uint32 sum = 0;
    if (!src_ip || !dst_ip || !tcp_bytes) return 0;
    if (tcp_hdr_len < (uint32)sizeof(tcp_hdr) || tcp_hdr_len > 60u) return 0;

    uint8 pseudo[12];
    for (int i = 0; i < 4; i++) pseudo[i] = src_ip[i];
    for (int i = 0; i < 4; i++) pseudo[4 + i] = dst_ip[i];
    pseudo[8] = 0;
    pseudo[9] = 6; // TCP
    uint16 tcp_len = (uint16)(tcp_hdr_len + payload_len);
    pseudo[10] = (uint8)(tcp_len >> 8);
    pseudo[11] = (uint8)(tcp_len & 0xFF);

    sum = checksum16_add_bytes(sum, pseudo, 12u);

    // TCP header (including options) with checksum field zeroed
    uint8 hdr_bytes[60];
    memcpy(hdr_bytes, tcp_bytes, tcp_hdr_len);
    hdr_bytes[16] = 0;
    hdr_bytes[17] = 0;
    sum = checksum16_add_bytes(sum, hdr_bytes, tcp_hdr_len);

    if (payload_len && payload) {
        sum = checksum16_add_bytes(sum, payload, payload_len);
    }

    uint16 csum = (uint16)(~sum);
    if (csum == 0) csum = 0xFFFF;
    return csum;
}

static uint16 tcp_checksum16(const uint8 src_ip[4], const uint8 dst_ip[4], const tcp_hdr* tcp,
                             const uint8* payload, uint32 payload_len)
{
    return tcp_checksum16_ex(src_ip, dst_ip, (const uint8*)tcp, (uint32)sizeof(tcp_hdr), payload, payload_len);
}

// Retransmit helpers (minimal reliability for SYN/SYN-ACK/DATA/FIN).
// Stores the last outbound segment and retries it a few times on timeout.
static void tcp_retx_cancel(void)
{
    if (g_net.tcp.retx_timer_id >= 0) {
        net_timer_cancel(g_net.tcp.retx_timer_id);
    }
    g_net.tcp.retx_timer_id = -1;
    g_net.tcp.retx_active = 0;
    g_net.tcp.retx_count = 0;
    g_net.tcp.last_flags = 0;
    g_net.tcp.last_payload_len = 0;
}

static void tcp_retx_arm(uint8 flags, uint32 seq, uint32 ack, const uint8* payload, uint32 payload_len)
{
    if (payload_len > NET_TCP_MAX_PAYLOAD) return;

    tcp_retx_cancel();

    g_net.tcp.last_flags = flags;
    g_net.tcp.last_seq = seq;
    g_net.tcp.last_ack = ack;
    g_net.tcp.last_payload_len = payload_len;
    g_net.tcp.last_end_seq = seq + payload_len;
    if (flags & TCP_FLAG_SYN) g_net.tcp.last_end_seq += 1u;
    if (flags & TCP_FLAG_FIN) g_net.tcp.last_end_seq += 1u;

    if (payload_len > 0) {
        memcpy(g_net.tcp.last_payload, payload, payload_len);
    }

    g_net.tcp.retx_count = 0;
    g_net.tcp.retx_active = 1;
    g_net.tcp.retx_timer_id = net_timer_start(net_timer_ticks_from_ms(TCP_RETX_INTERVAL_MS),
                                             net_timer_ticks_from_ms(TCP_RETX_INTERVAL_MS),
                                             tcp_retx_timer_cb, NULL);
}

static void tcp_retx_ack_received(uint32 ack)
{
    if (!g_net.tcp.retx_active) return;
    if (ack >= g_net.tcp.last_end_seq) {
        tcp_retx_cancel();
    }
}

static void tcp_retx_timer_cb(void* ctx)
{
    (void)ctx;
    if (!g_net.tcp.retx_active) return;
    if (g_net.tcp.state == TCP_CLOSED) {
        tcp_retx_cancel();
        return;
    }
    if (g_net.tcp.retx_count >= TCP_RETX_MAX_ATTEMPTS) {
        net_log("[NETSTACK] TCP retransmit limit reached\n");
        tcp_retx_cancel();
        g_net.tcp.state = TCP_CLOSED;
        return;
    }

    if (g_net.tcp.local_port == 0 || g_net.tcp.remote_port == 0) return;

    (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                           g_net.tcp.remote_ip, g_net.tcp.remote_port,
                           g_net.tcp.last_seq, g_net.tcp.last_ack,
                           g_net.tcp.last_flags,
                           g_net.tcp.last_payload_len ? g_net.tcp.last_payload : NULL,
                           g_net.tcp.last_payload_len, 800000);
    g_net.tcp.retx_count++;
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

static uint32 bswap32(uint32 x)
{
    return ((x >> 24) & 0x000000FFu) |
           ((x >> 8)  & 0x0000FF00u) |
           ((x << 8)  & 0x00FF0000u) |
           ((x << 24) & 0xFF000000u);
}

static uint32 be32(uint32 x)
{
    return bswap32(x);
}

static void net_log(const char* msg)
{
    if (!msg) return;
    serial_write(SERIAL_COM1, msg, (int)strlen(msg));
}

static uint32 net_get_ticks(void)
{
    return sched_get_tick_count();
}

static uint32 net_get_hz(void)
{
    return sched_get_tick_hz();
}

static uint32 net_ticks_from_ms(uint32 ms)
{
    uint32 hz = net_get_hz();
    if (hz == 0) hz = 100;
    // ceil(ms * hz / 1000)
    uint32 ticks = (ms * hz + 999u) / 1000u;
    if (ticks == 0) ticks = 1;
    return ticks;
}

uint32 net_timer_get_ticks(void) { return net_get_ticks(); }
uint32 net_timer_get_hz(void) { return net_get_hz(); }
uint32 net_timer_ticks_from_ms(uint32 ms) { return net_ticks_from_ms(ms); }

int net_timer_start(uint32 delay_ticks, uint32 interval_ticks, net_timer_cb cb, void* ctx)
{
    if (!cb) return -1;
    if (delay_ticks == 0) delay_ticks = 1;
    for (int i = 0; i < (int)(sizeof(g_net.timers) / sizeof(g_net.timers[0])); i++) {
        if (!g_net.timers[i].active) {
            g_net.timers[i].active = 1;
            g_net.timers[i].expiry_tick = net_get_ticks() + delay_ticks;
            g_net.timers[i].interval_ticks = interval_ticks;
            g_net.timers[i].cb = cb;
            g_net.timers[i].ctx = ctx;
            return i;
        }
    }
    return -2;
}

void net_timer_cancel(int timer_id)
{
    if (timer_id < 0 || timer_id >= (int)(sizeof(g_net.timers) / sizeof(g_net.timers[0]))) return;
    g_net.timers[timer_id].active = 0;
}

static void net_timer_run(void)
{
    uint32 now = net_get_ticks();
    if (now == g_net.last_timer_tick) return;
    g_net.last_timer_tick = now;

    for (int i = 0; i < (int)(sizeof(g_net.timers) / sizeof(g_net.timers[0])); i++) {
        net_timer_slot* t = &g_net.timers[i];
        if (!t->active) continue;
        if ((int32)(now - t->expiry_tick) >= 0) {
            t->cb(t->ctx);
            if (t->interval_ticks > 0) {
                t->expiry_tick = now + t->interval_ticks;
            } else {
                t->active = 0;
            }
        }
    }
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

static uint16 checksum16_add_bytes(uint32 sum, const uint8* data, uint32 len)
{
    // Adds network-order 16-bit words; pads odd trailing byte with zero.
    uint32 i = 0;
    while (i + 1u < len) {
        uint16 w = (uint16)(((uint16)data[i] << 8) | (uint16)data[i + 1u]);
        sum += (uint32)w;
        i += 2u;
    }
    if (i < len) {
        uint16 w = (uint16)((uint16)data[i] << 8);
        sum += (uint32)w;
    }

    // Fold to 16-bit (may still overflow once, fold again).
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16)sum;
}

static uint16 udp_checksum16(const uint8 src_ip[4], const uint8 dst_ip[4], const udp_hdr* udp, const uint8* payload, uint32 payload_len)
{
    // UDP checksum includes pseudoheader + UDP header + payload.
    // Returned value is in host order (caller be16()s it).
    if (!src_ip || !dst_ip || !udp) return 0;

    uint32 sum = 0;
    // Pseudoheader
    sum = checksum16_add_bytes(sum, src_ip, 4u);
    sum = checksum16_add_bytes(sum, dst_ip, 4u);
    {
        uint8 ph[4];
        ph[0] = 0;
        ph[1] = 17; // protocol
        ph[2] = (uint8)((uint16)be16(udp->len_be) >> 8);
        ph[3] = (uint8)((uint16)be16(udp->len_be) & 0xFF);
        sum = checksum16_add_bytes(sum, ph, 4u);
    }

    // UDP header with checksum field as 0
    {
        uint8 hdr_bytes[8];
        hdr_bytes[0] = (uint8)((uint16)be16(udp->src_port_be) >> 8);
        hdr_bytes[1] = (uint8)((uint16)be16(udp->src_port_be) & 0xFF);
        hdr_bytes[2] = (uint8)((uint16)be16(udp->dst_port_be) >> 8);
        hdr_bytes[3] = (uint8)((uint16)be16(udp->dst_port_be) & 0xFF);
        hdr_bytes[4] = (uint8)((uint16)be16(udp->len_be) >> 8);
        hdr_bytes[5] = (uint8)((uint16)be16(udp->len_be) & 0xFF);
        hdr_bytes[6] = 0;
        hdr_bytes[7] = 0;
        sum = checksum16_add_bytes(sum, hdr_bytes, 8u);
    }

    if (payload_len && payload) {
        sum = checksum16_add_bytes(sum, payload, payload_len);
    }

    uint16 csum = (uint16)(~sum);
    if (csum == 0) csum = 0xFFFF;
    return csum;
}

static int arp_cache_lookup(const uint8 ip[4], uint8 out_mac[6])
{
    uint32 now = net_get_ticks();
    uint32 ttl_ticks = net_ticks_from_ms(ARP_CACHE_TTL_MS);
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (!g_net.arp_cache[i].valid) continue;
        if (ttl_ticks != 0 && (int32)(now - g_net.arp_cache[i].last_seen_tick) > (int32)ttl_ticks) {
            g_net.arp_cache[i].valid = 0;
            continue;
        }
        if (ipv4_eq(g_net.arp_cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) out_mac[j] = g_net.arp_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

static void arp_cache_update(const uint8 ip[4], const uint8 mac[6])
{
    uint32 now = net_get_ticks();
    // Update in place if present.
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (g_net.arp_cache[i].valid && ipv4_eq(g_net.arp_cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) g_net.arp_cache[i].mac[j] = mac[j];
            g_net.arp_cache[i].last_seen_tick = now;
            return;
        }
    }

    // Insert into first free slot.
    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (!g_net.arp_cache[i].valid) {
            for (int j = 0; j < 4; j++) g_net.arp_cache[i].ip[j] = ip[j];
            for (int j = 0; j < 6; j++) g_net.arp_cache[i].mac[j] = mac[j];
            g_net.arp_cache[i].valid = 1;
            g_net.arp_cache[i].last_seen_tick = now;
            return;
        }
    }

    // Cache full: overwrite slot 0.
    for (int j = 0; j < 4; j++) g_net.arp_cache[0].ip[j] = ip[j];
    for (int j = 0; j < 6; j++) g_net.arp_cache[0].mac[j] = mac[j];
    g_net.arp_cache[0].valid = 1;
    g_net.arp_cache[0].last_seen_tick = now;
}

static int ipv4_is_zero(const uint8 ip[4]);
static int arp_send_request_silent(const uint8 sender_ip[4], const uint8 target_ip[4]);

static void arp_age_timer_cb(void* ctx)
{
    (void)ctx;
    uint32 now = net_get_ticks();
    uint32 ttl_ticks = net_ticks_from_ms(ARP_CACHE_TTL_MS);
    uint32 refresh_ticks = net_ticks_from_ms(ARP_CACHE_REFRESH_MS);

    for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
        if (!g_net.arp_cache[i].valid) continue;
        if (ttl_ticks != 0 && (int32)(now - g_net.arp_cache[i].last_seen_tick) > (int32)ttl_ticks) {
            g_net.arp_cache[i].valid = 0;
            net_log("[NETSTACK] ARP entry expired\n");
            continue;
        }
    }

    // Refresh gateway MAC periodically if present.
    if (!ipv4_is_zero(g_net.cfg.gateway_ip)) {
        for (int i = 0; i < (int)(sizeof(g_net.arp_cache) / sizeof(g_net.arp_cache[0])); i++) {
            if (!g_net.arp_cache[i].valid) continue;
            if (ipv4_eq(g_net.arp_cache[i].ip, g_net.cfg.gateway_ip)) {
                if (refresh_ticks != 0 && (int32)(now - g_net.arp_cache[i].last_seen_tick) >= (int32)refresh_ticks) {
                    (void)arp_send_request_silent(g_net.cfg.local_ip, g_net.cfg.gateway_ip);
                }
                break;
            }
        }
    }
}

static int arp_send_reply_silent(const uint8 target_mac[6],
                                const uint8 sender_ip[4], const uint8 sender_mac[6],
                                const uint8 target_ip[4]);

static int arp_resolve(const uint8 sender_ip[4], const uint8 target_ip[4], uint8 out_mac[6], int arp_spins);

static uint32 ipv4_to_u32(const uint8 ip[4])
{
    return ((uint32)ip[0] << 24) | ((uint32)ip[1] << 16) | ((uint32)ip[2] << 8) | (uint32)ip[3];
}

static int ipv4_is_zero(const uint8 ip[4])
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

static int ipv4_is_broadcast(const uint8 ip[4])
{
    return ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
}

static int ipv4_is_same_subnet(const uint8 a[4], const uint8 b[4], const uint8 mask[4])
{
    uint32 au = ipv4_to_u32(a);
    uint32 bu = ipv4_to_u32(b);
    uint32 mu = ipv4_to_u32(mask);
    return ((au & mu) == (bu & mu));
}

static void ipv4_choose_next_hop(const uint8 src_ip[4], const uint8 dst_ip[4], uint8 out_next_hop_ip[4])
{
    // Minimal routing: if dst is not on-link, send to gateway (L2) but keep IP dst unchanged.
    // If gateway is unset, fall back to direct ARP for dst.
    if (!src_ip || !dst_ip || !out_next_hop_ip) return;

    /*
     * ABI-INVARIANT: QEMU slirp DNS next-hop behavior.
     *
     * Why: In common slirp setups the synthetic DNS IP (10.0.2.3) is exposed
     *      as a virtual service and may not answer ARP directly. Packets must
     *      still be sent to the default gateway MAC at L2.
     * Invariant: When destination equals configured dns_ip and a gateway is
     *            configured, choose gateway as ARP target.
     * Breakage if changed: DNS lookups can fail with repeated ARP retries even
     *                     though IP configuration is correct.
     * ABI-sensitive: No.
     * Disk-format-sensitive: No.
     * Security-critical: No.
     */
    if (!ipv4_is_zero(g_net.cfg.gateway_ip) && ipv4_eq(dst_ip, g_net.cfg.dns_ip)) {
        for (int i = 0; i < 4; i++) out_next_hop_ip[i] = g_net.cfg.gateway_ip[i];
        return;
    }

    if (!ipv4_is_zero(g_net.cfg.gateway_ip) && !ipv4_is_same_subnet(src_ip, dst_ip, g_net.cfg.netmask)) {
        for (int i = 0; i < 4; i++) out_next_hop_ip[i] = g_net.cfg.gateway_ip[i];
        return;
    }
    for (int i = 0; i < 4; i++) out_next_hop_ip[i] = dst_ip[i];
}

static int parse_ipv4_str(const char* s, uint8 out[4])
{
    if (!s || !out) return -1;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = (v * 10) + (*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[part] = (uint8)v;
        if (part != 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    return (*s == '\0') ? 0 : -1;
}

static int dns_get_server_ip(uint8 out_ip[4])
{
    if (!out_ip) return -1;

    char buf[256];
    int n = vfs_read_file(0, "/etc/resolv.conf", buf, (int)sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char* line = buf;
        while (line && *line) {
            char* next = strchr(line, '\n');
            if (next) {
                *next = '\0';
                next++;
            }

            while (*line == ' ' || *line == '\t' || *line == '\r') line++;
            if (*line == '#' || *line == '\0') { line = next; continue; }

            const char* key = "nameserver";
            int match = 1;
            for (int i = 0; key[i]; i++) {
                if (line[i] != key[i]) { match = 0; break; }
            }
            if (match && (line[10] == ' ' || line[10] == '\t')) {
                const char* ip_str = line + 10;
                while (*ip_str == ' ' || *ip_str == '\t') ip_str++;
                if (parse_ipv4_str(ip_str, out_ip) == 0) return 0;
            }

            line = next;
        }
    }

    // Fallback to netcfg DNS if resolv.conf is missing or invalid.
    if (g_net.cfg.dns_ip[0] || g_net.cfg.dns_ip[1] || g_net.cfg.dns_ip[2] || g_net.cfg.dns_ip[3]) {
        for (int i = 0; i < 4; i++) out_ip[i] = g_net.cfg.dns_ip[i];
        return 0;
    }

    return -1;
}

static int dns_encode_name(const char* name, uint8* out, uint32 out_cap, uint32* out_len)
{
    if (!name || !out || !out_len) return -1;

    uint32 len = 0;
    const char* p = name;
    if (*p == '\0') return -2;
    if (strlen(name) > NET_DNS_MAX_NAME) return -3;

    while (*p) {
        const char* label = p;
        uint32 label_len = 0;
        while (*p && *p != '.') {
            label_len++;
            p++;
        }
        if (label_len == 0 || label_len > NET_DNS_MAX_LABEL) return -4;
        if (len + 1 + label_len + 1 > out_cap) return -5;
        out[len++] = (uint8)label_len;
        for (uint32 i = 0; i < label_len; i++) {
            out[len++] = (uint8)label[i];
        }
        if (*p == '.') p++;
    }
    if (len + 1 > out_cap) return -6;
    out[len++] = 0;
    *out_len = len;
    return 0;
}

static int dns_skip_name(const uint8* msg, uint32 len, uint32* io_off)
{
    if (!msg || !io_off) return -1;
    uint32 off = *io_off;
    while (off < len) {
        uint8 b = msg[off];
        if (b == 0) {
            *io_off = off + 1u;
            return 0;
        }
        if ((b & 0xC0u) == 0xC0u) {
            if (off + 1u >= len) return -2;
            *io_off = off + 2u;
            return 0;
        }
        if (b > NET_DNS_MAX_LABEL) return -3;
        off++;
        if (off + b > len) return -4;
        off += b;
    }
    return -5;
}

int net_dns_resolve(const char* name, uint8 out_ip[4], int timeout_spins)
{
    if (!name || !out_ip) return -1;
    if (net_init_default() != 0) return -2;

    uint8 dns_ip[4];
    if (dns_get_server_ip(dns_ip) != 0) return -3;

    uint8 local_ip[4];
    net_get_local_ip(local_ip);

    int socket_id = -1;
    uint16 base_port = (uint16)(40000u + (net_get_ticks() % 20000u));
    for (int attempt = 0; attempt < 16; attempt++) {
        uint16 port = (uint16)(base_port + attempt);
        socket_id = net_udp_bind(port);
        if (socket_id >= 0) break;
    }
    if (socket_id < 0) return -4;

    uint8 msg[NET_DNS_MAX_MESSAGE];
    memset(msg, 0, sizeof(msg));

    uint16 tx_id = (uint16)(net_get_ticks() & 0xFFFFu);
    msg[0] = (uint8)(tx_id >> 8);
    msg[1] = (uint8)(tx_id & 0xFFu);
    msg[2] = 0x01; // recursion desired
    msg[3] = 0x00;
    msg[4] = 0x00;
    msg[5] = 0x01; // QDCOUNT

    uint32 off = 12u;
    uint32 name_len = 0;
    int rc = dns_encode_name(name, msg + off, (uint32)sizeof(msg) - off, &name_len);
    if (rc != 0) { net_udp_close(socket_id); return -5; }
    off += name_len;
    if (off + 4u > (uint32)sizeof(msg)) { net_udp_close(socket_id); return -6; }
    msg[off++] = 0x00; msg[off++] = 0x01; // QTYPE = A
    msg[off++] = 0x00; msg[off++] = 0x01; // QCLASS = IN

    rc = net_udp_send_socket(socket_id, local_ip, dns_ip, (uint16)NET_DNS_PORT, msg, off, 800000);
    if (rc != 0) { net_udp_close(socket_id); return -7; }

    if (timeout_spins <= 0) timeout_spins = 2000000;
    uint32 ui_ticks = 0;
    for (int spin = 0; spin < timeout_spins; spin++) {
        if ((spin & 0x1FFF) == 0) {
            watchdog_kick("net-dns-wait");
            // Keep the GUI alive while resolver waits in a blocking syscall.
            if (tile_is_tiling_active() && (++ui_ticks & 0x1F) == 0) {
                tile_render_once();
            }
        }
        (void)net_poll(local_ip, 32);

        net_udp_rx_packet pkt;
        int got = net_udp_recv_socket(socket_id, &pkt);
        if (got <= 0) continue;

        if (!ipv4_eq(pkt.src_ip, dns_ip) || pkt.payload_len < 12u) continue;

        const uint8* rx = pkt.payload;
        uint32 rx_len = pkt.payload_len;
        uint16 rx_id = (uint16)((rx[0] << 8) | rx[1]);
        if (rx_id != tx_id) continue;

        uint16 flags = (uint16)((rx[2] << 8) | rx[3]);
        if ((flags & 0x8000u) == 0) continue; // not a response
        if ((flags & 0x000Fu) != 0) { net_udp_close(socket_id); return -8; }

        uint16 qdcount = (uint16)((rx[4] << 8) | rx[5]);
        uint16 ancount = (uint16)((rx[6] << 8) | rx[7]);

        uint32 roff = 12u;
        for (uint16 q = 0; q < qdcount; q++) {
            if (dns_skip_name(rx, rx_len, &roff) != 0) { net_udp_close(socket_id); return -9; }
            if (roff + 4u > rx_len) { net_udp_close(socket_id); return -9; }
            roff += 4u;
        }

        for (uint16 a = 0; a < ancount; a++) {
            if (dns_skip_name(rx, rx_len, &roff) != 0) { net_udp_close(socket_id); return -10; }
            if (roff + 10u > rx_len) { net_udp_close(socket_id); return -10; }

            uint16 type = (uint16)((rx[roff] << 8) | rx[roff + 1u]);
            uint16 class = (uint16)((rx[roff + 2u] << 8) | rx[roff + 3u]);
            uint16 rdlen = (uint16)((rx[roff + 8u] << 8) | rx[roff + 9u]);
            roff += 10u;

            if (roff + rdlen > rx_len) { net_udp_close(socket_id); return -11; }
            if (type == 1u && class == 1u && rdlen == 4u) {
                for (int i = 0; i < 4; i++) out_ip[i] = rx[roff + (uint32)i];
                net_udp_close(socket_id);
                return 0;
            }
            roff += rdlen;
        }
    }

    net_udp_close(socket_id);
    return -12;
}

int net_dhcp_request(int timeout_spins, int arp_spins, int persist_cfg)
{
    (void)persist_cfg;
    if (net_init_default() != 0) return -1;

    uint8 mac[6];
    if (net_get_mac(mac) != 0) return -2;

    int socket_id = net_udp_bind((uint16)NET_DHCP_CLIENT_PORT);
    if (socket_id < 0) return -3;

    uint8 zero_ip[4] = {0, 0, 0, 0};
    uint8 bcast_ip[4] = {255, 255, 255, 255};
    uint8 req[548];
    memset(req, 0, sizeof(req));

    uint32 xid = net_get_ticks() ^ 0x45594E4Fu;
    req[0] = 1;  // BOOTREQUEST
    req[1] = 1;  // Ethernet
    req[2] = 6;  // MAC len
    req[3] = 0;
    req[4] = (uint8)((xid >> 24) & 0xFFu);
    req[5] = (uint8)((xid >> 16) & 0xFFu);
    req[6] = (uint8)((xid >> 8) & 0xFFu);
    req[7] = (uint8)(xid & 0xFFu);
    req[10] = 0x80; // broadcast flag
    req[11] = 0x00;
    for (int i = 0; i < 6; i++) req[28 + i] = mac[i];

    req[236] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 24) & 0xFFu);
    req[237] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 16) & 0xFFu);
    req[238] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 8) & 0xFFu);
    req[239] = (uint8)(NET_DHCP_MAGIC_COOKIE & 0xFFu);

    uint32 off = 240u;
    req[off++] = DHCP_OPT_MSG_TYPE;
    req[off++] = 1;
    req[off++] = DHCP_MSG_DISCOVER;
    req[off++] = DHCP_OPT_PARAM_REQ_LIST;
    req[off++] = 3;
    req[off++] = DHCP_OPT_SUBNET_MASK;
    req[off++] = DHCP_OPT_ROUTER;
    req[off++] = DHCP_OPT_DNS;
    req[off++] = DHCP_OPT_END;

    if (net_udp_send_socket(socket_id, zero_ip, bcast_ip, (uint16)NET_DHCP_SERVER_PORT,
                            req, off, arp_spins) != 0) {
        net_udp_close(socket_id);
        return -4;
    }

    if (timeout_spins <= 0) timeout_spins = 3000000;

    uint8 offered_ip[4] = {0, 0, 0, 0};
    uint8 server_id[4] = {0, 0, 0, 0};
    uint8 netmask[4] = {255, 255, 255, 0};
    uint8 gateway[4] = {0, 0, 0, 0};
    uint8 dns[4] = {0, 0, 0, 0};
    int have_offer = 0;

    for (int spin = 0; spin < timeout_spins; spin++) {
        if ((spin & 0x1FFF) == 0) watchdog_kick("net-dhcp-offer");
        (void)net_poll(zero_ip, 32);

        net_udp_rx_packet pkt;
        if (net_udp_recv_socket(socket_id, &pkt) <= 0) continue;
        if (pkt.payload_len < 240u) continue;

        const uint8* rx = pkt.payload;
        uint32 rx_xid = ((uint32)rx[4] << 24) | ((uint32)rx[5] << 16) | ((uint32)rx[6] << 8) | (uint32)rx[7];
        if (rx[0] != 2 || rx_xid != xid) continue;

        uint32 cookie = ((uint32)rx[236] << 24) | ((uint32)rx[237] << 16) | ((uint32)rx[238] << 8) | (uint32)rx[239];
        if (cookie != NET_DHCP_MAGIC_COOKIE) continue;

        uint8 msg_type = 0;
        uint32 roff = 240u;
        while (roff < pkt.payload_len) {
            uint8 opt = rx[roff++];
            if (opt == DHCP_OPT_END) break;
            if (opt == DHCP_OPT_PAD) continue;
            if (roff >= pkt.payload_len) break;
            uint8 opt_len = rx[roff++];
            if (roff + (uint32)opt_len > pkt.payload_len) break;

            if (opt == DHCP_OPT_MSG_TYPE && opt_len == 1u) {
                msg_type = rx[roff];
            } else if (opt == DHCP_OPT_SUBNET_MASK && opt_len == 4u) {
                for (int i = 0; i < 4; i++) netmask[i] = rx[roff + (uint32)i];
            } else if (opt == DHCP_OPT_ROUTER && opt_len >= 4u) {
                for (int i = 0; i < 4; i++) gateway[i] = rx[roff + (uint32)i];
            } else if (opt == DHCP_OPT_DNS && opt_len >= 4u) {
                for (int i = 0; i < 4; i++) dns[i] = rx[roff + (uint32)i];
            } else if (opt == DHCP_OPT_SERVER_ID && opt_len == 4u) {
                for (int i = 0; i < 4; i++) server_id[i] = rx[roff + (uint32)i];
            }

            roff += (uint32)opt_len;
        }

        if (msg_type != DHCP_MSG_OFFER) continue;
        for (int i = 0; i < 4; i++) offered_ip[i] = rx[16 + (uint32)i];
        have_offer = 1;
        break;
    }

    if (!have_offer) {
        net_udp_close(socket_id);
        return -5;
    }

    memset(req, 0, sizeof(req));
    req[0] = 1;
    req[1] = 1;
    req[2] = 6;
    req[3] = 0;
    req[4] = (uint8)((xid >> 24) & 0xFFu);
    req[5] = (uint8)((xid >> 16) & 0xFFu);
    req[6] = (uint8)((xid >> 8) & 0xFFu);
    req[7] = (uint8)(xid & 0xFFu);
    req[10] = 0x80;
    req[11] = 0x00;
    for (int i = 0; i < 6; i++) req[28 + i] = mac[i];

    req[236] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 24) & 0xFFu);
    req[237] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 16) & 0xFFu);
    req[238] = (uint8)((NET_DHCP_MAGIC_COOKIE >> 8) & 0xFFu);
    req[239] = (uint8)(NET_DHCP_MAGIC_COOKIE & 0xFFu);

    off = 240u;
    req[off++] = DHCP_OPT_MSG_TYPE;
    req[off++] = 1;
    req[off++] = DHCP_MSG_REQUEST;
    req[off++] = DHCP_OPT_REQUESTED_IP;
    req[off++] = 4;
    for (int i = 0; i < 4; i++) req[off++] = offered_ip[i];
    if (!ipv4_is_zero(server_id)) {
        req[off++] = DHCP_OPT_SERVER_ID;
        req[off++] = 4;
        for (int i = 0; i < 4; i++) req[off++] = server_id[i];
    }
    req[off++] = DHCP_OPT_PARAM_REQ_LIST;
    req[off++] = 3;
    req[off++] = DHCP_OPT_SUBNET_MASK;
    req[off++] = DHCP_OPT_ROUTER;
    req[off++] = DHCP_OPT_DNS;
    req[off++] = DHCP_OPT_END;

    if (net_udp_send_socket(socket_id, zero_ip, bcast_ip, (uint16)NET_DHCP_SERVER_PORT,
                            req, off, arp_spins) != 0) {
        net_udp_close(socket_id);
        return -6;
    }

    int got_ack = 0;
    for (int spin = 0; spin < timeout_spins; spin++) {
        if ((spin & 0x1FFF) == 0) watchdog_kick("net-dhcp-ack");
        (void)net_poll(zero_ip, 32);

        net_udp_rx_packet pkt;
        if (net_udp_recv_socket(socket_id, &pkt) <= 0) continue;
        if (pkt.payload_len < 240u) continue;

        const uint8* rx = pkt.payload;
        uint32 rx_xid = ((uint32)rx[4] << 24) | ((uint32)rx[5] << 16) | ((uint32)rx[6] << 8) | (uint32)rx[7];
        if (rx[0] != 2 || rx_xid != xid) continue;

        uint32 cookie = ((uint32)rx[236] << 24) | ((uint32)rx[237] << 16) | ((uint32)rx[238] << 8) | (uint32)rx[239];
        if (cookie != NET_DHCP_MAGIC_COOKIE) continue;

        uint8 msg_type = 0;
        uint32 roff = 240u;
        while (roff < pkt.payload_len) {
            uint8 opt = rx[roff++];
            if (opt == DHCP_OPT_END) break;
            if (opt == DHCP_OPT_PAD) continue;
            if (roff >= pkt.payload_len) break;
            uint8 opt_len = rx[roff++];
            if (roff + (uint32)opt_len > pkt.payload_len) break;

            if (opt == DHCP_OPT_MSG_TYPE && opt_len == 1u) {
                msg_type = rx[roff];
            } else if (opt == DHCP_OPT_SUBNET_MASK && opt_len == 4u) {
                for (int i = 0; i < 4; i++) netmask[i] = rx[roff + (uint32)i];
            } else if (opt == DHCP_OPT_ROUTER && opt_len >= 4u) {
                for (int i = 0; i < 4; i++) gateway[i] = rx[roff + (uint32)i];
            } else if (opt == DHCP_OPT_DNS && opt_len >= 4u) {
                for (int i = 0; i < 4; i++) dns[i] = rx[roff + (uint32)i];
            }

            roff += (uint32)opt_len;
        }

        if (msg_type != DHCP_MSG_ACK) continue;

        net_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        for (int i = 0; i < 4; i++) {
            cfg.local_ip[i] = rx[16 + (uint32)i];
            cfg.gateway_ip[i] = gateway[i];
            cfg.netmask[i] = netmask[i];
            cfg.dns_ip[i] = dns[i];
        }
        if (ipv4_is_zero(cfg.dns_ip) && !ipv4_is_zero(cfg.gateway_ip)) {
            for (int i = 0; i < 4; i++) cfg.dns_ip[i] = cfg.gateway_ip[i];
        }

        if (net_config_set(&cfg) != 0) {
            net_udp_close(socket_id);
            return -7;
        }
        got_ack = 1;
        break;
    }

    net_udp_close(socket_id);
    return got_ack ? 0 : -8;
}

static void udp_rxq_clear(void)
{
    for (int i = 0; i < (int)(sizeof(g_net.udp_rxq) / sizeof(g_net.udp_rxq[0])); i++) {
        g_net.udp_rxq[i].valid = 0;
    }
    g_net.udp_stats.udp_rx_enqueued = 0;
    g_net.udp_stats.udp_rx_dropped = 0;
    g_net.udp_stats.udp_rx_truncated = 0;
    g_net.udp_stats.udp_rx_bad_checksum = 0;
    g_net.udp_stats.udp_tx_checksums = 0;
}

static int tcp_rxq_enqueue(const uint8 src_ip[4], uint16 src_port,
                           const uint8 dst_ip[4], uint16 dst_port,
                           const uint8* payload, uint32 payload_len)
{
    if (payload_len > NET_TCP_MAX_PAYLOAD) {
        // Never truncate TCP payloads; caller keeps ACK unchanged on enqueue
        // failure so the peer retransmits a segment we can fully buffer.
        g_net.tcp_stats.tcp_rx_dropped++;
        return -1;
    }

    uint32 cap = (uint32)(sizeof(g_net.tcp_rxq) / sizeof(g_net.tcp_rxq[0]));
    tcp_rx_slot* s = &g_net.tcp_rxq[g_net.tcp_rxq_tail];
    if (s->valid) {
        g_net.tcp_stats.tcp_rx_dropped++;
        return -1;
    }

    s->valid = 1;
    for (int j = 0; j < 4; j++) s->pkt.src_ip[j] = src_ip[j];
    for (int j = 0; j < 4; j++) s->pkt.dst_ip[j] = dst_ip[j];
    s->pkt.src_port = src_port;
    s->pkt.dst_port = dst_port;

    s->pkt.payload_len = payload_len;
    if (payload_len != 0u) {
        memcpy(s->pkt.payload, payload, payload_len);
    }

    g_net.tcp_rxq_tail = (g_net.tcp_rxq_tail + 1u) % cap;
    g_net.tcp_stats.tcp_rx_enqueued++;
    return 0;
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
    g_net.icmp_stats.dest_unreach_rx = 0;
    g_net.icmp_stats.time_exceeded_rx = 0;
    g_net.icmp_stats.frag_needed_rx = 0;
}

static void tcp_rxq_clear(void)
{
    for (int i = 0; i < (int)(sizeof(g_net.tcp_rxq) / sizeof(g_net.tcp_rxq[0])); i++) {
        g_net.tcp_rxq[i].valid = 0;
    }
    g_net.tcp_rxq_head = 0u;
    g_net.tcp_rxq_tail = 0u;
    g_net.tcp_stats.tcp_rx_enqueued = 0;
    g_net.tcp_stats.tcp_rx_dropped = 0;
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
    // First, try to route to a bound socket
    for (int sock_idx = 0; sock_idx < MAX_SOCKETS; sock_idx++) {
        udp_socket* sock = &g_net.sockets[sock_idx];
        if (sock->bound && sock->port == dst_port) {
            uint32 tail_idx = sock->rxq_tail;
            if (!sock->rxq[tail_idx].valid) {
                udp_rx_slot* s = &sock->rxq[tail_idx];
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
                sock->rxq_tail = (sock->rxq_tail + 1) % SOCKET_RXQ_SIZE;
                g_net.udp_stats.udp_rx_enqueued++;
                return 0;
            } else {
                // Socket queue full
                sock->dropped_count++;
                g_net.udp_stats.udp_rx_dropped++;
                return -1;
            }
        }
    }

    // No socket bound to this port; use global queue
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

int net_init_default(void)
{
    static netdev e100_dev;

    e100_dev.send_frame = e100_send_frame;
    e100_dev.rx_poll_frame = e100_rx_poll_frame;
    e100_dev.get_mac = e100_get_mac;

    if (e100_init() == 0) {
        return net_init(&e100_dev);
    }

    return net_init_e1000_default();
}

int net_init(const netdev* dev)
{
    if (g_net.inited) return 0;
    if (!dev || !dev->send_frame || !dev->rx_poll_frame || !dev->get_mac) return -1;

    if (dev->get_mac(g_net.mac) != 0) return -2;

    g_net.dev = dev;
    udp_rxq_clear();
    icmp_rxq_clear();
    tcp_rxq_clear();
    g_net.ip_stats.ipv4_rx_fragments = 0;
    g_net.ip_stats.ipv4_rx_frag_dropped = 0;
    memset(&g_net.tcp, 0, sizeof(g_net.tcp));
    g_net.tcp.retx_timer_id = -1;
    memset(&g_net.tcp_stats, 0, sizeof(g_net.tcp_stats));
    memset(g_net.timers, 0, sizeof(g_net.timers));
    g_net.last_timer_tick = 0;
    (void)net_timer_start(net_ticks_from_ms(1000), net_ticks_from_ms(1000), arp_age_timer_cb, NULL);
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

net_ip_stats net_ip_get_stats(void)
{
    return g_net.ip_stats;
}

net_icmp_stats net_icmp_get_stats(void)
{
    return g_net.icmp_stats;
}

net_tcp_stats net_tcp_get_stats(void)
{
    return g_net.tcp_stats;
}

int net_tcp_listen(uint16 local_port)
{
    if (local_port == 0) return -1;
    if (net_init_default() != 0) return -2;

    g_net.tcp.listening = 1;
    g_net.tcp.listen_port = local_port;
    g_net.tcp.state = TCP_CLOSED;
    tcp_retx_cancel();
    tcp_rxq_clear();
    return 0;
}

int net_tcp_close(void)
{
    if (g_net.tcp.state == TCP_ESTABLISHED) {
        uint8 src_ip[4];
        net_get_local_ip(src_ip);
        (void)tcp_send_segment(src_ip, g_net.tcp.local_port,
                               g_net.tcp.remote_ip, g_net.tcp.remote_port,
                               g_net.tcp.seq, g_net.tcp.ack,
                               TCP_FLAG_FIN | TCP_FLAG_ACK,
                               NULL, 0, 800000);
        tcp_retx_arm((uint8)(TCP_FLAG_FIN | TCP_FLAG_ACK), g_net.tcp.seq, g_net.tcp.ack, NULL, 0);
        g_net.tcp.seq += 1;
        g_net.tcp_stats.tcp_fin_tx++;
        g_net.tcp.state = TCP_FIN_WAIT;
    }
    g_net.tcp.listening = 0;
    return 0;
}

int net_tcp_is_closed(void)
{
    return (g_net.tcp.state == TCP_CLOSED) ? 1 : 0;
}

int net_tcp_recv(net_tcp_rx_packet* out)
{
    if (!out) return -1;

    uint32 cap = (uint32)(sizeof(g_net.tcp_rxq) / sizeof(g_net.tcp_rxq[0]));
    for (uint32 n = 0; n < cap; n++) {
        uint32 idx = (g_net.tcp_rxq_head + n) % cap;
        if (g_net.tcp_rxq[idx].valid) {
            *out = g_net.tcp_rxq[idx].pkt;
            g_net.tcp_rxq[idx].valid = 0;
            g_net.tcp_rxq_head = (idx + 1u) % cap;
            return 1;
        }
    }

    // Queue is empty: perform one poll pass and try once more.
    // This avoids enqueueing bursts before draining already-queued packets.
    if (g_net.inited) {
        uint8 local_ip[4];
        net_get_local_ip(local_ip);
        (void)net_poll(local_ip, 8);

        for (uint32 n = 0; n < cap; n++) {
            uint32 idx = (g_net.tcp_rxq_head + n) % cap;
            if (g_net.tcp_rxq[idx].valid) {
                *out = g_net.tcp_rxq[idx].pkt;
                g_net.tcp_rxq[idx].valid = 0;
                g_net.tcp_rxq_head = (idx + 1u) % cap;
                return 1;
            }
        }
    }

    return 0;
}

uint32 net_tcp_queue_count(void)
{
    uint32 count = 0;
    for (int i = 0; i < (int)(sizeof(g_net.tcp_rxq) / sizeof(g_net.tcp_rxq[0])); i++) {
        if (g_net.tcp_rxq[i].valid) count++;
    }
    return count;
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
    ip->flags_frag_off_be = be16(0x4000u);
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
    ip->flags_frag_off_be = be16(0x4000u);
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

// --- UDP Socket API ---

int net_udp_bind(uint16 port)
{
    if (!g_net.inited) return -1;
    if (port == 0) return -2;

    // Check if already bound
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (g_net.sockets[i].bound && g_net.sockets[i].port == port) {
            return -1; // Already bound
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_net.sockets[i].bound) {
            g_net.sockets[i].bound = 1;
            g_net.sockets[i].port = port;
            g_net.sockets[i].rxq_head = 0;
            g_net.sockets[i].rxq_tail = 0;
            g_net.sockets[i].dropped_count = 0;
            for (int j = 0; j < SOCKET_RXQ_SIZE; j++) {
                g_net.sockets[i].rxq[j].valid = 0;
            }
            return i; // socket_id
        }
    }

    return -1; // No free slots
}

int net_udp_close(int socket_id)
{
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) return -1;
    if (!g_net.sockets[socket_id].bound) return -1;

    g_net.sockets[socket_id].bound = 0;
    g_net.sockets[socket_id].port = 0;
    return 0;
}

int net_udp_send_socket(int socket_id, const uint8 src_ip[4], const uint8 dst_ip[4], uint16 dst_port,
                        const uint8* payload, uint32 payload_len, int arp_spins)
{
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) return -1;
    if (!g_net.sockets[socket_id].bound) return -1;

    uint16 src_port = g_net.sockets[socket_id].port;
    return net_udp_send(src_ip, src_port, dst_ip, dst_port, payload, payload_len, arp_spins);
}

int net_udp_recv_socket(int socket_id, net_udp_rx_packet* out)
{
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) return -1;
    if (!g_net.sockets[socket_id].bound) return -1;
    if (!out) return -2;

    udp_socket* sock = &g_net.sockets[socket_id];
    uint32 idx = sock->rxq_head;

    if (sock->rxq[idx].valid) {
        *out = sock->rxq[idx].pkt;
        sock->rxq[idx].valid = 0;
        sock->rxq_head = (sock->rxq_head + 1) % SOCKET_RXQ_SIZE;
        return 1;
    }

    return 0; // No packet
}

uint32 net_udp_socket_queue_count(int socket_id)
{
    if (socket_id < 0 || socket_id >= MAX_SOCKETS) return 0;
    if (!g_net.sockets[socket_id].bound) return 0;

    uint32 count = 0;
    for (int i = 0; i < SOCKET_RXQ_SIZE; i++) {
        if (g_net.sockets[socket_id].rxq[i].valid) count++;
    }
    return count;
}

uint32 net_get_sockets(net_socket_info* out, uint32 out_cap)
{
    if (!out || out_cap == 0) return 0;
    uint32 written = 0;
    for (int i = 0; i < MAX_SOCKETS && written < out_cap; i++) {
        if (g_net.sockets[i].bound) {
            out[written].bound = 1;
            out[written].port = g_net.sockets[i].port;
            out[written].queued = net_udp_socket_queue_count(i);
            out[written].dropped = g_net.sockets[i].dropped_count;
            written++;
        }
    }
    return written;
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
        if (net_init_default() != 0) return -2;
    }

    net_timer_run();

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
        uint16 frag = be16(ip->flags_frag_off_be);
        uint16 frag_off = (uint16)(frag & 0x1FFFu);
        uint16 frag_flags = (uint16)(frag & 0xE000u);
        if (frag_off != 0 || (frag_flags & 0x2000u)) {
            g_net.ip_stats.ipv4_rx_fragments++;
            g_net.ip_stats.ipv4_rx_frag_dropped++;
            net_log("[NETSTACK] Dropping IPv4 fragment\n");
            continue;
        }
        int dst_is_local = ipv4_eq(ip->dst, local_ip);
        int dst_is_broadcast = ipv4_is_broadcast(ip->dst);
        if (!dst_is_local && !dst_is_broadcast) continue;

        // Opportunistically learn IP->MAC mapping from IPv4 frames.
        arp_cache_update(ip->src, eh->src);

        if (ip->proto == 1) {
            if (!dst_is_local) continue;
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
            } else if (ic->type == 3) {
                // Destination unreachable
                g_net.icmp_stats.dest_unreach_rx++;
                if (ic->code == 4) {
                    g_net.icmp_stats.frag_needed_rx++;
                    net_log("[NETSTACK] ICMP frag needed\n");
                }
            } else if (ic->type == 11) {
                // Time exceeded
                g_net.icmp_stats.time_exceeded_rx++;
                net_log("[NETSTACK] ICMP time exceeded\n");
            }
            continue;
        }

        // TCP handling (minimal client state machine).
        if (ip->proto == 6) {
            if (!dst_is_local) continue;
            if (len < (uint32)sizeof(eth_hdr) + ip_hdr_len + (uint32)sizeof(tcp_hdr)) continue;

            tcp_hdr* tcp = (tcp_hdr*)(frame + sizeof(eth_hdr) + ip_hdr_len);
            uint32 tcp_hdr_len = (uint32)((tcp->data_off >> 4) & 0x0Fu) * 4u;
            if (tcp_hdr_len < (uint32)sizeof(tcp_hdr)) continue;

            uint16 src_port = be16(tcp->src_port_be);
            uint16 dst_port = be16(tcp->dst_port_be);
            if (dst_port == 0 || src_port == 0) continue;

            uint32 payload_off = (uint32)sizeof(eth_hdr) + ip_hdr_len + tcp_hdr_len;
            uint32 payload_len = 0;
            uint32 ip_total_len = (uint32)be16(ip->total_len_be);
            if (ip_total_len >= ip_hdr_len + tcp_hdr_len) {
                payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
            }
            if (payload_off > len) continue;
            if (payload_off + payload_len > len) {
                if (len > payload_off) payload_len = len - payload_off;
                else payload_len = 0;
            }

            // Verify TCP checksum.
            if (tcp->checksum_be != 0) {
                uint16 saved = tcp->checksum_be;
                uint16 want = tcp_checksum16_ex(ip->src, ip->dst, (const uint8*)tcp, tcp_hdr_len,
                                                frame + payload_off, payload_len);
                if (be16(saved) != want) {
                    continue;
                }
            }

            // Passive listen: SYN on listen port.
            if (g_net.tcp.listening && g_net.tcp.state == TCP_CLOSED &&
                dst_port == g_net.tcp.listen_port && (tcp->flags & TCP_FLAG_SYN)) {
                g_net.tcp_stats.tcp_listen_syn_rx++;
                for (int i = 0; i < 4; i++) g_net.tcp.local_ip[i] = ip->dst[i];
                for (int i = 0; i < 4; i++) g_net.tcp.remote_ip[i] = ip->src[i];
                g_net.tcp.local_port = dst_port;
                g_net.tcp.remote_port = src_port;
                g_net.tcp.seq = net_get_ticks();
                g_net.tcp.ack = be32(tcp->seq_be) + 1;
                g_net.tcp.state = TCP_SYN_RECEIVED;

                (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                                       g_net.tcp.remote_ip, g_net.tcp.remote_port,
                                       g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_SYN | TCP_FLAG_ACK,
                                       NULL, 0, 800000);
                tcp_retx_arm((uint8)(TCP_FLAG_SYN | TCP_FLAG_ACK), g_net.tcp.seq, g_net.tcp.ack, NULL, 0);
                g_net.tcp_stats.tcp_syn_sent++;
                continue;
            }

            // Match against current connection only.
            if (g_net.tcp.state != TCP_CLOSED &&
                dst_port == g_net.tcp.local_port &&
                src_port == g_net.tcp.remote_port &&
                ipv4_eq(ip->src, g_net.tcp.remote_ip)) {

                uint32 seq = be32(tcp->seq_be);
                uint32 ack = be32(tcp->ack_be);
                uint8 flags = tcp->flags;

                if (flags & TCP_FLAG_RST) {
                    g_net.tcp_stats.tcp_rst_rx++;
                    g_net.tcp.state = TCP_CLOSED;
                    tcp_retx_cancel();
                    continue;
                }

                if (g_net.tcp.state == TCP_SYN_SENT) {
                    if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
                        ack == g_net.tcp.seq + 1) {
                        g_net.tcp_stats.tcp_synack_rx++;
                        tcp_retx_cancel();
                        g_net.tcp.seq += 1;
                        g_net.tcp.ack = seq + 1;
                        (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                                               g_net.tcp.remote_ip, g_net.tcp.remote_port,
                                               g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_ACK,
                                               NULL, 0, 800000);
                        g_net.tcp_stats.tcp_ack_tx++;
                        g_net.tcp.state = TCP_ESTABLISHED;
                        g_net.tcp_stats.tcp_conn_established++;
                    }
                    continue;
                }

                if (g_net.tcp.state == TCP_SYN_RECEIVED) {
                    if ((flags & TCP_FLAG_ACK) && ack == g_net.tcp.seq + 1) {
                        tcp_retx_cancel();
                        g_net.tcp.seq += 1;
                        g_net.tcp.state = TCP_ESTABLISHED;
                        g_net.tcp_stats.tcp_conn_established++;
                    }
                    continue;
                }

                if (g_net.tcp.state == TCP_ESTABLISHED || g_net.tcp.state == TCP_FIN_WAIT) {
                    if (flags & TCP_FLAG_ACK) {
                        tcp_retx_ack_received(ack);
                    }
                    if (payload_len > 0) {
                        g_net.tcp_stats.tcp_data_rx++;

                        // Reliability invariant: only advance ACK for payload that is
                        // in-order and successfully enqueued. If RX queue is full,
                        // keep ACK unchanged so the peer retransmits.
                        if (seq == g_net.tcp.ack) {
                            int enq_rc = tcp_rxq_enqueue(ip->src, src_port, ip->dst, dst_port,
                                                         frame + payload_off, payload_len);
                            if (enq_rc == 0) {
                                g_net.tcp.ack += payload_len;
                            }
                        }

                        (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                                               g_net.tcp.remote_ip, g_net.tcp.remote_port,
                                               g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_ACK,
                                               NULL, 0, 800000);
                        g_net.tcp_stats.tcp_ack_tx++;
                    }

                    if (flags & TCP_FLAG_FIN) {
                        // Accept FIN only when it is in-order relative to current ACK.
                        uint32 fin_seq = seq + payload_len;
                        if (fin_seq == g_net.tcp.ack) {
                            g_net.tcp_stats.tcp_fin_rx++;
                            g_net.tcp.ack += 1u;
                            (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                                                   g_net.tcp.remote_ip, g_net.tcp.remote_port,
                                                   g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_ACK,
                                                   NULL, 0, 800000);
                            g_net.tcp_stats.tcp_ack_tx++;
                            g_net.tcp.state = TCP_CLOSED;
                            tcp_retx_cancel();
                        } else {
                            // Duplicate/out-of-order FIN: send current ACK only.
                            (void)tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                                                   g_net.tcp.remote_ip, g_net.tcp.remote_port,
                                                   g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_ACK,
                                                   NULL, 0, 800000);
                            g_net.tcp_stats.tcp_ack_tx++;
                        }
                    } else if (g_net.tcp.state == TCP_FIN_WAIT && (flags & TCP_FLAG_ACK)) {
                        g_net.tcp.state = TCP_CLOSED;
                        tcp_retx_cancel();
                    }
                }
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

        // Verify UDP checksum if present (0 means "no checksum" for IPv4).
        if (udp->checksum_be != 0) {
            uint16 saved = udp->checksum_be;
            // Compute against the truncated payload we actually have.
            // This is best-effort; frames truncated by NIC/driver will fail checksum and be dropped.
            uint16 want = udp_checksum16(ip->src, ip->dst, udp, frame + payload_off, payload_len);
            if (be16(saved) != want) {
                g_net.udp_stats.udp_rx_bad_checksum++;
                continue;
            }
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

    if (net_init_default() != 0) return -2;

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
        uint8 next_hop_ip[4];
        ipv4_choose_next_hop(local_ip, dst_ip, next_hop_ip);
        int rc = arp_resolve(local_ip, next_hop_ip, dst_mac, 800000);
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

    int base_spins = arp_spins;
    if (base_spins <= 0) base_spins = 12000000;
    int per_try_spins = base_spins / 3;
    if (per_try_spins < 100000) per_try_spins = 100000;

    for (int attempt = 0; attempt < 3; attempt++) {
        int rc = arp_send_request_silent(sender_ip, target_ip);
        if (rc != 0) return -100 + rc;

        rc = arp_poll_reply_mac(target_ip, out_mac, per_try_spins);
        if (rc == 0) {
            arp_cache_update(target_ip, out_mac);
            return 0;
        }

        if (attempt < 2) {
            net_log("[NETSTACK] ARP retry\n");
            sched_sleep_us(ARP_RETRY_BACKOFF_MS * 1000u);
        }
    }

    return -203;
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
    if (net_init_default() != 0) return -2;

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

static int tcp_send_segment(const uint8 src_ip[4], uint16 src_port,
                            const uint8 dst_ip[4], uint16 dst_port,
                            uint32 seq, uint32 ack, uint8 flags,
                            const uint8* payload, uint32 payload_len,
                            int arp_spins)
{
    if (!src_ip || !dst_ip) return -1;
    if (payload_len != 0u && !payload) return -1;
    if (payload_len > 1400u) return -2;
    if (src_port == 0 || dst_port == 0) return -3;

    uint8 dst_mac[6];
    uint8 next_hop_ip[4];
    ipv4_choose_next_hop(src_ip, dst_ip, next_hop_ip);
    int rc = arp_resolve(src_ip, next_hop_ip, dst_mac, arp_spins);
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
    ip->proto = 6;
    for (int i = 0; i < 4; i++) ip->src[i] = src_ip[i];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];

    uint16 ip_total_len = (uint16)(sizeof(ipv4_hdr) + sizeof(tcp_hdr) + payload_len);
    ip->total_len_be = be16(ip_total_len);
    static uint16 ip_id = 0x6000;
    ip->id_be = be16(ip_id++);
    ip->flags_frag_off_be = be16(0x4000u);
    ip->hdr_checksum_be = 0;
    ip->hdr_checksum_be = be16(ipv4_checksum16(ip, (uint32)sizeof(*ip)));
    off += (uint32)sizeof(ipv4_hdr);

    tcp_hdr* tcp = (tcp_hdr*)(frame + off);
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port_be = be16(src_port);
    tcp->dst_port_be = be16(dst_port);
    tcp->seq_be = be32(seq);
    tcp->ack_be = be32(ack);
    tcp->data_off = (uint8)(5u << 4);
    tcp->flags = flags;
    tcp->window_be = be16(4096u);
    tcp->urg_ptr_be = 0;
    tcp->checksum_be = 0;
    tcp->checksum_be = be16(tcp_checksum16(src_ip, dst_ip, tcp, payload, payload_len));
    off += (uint32)sizeof(tcp_hdr);

    if (payload_len) {
        memcpy(frame + off, payload, payload_len);
        off += payload_len;
    }

    return g_net.dev->send_frame(frame, off);
}

int net_tcp_send(const uint8 local_ip[4], uint16 local_port,
                 const uint8 dst_ip[4], uint16 dst_port,
                 const uint8* payload, uint32 payload_len,
                 int timeout_spins)
{
    if (!local_ip || !dst_ip) return -1;
    if (payload_len != 0u && !payload) return -1;
    if (dst_port == 0) return -2;
    if (net_init_default() != 0) return -3;

    if (payload_len > NET_TCP_MAX_PAYLOAD) return -4;

    if (local_port == 0) {
        local_port = (uint16)(40000u + (net_get_ticks() % 20000u));
    }

    // Initialize connection state.
    memset(&g_net.tcp, 0, sizeof(g_net.tcp));
    for (int i = 0; i < 4; i++) g_net.tcp.local_ip[i] = local_ip[i];
    for (int i = 0; i < 4; i++) g_net.tcp.remote_ip[i] = dst_ip[i];
    g_net.tcp.local_port = local_port;
    g_net.tcp.remote_port = dst_port;
    g_net.tcp.seq = net_get_ticks();
    g_net.tcp.ack = 0;
    g_net.tcp.state = TCP_SYN_SENT;

    g_net.tcp_stats.tcp_syn_sent++;
    int rc = tcp_send_segment(local_ip, local_port, dst_ip, dst_port,
                              g_net.tcp.seq, 0, TCP_FLAG_SYN, NULL, 0, timeout_spins);
    tcp_retx_arm(TCP_FLAG_SYN, g_net.tcp.seq, 0, NULL, 0);
    if (rc != 0) return rc;

    // Wait for SYN-ACK
    if (timeout_spins <= 0) timeout_spins = 12000000;
    for (int spin = 0; spin < timeout_spins; spin++) {
        if ((spin & 0x1FFF) == 0) watchdog_kick("net-tcp-wait");
        (void)net_poll(local_ip, 16);
        if (g_net.tcp.state == TCP_ESTABLISHED) break;
    }
    if (g_net.tcp.state != TCP_ESTABLISHED) return -5;

    // Send payload if any.
    if (payload_len > 0) {
        rc = tcp_send_segment(local_ip, local_port, dst_ip, dst_port,
                              g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_PSH | TCP_FLAG_ACK,
                              payload, payload_len, timeout_spins);
        tcp_retx_arm((uint8)(TCP_FLAG_PSH | TCP_FLAG_ACK), g_net.tcp.seq, g_net.tcp.ack,
                     payload, payload_len);
        if (rc != 0) return rc;
        g_net.tcp.seq += payload_len;
        g_net.tcp_stats.tcp_data_tx++;
    }

    // Send FIN
    rc = tcp_send_segment(local_ip, local_port, dst_ip, dst_port,
                          g_net.tcp.seq, g_net.tcp.ack, TCP_FLAG_FIN | TCP_FLAG_ACK,
                          NULL, 0, timeout_spins);
    tcp_retx_arm((uint8)(TCP_FLAG_FIN | TCP_FLAG_ACK), g_net.tcp.seq, g_net.tcp.ack, NULL, 0);
    if (rc != 0) return rc;
    g_net.tcp.seq += 1;
    g_net.tcp.state = TCP_FIN_WAIT;
    g_net.tcp_stats.tcp_fin_tx++;

    return (int)payload_len;
}

int net_tcp_connect(const uint8 local_ip[4], uint16 local_port,
                    const uint8 dst_ip[4], uint16 dst_port,
                    int timeout_spins)
{
    if (!local_ip || !dst_ip) return -1;
    if (dst_port == 0) return -2;
    if (net_init_default() != 0) return -3;

    if (local_port == 0) {
        local_port = (uint16)(40000u + (net_get_ticks() % 20000u));
    }

    // Initialize connection state.
    memset(&g_net.tcp, 0, sizeof(g_net.tcp));
    for (int i = 0; i < 4; i++) g_net.tcp.local_ip[i] = local_ip[i];
    for (int i = 0; i < 4; i++) g_net.tcp.remote_ip[i] = dst_ip[i];
    g_net.tcp.local_port = local_port;
    g_net.tcp.remote_port = dst_port;
    g_net.tcp.seq = net_get_ticks();
    g_net.tcp.ack = 0;
    g_net.tcp.state = TCP_SYN_SENT;
    g_net.tcp.listening = 0;
    tcp_retx_cancel();
    tcp_rxq_clear();

    g_net.tcp_stats.tcp_syn_sent++;
    int rc = tcp_send_segment(local_ip, local_port, dst_ip, dst_port,
                              g_net.tcp.seq, 0, TCP_FLAG_SYN, NULL, 0, timeout_spins);
    tcp_retx_arm(TCP_FLAG_SYN, g_net.tcp.seq, 0, NULL, 0);
    if (rc != 0) return rc;

    if (timeout_spins <= 0) timeout_spins = 2000000;
    uint32 ui_ticks = 0;
    for (int spin = 0; spin < timeout_spins; spin++) {
        if ((spin & 0x1FFF) == 0) {
            watchdog_kick("net-tcp-connect");
            // Keep the GUI alive while connect waits in a blocking syscall.
            if (tile_is_tiling_active() && (++ui_ticks & 0x1F) == 0) {
                tile_render_once();
            }
        }
        (void)net_poll(local_ip, 16);
        if (g_net.tcp.state == TCP_ESTABLISHED) break;
    }
    if (g_net.tcp.state != TCP_ESTABLISHED) return -5;

    return 0;
}

int net_tcp_send_current(const uint8* payload, uint32 payload_len)
{
    if (payload_len != 0u && !payload) return -1;
    if (!g_net.inited || !g_net.dev) return -2;
    if (g_net.tcp.state != TCP_ESTABLISHED) return -3;
    if (g_net.tcp.local_port == 0 || g_net.tcp.remote_port == 0) return -4;

    if (payload_len > NET_TCP_MAX_PAYLOAD) return -5;

    if (payload_len == 0u) return 0;

    int rc = tcp_send_segment(g_net.tcp.local_ip, g_net.tcp.local_port,
                              g_net.tcp.remote_ip, g_net.tcp.remote_port,
                              g_net.tcp.seq, g_net.tcp.ack,
                              TCP_FLAG_PSH | TCP_FLAG_ACK,
                              payload, payload_len, 800000);
    tcp_retx_arm((uint8)(TCP_FLAG_PSH | TCP_FLAG_ACK), g_net.tcp.seq, g_net.tcp.ack,
                 payload, payload_len);
    if (rc != 0) return rc;
    g_net.tcp.seq += payload_len;
    g_net.tcp_stats.tcp_data_tx++;
    return (int)payload_len;
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

    if (net_init_default() != 0) return -4;

    uint8 dst_mac[6];
    int rc = 0;
    if (ipv4_is_broadcast(dst_ip)) {
        for (int i = 0; i < 6; i++) dst_mac[i] = 0xFFu;
    } else {
        uint8 next_hop_ip[4];
        ipv4_choose_next_hop(src_ip, dst_ip, next_hop_ip);
        rc = arp_resolve(src_ip, next_hop_ip, dst_mac, arp_spins);
        if (rc != 0) return rc;
    }

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
    ip->flags_frag_off_be = be16(0x4000u);
    ip->hdr_checksum_be = 0;
    ip->hdr_checksum_be = be16(ipv4_checksum16(ip, (uint32)sizeof(*ip)));
    off += (uint32)sizeof(ipv4_hdr);

    udp_hdr* udp = (udp_hdr*)(frame + off);
    udp->src_port_be = be16(src_port);
    udp->dst_port_be = be16(dst_port);
    udp->len_be = be16((uint16)(sizeof(udp_hdr) + payload_len));
    udp->checksum_be = 0;
    off += (uint32)sizeof(udp_hdr);

    const uint8* frame_payload = (const uint8*)(frame + off);

    memcpy(frame + off, payload, payload_len);
    off += payload_len;

    {
        uint16 csum = udp_checksum16(src_ip, dst_ip, udp, frame_payload, payload_len);
        udp->checksum_be = be16(csum);
        g_net.udp_stats.udp_tx_checksums++;
    }

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

    if (net_init_default() != 0) return -3;

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

    if (net_init_default() != 0) return -3;

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
