#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <mm/vmm.h>
#include <vga.h>
#include <string.h>

// e1000 register offsets (subset).
//
// Why these specific regs:
// - CTRL/STATUS confirms MMIO reads are sane and the device is alive.
// - RAL/RAH gives us the programmed MAC address QEMU expects us to use.
#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_RAL0   0x5400
#define E1000_REG_RAH0   0x5404

// TX registers (legacy descriptor mode)
#define E1000_REG_TDBAL  0x3800
#define E1000_REG_TDBAH  0x3804
#define E1000_REG_TDLEN  0x3808
#define E1000_REG_TDH    0x3810
#define E1000_REG_TDT    0x3818
#define E1000_REG_TCTL   0x0400
#define E1000_REG_TIPG   0x0410

// RX registers (legacy descriptor mode)
#define E1000_REG_RDBAL  0x2800
#define E1000_REG_RDBAH  0x2804
#define E1000_REG_RDLEN  0x2808
#define E1000_REG_RDH    0x2810
#define E1000_REG_RDT    0x2818
#define E1000_REG_RCTL   0x0100

// Use a fixed high kernel VA window for device MMIO.
//
// Why a fixed VA:
// - Keeps bring-up simple and avoids depending on a full virtual allocator.
// - Avoids colliding with the kernel heap (starts at 0xD0000000).
//
// This is intentionally narrow: we only map what we need for probing.
#define E1000_MMIO_VA_BASE   0xF0000000u
#define E1000_MMIO_MAP_BYTES 0x7000u  /* covers up through RAH0 */

// TX ring settings.
//
// Why small:
// - This is a bring-up milestone, not a throughput benchmark.
// - Small rings reduce RAM use and keep debugging simple.
#define E1000_TX_RING_COUNT 16u
#define E1000_TX_BUF_SIZE   2048u

#define E1000_RX_RING_COUNT 32u
#define E1000_RX_BUF_SIZE   2048u

// TX descriptor command/status bits.
#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)
#define E1000_TXD_STAT_DD  (1u << 0)

typedef struct __attribute__((packed)) e1000_tx_desc {
    uint64 addr;
    uint16 length;
    uint8 cso;
    uint8 cmd;
    uint8 status;
    uint8 css;
    uint16 special;
} e1000_tx_desc;

typedef struct __attribute__((packed)) e1000_rx_desc {
    uint64 addr;
    uint16 length;
    uint16 csum;
    uint8 status;
    uint8 errors;
    uint16 special;
} e1000_rx_desc;

typedef struct e1000_state {
    int mmio_mapped;
    uint8 bus, dev, fun;
    uint32 bar0;

    int tx_ready;
    uint32 tx_ring_phys;
    e1000_tx_desc* tx_ring;
    uint32 tx_buf_phys;
    uint8* tx_bufs;
    uint32 tx_tail;

    int rx_ready;
    uint32 rx_ring_phys;
    e1000_rx_desc* rx_ring;
    uint32 rx_buf_phys;
    uint8* rx_bufs;
    uint32 rx_cur;

    uint8 mac[6];
} e1000_state;

static e1000_state g_e1000;

typedef struct __attribute__((packed)) eth_hdr {
    uint8 dst[6];
    uint8 src[6];
    uint16 ethertype_be;
} eth_hdr;

static uint16 be16(uint16 x)
{
    return (uint16)((x >> 8) | (x << 8));
}

static void print_mac(const uint8 mac[6])
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

static void e1000_map_mmio(uint32 phys_base)
{
    // MMIO should not be cached; PCD is the usual safe choice.
    // We also set PWT to avoid surprises on some platforms.
    const uint32 flags = PTE_RW | PTE_PCD | PTE_PWT;

    uint32 phys_page = phys_base & PAGE_MASK;
    uint32 va = E1000_MMIO_VA_BASE;

    for (uint32 off = 0; off < E1000_MMIO_MAP_BYTES; off += PAGE_SIZE) {
        if (vmm_map_page(&vmm_kernel_as, va + off, phys_page + off, flags) != 0) {
            // Mapping failure will almost certainly lead to a page fault on access.
            // Keep the message short; callers can treat probe failure as "NIC not ready".
            printf("%cError: e1000 MMIO map failed.\n", 255, 0, 0);
            return;
        }
    }
}

static inline uint32 e1000_mmio_read32(uint32 reg)
{
    volatile uint32* p = (volatile uint32*)(E1000_MMIO_VA_BASE + reg);
    return *p;
}

static inline void e1000_mmio_write32(uint32 reg, uint32 value)
{
    volatile uint32* p = (volatile uint32*)(E1000_MMIO_VA_BASE + reg);
    *p = value;
}

static inline void e1000_compiler_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

static int e1000_find_first(uint8* out_bus, uint8* out_dev, uint8* out_fun, uint32* out_bar0)
{
    // Minimal device discovery: find the first Intel 82540EM/"e1000" function.
    // We can grow this into a more general NIC registry later.
    for (uint16 bus = 0; bus < 256; bus++) {
        for (uint8 dev = 0; dev < 32; dev++) {
            uint16 vendor0 = pci_read_config_word((uint8)bus, dev, 0, 0x00);
            if (vendor0 == 0xFFFFu) continue;

            uint8 header0 = pci_read_config_byte((uint8)bus, dev, 0, 0x0E);
            uint8 max_fun = (header0 & 0x80u) ? 7 : 0;

            for (uint8 fun = 0; fun <= max_fun; fun++) {
                uint16 vendor = pci_read_config_word((uint8)bus, dev, fun, 0x00);
                if (vendor == 0xFFFFu) continue;

                uint16 device = pci_read_config_word((uint8)bus, dev, fun, 0x02);
                if (vendor == 0x8086u && device == 0x100Eu) {
                    uint32 bar0 = pci_read_config_dword((uint8)bus, dev, fun, 0x10);
                    uint8 is_io = (uint8)(bar0 & 0x1u);
                    if (is_io) {
                        // QEMU's e1000 should expose MMIO; if it's I/O space something is off.
                        return -2;
                    }

                    if (out_bus) *out_bus = (uint8)bus;
                    if (out_dev) *out_dev = dev;
                    if (out_fun) *out_fun = fun;
                    if (out_bar0) *out_bar0 = (bar0 & ~0xFu);
                    return 0;
                }
            }
        }
    }

    return -1;
}

static void e1000_enable_pci_bus_master(uint8 bus, uint8 dev, uint8 fun)
{
    // DMA (TX/RX rings) requires bus mastering. Enabling it early is safe as
    // long as we haven't programmed the device to actually DMA anywhere.
    uint32 dword = pci_read_config_dword(bus, dev, fun, 0x04);
    uint16 cmd = (uint16)(dword & 0xFFFFu);

    cmd |= (1u << 1); /* MEM space */
    cmd |= (1u << 2); /* bus master */

    dword = (dword & 0xFFFF0000u) | (uint32)cmd;
    pci_write_config_dword(bus, dev, fun, 0x04, dword);
}

int e1000_probe(e1000_probe_info* out)
{
    uint8 bus = 0, dev = 0, fun = 0;
    uint32 bar0 = 0;

    int rc = e1000_find_first(&bus, &dev, &fun, &bar0);
    if (rc == -1) {
        return -1;
    }
    if (rc == -2) {
        return -1;
    }

    e1000_enable_pci_bus_master(bus, dev, fun);

    // Map the device registers into kernel VA so MMIO reads won't fault.
    // We keep a single fixed mapping window for now (one NIC expected in QEMU).
    if (!g_e1000.mmio_mapped || g_e1000.bar0 != bar0) {
        e1000_map_mmio(bar0);
        g_e1000.mmio_mapped = 1;
    }
    g_e1000.bus = bus;
    g_e1000.dev = dev;
    g_e1000.fun = fun;
    g_e1000.bar0 = bar0;

    uint32 ctrl = e1000_mmio_read32(E1000_REG_CTRL);
    uint32 status = e1000_mmio_read32(E1000_REG_STATUS);

    // Link-up bit in STATUS is bit 1 for e1000.
    int link_up = (status & (1u << 1)) ? 1 : 0;

    uint32 ral = e1000_mmio_read32(E1000_REG_RAL0);
    uint32 rah = e1000_mmio_read32(E1000_REG_RAH0);

    uint8 mac0 = (uint8)((ral >> 0) & 0xFFu);
    uint8 mac1 = (uint8)((ral >> 8) & 0xFFu);
    uint8 mac2 = (uint8)((ral >> 16) & 0xFFu);
    uint8 mac3 = (uint8)((ral >> 24) & 0xFFu);
    uint8 mac4 = (uint8)((rah >> 0) & 0xFFu);
    uint8 mac5 = (uint8)((rah >> 8) & 0xFFu);

    g_e1000.mac[0] = mac0;
    g_e1000.mac[1] = mac1;
    g_e1000.mac[2] = mac2;
    g_e1000.mac[3] = mac3;
    g_e1000.mac[4] = mac4;
    g_e1000.mac[5] = mac5;

    if (out) {
        out->bus = bus;
        out->device = dev;
        out->function = fun;
        out->bar0 = bar0;
        out->ctrl = ctrl;
        out->status = status;
        out->link_up = link_up;
        out->mac[0] = mac0;
        out->mac[1] = mac1;
        out->mac[2] = mac2;
        out->mac[3] = mac3;
        out->mac[4] = mac4;
        out->mac[5] = mac5;
    }
    return 0;
}

static int e1000_tx_init_once(void)
{
    if (g_e1000.tx_ready) return 0;

    // Ensure we can talk to the device and have its BAR0 + MAC.
    // Why probe first:
    // - Verifies MMIO access works before we touch any TX registers.
    // - Gives us a source MAC for test frames.
    e1000_probe_info info;
    if (e1000_probe(&info) != 0) {
        return -1;
    }

    // Allocate DMA memory.
    //
    // Why contiguous:
    // - e1000 descriptors/buffers only require physical addresses, but using
    //   contiguous frames simplifies bookkeeping and avoids edge cases.
    const uint32 ring_bytes = (uint32)(E1000_TX_RING_COUNT * (uint32)sizeof(e1000_tx_desc));
    const uint32 ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    const uint32 buf_bytes = E1000_TX_RING_COUNT * E1000_TX_BUF_SIZE;
    const uint32 buf_pages = (buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32 ring_phys = frame_alloc_contiguous(ring_pages);
    if (ring_phys == 0) return -2;
    uint32 buf_phys = frame_alloc_contiguous(buf_pages);
    if (buf_phys == 0) return -3;

    g_e1000.tx_ring_phys = ring_phys;
    g_e1000.tx_ring = (e1000_tx_desc*)(KERNEL_BASE + ring_phys);
    g_e1000.tx_buf_phys = buf_phys;
    g_e1000.tx_bufs = (uint8*)(KERNEL_BASE + buf_phys);
    g_e1000.tx_tail = 0;

    memset(g_e1000.tx_ring, 0, ring_pages * PAGE_SIZE);
    memset(g_e1000.tx_bufs, 0, buf_pages * PAGE_SIZE);

    // Program descriptor ring base/len.
    // The spec wants TDLEN to be a multiple of 128.
    e1000_mmio_write32(E1000_REG_TDBAL, ring_phys);
    e1000_mmio_write32(E1000_REG_TDBAH, 0);
    e1000_mmio_write32(E1000_REG_TDLEN, ring_bytes);

    // Start with empty ring.
    e1000_mmio_write32(E1000_REG_TDH, 0);
    e1000_mmio_write32(E1000_REG_TDT, 0);

    // Configure transmit control.
    //
    // Why these bits:
    // - EN enables transmission.
    // - PSP pads short frames up to the minimum Ethernet size.
    // - CT/COLD are collision tuning fields required by the hardware.
    //   Values here are the common "works everywhere" defaults.
    uint32 tctl = 0;
    tctl |= (1u << 1);  // EN
    tctl |= (1u << 3);  // PSP
    tctl |= (0x10u << 4);  // CT
    tctl |= (0x40u << 12); // COLD
    e1000_mmio_write32(E1000_REG_TCTL, tctl);

    // Inter-packet gap (recommended defaults for 8254x).
    uint32 tipg = 0;
    tipg |= 10u;
    tipg |= (8u << 10);
    tipg |= (6u << 20);
    e1000_mmio_write32(E1000_REG_TIPG, tipg);

    g_e1000.tx_ready = 1;
    return 0;
}

static int e1000_tx_send_frame(const void* frame, uint32 len)
{
    if (!frame || len == 0 || len > E1000_TX_BUF_SIZE) return -1;
    if (e1000_tx_init_once() != 0) return -2;

    uint32 idx = g_e1000.tx_tail % E1000_TX_RING_COUNT;
    e1000_tx_desc* d = &g_e1000.tx_ring[idx];

    uint8* buf = g_e1000.tx_bufs + (idx * E1000_TX_BUF_SIZE);
    uint32 buf_phys = g_e1000.tx_buf_phys + (idx * E1000_TX_BUF_SIZE);

    // Copy the frame into the DMA buffer the NIC will read.
    memcpy(buf, frame, len);

    // Give the NIC a fresh descriptor.
    //
    // Why RS (report status):
    // - Lets us poll DD to confirm the device consumed the descriptor.
    memset(d, 0, sizeof(*d));
    d->addr = (uint64)buf_phys;
    d->length = (uint16)len;
    d->cmd = (uint8)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);
    d->status = 0;

    // Ensure descriptor writes are visible before updating TDT.
    e1000_compiler_barrier();

    g_e1000.tx_tail = (idx + 1) % E1000_TX_RING_COUNT;
    e1000_mmio_write32(E1000_REG_TDT, g_e1000.tx_tail);

    // Poll for completion (DD set by hardware).
    // Why polling first:
    // - Interrupts are the next step, but polling is deterministic for bring-up.
    for (uint32 spin = 0; spin < 1000000u; spin++) {
        if (d->status & E1000_TXD_STAT_DD) return 0;
    }

    return -3;
}

static int e1000_rx_init_once(void)
{
    if (g_e1000.rx_ready) return 0;

    // Like TX, do a probe first so BAR0/MMIO and MAC are stable.
    e1000_probe_info info;
    if (e1000_probe(&info) != 0) return -1;

    const uint32 ring_bytes = (uint32)(E1000_RX_RING_COUNT * (uint32)sizeof(e1000_rx_desc));
    const uint32 ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    const uint32 buf_bytes = E1000_RX_RING_COUNT * E1000_RX_BUF_SIZE;
    const uint32 buf_pages = (buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32 ring_phys = frame_alloc_contiguous(ring_pages);
    if (ring_phys == 0) return -2;
    uint32 buf_phys = frame_alloc_contiguous(buf_pages);
    if (buf_phys == 0) return -3;

    g_e1000.rx_ring_phys = ring_phys;
    g_e1000.rx_ring = (e1000_rx_desc*)(KERNEL_BASE + ring_phys);
    g_e1000.rx_buf_phys = buf_phys;
    g_e1000.rx_bufs = (uint8*)(KERNEL_BASE + buf_phys);
    g_e1000.rx_cur = 0;

    memset(g_e1000.rx_ring, 0, ring_pages * PAGE_SIZE);
    memset(g_e1000.rx_bufs, 0, buf_pages * PAGE_SIZE);

    // Give each descriptor a buffer the NIC can DMA into.
    for (uint32 i = 0; i < E1000_RX_RING_COUNT; i++) {
        uint32 bphys = g_e1000.rx_buf_phys + (i * E1000_RX_BUF_SIZE);
        g_e1000.rx_ring[i].addr = (uint64)bphys;
        g_e1000.rx_ring[i].status = 0;
    }

    // Program ring base/len.
    e1000_mmio_write32(E1000_REG_RDBAL, ring_phys);
    e1000_mmio_write32(E1000_REG_RDBAH, 0);
    e1000_mmio_write32(E1000_REG_RDLEN, ring_bytes);

    // Initialize head/tail.
    // Hardware owns [RDH..RDT] as the receive queue; we set RDT to the last
    // available descriptor so the NIC can start writing immediately.
    e1000_mmio_write32(E1000_REG_RDH, 0);
    e1000_mmio_write32(E1000_REG_RDT, E1000_RX_RING_COUNT - 1);

    // Enable receiver.
    // EN: enable receive
    // BAM: accept broadcast
    // SECRC: strip Ethernet CRC from received buffers
    // BSIZE=2048 (00)
    uint32 rctl = 0;
    rctl |= (1u << 1);   // EN
    rctl |= (1u << 15);  // BAM
    rctl |= (1u << 26);  // SECRC
    e1000_mmio_write32(E1000_REG_RCTL, rctl);

    g_e1000.rx_ready = 1;
    return 0;
}

// Poll and print up to `max_packets` frames.
// Returns number of packets printed.
int e1000_rx_poll_and_print(int max_packets, int spin_limit)
{
    if (max_packets <= 0) return 0;
    if (spin_limit <= 0) spin_limit = 1000000;
    if (e1000_rx_init_once() != 0) return -1;

    int printed = 0;
    for (int spin = 0; spin < spin_limit && printed < max_packets; spin++) {
        uint32 idx = g_e1000.rx_cur % E1000_RX_RING_COUNT;
        e1000_rx_desc* d = &g_e1000.rx_ring[idx];
        if (!(d->status & 0x01u)) {
            continue; // DD not set yet
        }

        uint8* buf = g_e1000.rx_bufs + (idx * E1000_RX_BUF_SIZE);
        uint32 len = (uint32)d->length;

        if (len >= (uint32)sizeof(eth_hdr)) {
            eth_hdr* eh = (eth_hdr*)buf;
            uint16 et = be16(eh->ethertype_be);

            printf("RX %d bytes et=0x%04x dst=", (int)len, (unsigned)et);
            print_mac(eh->dst);
            printf(" src=");
            print_mac(eh->src);
            printf("\n");
        } else {
            printf("RX %d bytes\n", (int)len);
        }

        printed++;

        // Hand the descriptor back to hardware.
        d->status = 0;
        d->length = 0;
        e1000_compiler_barrier();

        e1000_mmio_write32(E1000_REG_RDT, idx);
        g_e1000.rx_cur = (idx + 1) % E1000_RX_RING_COUNT;
    }

    return printed;
}

int e1000_tx_test_send(const char* message)
{
    // Build a small Ethernet frame we can transmit without any IP stack.
    //
    // Why broadcast:
    // - Avoids ARP/neighbor discovery; it's guaranteed to be deliverable at L2.
    // - This is strictly a TX-path validation checkpoint.
    uint8 frame[64];
    memset(frame, 0, sizeof(frame));

    // dst = ff:ff:ff:ff:ff:ff
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    // src = NIC MAC
    for (int i = 0; i < 6; i++) frame[6 + i] = g_e1000.mac[i];
    // EtherType = 0x88B5 (locally used/test)
    frame[12] = 0x88;
    frame[13] = 0xB5;

    // Payload
    uint32 payload_off = 14;
    uint32 payload_cap = (uint32)sizeof(frame) - payload_off;
    uint32 payload_len = 0;
    if (!message) message = "EYN-OS e1000 tx-test";
    while (message[payload_len] && payload_len < payload_cap) {
        frame[payload_off + payload_len] = (uint8)message[payload_len];
        payload_len++;
    }

    // Ethernet minimum frame size (without FCS) is 60 bytes.
    uint32 total_len = payload_off + payload_len;
    if (total_len < 60u) total_len = 60u;
    if (total_len > (uint32)sizeof(frame)) total_len = (uint32)sizeof(frame);

    int rc = e1000_tx_send_frame(frame, total_len);
    if (rc == 0) {
        printf("%cTX ok (%d bytes).\n", 0, 255, 0, (int)total_len);
    } else {
        printf("%cTX failed (%d).\n", 255, 0, 0, rc);
    }
    return rc;
}

int e1000_probe_and_print(void)
{
    e1000_probe_info info;
    int rc = e1000_probe(&info);
    if (rc == -1) {
        printf("%cError: e1000 not found (expected 8086:100E).\n", 255, 0, 0);
        return -1;
    }
    if (rc == -2) {
        printf("%cError: e1000 BAR0 is I/O space, expected MMIO.\n", 255, 0, 0);
        return -1;
    }
    if (rc != 0) {
        printf("%cError: e1000 probe failed (%d).\n", 255, 0, 0, rc);
        return -1;
    }

    printf("e1000 @ %02x:%02x.%d bar0=%08x\n",
           (unsigned)info.bus, (unsigned)info.device, (int)info.function, (unsigned)info.bar0);
    printf("  CTRL=%08x STATUS=%08x link=%s\n",
           (unsigned)info.ctrl, (unsigned)info.status, info.link_up ? "up" : "down");
    printf("  MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           (unsigned)info.mac[0], (unsigned)info.mac[1], (unsigned)info.mac[2],
           (unsigned)info.mac[3], (unsigned)info.mac[4], (unsigned)info.mac[5]);

    return 0;
}
