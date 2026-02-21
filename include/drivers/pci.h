#ifndef PCI_H
#define PCI_H

#include <misc/types.h>

// PCI configuration-space (not finished)

typedef struct pci_device_info {
    uint8 bus;
    uint8 device;
    uint8 function;

    uint16 vendor_id;
    uint16 device_id;

    uint8 class_code;
    uint8 subclass;
    uint8 prog_if;
    uint8 revision_id;

    uint8 header_type;
} pci_device_info;

typedef void (*pci_enum_cb)(const pci_device_info* info, void* user);

// Read/write raw config dwords.
// Offsets are in bytes. Reads are aligned to the containing 32-bit register.

uint32 pci_read_config_dword(uint8 bus, uint8 device, uint8 function, uint8 offset);
void pci_write_config_dword(uint8 bus, uint8 device, uint8 function, uint8 offset, uint32 value);

uint16 pci_read_config_word(uint8 bus, uint8 device, uint8 function, uint8 offset);
uint8 pci_read_config_byte(uint8 bus, uint8 device, uint8 function, uint8 offset);

// Enumerate all device functions.
// We deliberately scan all buses for simplicity. Performance is fine at boot on QEMU.
void pci_enumerate(pci_enum_cb cb, void* user);

#endif
