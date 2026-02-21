#include <drivers/pci.h>
#include <drivers/system.h>
#include <context.h>
#include <misc/sched.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static int pci_ctx_allow(uint32 caps, uint32 cost)
{
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void pci_ctx_account(uint32 cost)
{
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

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
    if (!pci_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return 0xFFFFFFFFu;
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8 bus, uint8 device, uint8 function, uint8 offset, uint32 value)
{
    if (!pci_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return;
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
    if (!pci_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return;

    // Intentional brute-force scan, number of buses is likely small enough to not care
    for (uint16 bus = 0; bus < 256; bus++) {
        if ((bus & 0x0Fu) == 0u) pci_ctx_account(SCHED_COST_FS);
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
