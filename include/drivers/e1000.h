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

#endif
