# Network Shell Commands

EYN-OS provides several shell commands for network operations and diagnostics.

## E1000 NIC Commands

### e1000 init
Initialize the Intel e1000 network interface card.

```bash
e1000 init
```

**What it does**:
1. Detects e1000 device via PCI scan
2. Enables bus mastering
3. Reads MAC address from EEPROM
4. Performs software reset
5. Allocates TX/RX descriptor rings
6. Configures receiver and transmitter
7. Enables the NIC

**Output**:
```
E1000: Found device at PCI 00:03.0
E1000: MAC address: 52:54:00:12:34:56
E1000: Link is UP
E1000: Initialized successfully
```

![e1000 init](image.png)

**Troubleshooting**:
- "No e1000 device found": NIC not present (check QEMU config)
- "Initialization failed": Check PCI access or memory allocation

---

### e1000 probe
Scan for e1000 device and display basic info (read-only).

```bash
e1000probe
```

**Output**:
```
E1000 Probe Results:
  PCI: 00:03.0
  Vendor: 8086 Device: 100E
  MMIO Base: 0xFEBC0000
  MAC: 52:54:00:12:34:56
  Link: UP
```

---

### e1000 regs
Dump key e1000 register values for debugging.

```bash
e1000 regs
```

**Output**:
```
E1000 Registers:
  CTRL:   0x00040248
  STATUS: 0x80080783 (Link UP)
  RCTL:   0x04008002
  TCTL:   0x0004010A
  RDH:    5  RDT: 4
  TDH:    2  TDT: 2
```

![e1000 regs](image-1.png)

---

### e1000 test
Run diagnostic tests on e1000 device.

```bash
# Basic test
e1000 test

# Test with expected link state
e1000 test --expect-link up

# Test with expected MAC address
e1000 test --expect-mac 52:54:00:12:34:56
```

**Tests**:
- Register access (read/write)
- Link status verification
- MAC address validation
- (Future: loopback test)

---

## PCI Commands

### pci scan
Enumerate all PCI devices on the system.

```bash
pci scan
```

**Output**:
```
PCI Devices Found:
  00:00.0 - 8086:1237 (Host Bridge)
  00:01.0 - 8086:7000 (ISA Bridge)
  00:01.1 - 8086:7010 (IDE Controller)
  00:01.3 - 8086:7113 (Power Management)
  00:02.0 - 1234:1111 (VGA Controller)
  00:03.0 - 8086:100E (Ethernet Controller) [e1000]
```

**Tip**: e1000 usually shows as `8086:100E`.

---

## UDP Commands

### udp listen
Listen for incoming UDP packets on a specified port.

```bash
udp listen <port>
```

**Arguments**:
- `<port>`: Port number (0-65535)

**Example**:
```bash
udp listen 10000
```

**Behavior**:
- Blocks until packet received or Ctrl+C pressed
- Polls for packets every 10ms
- Kicks watchdog to prevent hang detection
- Displays received packet details

**Output (on packet received)**:
```
UDP packet received:
  From: 10.0.2.2:5000
  To: 10.0.2.15:10000
  Length: 12 bytes
  Data: Hello World!
```

![sending and receiving](image-2.png)

**Testing from host**:
```bash
# On host machine (Linux/Mac):
echo "test message" | nc -u 127.0.0.1 10000

# Or using Python:
python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(b'Hello', ('10.0.2.15', 10000))"
```

**Notes**:
- QEMU user-mode networking forwards localhost ports to guest
- Default QEMU network: Host=10.0.2.2, Guest=10.0.2.15
- Press Ctrl+C to stop listening

---

### udp send
Send a UDP packet to a remote host.

```bash
udp send <ip> <port> <message>
```

**Arguments**:
- `<ip>`: Destination IPv4 address (dotted decimal)
- `<port>`: Destination port number
- `<message>`: Text message to send (rest of command line)

**Example**:
```bash
udp send 10.0.2.2 5000 Hello from EYN-OS
```

**What it does**:
1. Performs ARP lookup for destination IP
   - Sends ARP request if not cached
   - Waits for ARP reply
2. Constructs UDP packet with message
3. Wraps in IPv4 packet
4. Wraps in Ethernet frame
5. Transmits via e1000

**Output**:
```
ARP: Looking up 10.0.2.2...
ARP: Resolved to 52:54:00:12:34:56
UDP: Sent 17 bytes to 10.0.2.2:5000
```

**Common destinations**:
- `10.0.2.2`: QEMU host (in user-mode networking)
- `10.0.2.3`: QEMU DNS (if enabled)

---

### udp stats
Display UDP statistics.

```bash
udp stats
```

**Output**:
```
UDP Statistics:
  RX Packets:      42
  TX Packets:      15
  RX Dropped:      0
  Bad Checksums:   0
  Queue Usage:     2/8
```

![udp stats showing queue usage](image-3.png)

**Fields**:
- **RX Packets**: Total packets received
- **TX Packets**: Total packets sent
- **RX Dropped**: Packets dropped (queue full)
- **Bad Checksums**: Packets with invalid checksums
- **Queue Usage**: Current receive queue usage

---

### udp drain
Clear the UDP receive queue.

```bash
udp drain
```

**Output**:
```
UDP: Drained 3 packets from queue
```

**Use cases**:
- Clear old packets before listening
- Reset state after testing
- Free receive buffers

---

## Network Initialization Workflow

Typical startup sequence:

```bash
# 1. Initialize system
init

# 2. Scan PCI to verify NIC present
pci scan

# 3. Initialize e1000 NIC
e1000 init

# 4. Test connectivity
udp send 10.0.2.2 5000 ping

# 5. Listen for responses
udp listen 5001
```

---

## Network Testing Examples

### Echo Server (Simple)
```bash
# In one terminal (EYN-OS):
while true; do
    udp listen 10000
done

# In another terminal (host):
echo "test" | nc -u 127.0.0.1 10000
```

### Send/Receive Test
```bash
# EYN-OS side:
e1000 init
udp listen 10000

# Host side:
nc -u 10.0.2.15 10000
type messages and press Enter
```

### Statistics Monitoring
```bash
# Send some packets
udp send 10.0.2.2 5000 test1
udp send 10.0.2.2 5000 test2
udp send 10.0.2.2 5000 test3

# Check stats
udp stats
```

---

## Troubleshooting

### "No e1000 device found"
- **Cause**: NIC not present or not detected
- **Fix**: Check QEMU command includes e1000 device
- **QEMU**: `-netdev user,id=net0 -device e1000,netdev=net0`

### "ARP timeout"
- **Cause**: Destination not responding to ARP
- **Fix**: Check IP address is reachable (usually 10.0.2.x in QEMU)
- **Debug**: Use `e1000 regs` to check if receiver enabled

### "UDP: Queue full"
- **Cause**: Receive queue overflow (8 packets max)
- **Fix**: Call `udp drain` to clear queue
- **Future**: Increase queue size or process packets faster

### "Link is DOWN"
- **Cause**: NIC not properly initialized or no link
- **Fix**: 
  - Try `e1000 init` again
  - Check `e1000 regs` for STATUS register
  - Verify QEMU network configuration

### "Command not found: udp"
- **Cause**: Network commands not loaded
- **Fix**: Run `load` to load streaming commands

---

## Configuration

### Default Network Settings
Currently hardcoded in `src/network/netstack.c`:

```c
Local IP:   10.0.2.15
Gateway:    10.0.2.2
Netmask:    255.255.255.0
```

### QEMU User-Mode Networking
Default QEMU network topology:
```
Host:     10.0.2.2  (accessible from guest)
Gateway:  10.0.2.2
DNS:      10.0.2.3
Guest:    10.0.2.15 (EYN-OS)
```

Port forwarding (host → guest):
```bash
qemu-system-i386 ... -netdev user,id=net0,hostfwd=udp::5555-:10000
# Now host port 5555 forwards to EYN-OS port 10000
```

---

## Performance Tips

1. **Minimize ARP requests**: Packets to same host use cached MAC
2. **Batch operations**: Send multiple packets without delay
3. **Clear queue**: Use `udp drain` before important receive operations
4. **Monitor stats**: Check `udp stats` for dropped packets

---

## Related Documentation

- [E1000 Driver](e1000-driver.md) - NIC driver internals
- [Network Stack](network-stack.md) - Protocol implementation
- [Syscalls](../api/syscalls.md) - Future: network syscalls for userland
- [Command Reference](../command-reference.md) - All shell commands

---

## Command Summary

| Command | Purpose | Example |
|---------|---------|---------|
| `e1000 init` | Initialize NIC | `e1000 init` |
| `e1000 probe` | Probe NIC | `e1000probe` |
| `e1000 regs` | Dump registers | `e1000 regs` |
| `e1000 test` | Run diagnostics | `e1000 test --expect-link up` |
| `pci scan` | List PCI devices | `pci scan` |
| `udp listen` | Receive packets | `udp listen 10000` |
| `udp send` | Send packet | `udp send 10.0.2.2 5000 msg` |
| `udp stats` | Show statistics | `udp stats` |
| `udp drain` | Clear queue | `udp drain` |
