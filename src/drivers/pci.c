#include <drivers/pci.h>
#include <drivers/system.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32 pci_make_address(uint8 bus, uint8 device, uint8 function, uint8 offset)
{
    // PCI configuration mechanism #1 address format
    
    // Offset masked as accesses through 0xCFC are 32-bit. The low two bits select byte lanes
    // Callers can still request byte/word offsets. Align here and shift later
    return 0x80000000u | // 0x80000000 is the enable bit
           (((uint32)bus) << 16) |
           (((uint32)device) << 11) |
           (((uint32)function) << 8) |
           ((uint32)(offset & 0xFC));
}

uint32 pci_read_config_dword(uint8 bus, uint8 device, uint8 function, uint8 offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8 bus, uint8 device, uint8 function, uint8 offset, uint32 value)
{
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16 pci_read_config_word(uint8 bus, uint8 device, uint8 function, uint8 offset)
{
    uint32 dword = pci_read_config_dword(bus, device, function, offset);
    uint8 shift = (uint8)((offset & 2) * 8);
    return (uint16)((dword >> shift) & 0xFFFFu);
}

uint8 pci_read_config_byte(uint8 bus, uint8 device, uint8 function, uint8 offset)
{
    uint32 dword = pci_read_config_dword(bus, device, function, offset);
    uint8 shift = (uint8)((offset & 3) * 8);
    return (uint8)((dword >> shift) & 0xFFu);
}

void pci_enumerate(pci_enum_cb cb, void* user)
{
    if (!cb) return;

    // Intentional brute-force scan, number of buses is likely small enough to not care
    for (uint16 bus = 0; bus < 256; bus++) {
        for (uint8 device = 0; device < 32; device++) {
            uint16 vendor0 = pci_read_config_word((uint8)bus, device, 0, 0x00);
            if (vendor0 == 0xFFFFu) {
                continue;
            }

            uint8 header0 = pci_read_config_byte((uint8)bus, device, 0, 0x0E);
            uint8 multi = (uint8)(header0 & 0x80u);
            uint8 max_func = multi ? 7 : 0;

            for (uint8 function = 0; function <= max_func; function++) {
                uint16 vendor = pci_read_config_word((uint8)bus, device, function, 0x00);
                if (vendor == 0xFFFFu) {
                    continue;
                }

                pci_device_info info;
                info.bus = (uint8)bus;
                info.device = device;
                info.function = function;

                info.vendor_id = vendor;
                info.device_id = pci_read_config_word((uint8)bus, device, function, 0x02);

                info.revision_id = pci_read_config_byte((uint8)bus, device, function, 0x08);
                info.prog_if = pci_read_config_byte((uint8)bus, device, function, 0x09);
                info.subclass = pci_read_config_byte((uint8)bus, device, function, 0x0A);
                info.class_code = pci_read_config_byte((uint8)bus, device, function, 0x0B);

                info.header_type = pci_read_config_byte((uint8)bus, device, function, 0x0E);

                cb(&info, user);
            }
        }
    }
}
