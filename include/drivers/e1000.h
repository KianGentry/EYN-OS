#ifndef E1000_H
#define E1000_H

#include <misc/types.h>

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

// Debug helper for bring-up: prints key RX/TX registers.
void e1000_debug_regs_print(void);

// Initializes RX+TX rings into a known-good state.
int e1000_init(void);

// Returns the NIC MAC address (ensures probe has run).
int e1000_get_mac(uint8 out_mac[6]);

// Raw Ethernet frame TX/RX for the network stack.
//
// The driver stays responsible for ring/DMA/MMIO; higher layers build Ethernet/ARP/IP/UDP.
int e1000_send_frame(const void* frame, uint32 len);

// Polls until a single frame is available (or spin_limit is exhausted).
// Returns: 1 if a frame was copied, 0 if no frame, <0 on error.
int e1000_rx_poll_frame(uint8* out_buf, uint32 out_buf_cap, uint32* out_len, int spin_limit);

// Interrupt-assisted RX support.
// Enables RX interrupts if IRQ line is available.
int e1000_irq_enable_rx(void);

// Returns non-zero if RX interrupt was seen since last clear.
int e1000_irq_rx_pending(void);

// Clears RX pending flag (called after polling).
void e1000_irq_clear_rx_pending(void);

#endif
