#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Scan PCI devices.", "pciscan net");

static int net_only_from_args(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "net") == 0) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: pciscan [net]");
        return 0;
    }

    int net_only = net_only_from_args(argc, argv);
    int total = eyn_sys_pci_get_count(net_only);
    if (total < 0) {
        puts("pciscan: failed to enumerate PCI");
        return 1;
    }

    printf("PCI scan (%s):\n", net_only ? "net" : "all");
    for (int i = 0; i < total; ++i) {
        eyn_pci_entry_t e;
        if (eyn_sys_pci_get_entry(net_only, i, &e) != 0) continue;
        printf("%02x:%02x.%d %04x:%04x class=%02x/%02x/%02x hdr=%02x cmd=%04x bar0=%s:%08x\n",
               (unsigned)e.bus, (unsigned)e.device, (int)e.function,
               (unsigned)e.vendor_id, (unsigned)e.device_id,
               (unsigned)e.class_code, (unsigned)e.subclass, (unsigned)e.prog_if,
               (unsigned)e.header_type, (unsigned)e.command,
               e.bar0_is_io ? "io" : "mmio", (unsigned)e.bar0_base);
    }
    printf("Found %d device functions (%d shown).\n", total, total);
    return 0;
}
