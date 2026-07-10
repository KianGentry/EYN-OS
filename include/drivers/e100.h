#ifndef E100_H
#define E100_H

#include <misc/types.h>

typedef struct e100_probe_info {
    uint8 bus;
    uint8 device;
    uint8 function;
    uint16 io_base;
    uint16 vendor_id;
    uint16 device_id;
    uint8 irq_line;
    uint8 mac[6];
} e100_probe_info;

int e100_probe(e100_probe_info* out);
int e100_init(void);
int e100_get_mac(uint8 out_mac[6]);

int e100_send_frame(const void* frame, uint32 len);
int e100_rx_poll_frame(uint8* out_buf, uint32 out_buf_cap, uint32* out_len, int spin_limit);

// Interrupt-assisted RX helpers.
int e100_irq_enable_rx(void);
int e100_irq_rx_pending(void);
void e100_irq_clear_rx_pending(void);

#endif