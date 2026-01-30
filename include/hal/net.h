#ifndef EYNOS_HAL_NET_H
#define EYNOS_HAL_NET_H

#include <network/netdev.h>

/*
 * HAL Network
 *
 * The existing netstack is already abstracted via `netdev`.
 * This header exists to make the "HAL boundary" explicit for networking.
 */

/* Returns the default network device, or NULL if none present. */
netdev* hal_net_default(void);

#endif
