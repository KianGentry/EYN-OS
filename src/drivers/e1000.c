#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <mm/vmm.h>
#include <vga.h>

// e1000 register offsets (subset).
//
// Why these specific regs:
// - CTRL/STATUS confirms MMIO reads are sane and the device is alive.
// - RAL/RAH gives us the programmed MAC address QEMU expects us to use.
#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_RAL0   0x5400
#define E1000_REG_RAH0   0x5404

// Use a fixed high kernel VA window for device MMIO.
//
// Why a fixed VA:
// - Keeps bring-up simple and avoids depending on a full virtual allocator.
// - Avoids colliding with the kernel heap (starts at 0xD0000000).
//
// This is intentionally narrow: we only map what we need for probing.
#define E1000_MMIO_VA_BASE   0xF0000000u
#define E1000_MMIO_MAP_BYTES 0x7000u  /* covers up through RAH0 */

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
    e1000_map_mmio(bar0);

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
