#ifndef E1000_H
#define E1000_H

// Minimal Intel e1000 driver surface for early bring-up.
//
// Why this exists:
// - Before implementing TX/RX rings and a full network stack, we want a safe,
//   deterministic way to confirm we can talk to the NIC over MMIO and read
//   basic identity/link information.
// - This keeps hardware-specific details out of shell code and gives us a
//   natural home for the later TX/RX implementation.

typedef struct e1000_probe_info {
	unsigned char bus;
	unsigned char device;
	unsigned char function;
	unsigned int bar0;

	unsigned int ctrl;
	unsigned int status;
	unsigned char mac[6];
	int link_up;
} e1000_probe_info;

// Read-only probe that fills out identity/state.
//
// Why separate from printing:
// - Lets shell commands implement simple self-tests without duplicating MMIO reads.
// - Keeps logging format decisions out of the driver.
int e1000_probe(e1000_probe_info* out);

int e1000_probe_and_print(void);

// Transmit-path bring-up helper.
// Sends a single small Ethernet frame via the TX ring and polls for completion.
int e1000_tx_test_send(const char* message);

// Receive-path bring-up helper.
// Polls the RX ring and prints a short Ethernet header summary for each frame.
int e1000_rx_poll_and_print(int max_packets, int spin_limit);

// ARP bring-up helper.
// Sends an ARP request (who-has) so you can observe an inbound ARP reply via rx-poll.
int e1000_arp_test_send(unsigned char sender_ip[4], unsigned char target_ip[4]);

// Debug helper for bring-up: prints key RX/TX registers.
void e1000_debug_regs_print(void);

// Initializes RX+TX rings into a known-good state.
int e1000_init(void);

#endif
