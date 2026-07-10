# Network Stack

EYN-OS implements a minimal but functional network stack supporting UDP/TCP over IPv4 with ARP and a DNS resolver.

## Overview

**File**: `src/network/netstack.c`  
**Header**: `include/network/netstack.h`

The stack is built in layers:
1. **Ethernet** - Frame parsing and transmission
2. **ARP** - IP to MAC address resolution
3. **IPv4** - Basic packet handling
4. **UDP** - Datagram sockets
5. **TCP** - Minimal client/server state machine
6. **DNS** - UDP A-record resolver

## Architecture

### Initialization

Boot now performs eager network bring-up from kernel init:
- `kmain` calls `net_init_e1000_default()` after IRQ/PIT setup.
- Failure is non-fatal, so systems without a usable NIC still boot.

Runtime initialization entry points:
- `net_init_e1000_default()` (driver + stack)
- `net_init(const netdev* dev)` (stack over a provided device)

Sets up:
- Network device reference (e1000)
- Local MAC address
- ARP cache
- UDP receive queue
- Statistics counters

Many network operations still include lazy init calls as a fallback when boot
did not initialize networking.

### Packet Reception Flow

```
Hardware → e1000_receive() → net_poll_rx() → Protocol handlers
                                               ├→ handle_arp()
                                               ├→ handle_ipv4()
                                               │   ├→ handle_udp()
                                               │   ├→ handle_tcp()
                                               │   └→ handle_icmp()
```

The `net_poll_rx()` function:
1. Polls e1000 for received frames
2. Parses Ethernet header
3. Dispatches by EtherType:
   - 0x0806: ARP
   - 0x0800: IPv4

## Protocol Details

### Ethernet Layer

**Frame Format**:
```
┌─────────────┬─────────────┬──────────┬─────────┬─────┐
│  Dst MAC    │  Src MAC    │ EtherType│ Payload │ FCS │
│  (6 bytes)  │  (6 bytes)  │ (2 bytes)│ (var)   │(4B) │
└─────────────┴─────────────┴──────────┴─────────┴─────┘
```

EtherType values:
- `0x0800`: IPv4
- `0x0806`: ARP

FCS (Frame Check Sequence) is handled by hardware.

### ARP (Address Resolution Protocol)

**Purpose**: Map IPv4 addresses to MAC addresses

**Packet Format** (28 bytes):
```c
struct arp_pkt {
    uint16 htype;    // Hardware type (1 = Ethernet)
    uint16 ptype;    // Protocol type (0x0800 = IPv4)
    uint8  hlen;     // Hardware addr len (6)
    uint8  plen;     // Protocol addr len (4)
    uint16 oper;     // Operation: 1=request, 2=reply
    uint8  sha[6];   // Sender hardware (MAC) addr
    uint8  spa[4];   // Sender protocol (IP) addr
    uint8  tha[6];   // Target hardware addr
    uint8  tpa[4];   // Target protocol addr
};
```

**Operations**:
1. **ARP Request** (oper=1):
   - Broadcast to FF:FF:FF:FF:FF:FF
   - "Who has IP X.X.X.X? Tell Y.Y.Y.Y"
   
2. **ARP Reply** (oper=2):
   - Unicast response
   - "X.X.X.X is at MAC aa:bb:cc:dd:ee:ff"

**Cache**:
- 4-entry cache with aging
- Entries expire after 30 seconds without refresh
- Gateway MAC is refreshed periodically (~8 seconds)
- Lookup: `arp_cache_lookup(ip[4]) → mac[6]`
- Update: Automatic on RX and ARP replies
- Resolve uses up to 3 retries with backoff

### IPv4 Layer

**Packet Format**:
```c
struct ipv4_hdr {
    uint8  ver_ihl;        // Version (4) | Header length
    uint8  dscp_ecn;       // Type of service
    uint16 total_len;      // Total length (header + data)
    uint16 id;             // Identification
    uint16 flags_frag_off; // Flags | Fragment offset
    uint8  ttl;            // Time to live
    uint8  proto;          // Protocol (17 = UDP)
    uint16 hdr_checksum;   // Header checksum
    uint8  src[4];         // Source IP
    uint8  dst[4];         // Destination IP
};
```

**Features**:
- Basic header parsing
- Checksum verification
- Protocol dispatch (UDP/TCP/ICMP)
- Drop IPv4 fragments (no reassembly)
- DF (Don't Fragment) set on TX

**Limitations**:
- No reassembly (fragments are dropped)
- No options parsing
- No routing table (single subnet)

**ICMP Error Tracking**:
- Destination Unreachable (type 3)
- Time Exceeded (type 11)
- Fragmentation Needed (type 3 code 4)
- Exposed via `netstat` counters

**Checksum Algorithm**:
```c
uint16 ipv4_checksum16(const void* data, uint32 len) {
    // 16-bit one's complement sum
    uint32 sum = 0;
    for each 16-bit word:
        sum += word;
    while (sum >> 16):
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}
```

### UDP Layer

**Datagram Format**:
```c
struct udp_hdr {
    uint16 src_port;   // Source port
    uint16 dst_port;   // Destination port
    uint16 len;        // Length (header + data)
    uint16 checksum;   // Checksum (optional, 0 = skip)
};
```

**Features**:
- Port-based delivery
- Receive queue (8 slots)
- Statistics tracking
- Simple socket-like API
- Socket bindings (per-port queues)

### TCP Layer (Minimal)

**Features**:
- Active open (SYN → SYN-ACK → ACK)
- Single connection at a time
- Data send with PSH+ACK
- FIN close
- Passive listen (single connection)
- RX payload queue (non-blocking read)
- Minimal retransmission (SYN/SYN-ACK/DATA/FIN)

**Limitations**:
- No window scaling or options
- No stream reassembly (payloads are queued as received)
- No out-of-order reassembly or receive window management

**Receive Queue**:
```c
struct tcp_rx_slot {
    uint8 valid;                    // Slot occupied
    net_tcp_rx_packet pkt;          // Packet data
};

// Up to 8 packets buffered
tcp_rx_slot tcp_rxq[8];
```

**Statistics**:
```c
struct net_tcp_stats {
    uint32 tcp_syn_sent;
    uint32 tcp_synack_rx;
    uint32 tcp_ack_tx;
    uint32 tcp_data_tx;
    uint32 tcp_data_rx;
    uint32 tcp_fin_tx;
    uint32 tcp_fin_rx;
    uint32 tcp_rst_rx;
    uint32 tcp_listen_syn_rx;
    uint32 tcp_conn_established;
    uint32 tcp_rx_enqueued;
    uint32 tcp_rx_dropped;
};
```

### DNS Resolver (Minimal)

**Purpose**: Resolve hostnames to IPv4 addresses using UDP queries.

**Features**:
- RFC 1035 wire format, A-record queries only
- Recursion desired (RD=1)
- Nameserver from `/etc/resolv.conf`, fallback to runtime `netcfg` DNS
- Fixed 512-byte UDP payload cap (no EDNS)

## API

### Initialization
```c
int net_init_e1000_default(void);
int net_init(const netdev* dev);
```
Initialize the network stack (or no-op if already initialized).

### Polling
```c
void net_poll_rx(void);
```
Poll for incoming packets. Called by command handlers.

### UDP Operations

#### Listen for packets
```c
int net_udp_listen(uint16 port, uint32 timeout_ms);
```
- Blocks until packet arrives on `port` or timeout
- Returns 0 on success, -1 on timeout
- Kicks watchdog to prevent hang

#### Retrieve received packet
```c
int net_udp_get_packet(net_udp_rx_packet* out);
```
- Pop next packet from queue
- Returns 1 if packet available, 0 if empty

#### Send UDP packet
```c
int net_udp_send_to(const uint8 dst_ip[4], uint16 dst_port, 
                    uint16 src_port, const void* data, uint16 len);
```
- Performs ARP lookup for destination MAC
- Constructs IPv4 + UDP headers
- Transmits via e1000

#### Get statistics
```c
void net_udp_get_stats(net_udp_stats* out);
```
Returns packet counters.

#### Clear receive queue
```c
void net_udp_drain_rx(void);
```
Discards all queued packets.

## Packet Flow Examples

### Sending UDP Packet

1. User: `e1000 udp-send 10.0.2.2 5000 Hello`
2. Command handler calls `net_udp_send_to()`
3. ARP lookup for 10.0.2.2 → MAC
   - If not cached, send ARP request
   - Wait for ARP reply
   - Cache result
4. Build Ethernet frame:
   - Dst MAC from ARP
   - Src MAC from NIC
   - EtherType = 0x0800 (IPv4)
5. Build IPv4 header:
   - Protocol = 17 (UDP)
   - Compute header checksum
6. Build UDP header:
   - Ports, length
   - Checksum = 0 (skip)
7. Copy payload: "Hello"
8. Call `e1000_transmit(frame, total_len)`

### Receiving UDP Packet

1. `net_poll_rx()` calls `e1000_receive()`
2. Parse Ethernet: EtherType = 0x0800
3. Parse IPv4: Protocol = 17 (UDP)
4. Verify IPv4 checksum
5. Parse UDP header
6. Check receive queue space
7. Store in `udp_rxq[]`:
   - Source IP/port
   - Destination port
   - Payload data
   - Length
8. Update statistics: `rx_packets++`

### ARP Request/Reply

**Request** (looking up 10.0.2.2):
```
Ethernet: FF:FF:FF:FF:FF:FF ← [our MAC]
ARP: Who has 10.0.2.2? Tell 10.0.2.15
     oper=1, tha=00:00:00:00:00:00
```

**Reply** (from 10.0.2.2):
```
Ethernet: [our MAC] ← 52:54:00:12:34:56
ARP: 10.0.2.2 is at 52:54:00:12:34:56
     oper=2, tha=[our MAC]
```

Cache updated: `10.0.2.2 → 52:54:00:12:34:56`

## Network Configuration

Runtime-configurable via the netstack configuration layer and the `netcfg` shell command.

Defaults match QEMU user-mode networking (slirp):
- Local IP: 10.0.2.15
- Gateway/host: 10.0.2.2
- Netmask: 255.255.255.0
- DNS: 10.0.2.3

Key APIs (see `include/network/netstack.h`):
- `net_config_get(...)` / `net_config_set(...)`
- `net_config_set_defaults()`
- `net_get_local_ip(...)`

For QEMU user-mode networking:
- Gateway/host: 10.0.2.2
- Guest (EYN-OS): 10.0.2.15
- Netmask: 255.255.255.0

## Debugging

Enable network tracing:
```c
#define NET_DEBUG 1
```

Prints:
- Received frame details (src/dst MAC, EtherType)
- ARP operations (requests/replies)
- IPv4 packet info (src/dst IP, protocol)
- UDP delivery (port, length)

Use `e1000 udp-stats` to monitor:
- Packet counts
- Dropped packets (queue overflow)
- Checksum errors

Commands:
- `e1000 udp-stats`
- `netstat`

## Limitations

1. **No TCP**: Only UDP implemented
2. **Limited ICMP**: Only ICMP echo (ping) request/reply
3. **No Fragmentation**: Max packet ~1500 bytes
4. **Static IP**: No DHCP
5. **Small Queue**: 8 packets max
6. **No Multicast**: Only unicast + broadcast
7. **Basic ARP**: 4-entry cache, no timeout
8. **No Routing**: Single subnet only

## Performance

Typical packet rates (QEMU):
- **TX**: ~1000 packets/sec
- **RX**: Limited by polling frequency

Latency:
- Round-trip (EYN-OS ↔ Host): ~1-5ms in QEMU

Memory usage:
- ARP cache: 4 entries × 11 bytes = 44 bytes
- UDP queue: 8 slots × ~1536 bytes = ~12 KB
- Statistics: ~16 bytes

## Future Enhancements

- TCP implementation (3-way handshake, retransmission, flow control)
- DHCP client
- DNS resolver
- Multiple sockets (fd-like interface)
- IPv4 fragmentation
- Interrupt-driven RX (no polling)
- Larger receive buffers
- Socket options (SO_REUSEADDR, etc.)

## Related Documentation

- [e1000-driver.md](e1000-driver.md) - NIC driver details
- [network-commands.md](network-commands.md) - Shell commands
- [../api/syscalls.md](../api/syscalls.md) - Future: network syscalls
