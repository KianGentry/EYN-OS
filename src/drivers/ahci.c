#include <ahci.h>

#include <drivers/pci.h>
#include <string.h>
#include <vga.h>

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06
#define PCI_PROGIF_AHCI        0x01

typedef struct ahci_controller_info_t {
    uint8 bus;
    uint8 device;
    uint8 function;
    uint16 vendor_id;
    uint16 device_id;
    uint8 revision_id;
    uint8 prog_if;
} ahci_controller_info_t;

static ahci_controller_info_t g_ahci_controllers[8];
static uint8 g_ahci_controller_count;

static void ahci_pci_enum_cb(const pci_device_info* info, void* user) {
    (void)user;

    if (!info) {
        return;
    }

    if (info->class_code != PCI_CLASS_MASS_STORAGE || info->subclass != PCI_SUBCLASS_SATA || info->prog_if != PCI_PROGIF_AHCI) {
        return;
    }

    if (g_ahci_controller_count >= (uint8)(sizeof(g_ahci_controllers) / sizeof(g_ahci_controllers[0]))) {
        return;
    }

    ahci_controller_info_t* controller = &g_ahci_controllers[g_ahci_controller_count++];
    controller->bus = info->bus;
    controller->device = info->device;
    controller->function = info->function;
    controller->vendor_id = info->vendor_id;
    controller->device_id = info->device_id;
    controller->revision_id = info->revision_id;
    controller->prog_if = info->prog_if;

    printf("[ahci] controller %u: bus=%u dev=%u fn=%u vendor=0x%04X device=0x%04X rev=0x%02X\n",
           (unsigned)(g_ahci_controller_count - 1u),
           (unsigned)controller->bus,
           (unsigned)controller->device,
           (unsigned)controller->function,
           (unsigned)controller->vendor_id,
           (unsigned)controller->device_id,
           (unsigned)controller->revision_id);
}

void ahci_probe_controllers(void) {
    memset(g_ahci_controllers, 0, sizeof(g_ahci_controllers));
    g_ahci_controller_count = 0;

    pci_enumerate(ahci_pci_enum_cb, 0);

    if (g_ahci_controller_count == 0) {
        printf("[ahci] no AHCI controllers detected\n");
        return;
    }

    printf("[ahci] controllers detected: %u\n", (unsigned)g_ahci_controller_count);
}