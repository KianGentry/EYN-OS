#include <drivers/e100.h>

#include <drivers/pci.h>
#include <drivers/system.h>
#include <mm/vmm.h>
#include <string.h>
#include <vga.h>
#include <context.h>
#include <misc/sched.h>

#define E100_INTEL_VENDOR_ID 0x8086u

#define E100_CSR_SCB_STATUS  0x00u
#define E100_CSR_SCB_CMD     0x02u
#define E100_CSR_GEN_PTR     0x04u
#define E100_CSR_PORT        0x08u
#define E100_CSR_EECTL       0x0Eu

#define E100_PORT_SOFT_RESET 0x00000000u

#define E100_SCB_CU_START    0x0010u
#define E100_SCB_RU_START    0x0001u
#define E100_SCB_RU_ABORT    0x0004u

#define E100_CB_CMD_XMIT     0x0004u
#define E100_CB_FLAG_S       0x4000u
#define E100_CB_FLAG_EL      0x8000u
#define E100_CB_STATUS_C     0x8000u
#define E100_CB_STATUS_OK    0x2000u

#define E100_RX_BUF_SIZE     1600u

#define E100_EE_SK           0x01u
#define E100_EE_CS           0x02u
#define E100_EE_DI           0x04u
#define E100_EE_DO           0x08u

typedef struct __attribute__((packed)) e100_tx_cb {
    uint16 status;
    uint16 command;
    uint32 link;
    uint32 tbd_addr;
    uint16 byte_count;
    uint8 threshold;
    uint8 tbd_num;
    uint8 data[E100_RX_BUF_SIZE];
} e100_tx_cb;

typedef struct __attribute__((packed)) e100_rfd {
    uint16 status;
    uint16 command;
    uint32 link;
    uint32 rbd_addr;
    uint16 count_flags;
    uint16 size_flags;
    uint8 data[E100_RX_BUF_SIZE];
} e100_rfd;

typedef struct e100_state {
    int probed;
    int initialized;

    uint8 bus;
    uint8 dev;
    uint8 fun;
    uint16 vendor_id;
    uint16 device_id;
    uint16 io_base;
    uint8 irq_line;

    uint8 mac[6];

    uint32 tx_cb_phys;
    e100_tx_cb* tx_cb;

    uint32 rx_rfd_phys;
    e100_rfd* rx_rfd;
} e100_state;

static e100_state g_e100;

static int e100_ctx_allow(uint32 caps, uint32 cost)
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

static void e100_ctx_account(uint32 cost)
{
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

static inline void* e100_kva_from_phys(uint32 phys)
{
    return (void*)((uintptr)KERNEL_BASE + (uintptr)phys);
}

static int e100_wait_scb_idle(uint32 spins)
{
    for (uint32 i = 0; i < spins; i++) {
        if ((i & 0x1FFu) == 0u) e100_ctx_account(SCHED_COST_FS);
        if (inw((uint16)(g_e100.io_base + E100_CSR_SCB_CMD)) == 0u) return 0;
    }
    return -1;
}

static int e100_issue_scb_cmd(uint16 cmd)
{
    if (e100_wait_scb_idle(100000u) != 0) return -1;
    outw((uint16)(g_e100.io_base + E100_CSR_SCB_CMD), cmd);
    return e100_wait_scb_idle(100000u);
}

static void e100_eeprom_set(uint8 v)
{
    outportb((uint16)(g_e100.io_base + E100_CSR_EECTL), v);
    (void)inportb((uint16)(g_e100.io_base + E100_CSR_EECTL));
}

static void e100_eeprom_clock(uint8 v)
{
    e100_eeprom_set((uint8)(v | E100_EE_SK));
    e100_eeprom_set((uint8)(v & (uint8)~E100_EE_SK));
}

static void e100_eeprom_shift_out(uint16 value, uint8 bits)
{
    uint16 mask = (uint16)(1u << (bits - 1u));
    uint8 ctl = E100_EE_CS;

    while (mask) {
        if (value & mask) ctl |= E100_EE_DI;
        else ctl &= (uint8)~E100_EE_DI;
        e100_eeprom_clock(ctl);
        mask >>= 1u;
    }
}

static uint16 e100_eeprom_read_word_bits(uint8 addr_bits, uint8 addr)
{
    uint16 value = 0;
    uint8 ctl = E100_EE_CS;

    e100_eeprom_set(0u);
    e100_eeprom_set(ctl);

    e100_eeprom_shift_out(0x6u, 3u); // start + READ opcode
    e100_eeprom_shift_out((uint16)(addr & ((1u << addr_bits) - 1u)), addr_bits);

    for (int i = 0; i < 16; i++) {
        e100_eeprom_set((uint8)(ctl | E100_EE_SK));
        value <<= 1u;
        if (inportb((uint16)(g_e100.io_base + E100_CSR_EECTL)) & E100_EE_DO) {
            value |= 1u;
        }
        e100_eeprom_set(ctl);
    }

    e100_eeprom_set(0u);
    return value;
}

static void e100_enable_pci(uint8 bus, uint8 dev, uint8 fun)
{
    uint32 dword = pci_read_config_dword(bus, dev, fun, 0x04);
    uint16 cmd = (uint16)(dword & 0xFFFFu);

    cmd |= (1u << 0); // IO space
    cmd |= (1u << 2); // bus master

    dword = (dword & 0xFFFF0000u) | (uint32)cmd;
    pci_write_config_dword(bus, dev, fun, 0x04, dword);
}

static int e100_is_supported_device(uint16 vendor, uint16 device)
{
    if (vendor != E100_INTEL_VENDOR_ID) return 0;

    switch (device) {
        case 0x1029u: // 82559ER
        case 0x1030u: // PRO/100 VE
        case 0x1031u:
        case 0x1032u:
        case 0x1033u:
        case 0x1034u:
        case 0x1038u:
        case 0x1039u:
        case 0x103Au:
        case 0x103Bu:
        case 0x2449u:
        case 0x2459u:
            return 1;
        default:
            return 0;
    }
}

static int e100_find_first(void)
{
    for (uint16 bus = 0; bus < 256; bus++) {
        if ((bus & 0x0Fu) == 0u) e100_ctx_account(SCHED_COST_FS);

        for (uint8 dev = 0; dev < 32; dev++) {
            uint16 vendor0 = pci_read_config_word((uint8)bus, dev, 0, 0x00);
            if (vendor0 == 0xFFFFu) continue;

            uint8 header0 = pci_read_config_byte((uint8)bus, dev, 0, 0x0E);
            uint8 max_fun = (header0 & 0x80u) ? 7u : 0u;

            for (uint8 fun = 0; fun <= max_fun; fun++) {
                uint16 vendor = pci_read_config_word((uint8)bus, dev, fun, 0x00);
                if (vendor == 0xFFFFu) continue;

                uint16 device = pci_read_config_word((uint8)bus, dev, fun, 0x02);
                if (!e100_is_supported_device(vendor, device)) continue;

                uint8 class_code = pci_read_config_byte((uint8)bus, dev, fun, 0x0B);
                uint8 subclass = pci_read_config_byte((uint8)bus, dev, fun, 0x0A);
                if (class_code != 0x02u || subclass != 0x00u) continue;

                uint32 bar0 = pci_read_config_dword((uint8)bus, dev, fun, 0x10);
                uint32 bar1 = pci_read_config_dword((uint8)bus, dev, fun, 0x14);
                uint16 io_base = 0;

                if (bar0 & 0x1u) {
                    io_base = (uint16)(bar0 & 0xFFFCu);
                } else if (bar1 & 0x1u) {
                    io_base = (uint16)(bar1 & 0xFFFCu);
                } else {
                    continue;
                }

                g_e100.bus = (uint8)bus;
                g_e100.dev = dev;
                g_e100.fun = fun;
                g_e100.vendor_id = vendor;
                g_e100.device_id = device;
                g_e100.io_base = io_base;
                g_e100.irq_line = pci_read_config_byte((uint8)bus, dev, fun, 0x3C);
                return 0;
            }
        }
    }

    return -1;
}

static int e100_read_mac_from_eeprom(uint8 out_mac[6])
{
    // Most i8255x parts use 6-bit EEPROM addressing; some variants use 8-bit.
    uint16 w0 = e100_eeprom_read_word_bits(6u, 0u);
    uint16 w1 = e100_eeprom_read_word_bits(6u, 1u);
    uint16 w2 = e100_eeprom_read_word_bits(6u, 2u);

    if (w0 == 0xFFFFu && w1 == 0xFFFFu && w2 == 0xFFFFu) {
        w0 = e100_eeprom_read_word_bits(8u, 0u);
        w1 = e100_eeprom_read_word_bits(8u, 1u);
        w2 = e100_eeprom_read_word_bits(8u, 2u);
    }

    if (w0 == 0x0000u && w1 == 0x0000u && w2 == 0x0000u) return -1;
    if (w0 == 0xFFFFu && w1 == 0xFFFFu && w2 == 0xFFFFu) return -1;

    out_mac[0] = (uint8)(w0 & 0xFFu);
    out_mac[1] = (uint8)((w0 >> 8) & 0xFFu);
    out_mac[2] = (uint8)(w1 & 0xFFu);
    out_mac[3] = (uint8)((w1 >> 8) & 0xFFu);
    out_mac[4] = (uint8)(w2 & 0xFFu);
    out_mac[5] = (uint8)((w2 >> 8) & 0xFFu);
    return 0;
}

int e100_probe(e100_probe_info* out)
{
    if (!e100_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return -1;

    if (!g_e100.probed) {
        if (e100_find_first() != 0) return -1;

        e100_enable_pci(g_e100.bus, g_e100.dev, g_e100.fun);
        outl((uint16)(g_e100.io_base + E100_CSR_PORT), E100_PORT_SOFT_RESET);
        sleep(1);

        if (e100_read_mac_from_eeprom(g_e100.mac) != 0) return -2;
        g_e100.probed = 1;
    }

    if (out) {
        out->bus = g_e100.bus;
        out->device = g_e100.dev;
        out->function = g_e100.fun;
        out->io_base = g_e100.io_base;
        out->vendor_id = g_e100.vendor_id;
        out->device_id = g_e100.device_id;
        out->irq_line = g_e100.irq_line;
        for (int i = 0; i < 6; i++) out->mac[i] = g_e100.mac[i];
    }

    return 0;
}

static int e100_alloc_dma(void)
{
    if (!e100_ctx_allow(CAP_DEV_NET | CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;

    if (!g_e100.tx_cb) {
        g_e100.tx_cb_phys = frame_alloc_contiguous(1u);
        if (g_e100.tx_cb_phys == 0u) return -2;
        g_e100.tx_cb = (e100_tx_cb*)e100_kva_from_phys(g_e100.tx_cb_phys);
        memset(g_e100.tx_cb, 0, PAGE_SIZE);
    }

    if (!g_e100.rx_rfd) {
        g_e100.rx_rfd_phys = frame_alloc_contiguous(1u);
        if (g_e100.rx_rfd_phys == 0u) return -3;
        g_e100.rx_rfd = (e100_rfd*)e100_kva_from_phys(g_e100.rx_rfd_phys);
        memset(g_e100.rx_rfd, 0, PAGE_SIZE);
    }

    return 0;
}

static int e100_prepare_rx_once(void)
{
    if (!g_e100.rx_rfd) return -1;

    g_e100.rx_rfd->status = 0u;
    g_e100.rx_rfd->command = (uint16)(E100_CB_FLAG_EL | E100_CB_FLAG_S);
    g_e100.rx_rfd->link = 0xFFFFFFFFu;
    g_e100.rx_rfd->rbd_addr = 0xFFFFFFFFu;
    g_e100.rx_rfd->count_flags = 0u;
    g_e100.rx_rfd->size_flags = (uint16)(E100_RX_BUF_SIZE & 0x3FFFu);

    outl((uint16)(g_e100.io_base + E100_CSR_GEN_PTR), g_e100.rx_rfd_phys);
    if (e100_issue_scb_cmd(E100_SCB_RU_ABORT) != 0) {
        return -2;
    }
    if (e100_issue_scb_cmd(E100_SCB_RU_START) != 0) {
        return -3;
    }

    return 0;
}

int e100_init(void)
{
    if (!e100_ctx_allow(CAP_DEV_NET | CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    if (g_e100.initialized) return 0;

    if (e100_probe(NULL) != 0) return -2;
    if (e100_alloc_dma() != 0) return -3;
    if (e100_prepare_rx_once() != 0) return -4;

    g_e100.initialized = 1;
    return 0;
}

int e100_get_mac(uint8 out_mac[6])
{
    if (!out_mac) return -1;
    if (e100_probe(NULL) != 0) return -2;
    for (int i = 0; i < 6; i++) out_mac[i] = g_e100.mac[i];
    return 0;
}

int e100_send_frame(const void* frame, uint32 len)
{
    if (!e100_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return -1;
    if (!frame || len < 14u || len > E100_RX_BUF_SIZE) return -2;
    if (e100_init() != 0) return -3;
    if (!g_e100.tx_cb) return -4;

    g_e100.tx_cb->status = 0u;
    g_e100.tx_cb->command = (uint16)(E100_CB_CMD_XMIT | E100_CB_FLAG_EL | E100_CB_FLAG_S);
    g_e100.tx_cb->link = 0xFFFFFFFFu;
    g_e100.tx_cb->tbd_addr = 0xFFFFFFFFu;
    g_e100.tx_cb->byte_count = (uint16)len;
    g_e100.tx_cb->threshold = 0xE0u;
    g_e100.tx_cb->tbd_num = 0u;
    memcpy(g_e100.tx_cb->data, frame, len);

    outl((uint16)(g_e100.io_base + E100_CSR_GEN_PTR), g_e100.tx_cb_phys);
    if (e100_issue_scb_cmd(E100_SCB_CU_START) != 0) return -5;

    for (uint32 spin = 0; spin < 500000u; spin++) {
        if ((spin & 0x3FFu) == 0u) e100_ctx_account(SCHED_COST_FS);
        if (g_e100.tx_cb->status & E100_CB_STATUS_C) {
            return (g_e100.tx_cb->status & E100_CB_STATUS_OK) ? 0 : -6;
        }
    }

    return -7;
}

int e100_rx_poll_frame(uint8* out_buf, uint32 out_buf_cap, uint32* out_len, int spin_limit)
{
    if (!e100_ctx_allow(CAP_DEV_NET, SCHED_COST_FS)) return -1;
    if (!out_buf || !out_len || out_buf_cap == 0u) return -2;
    if (e100_init() != 0) return -3;
    if (!g_e100.rx_rfd) return -4;
    if (spin_limit <= 0) spin_limit = 20000;

    for (int spin = 0; spin < spin_limit; spin++) {
        if ((spin & 0x1FF) == 0) e100_ctx_account(SCHED_COST_FS);

        if (g_e100.rx_rfd->status & E100_CB_STATUS_C) {
            uint32 frame_len = (uint32)(g_e100.rx_rfd->count_flags & 0x3FFFu);
            uint32 copy_len = frame_len;
            if (copy_len > out_buf_cap) copy_len = out_buf_cap;
            memcpy(out_buf, g_e100.rx_rfd->data, copy_len);
            *out_len = frame_len;

            (void)e100_prepare_rx_once();
            return 1;
        }
    }

    return 0;
}