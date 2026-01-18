# EYN-OS Networking

EYN-OS implements a basic but functional networking stack supporting UDP/IPv4 communication over Intel e1000-compatible NICs.

## Architecture Overview

The networking implementation is separated into distinct layers:

```
┌─────────────────────────────────────────────┐
│         User Programs / Shell               │
│ (e1000 udp-listen, e1000 udp-send, ping)    │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│         Network Stack (netstack.c)          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │   UDP    │  │   IPv4   │  │   ARP    │  │
│  └──────────┘  └──────────┘  └──────────┘  │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│         NIC Driver (e1000.c)                │
│  - Descriptor rings (TX/RX)                 │
│  - MMIO register access                     │
│  - Interrupt handling                       │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│      Intel e1000 Hardware (82540EM)         │
└─────────────────────────────────────────────┘
```

## Components

### 1. E1000 Driver
- **File**: `src/drivers/e1000.c`, `include/drivers/e1000.h`
- **Purpose**: Low-level NIC control and packet transmission/reception
- **Features**:
  - PCI device detection and initialization
  - MMIO-based register access (no port I/O)
  - TX/RX descriptor ring management
  - Interrupt-assisted RX polling
  - Link status monitoring
  - MAC address reading from EEPROM

See [e1000-driver.md](e1000-driver.md) for details.

### 2. Network Stack
- **File**: `src/network/netstack.c`, `include/network/netstack.h`
- **Purpose**: Protocol implementation (Ethernet, ARP, IPv4, UDP, ICMP echo)
- **Features**:
  - Ethernet frame parsing
  - ARP cache for IP→MAC resolution (aging + retries)
  - IPv4 packet handling (DF set; fragments dropped)
  - UDP socket-like interface
  - ICMP echo (ping) support
  - ICMP error tracking (unreachable, time exceeded, frag needed)
  - Packet statistics

See [network-stack.md](network-stack.md) for protocol details.

### 3. User Interface
Commands for network interaction:
- `e1000 probe|init|regs|test` - NIC management
- `pciscan [net]` - PCI device enumeration
- `e1000 udp-send|udp-listen|udp-echo|udp-stats|udp-drain` - UDP utilities
- `e1000 tcp-send|tcp-listen|tcp-recv|tcp-close` - Minimal TCP tools
- `ping <dst_ip> [count] [local_ip]` - ICMP echo
- `netstat` - Network status dump
- `netcfg show|defaults|set|save|load ...` - Network configuration (runtime + persisted)

See [network-commands.md](network-commands.md) for usage examples.

## Quick Start

### 1. Initialize Network
```bash
# Boot EYN-OS in QEMU with networking
make run

# Inside EYN-OS shell:
init              # Initialize ATA drives first
e1000 init        # Initialize e1000 NIC
```

### 2. Send UDP Packet
```bash
e1000 udp-send 10.0.2.2 5000 Hello from EYN-OS
```

### 3. Listen for UDP Packets
```bash
e1000 udp-listen 9999
# Press Ctrl+C to stop
```

### 4. Check Statistics
```bash
e1000 udp-stats   # Show packet counters
```

## Limitations

Current implementation has these limitations:

1. **Single NIC**: Only first e1000 device is used
2. **No TCP**: Only UDP is implemented
3. **Limited ICMP**: Only ICMP echo (ping) request/reply
4. **No Fragmentation**: MTU is effectively ~1500 bytes
5. **No DHCP**: Static config (runtime configurable via `netcfg`)
6. **Polling**: Polling-based with interrupt-assisted RX
7. **Single Thread**: No concurrent socket operations

## Testing with QEMU

QEMU provides user-mode networking by default:

```bash
# Default QEMU network (make run):
# - Host: 10.0.2.2
# - Guest (EYN-OS): 10.0.2.15 (or similar)

# Test host → guest using Makefile's default hostfwd:
# - UDP: host 10000 → guest 9999
# - TCP: host 10000 → guest 9999
echo "test message" | nc -u 127.0.0.1 10000
# Then in EYN-OS:
e1000 udp-listen 9999

# TCP example:
# In EYN-OS:
#   e1000 tcp-listen 9999
# On the host:
#   nc 127.0.0.1 10000
```

## Implementation Files

| File | Purpose |
|------|---------|
| `src/drivers/e1000.c` | E1000 NIC driver |
| `src/network/netstack.c` | UDP/IPv4/ARP stack |
| `include/drivers/e1000.h` | E1000 API |
| `include/network/netstack.h` | Network stack API |
| `src/utilities/shell/shell_commands.c` | Network commands |

## Future Enhancements

Potential improvements for networking:

- TCP implementation
- DHCP client for automatic IP configuration
- Persisted network configuration (VFS-backed)
- Multiple socket support
- Interrupt-based packet reception
- IPv4 fragmentation/reassembly
- DNS client
- Higher-level socket API for userland programs

## References

- [Intel 82540EM Datasheet](https://www.intel.com/content/dam/doc/datasheet/82540ep-gbe-controller-datasheet.pdf)
- [RFC 768 - UDP](https://tools.ietf.org/html/rfc768)
- [RFC 791 - IPv4](https://tools.ietf.org/html/rfc791)
- [RFC 826 - ARP](https://tools.ietf.org/html/rfc826)
- [OSDev Wiki - E1000](https://wiki.osdev.org/Intel_8254x)
