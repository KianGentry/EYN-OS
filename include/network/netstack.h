#ifndef NETSTACK_H
#define NETSTACK_H

#include <misc/types.h>
#include <network/netdev.h>

// Minimal network stack for early bring-up.
//
// Responsibilities:
// - ARP cache + ARP resolution
// - IPv4 header construction + checksum
// - UDP send (IPv4, checksum disabled)
//
// Intentionally small and polling-based. The NIC driver supplies raw frame TX/RX.

int net_init_e1000_default(void);

// Generic init: allow binding netstack to any netdev.
// The netstack reads and caches the device MAC during init.
int net_init(const netdev* dev);

// Returns non-zero if netstack has been initialized.
int net_is_inited(void);

// --- Net configuration ---
// Stored in the netstack and used as defaults for commands/background polling.
// Default values match QEMU user-net (slirp):
//   local_ip = 10.0.2.15
//   gateway  = 10.0.2.2
//   netmask  = 255.255.255.0
//   dns      = 10.0.2.3
typedef struct net_config {
    uint8 local_ip[4];
    uint8 gateway_ip[4];
    uint8 netmask[4];
    uint8 dns_ip[4];
} net_config;

// --- Netstack timer facility ---
// Uses system tick counter for lightweight scheduling.
typedef void (*net_timer_cb)(void* ctx);

// Returns current tick count and tick rate (Hz).
uint32 net_timer_get_ticks(void);
uint32 net_timer_get_hz(void);

// Convert milliseconds to ticks (rounded up).
uint32 net_timer_ticks_from_ms(uint32 ms);

// Schedule a timer.
// delay_ticks: ticks until first fire
// interval_ticks: 0 for one-shot, >0 for periodic
// Returns timer id (>=0) or <0 on error.
int net_timer_start(uint32 delay_ticks, uint32 interval_ticks, net_timer_cb cb, void* ctx);

// Cancel a timer by id.
void net_timer_cancel(int timer_id);

// Get/set the active configuration.
// These do not require the netstack to be initialized.
void net_config_get(net_config* out);
int net_config_set(const net_config* in);
void net_config_set_defaults(void);

// Convenience: copy out the configured local IP.
void net_get_local_ip(uint8 out_ip[4]);

int net_arp_test_send(const uint8 sender_ip[4], const uint8 target_ip[4], int rx_spins);

int net_udp_send(const uint8 src_ip[4], uint16 src_port,
                 const uint8 dst_ip[4], uint16 dst_port,
                 const uint8* payload, uint32 payload_len,
                 int arp_spins);

// --- Receive path (polling + small fixed queue) ---
// The goal is to decouple "receive packets" from "print packets" so the shell/UI
// doesn't have to block just to keep networking alive.

// Maximum bytes stored per queued UDP payload.
// Payloads larger than this are truncated and counted.
#define NET_UDP_MAX_PAYLOAD 512u

typedef struct net_udp_rx_packet {
    uint8 src_ip[4];
    uint16 src_port;
    uint8 dst_ip[4];
    uint16 dst_port;
    uint32 payload_len;
    uint8 payload[NET_UDP_MAX_PAYLOAD];
} net_udp_rx_packet;

typedef struct net_udp_stats {
    uint32 udp_rx_enqueued;
    uint32 udp_rx_dropped;
    uint32 udp_rx_truncated;
    uint32 udp_rx_bad_checksum;
    uint32 udp_tx_checksums;
} net_udp_stats;

// IPv4-level stats (diagnostics for fragmentation policy).
typedef struct net_ip_stats {
    uint32 ipv4_rx_fragments;
    uint32 ipv4_rx_frag_dropped;
} net_ip_stats;

// Poll the NIC for RX frames and feed:
// - ARP cache learning and ARP replies for local_ip
// - UDP packet enqueueing (for any dst_port)
//
// budget_frames:
// - 0 means a small default budget (non-blocking)
// Returns number of frames processed (>=0) or <0 on error.
int net_poll(const uint8 local_ip[4], uint32 budget_frames);

// Non-blocking dequeue for a given UDP local port.
// Returns:
// - 1 if a packet was returned in out
// - 0 if no packet available
// - <0 on error
int net_udp_recv(uint16 local_port, net_udp_rx_packet* out);

// Returns a snapshot of current UDP receive stats.
net_udp_stats net_udp_get_stats(void);

// Returns how many queued UDP packets match local_port.
uint32 net_udp_queue_count(uint16 local_port);

// Clears all queued UDP packets matching local_port.
// Returns number of packets cleared.
uint32 net_udp_queue_clear(uint16 local_port);

// --- UDP Socket API ---
// Provides per-port binding for multiple listeners.
// Each socket has a dedicated RX queue.

// Bind a UDP port and return a socket ID (>=0), or <0 on error.
// Errors:
// - -1: port already bound or no free socket slots
// - -2: port is 0 (invalid)
int net_udp_bind(uint16 port);

// Close a UDP socket by ID.
// Returns 0 on success, <0 on error.
int net_udp_close(int socket_id);

// Send UDP via a bound socket.
// Returns number of bytes sent (>= 0) or <0 on error.
// Error codes:
// - -1: invalid socket_id
// - -200-N: ARP resolution failure (see arp_resolve)
int net_udp_send_socket(int socket_id, const uint8 src_ip[4], const uint8 dst_ip[4], uint16 dst_port,
                        const uint8* payload, uint32 payload_len, int arp_spins);

// Receive from a bound socket's queue (non-blocking).
// Returns:
// - 1 if packet received
// - 0 if no packet available
// - <0 on error
int net_udp_recv_socket(int socket_id, net_udp_rx_packet* out);

// Returns number of queued packets for a given socket.
uint32 net_udp_socket_queue_count(int socket_id);

// Socket info structure for diagnostics
typedef struct net_socket_info {
    uint8 bound;
    uint16 port;
    uint32 queued;
    uint32 dropped;
} net_socket_info;

// Get list of bound sockets for diagnostics.
// Returns number of bound sockets written to out (up to out_cap).
uint32 net_get_sockets(net_socket_info* out, uint32 out_cap);

// Polls for UDP packets destined to local_ip:local_port and prints payload.
// Returns number of packets printed, or <0 on error.
//
// Notes:
// - max_packets == 0 means unlimited (run until Ctrl-C).
// - spin_limit == 0 means unlimited (run until Ctrl-C).
int net_udp_listen(const uint8 local_ip[4], uint16 local_port, int max_packets, int spin_limit);

// Like net_udp_listen, but also replies to the sender with the same payload.
// Useful for validating bidirectional UDP through QEMU user-net hostfwd.
int net_udp_echo(const uint8 local_ip[4], uint16 local_port, int max_packets, int spin_limit);

// --- ICMP (Ping) ---

// Snapshot of a single ARP cache entry for diagnostics.
typedef struct net_arp_entry {
    uint8 ip[4];
    uint8 mac[6];
    uint8 valid;
} net_arp_entry;

// Snapshot of ICMP counters for diagnostics.
typedef struct net_icmp_stats {
    uint32 echo_req_rx;
    uint32 echo_rep_rx;
    uint32 echo_rep_tx;
    uint32 echo_rep_dropped;
    uint32 dest_unreach_rx;
    uint32 time_exceeded_rx;
    uint32 frag_needed_rx;
} net_icmp_stats;

// Returns cached MAC address after init.
// Returns 0 on success, <0 if netstack is not initialized.
int net_get_mac(uint8 out_mac[6]);

// Copies current ARP cache snapshot into out (up to out_cap entries).
// Returns number of entries written (may be 0).
uint32 net_get_arp_cache(net_arp_entry* out, uint32 out_cap);

// Returns total number of queued UDP packets across all ports.
uint32 net_udp_queue_total(void);

// Returns a snapshot of current IPv4 stats.
net_ip_stats net_ip_get_stats(void);

// Returns a snapshot of current ICMP stats.
net_icmp_stats net_icmp_get_stats(void);

// Sends ICMP echo request(s) and waits for reply.
//
// count:
// - <=0 defaults to 4
// timeout_spins:
// - <=0 defaults to a reasonable wait loop
//
// Returns number of replies received (>=0), or <0 on error.
int net_icmp_ping(const uint8 local_ip[4], const uint8 dst_ip[4], int count, int timeout_spins);

#endif
