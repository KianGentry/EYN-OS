#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Probe Intel e1000 NIC.", "e1000probe");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: e1000probe");
        return 0;
    }

    eyn_e1000_probe_info_t info;
    if (eyn_sys_e1000_probe(&info) != 0) {
        puts("e1000probe: probe failed");
        return 1;
    }

    puts("E1000 Probe Results:");
    printf("  PCI: %02x:%02x.%d\n", (unsigned)info.bus, (unsigned)info.device, (int)info.function);
    printf("  MMIO Base: 0x%08x\n", (unsigned)info.bar0);
    printf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           (unsigned)info.mac[0], (unsigned)info.mac[1], (unsigned)info.mac[2],
           (unsigned)info.mac[3], (unsigned)info.mac[4], (unsigned)info.mac[5]);
    printf("  Link: %s\n", info.link_up ? "UP" : "DOWN");

    return 0;
}
