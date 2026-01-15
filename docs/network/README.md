# EYN-OS Networking

EYN-OS implements a basic but functional networking stack supporting UDP/IPv4 communication over Intel e1000-compatible NICs.

## Architecture Overview

The networking implementation is separated into distinct layers:

```
┌─────────────────────────────────────────────┐
│         User Programs / Shell               │
│    (udp listen, udp send commands)          │
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

<!-- SCREENSHOT: Network stack in action -->
<!-- Caption: Testing UDP communication with 'udp listen' and 'udp stats' -->
<!-- Show: Shell with udp commands and received packet output -->

## Components

### 1. E1000 Driver
- **File**: `src/drivers/e1000.c`, `include/drivers/e1000.h`
- **Purpose**: Low-level NIC control and packet transmission/reception
- **Features**:
  - PCI device detection and initialization
  - MMIO-based register access (no port I/O)
  - TX/RX descriptor ring management
  - Link status monitoring
  - MAC address reading from EEPROM

See [e1000-driver.md](e1000-driver.md) for details.

### 2. Network Stack
- **File**: `src/network/netstack.c`, `include/network/netstack.h`
- **Purpose**: Protocol implementation (Ethernet, ARP, IPv4, UDP)
- **Features**:
  - Ethernet frame parsing
  - ARP cache for IP→MAC resolution
  - IPv4 packet handling (no fragmentation)
  - UDP socket-like interface
  - Packet statistics

See [network-stack.md](network-stack.md) for protocol details.

### 3. User Interface
Commands for network interaction:
- `e1000 probe|init|regs|test` - NIC management
- `pci scan` - PCI device enumeration
- `udp listen <port>` - Listen for UDP packets
- `udp send <ip> <port> <msg>` - Send UDP packets
- `udp stats` - Display statistics
- `udp drain` - Clear receive queue

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
udp send 10.0.2.2 5000 Hello from EYN-OS
```

### 3. Listen for UDP Packets
```bash
udp listen 10000
# Press Ctrl+C to stop
```

### 4. Check Statistics
```bash
udp stats         # Show packet counters
```

## Limitations

Current implementation has these limitations:

1. **Single NIC**: Only first e1000 device is used
2. **No TCP**: Only UDP is implemented
3. **No ICMP**: Ping not supported
4. **No Fragmentation**: MTU is effectively ~1500 bytes
5. **No DHCP**: Static IP only (hardcoded for now)
6. **Polling**: Interrupt-based RX not yet implemented
7. **Single Thread**: No concurrent socket operations

## Testing with QEMU

QEMU provides user-mode networking by default:

```bash
# Default QEMU network (make run):
# - Host: 10.0.2.2
# - Guest (EYN-OS): 10.0.2.15 (or similar)

# Test from host machine:
echo "test message" | nc -u 127.0.0.1 10000
# Then in EYN-OS:
udp listen 10000
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
- ICMP (ping) support
- DHCP client for automatic IP configuration
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
