#ifndef NETDEV_H
#define NETDEV_H

#include <misc/types.h>

// Minimal network device interface for the protocol stack.
//
// This keeps netstack independent from specific NIC drivers (e1000 now; others later).
// Everything is polling-based for bring-up simplicity.

typedef struct netdev {
    int (*send_frame)(const void* frame, uint32 len);

    // Returns: 1 if a frame was copied, 0 if no frame, <0 on error.
    int (*rx_poll_frame)(uint8* out_buf, uint32 out_buf_cap, uint32* out_len, int spin_limit);

    // Returns 0 on success.
    int (*get_mac)(uint8 out_mac[6]);
} netdev;

#endif
