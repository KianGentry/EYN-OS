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

int net_arp_test_send(const uint8 sender_ip[4], const uint8 target_ip[4], int rx_spins);

int net_udp_send(const uint8 src_ip[4], uint16 src_port,
                 const uint8 dst_ip[4], uint16 dst_port,
                 const uint8* payload, uint32 payload_len,
                 int arp_spins);

// Polls for UDP packets destined to local_ip:local_port and prints payload.
// Returns number of packets printed, or <0 on error.
//
// Notes:
// - max_packets == 0 means unlimited (run until Ctrl-C).
// - spin_limit == 0 means unlimited (run until Ctrl-C).
int net_udp_listen(const uint8 local_ip[4], uint16 local_port, int max_packets, int spin_limit);

#endif
