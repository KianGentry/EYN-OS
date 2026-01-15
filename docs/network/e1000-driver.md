# E1000 NIC Driver

The e1000 driver provides low-level control of Intel 82540EM and compatible network interface cards.

## Overview

**File**: `src/drivers/e1000.c`  
**Header**: `include/drivers/e1000.h`  
**Supported Devices**: Intel 82540EM (PCI ID: 8086:100E)

The driver uses Memory-Mapped I/O (MMIO) to communicate with the NIC, setting up transmit and receive descriptor rings for packet handling.

## Initialization Process

### 1. PCI Detection
```c
e1000_info* e1000_probe(void);
```

The driver scans the PCI bus for device ID 8086:100E and reads:
- Base Address Register 0 (BAR0) for MMIO address
- Device capabilities
- Link status

### 2. Device Initialization
```c
int e1000_init(void);
```

Initialization steps:
1. **Enable Bus Mastering**: Allow NIC to perform DMA
2. **Read MAC Address**: From EEPROM (address 0-2)
3. **Reset Device**: Software reset via CTRL register
4. **Allocate Descriptor Rings**:
   - RX ring: 16 descriptors × 2048 bytes each
   - TX ring: 8 descriptors × 2048 bytes each
5. **Configure Receiver**:
   - Enable receiver (RCTL.EN)
   - Store bad packets (RCTL.SBP) for debugging
   - Broadcast accept (RCTL.BAM)
   - Buffer size: 2KB (RCTL.BSIZE)
6. **Configure Transmitter**:
   - Enable transmitter (TCTL.EN)
   - Pad short frames (TCTL.PSP)
7. **Setup Interrupts**: (Future - currently polling)

## Register Access

All registers are accessed via MMIO at base address from BAR0:

```c
static inline uint32 e1000_read_reg(uint32 offset) {
    volatile uint32* reg = (volatile uint32*)(g_e1000.mmio_base + offset);
    return *reg;
}

static inline void e1000_write_reg(uint32 offset, uint32 val) {
    volatile uint32* reg = (volatile uint32*)(g_e1000.mmio_base + offset);
    *reg = val;
}
```

### Key Registers

| Offset | Name | Purpose |
|--------|------|---------|
| 0x0000 | CTRL | Device control (reset, link) |
| 0x0008 | STATUS | Device status, link state |
| 0x0014 | EECD | EEPROM control |
| 0x5400 | RAL | Receive address low (MAC bytes 0-3) |
| 0x5404 | RAH | Receive address high (MAC bytes 4-5) |
| 0x2800 | RDBAL | RX descriptor base (low) |
| 0x2808 | RDLEN | RX descriptor ring length |
| 0x2810 | RDH | RX descriptor head |
| 0x2818 | RDT | RX descriptor tail |
| 0x0100 | RCTL | Receiver control |
| 0x3800 | TDBAL | TX descriptor base (low) |
| 0x3808 | TDLEN | TX descriptor ring length |
| 0x3810 | TDH | TX descriptor head |
| 0x3818 | TDT | TX descriptor tail |
| 0x0400 | TCTL | Transmitter control |

## Descriptor Rings

### Receive Descriptor Format
```c
struct e1000_rx_desc {
    uint64 buffer_addr;    // Physical address of packet buffer
    uint16 length;         // Packet length
    uint16 checksum;       // Packet checksum
    uint8  status;         // Descriptor status
    uint8  errors;         // Error flags
    uint16 special;        // VLAN tag, etc.
} __attribute__((packed));
```

**Status Bits**:
- `DD` (0x01): Descriptor Done - packet received
- `EOP` (0x02): End of Packet
- Other bits for VLAN, checksum validation, etc.

### Transmit Descriptor Format
```c
struct e1000_tx_desc {
    uint64 buffer_addr;    // Physical address of packet buffer
    uint16 length;         // Packet length
    uint8  cso;            // Checksum offset
    uint8  cmd;            // Command byte
    uint8  status;         // Transmit status
    uint8  css;            // Checksum start
    uint16 special;        // VLAN, etc.
} __attribute__((packed));
```

**Command Bits**:
- `EOP` (0x01): End of Packet
- `IFCS` (0x02): Insert FCS (frame check sequence)
- `RS` (0x08): Report Status - set DD when done

## Packet Transmission

```c
int e1000_transmit(const void* data, uint16 len);
```

Process:
1. Get current tail index
2. Check if descriptor is available (DD bit clear)
3. Copy packet data to descriptor buffer
4. Set descriptor: length, cmd (EOP|IFCS|RS)
5. Update tail pointer (TDT register)
6. NIC sends packet asynchronously

## Packet Reception

```c
int e1000_receive(void* buffer, uint16* len);
```

Process:
1. Get current tail index
2. Check next descriptor for DD bit
3. If packet available:
   - Copy from descriptor buffer
   - Clear DD status
   - Advance tail pointer
4. Return packet length or 0 if none

The receiver uses a circular buffer; software must keep RDT one behind actual tail.

## Diagnostic Commands

### `e1000 probe`
Scan PCI for e1000 device and display:
- PCI bus/device/function
- Vendor/Device ID
- MMIO base address
- MAC address
- Link status

### `e1000 init`
Perform full initialization and report results.

### `e1000 regs`
Dump key register values:
- CTRL, STATUS
- MAC address registers
- RX/TX ring pointers
- Control registers

### `e1000 test`
Run basic sanity checks:
- Register read/write
- Link status verification
- MAC address validation
- Loopback test (future)

Options:
- `--expect-link up|down`: Assert link state
- `--expect-mac xx:xx:xx:xx:xx:xx`: Assert MAC address

## Memory Layout

The driver allocates memory for:
- **RX Ring**: 16 descriptors × 16 bytes = 256 bytes
- **RX Buffers**: 16 buffers × 2048 bytes = 32 KB
- **TX Ring**: 8 descriptors × 16 bytes = 128 bytes
- **TX Buffers**: 8 buffers × 2048 bytes = 16 KB

**Total**: ~48.5 KB per NIC

Buffers are allocated from kernel heap and must be physically contiguous (no scatter-gather yet).

## Link Status

Link state is read from STATUS register (bit 1):
```c
bool e1000_link_up(void) {
    uint32 status = e1000_read_reg(E1000_REG_STATUS);
    return (status & 0x02) != 0;
}
```

Link auto-negotiation is handled by hardware.

## Limitations

- **Single NIC**: Only first e1000 device supported
- **No MSI/MSI-X**: Using legacy interrupts (or polling)
- **No Scatter-Gather**: Single-buffer descriptors only
- **No Offloads**: Software handles checksums
- **No VLAN**: 802.1Q tags not processed
- **Fixed Buffer Size**: 2KB per descriptor

## QEMU Testing

QEMU emulates the 82540EM by default:
```bash
# Default (make run):
qemu-system-i386 -kernel kernel.bin

# Explicit e1000 model:
qemu-system-i386 -netdev user,id=net0 -device e1000,netdev=net0
```

QEMU's user-mode networking provides:
- DHCP server at 10.0.2.2
- Guest IPs in 10.0.2.0/24 range
- Port forwarding capability

## Reference

- [Intel 82540EP/EM Datasheet](https://www.intel.com/content/dam/doc/datasheet/82540ep-gbe-controller-datasheet.pdf)
- [OSDev Wiki: Intel 8254x](https://wiki.osdev.org/Intel_8254x)
- QEMU e1000 emulation: `hw/net/e1000.c` in QEMU source

## Related Files

- Driver: `src/drivers/e1000.c`
- Header: `include/drivers/e1000.h`
- PCI: `src/drivers/pci.c` (device enumeration)
- Network stack: `src/network/netstack.c` (uses driver)
- Commands: `src/utilities/shell/shell_commands.c` (`e1000*` commands)
