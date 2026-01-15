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
} net_udp_stats;

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

#endif
