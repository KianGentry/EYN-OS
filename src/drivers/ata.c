#include <misc/types.h>
#include <system.h>
#include <vga.h>
#include <string.h>
#include <ata.h>
#include <context.h>
#include <misc/sched.h>
#include <cpu/arch.h>
#include <drivers/pci.h>

// Define NULL if not available
#ifndef NULL
#define NULL ((void*)0)
#endif

#define ATA_PRIMARY_IO 0x1F0
#define ATA_SECONDARY_IO 0x170
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_ALTSTATUS  0x206

// Enhanced commands for SATA compatibility
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_SET_FEATURES 0xEF
#define ATA_CMD_SLEEP      0xE6
#define ATA_CMD_STANDBY    0xE2
#define ATA_CMD_IDLE       0xE3

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DSC     0x10
#define ATA_SR_DRQ     0x08
#define ATA_SR_CORR    0x04
#define ATA_SR_IDX     0x02
#define ATA_SR_ERR     0x01

#define ATA_IDENTIFY_BSY_TIMEOUT 200000
#define ATA_IDENTIFY_DRQ_TIMEOUT 200000
#define ATA_PIO_BSY_TIMEOUT 500000
#define ATA_PIO_DRQ_TIMEOUT 500000
#define ATA_PIO_POST_WRITE_BSY_TIMEOUT 1000000
#define ATA_PIO_POST_WRITE_DRDY_TIMEOUT 1000000
#define ATA_PIO_MAX_ATTEMPTS 3
#define ATA_RESET_BACKOFF_MAX_SHIFT 6
#define ATA_RESET_BACKOFF_MIN_WINDOW 1u

#ifndef CONFIG_ATA_LBA48_SMOKE
#define CONFIG_ATA_LBA48_SMOKE 0
#endif

#define ATA_LBA28_MAX_SECTOR 0x0FFFFFFFu
#define ATA_LBA48_SMOKE_TARGET_LBA 0x10000000u

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_IDE 0x01

#define ATA_PCI_PROGIF_PRIMARY_NATIVE 0x01
#define ATA_PCI_PROGIF_SECONDARY_NATIVE 0x04

#define ATA_PCI_BAR0 0x10
#define ATA_PCI_BAR1 0x14
#define ATA_PCI_BAR2 0x18
#define ATA_PCI_BAR3 0x1C

// SATA specific features
#define ATA_FEATURE_SATA_ENABLE 0x10
#define ATA_FEATURE_SATA_DISABLE 0x90

static int ata_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void ata_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

static drive_info_t detected_drives[8];
static uint8 detected_drive_atapi[8];
static uint8 detected_drive_dma[8];
static uint8 detected_drive_lba[8];
static uint8 detected_drive_lba48[8];
static uint16 detected_drive_cylinders[8];
static uint16 detected_drive_heads[8];
static uint16 detected_drive_spt[8];
static uint64 detected_drive_lba48_sectors[8];
static uint32 g_ata_recovery_epoch = 0;
static uint32 g_ata_reset_cooldown_until[2];
static uint8 g_ata_reset_backoff_shift[2];

static uint16 g_ata_channel_io_base[2] = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
static uint16 g_ata_channel_ctrl_base[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };
static uint8 g_ata_channel_native_mode[2];
static uint8 g_ata_channel_configured_by_pci[2];
static int g_ata_channel_health_score[2];

typedef struct {
    uint8 found;
    uint8 candidates_seen;
    uint8 candidates_conflict;
    uint8 bus;
    uint8 device;
    uint8 function;
    uint8 prog_if;
    uint16 vendor_id;
    uint16 device_id;
    uint32 bar0;
    uint32 bar1;
    uint32 bar2;
    uint32 bar3;
    int score;
} ata_ide_pci_info_t;

typedef struct {
    uint8 candidates_seen;
    uint8 candidates_conflict;
    ata_ide_pci_info_t best;
    ata_ide_pci_info_t second;
} ata_ide_pci_scan_t;

static ata_ide_pci_info_t g_ata_ide_selected_candidate;
static ata_ide_pci_info_t g_ata_ide_secondary_candidate;
static ata_ide_pci_info_t g_ata_prev_selected_candidate;
static int g_ata_prev_selected_weight = 0;
static int g_ata_prev_legacy_weight = 0;

/*
 * ABI-INVARIANT: ATA re-entrancy guard.
 *
 * Why: The IRQ guard (arch_irq_save/restore) prevents timer-IRQ-triggered
 * re-entrancy, but NOT synchronous exception re-entrancy.  A page fault that
 * fires mid-PIO (e.g. while BSY-polling touches a swapped-out stack page)
 * runs the kernel #PF handler synchronously at ring0 with the same stack.
 * The PF handler may call swap_read_page -> ata_read_sector before the
 * enclosing ata_read_sector has completed its command, corrupting the
 * controller state and causing the inner BSY poll to spin forever.
 *
 * Fix: a simple volatile flag acts as a non-recursive mutex.  The inner
 * (re-entrant) caller spins on the flag until the outer call exits.  On QEMU
 * IDE, the outer PIO completes in microseconds so the spin is brief.
 *
 * Invariant: flag is set before the first outportb and cleared after the
 * last inw/ata_io_wait in both read and write paths.
 * Breakage if removed: nested PF-triggered swap reads corrupt the BSY poll
 * and cause "ATA read timeout: BSY timeout" → swap-in failure → SIGSEGV.
 * Security-critical: No (no privilege boundary crossed).
 * ABI-sensitive: No (internal kernel use only).
 */
static volatile int g_ata_busy = 0;

// logical drive mapping system
static uint8 logical_to_physical_map[8];  // maps logical drive (0,1,2...) to physical drive (0,1,2,3,4,5,6,7)
static uint8 physical_to_logical_map[8];  // maps physical drive to logical drive (0xFF = not mapped)
static uint8 num_logical_drives = 0;

// function declarations
static void init_logical_drive_mapping(void);
static void ata_set_legacy_channels(void);
static void ata_log_channel_config(void);
static uint8 ata_count_detected_physical_drives(void);
static void ata_clear_detected_drive_state(void);
static uint8 ata_probe_standard_slots_with_health(void);
static int ata_candidate_matches_identity(const ata_ide_pci_info_t* a, const ata_ide_pci_info_t* b);

static int ata_decode_ide_io_bar(uint32 bar, uint16* out_base) {
    if (!out_base) return -1;
    if (bar == 0 || bar == 0xFFFFFFFFu) return -1;
    if ((bar & 0x1u) == 0) return -1;

    uint16 base = (uint16)(bar & 0xFFFCu);
    if (base == 0) return -1;

    *out_base = base;
    return 0;
}

static int ata_decode_ide_ctrl_bar(uint32 bar, uint16* out_ctrl_port) {
    if (!out_ctrl_port) return -1;
    uint16 block_base = 0;
    if (ata_decode_ide_io_bar(bar, &block_base) != 0) return -1;
    *out_ctrl_port = (uint16)(block_base + 2u);
    return 0;
}

static void ata_set_legacy_channels(void) {
    g_ata_channel_io_base[0] = ATA_PRIMARY_IO;
    g_ata_channel_io_base[1] = ATA_SECONDARY_IO;
    g_ata_channel_ctrl_base[0] = ATA_PRIMARY_CTRL;
    g_ata_channel_ctrl_base[1] = ATA_SECONDARY_CTRL;
    g_ata_channel_native_mode[0] = 0;
    g_ata_channel_native_mode[1] = 0;
    g_ata_channel_configured_by_pci[0] = 0;
    g_ata_channel_configured_by_pci[1] = 0;
}

static void ata_log_channel_config(void) {
    printf("[ata] channel0: mode=%s io=0x%X ctrl=0x%X source=%s\n",
           g_ata_channel_native_mode[0] ? "native" : "compat",
           (unsigned)g_ata_channel_io_base[0],
           (unsigned)g_ata_channel_ctrl_base[0],
           g_ata_channel_configured_by_pci[0] ? "pci" : "legacy");
    printf("[ata] channel1: mode=%s io=0x%X ctrl=0x%X source=%s\n",
           g_ata_channel_native_mode[1] ? "native" : "compat",
           (unsigned)g_ata_channel_io_base[1],
           (unsigned)g_ata_channel_ctrl_base[1],
           g_ata_channel_configured_by_pci[1] ? "pci" : "legacy");
}

static uint8 ata_count_detected_physical_drives(void) {
    uint8 physical_count = 0;
    for (int i = 0; i < 8; i++) {
        if (detected_drives[i].present) physical_count++;
    }
    return physical_count;
}

static void ata_clear_detected_drive_state(void) {
    for (int i = 0; i < 8; i++) {
        detected_drives[i].present = 0;
        detected_drives[i].type = 0;
        detected_drives[i].sectors = 0;
        detected_drives[i].size_mb = 0;
        detected_drives[i].model[0] = '\0';
        detected_drive_atapi[i] = 0;
        detected_drive_dma[i] = 0;
        detected_drive_lba[i] = 0;
        detected_drive_lba48[i] = 0;
        detected_drive_cylinders[i] = 0;
        detected_drive_heads[i] = 0;
        detected_drive_spt[i] = 0;
        detected_drive_lba48_sectors[i] = 0;
    }
}

static uint8 ata_probe_standard_slots_with_health(void) {
    g_ata_channel_health_score[0] = 0;
    g_ata_channel_health_score[1] = 0;

    for (int drive = 0; drive < 4; drive++) {
        int rc = ata_detect_drive((uint8)drive);
        uint8 ch = (drive & 2) ? 1u : 0u;
        if (rc == 0) {
            g_ata_channel_health_score[ch] += 4;
        } else {
            g_ata_channel_health_score[ch] -= 1;
        }
    }

    uint8 physical_count = ata_count_detected_physical_drives();
    int total_health = g_ata_channel_health_score[0] + g_ata_channel_health_score[1];
    printf("[ata] probe health: ch0=%d ch1=%d total=%d physical=%u\n",
           g_ata_channel_health_score[0],
           g_ata_channel_health_score[1],
           total_health,
           (unsigned)physical_count);
    return physical_count;
}

static int ata_candidate_matches_identity(const ata_ide_pci_info_t* a, const ata_ide_pci_info_t* b) {
    if (!a || !b) return 0;
    if (!a->found || !b->found) return 0;

    return (a->bus == b->bus &&
            a->device == b->device &&
            a->function == b->function &&
            a->vendor_id == b->vendor_id &&
            a->device_id == b->device_id) ? 1 : 0;
}

static int ata_port_ranges_overlap(uint16 base_a, uint16 size_a, uint16 base_b, uint16 size_b) {
    uint32 a0 = (uint32)base_a;
    uint32 b0 = (uint32)base_b;
    uint32 a1 = a0 + (uint32)size_a - 1u;
    uint32 b1 = b0 + (uint32)size_b - 1u;
    return (a1 < b0 || b1 < a0) ? 0 : 1;
}

static int ata_candidate_layout_is_safe(const pci_device_info* info,
                                        uint32 bar0,
                                        uint32 bar1,
                                        uint32 bar2,
                                        uint32 bar3) {
    uint16 io0 = ATA_PRIMARY_IO;
    uint16 ctrl0 = ATA_PRIMARY_CTRL;
    uint16 io1 = ATA_SECONDARY_IO;
    uint16 ctrl1 = ATA_SECONDARY_CTRL;

    if (info->prog_if & ATA_PCI_PROGIF_PRIMARY_NATIVE) {
        if (ata_decode_ide_io_bar(bar0, &io0) != 0 || ata_decode_ide_ctrl_bar(bar1, &ctrl0) != 0) {
            return 0;
        }
    }

    if (info->prog_if & ATA_PCI_PROGIF_SECONDARY_NATIVE) {
        if (ata_decode_ide_io_bar(bar2, &io1) != 0 || ata_decode_ide_ctrl_bar(bar3, &ctrl1) != 0) {
            return 0;
        }
    }

    /*
     * Command block uses 8 ports, control block uses 2 ports.
     * Reject controllers that collapse channel address windows unexpectedly.
     */
    if (ata_port_ranges_overlap(io0, 8, io1, 8)) return 0;
    if (ata_port_ranges_overlap(io0, 8, ctrl1, 2)) return 0;
    if (ata_port_ranges_overlap(io1, 8, ctrl0, 2)) return 0;
    if (ata_port_ranges_overlap(ctrl0, 2, ctrl1, 2)) return 0;

    return 1;
}

static int ata_score_ide_candidate(const pci_device_info* info,
                                   uint32 bar0,
                                   uint32 bar1,
                                   uint32 bar2,
                                   uint32 bar3) {
    if (!ata_candidate_layout_is_safe(info, bar0, bar1, bar2, bar3)) {
        return -1000;
    }

    int score = 0;
    int primary_native = (info->prog_if & ATA_PCI_PROGIF_PRIMARY_NATIVE) ? 1 : 0;
    int secondary_native = (info->prog_if & ATA_PCI_PROGIF_SECONDARY_NATIVE) ? 1 : 0;

    if (primary_native) {
        uint16 io = 0;
        uint16 ctrl = 0;
        if (ata_decode_ide_io_bar(bar0, &io) == 0 && ata_decode_ide_ctrl_bar(bar1, &ctrl) == 0) {
            score += 16;
        } else {
            score -= 64;
        }
    } else {
        score += 2;
    }

    if (secondary_native) {
        uint16 io = 0;
        uint16 ctrl = 0;
        if (ata_decode_ide_io_bar(bar2, &io) == 0 && ata_decode_ide_ctrl_bar(bar3, &ctrl) == 0) {
            score += 16;
        } else {
            score -= 64;
        }
    } else {
        score += 2;
    }

    if (primary_native && secondary_native) {
        score += 1;
    }

    int native_channels = primary_native + secondary_native;
    int compat_channels = 2 - native_channels;

    {
        ata_ide_pci_info_t current;
        memset(&current, 0, sizeof(current));
        current.found = 1;
        current.bus = info->bus;
        current.device = info->device;
        current.function = info->function;
        current.vendor_id = info->vendor_id;
        current.device_id = info->device_id;

        if (ata_candidate_matches_identity(&current, &g_ata_prev_selected_candidate)) {
            score += g_ata_prev_selected_weight;
        }

        score += (g_ata_prev_legacy_weight * compat_channels);
        score -= (g_ata_prev_legacy_weight * native_channels);
    }

    if (score > 200) score = 200;
    if (score < -200) score = -200;

    return score;
}

static void ata_apply_pci_ide_candidate(const ata_ide_pci_info_t* ide) {
    ata_set_legacy_channels();

    if (!ide || !ide->found) {
        ata_log_channel_config();
        return;
    }

    printf("[ata] pci ide: selected bdf=%u:%u.%u ven=0x%X dev=0x%X prog_if=0x%X score=%d\n",
           (unsigned)ide->bus,
           (unsigned)ide->device,
           (unsigned)ide->function,
           (unsigned)ide->vendor_id,
           (unsigned)ide->device_id,
           (unsigned)ide->prog_if,
           ide->score);

    if (ide->prog_if & ATA_PCI_PROGIF_PRIMARY_NATIVE) {
        uint16 io = 0;
        uint16 ctrl = 0;
        if (ata_decode_ide_io_bar(ide->bar0, &io) == 0 && ata_decode_ide_ctrl_bar(ide->bar1, &ctrl) == 0) {
            g_ata_channel_io_base[0] = io;
            g_ata_channel_ctrl_base[0] = ctrl;
            g_ata_channel_native_mode[0] = 1;
            g_ata_channel_configured_by_pci[0] = 1;
        } else {
            printf("[ata] pci ide: primary native requested but BAR decode failed, keeping legacy\n");
        }
    }

    if (ide->prog_if & ATA_PCI_PROGIF_SECONDARY_NATIVE) {
        uint16 io = 0;
        uint16 ctrl = 0;
        if (ata_decode_ide_io_bar(ide->bar2, &io) == 0 && ata_decode_ide_ctrl_bar(ide->bar3, &ctrl) == 0) {
            g_ata_channel_io_base[1] = io;
            g_ata_channel_ctrl_base[1] = ctrl;
            g_ata_channel_native_mode[1] = 1;
            g_ata_channel_configured_by_pci[1] = 1;
        } else {
            printf("[ata] pci ide: secondary native requested but BAR decode failed, keeping legacy\n");
        }
    }

    ata_log_channel_config();
}

static void ata_ide_pci_enum_cb(const pci_device_info* info, void* user) {
    ata_ide_pci_scan_t* scan = (ata_ide_pci_scan_t*)user;
    if (!info || !scan) return;
    if (info->class_code != PCI_CLASS_MASS_STORAGE || info->subclass != PCI_SUBCLASS_IDE) return;

    scan->candidates_seen++;

    uint32 bar0 = pci_read_config_dword(info->bus, info->device, info->function, ATA_PCI_BAR0);
    uint32 bar1 = pci_read_config_dword(info->bus, info->device, info->function, ATA_PCI_BAR1);
    uint32 bar2 = pci_read_config_dword(info->bus, info->device, info->function, ATA_PCI_BAR2);
    uint32 bar3 = pci_read_config_dword(info->bus, info->device, info->function, ATA_PCI_BAR3);

    int score = ata_score_ide_candidate(info, bar0, bar1, bar2, bar3);
    if (score <= -1000) {
        scan->candidates_conflict++;
        return;
    }

    ata_ide_pci_info_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.found = 1;
    candidate.bus = info->bus;
    candidate.device = info->device;
    candidate.function = info->function;
    candidate.prog_if = info->prog_if;
    candidate.vendor_id = info->vendor_id;
    candidate.device_id = info->device_id;
    candidate.bar0 = bar0;
    candidate.bar1 = bar1;
    candidate.bar2 = bar2;
    candidate.bar3 = bar3;
    candidate.score = score;

    if (!scan->best.found || candidate.score > scan->best.score) {
        scan->second = scan->best;
        scan->best = candidate;
        return;
    }

    if (!scan->second.found || candidate.score > scan->second.score) {
        scan->second = candidate;
    }
}

static void ata_configure_channels_from_pci(void) {
    ata_ide_pci_scan_t scan;
    memset(&scan, 0, sizeof(scan));
    pci_enumerate(ata_ide_pci_enum_cb, &scan);

    if (scan.candidates_seen) {
        printf("[ata] pci ide: candidates seen=%u rejected=%u\n",
               (unsigned)scan.candidates_seen,
               (unsigned)scan.candidates_conflict);
    }

    g_ata_ide_selected_candidate = scan.best;
    g_ata_ide_secondary_candidate = scan.second;

    if (!g_ata_ide_selected_candidate.found) {
        printf("[ata] pci ide: no controller found, using legacy channels\n");
        ata_set_legacy_channels();
        ata_log_channel_config();
        return;
    }

    if (g_ata_ide_secondary_candidate.found) {
        printf("[ata] pci ide: secondary candidate bdf=%u:%u.%u score=%d\n",
               (unsigned)g_ata_ide_secondary_candidate.bus,
               (unsigned)g_ata_ide_secondary_candidate.device,
               (unsigned)g_ata_ide_secondary_candidate.function,
               g_ata_ide_secondary_candidate.score);
    }

    ata_apply_pci_ide_candidate(&g_ata_ide_selected_candidate);
}

static uint16 ata_drive_io_base(uint8 drive) {
    return g_ata_channel_io_base[(drive & 2u) ? 1u : 0u];
}

static uint16 ata_drive_ctrl_base(uint8 drive) {
    return g_ata_channel_ctrl_base[(drive & 2u) ? 1u : 0u];
}

static void ata_io_wait(uint16 ctrl_base) {
    for (int i = 0; i < 4; i++) inportb(ctrl_base);
}

static void ata_soft_reset(uint16 ctrl_base) {
    outportb(ctrl_base, 0x04); /* SRST */
    ata_io_wait(ctrl_base);
    outportb(ctrl_base, 0x00);
    ata_io_wait(ctrl_base);
}

static int ata_poll(uint16 io_base) {
    for (int i = 0; i < 100000; i++) {
        uint8 status = inportb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) return 0;
    }
    return -1;
}

static const char* ata_drive_slot_name(uint8 drive) {
    switch (drive) {
        case 0: return "primary-master";
        case 1: return "primary-slave";
        case 2: return "secondary-master";
        case 3: return "secondary-slave";
        default: return "aux";
    }
}

static int ata_get_chs_geometry(uint8 drive, uint16* out_cyl, uint16* out_heads, uint16* out_spt) {
    if (!out_cyl || !out_heads || !out_spt) return -1;
    if (drive >= 8) return -1;

    uint16 cyl = detected_drive_cylinders[drive];
    uint16 heads = detected_drive_heads[drive];
    uint16 spt = detected_drive_spt[drive];

    if (cyl == 0 || heads == 0 || spt == 0) return -1;
    if (heads > 16 || spt > 63) return -1;

    *out_cyl = cyl;
    *out_heads = heads;
    *out_spt = spt;
    return 0;
}

static uint32 ata_u64_to_u32_sat(uint64 value) {
    if (value > (uint64)0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }
    return (uint32)value;
}

static uint8 ata_drive_channel_index(uint8 drive) {
    return (drive & 2u) ? 1u : 0u;
}

static void ata_note_io_success(uint8 drive) {
    uint8 ch = ata_drive_channel_index(drive);
    if (g_ata_reset_backoff_shift[ch] > 0) {
        g_ata_reset_backoff_shift[ch]--;
    }
    if (g_ata_reset_backoff_shift[ch] == 0) {
        g_ata_reset_cooldown_until[ch] = 0;
    }
}

static int ata_compute_drive_max_lba(uint8 drive, uint32* out_max_lba) {
    if (!out_max_lba) return -1;
    if (drive >= 8) return -1;

    if (detected_drive_lba[drive]) {
        if (detected_drive_lba48[drive] && detected_drive_lba48_sectors[drive] != 0) {
            uint64 max64 = detected_drive_lba48_sectors[drive] - 1u;
            if (max64 > (uint64)0xFFFFFFFFu) max64 = (uint64)0xFFFFFFFFu;
            *out_max_lba = (uint32)max64;
            return 0;
        }

        if (detected_drives[drive].sectors == 0) {
            return -1;
        }

        uint32 max_lba = detected_drives[drive].sectors - 1u;
        if (max_lba > ATA_LBA28_MAX_SECTOR) {
            max_lba = ATA_LBA28_MAX_SECTOR;
        }
        *out_max_lba = max_lba;
        return 0;
    }

    uint16 cyl = 0;
    uint16 heads = 0;
    uint16 spt = 0;
    if (ata_get_chs_geometry(drive, &cyl, &heads, &spt) != 0) {
        return -1;
    }

    uint32 sectors_per_cyl = (uint32)heads * (uint32)spt;
    uint32 total = (uint32)cyl * sectors_per_cyl;
    if (total == 0) {
        return -1;
    }

    *out_max_lba = total - 1u;
    return 0;
}

static int ata_try_recover_after_failure(uint8 drive,
                                         uint16 io_base,
                                         uint16 ctrl_base,
                                         const char* op,
                                         uint32 lba,
                                         int attempt,
                                         int max_attempts,
                                         const char* reason,
                                         uint8 status,
                                         int error_code) {
    uint8 channel = ata_drive_channel_index(drive);

    g_ata_recovery_epoch++;

    if (attempt >= max_attempts) {
        return -1;
    }

    if (g_ata_recovery_epoch < g_ata_reset_cooldown_until[channel]) {
        printf("[ata] %s recover: drive=%u lba=%u attempt=%d/%d reason=%s status=0x%X reset=deferred cooldown=%u\n",
               op,
               (unsigned)drive,
               (unsigned)lba,
               attempt,
               max_attempts,
               reason,
               (unsigned)status,
               (unsigned)(g_ata_reset_cooldown_until[channel] - g_ata_recovery_epoch));
        return 0;
    }

    if (error_code >= 0) {
        printf("[ata] %s recover: drive=%u lba=%u attempt=%d/%d reason=%s status=0x%X err=0x%X\n",
               op,
               (unsigned)drive,
               (unsigned)lba,
               attempt,
               max_attempts,
               reason,
               (unsigned)status,
               (unsigned)error_code);
    } else {
        printf("[ata] %s recover: drive=%u lba=%u attempt=%d/%d reason=%s status=0x%X\n",
               op,
               (unsigned)drive,
               (unsigned)lba,
               attempt,
               max_attempts,
               reason,
               (unsigned)status);
    }

    ata_soft_reset(ctrl_base);
    outportb(io_base + ATA_REG_HDDEVSEL, (drive & 1u) ? 0xB0 : 0xA0);
    ata_io_wait(ctrl_base);

    uint8 shift = g_ata_reset_backoff_shift[channel];
    if (shift > ATA_RESET_BACKOFF_MAX_SHIFT) shift = ATA_RESET_BACKOFF_MAX_SHIFT;
    uint32 window = (uint32)ATA_RESET_BACKOFF_MIN_WINDOW << shift;
    g_ata_reset_cooldown_until[channel] = g_ata_recovery_epoch + window;
    if (g_ata_reset_backoff_shift[channel] < ATA_RESET_BACKOFF_MAX_SHIFT) {
        g_ata_reset_backoff_shift[channel]++;
    }

    if (ata_poll(io_base) != 0) {
        printf("[ata] %s recover warning: drive=%u not ready after soft reset\n",
               op,
               (unsigned)drive);
    }
    return 0;
}

static void ata_run_lba48_smoke(void) {
#if CONFIG_ATA_LBA48_SMOKE
    int ran = 0;
    uint8 sector[512];

    printf("[ata] lba48 smoke: enabled\n");
    for (uint8 drive = 0; drive < 8; ++drive) {
        if (!detected_drives[drive].present) continue;
        if (!detected_drive_lba48[drive]) continue;

        uint64 total = detected_drive_lba48_sectors[drive];
        if (total <= (uint64)ATA_LBA48_SMOKE_TARGET_LBA) {
            printf("[ata] lba48 smoke skip: drive=%u total_sectors=%u (insufficient high-lba range)\n",
                   (unsigned)drive,
                   (unsigned)ata_u64_to_u32_sat(total));
            continue;
        }

        uint64 target64 = (total - 1u);
        if (target64 > (uint64)0xFFFFFFFFu) {
            target64 = (uint64)ATA_LBA48_SMOKE_TARGET_LBA;
        }
        if (target64 <= (uint64)ATA_LBA28_MAX_SECTOR) {
            target64 = (uint64)ATA_LBA48_SMOKE_TARGET_LBA;
        }

        uint32 target = (uint32)target64;
        int rc = ata_read_sector(drive, target, sector);
        printf("[ata] lba48 smoke: drive=%u lba=%u result=%s\n",
               (unsigned)drive,
               (unsigned)target,
               (rc == 0) ? "ok" : "fail");
        ran = 1;
    }

    if (!ran) {
        printf("[ata] lba48 smoke: no eligible drives\n");
    }
#endif
}

static int ata_program_sector_address(uint8 drive,
                                      uint16 io_base,
                                      uint16 ctrl_base,
                                      uint32 lba,
                                      uint8* out_cmd,
                                      int is_write) {
    if (!out_cmd) return -1;

    uint32 max_lba = 0;
    if (ata_compute_drive_max_lba(drive, &max_lba) != 0) {
        printf("ATA %s error: Drive %d capacity unknown (cannot validate LBA %u)\n",
               is_write ? "write" : "read",
               (int)drive,
               (unsigned)lba);
        return -1;
    }
    if (lba > max_lba) {
        printf("ATA %s error: Drive %d LBA %u out of range (max=%u)\n",
               is_write ? "write" : "read",
               (int)drive,
               (unsigned)lba,
               (unsigned)max_lba);
        return -1;
    }

    uint8 is_slave = (drive & 1u) ? 1u : 0u;

    if (detected_drive_lba[drive]) {
        if (lba > ATA_LBA28_MAX_SECTOR) {
            if (!detected_drive_lba48[drive]) {
                printf("ATA %s error: Drive %d LBA %u exceeds LBA28 and drive lacks LBA48\n",
                       is_write ? "write" : "read",
                       (int)drive,
                       (unsigned)lba);
                return -1;
            }

            /* LBA48 taskfile write order: high bytes first, then low bytes. */
            outportb(io_base + ATA_REG_HDDEVSEL, (uint8)(0x40 | (is_slave ? 0x10 : 0x00)));
            ata_io_wait(ctrl_base);

            outportb(io_base + ATA_REG_SECCOUNT0, 0x00);
            outportb(io_base + ATA_REG_LBA0, 0x00);
            outportb(io_base + ATA_REG_LBA1, 0x00);
            outportb(io_base + ATA_REG_LBA2, 0x00);

            outportb(io_base + ATA_REG_SECCOUNT0, 0x01);
            outportb(io_base + ATA_REG_LBA0, (uint8)(lba & 0xFFu));
            outportb(io_base + ATA_REG_LBA1, (uint8)((lba >> 8) & 0xFFu));
            outportb(io_base + ATA_REG_LBA2, (uint8)((lba >> 16) & 0xFFu));

            *out_cmd = is_write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT;
            return 0;
        }

        outportb(io_base + ATA_REG_HDDEVSEL,
                 (uint8)(0xE0 | (is_slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0Fu)));
        ata_io_wait(ctrl_base);
        outportb(io_base + ATA_REG_SECCOUNT0, 1);
        outportb(io_base + ATA_REG_LBA0, (uint8)(lba & 0xFFu));
        outportb(io_base + ATA_REG_LBA1, (uint8)((lba >> 8) & 0xFFu));
        outportb(io_base + ATA_REG_LBA2, (uint8)((lba >> 16) & 0xFFu));

        *out_cmd = is_write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO;
        return 0;
    }

    uint16 cyl = 0;
    uint16 heads = 0;
    uint16 spt = 0;
    if (ata_get_chs_geometry(drive, &cyl, &heads, &spt) != 0) {
        printf("ATA %s error: Drive %d lacks valid CHS geometry for non-LBA access\n",
               is_write ? "write" : "read",
               (int)drive);
        return -1;
    }

    uint32 sectors_per_cylinder = (uint32)heads * (uint32)spt;
    uint32 total_chs_sectors = (uint32)cyl * sectors_per_cylinder;
    if (sectors_per_cylinder == 0 || lba >= total_chs_sectors) {
        printf("ATA %s error: Drive %d CHS range exceeded for LBA %u (max=%u)\n",
               is_write ? "write" : "read",
               (int)drive,
               (unsigned)lba,
               (unsigned)(total_chs_sectors ? (total_chs_sectors - 1u) : 0u));
        return -1;
    }

    uint16 chs_cyl = (uint16)(lba / sectors_per_cylinder);
    uint32 rem = lba % sectors_per_cylinder;
    uint8 chs_head = (uint8)(rem / spt);
    uint8 chs_sector = (uint8)((rem % spt) + 1u);

    outportb(io_base + ATA_REG_HDDEVSEL,
             (uint8)(0xA0 | (is_slave ? 0x10 : 0x00) | (chs_head & 0x0Fu)));
    ata_io_wait(ctrl_base);
    outportb(io_base + ATA_REG_SECCOUNT0, 1);
    outportb(io_base + ATA_REG_LBA0, chs_sector);
    outportb(io_base + ATA_REG_LBA1, (uint8)(chs_cyl & 0xFFu));
    outportb(io_base + ATA_REG_LBA2, (uint8)((chs_cyl >> 8) & 0xFFu));

    *out_cmd = is_write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO;
    return 0;
}

// Enhanced drive detection for SATA compatibility
int ata_detect_drive(uint8 drive) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;
    uint16 io_base = ata_drive_io_base(drive);
    uint16 ctrl_base = ata_drive_ctrl_base(drive);
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset the drive first
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(ctrl_base);
    ata_soft_reset(ctrl_base);
    
    // Try to detect if drive is present
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return -1; // No drive present
    }
    
    // Try to identify the drive
    uint16 identify_data[256];
    int result = ata_identify(drive, identify_data);
    
    if (result == 0) {
        // Drive found, extract information
        detected_drives[drive].present = 1;
        
        // Extract model name
        for (int i = 0; i < 20; i++) {
            detected_drives[drive].model[i*2] = (identify_data[27+i] >> 8) & 0xFF;
            detected_drives[drive].model[i*2+1] = identify_data[27+i] & 0xFF;
        }
        detected_drives[drive].model[40] = '\0';
        
        // Get drive size
        detected_drives[drive].sectors = identify_data[60] | (identify_data[61] << 16);
        detected_drives[drive].size_mb = (detected_drives[drive].sectors / 2048);
        
        // Keep legacy semantics: default to "IDE".
        // Note: IDENTIFY does not reliably indicate SATA vs PATA transport.
        detected_drives[drive].type = 0;

        {
            uint16 caps49 = identify_data[49];
            uint16 caps83 = identify_data[83];
            detected_drive_atapi[drive] = (identify_data[0] & 0x8000u) ? 1u : 0u;
            detected_drive_dma[drive] = (caps49 & (1u << 8)) ? 1u : 0u;
            detected_drive_lba[drive] = (caps49 & (1u << 9)) ? 1u : 0u;
            detected_drive_lba48[drive] = (caps83 & (1u << 10)) ? 1u : 0u;
            detected_drive_cylinders[drive] = identify_data[1];
            detected_drive_heads[drive] = identify_data[3];
            detected_drive_spt[drive] = identify_data[6];

            if (detected_drive_lba48[drive]) {
                detected_drive_lba48_sectors[drive] =
                    (uint64)identify_data[100] |
                    ((uint64)identify_data[101] << 16) |
                    ((uint64)identify_data[102] << 32) |
                    ((uint64)identify_data[103] << 48);
            }
        }

        if (detected_drive_lba[drive]) {
            uint32 lba28_sectors = (uint32)identify_data[60] | ((uint32)identify_data[61] << 16);
            if (lba28_sectors != 0) {
                detected_drives[drive].sectors = lba28_sectors;
            }
            if (detected_drive_lba48[drive] && detected_drive_lba48_sectors[drive] != 0) {
                detected_drives[drive].sectors = ata_u64_to_u32_sat(detected_drive_lba48_sectors[drive]);
            }
        } else {
            uint16 chs_cyl = detected_drive_cylinders[drive];
            uint16 chs_heads = detected_drive_heads[drive];
            uint16 chs_spt = detected_drive_spt[drive];
            if (chs_cyl && chs_heads && chs_spt) {
                detected_drives[drive].sectors = (uint32)chs_cyl * (uint32)chs_heads * (uint32)chs_spt;
            }
        }
        detected_drives[drive].size_mb = (detected_drives[drive].sectors / 2048);

        const char* addr_mode = "chs";
        if (detected_drive_lba[drive]) {
            addr_mode = detected_drive_lba48[drive] ? "lba48" : "lba28";
        }

        printf("[ata] detect %u (%s): model='%s' sectors=%u lba=%u lba48=%u dma=%u atapi=%u addr=%s chs=%u/%u/%u\n",
               (unsigned)drive,
               ata_drive_slot_name(drive),
               detected_drives[drive].model,
               (unsigned)detected_drives[drive].sectors,
               (unsigned)detected_drive_lba[drive],
               (unsigned)detected_drive_lba48[drive],
               (unsigned)detected_drive_dma[drive],
               (unsigned)detected_drive_atapi[drive],
               addr_mode,
               (unsigned)detected_drive_cylinders[drive],
               (unsigned)detected_drive_heads[drive],
               (unsigned)detected_drive_spt[drive]);
        
        return 0;
    }
    
    return -1;
}

// Initialize all drives during system startup
void ata_init_drives() {
    printf("[ata] transfer path: pio-only (bus-master DMA not enabled)\n");

    g_ata_recovery_epoch = 0;
    g_ata_reset_cooldown_until[0] = 0;
    g_ata_reset_cooldown_until[1] = 0;
    g_ata_reset_backoff_shift[0] = 0;
    g_ata_reset_backoff_shift[1] = 0;
    g_ata_channel_health_score[0] = 0;
    g_ata_channel_health_score[1] = 0;

    ata_configure_channels_from_pci();

    ata_clear_detected_drive_state();
    uint8 physical_count = ata_probe_standard_slots_with_health();
    int selected_attempted = g_ata_ide_selected_candidate.found ? 1 : 0;
    int selected_succeeded = (physical_count > 0) ? 1 : 0;
    int legacy_attempted = 0;
    int legacy_succeeded = 0;

    {
        int any_native = (g_ata_channel_native_mode[0] || g_ata_channel_native_mode[1]) ? 1 : 0;
        if (any_native && physical_count == 0) {
            printf("[ata] pci ide: native probe found no drives, retrying legacy compatibility channels\n");
            legacy_attempted = 1;
            ata_set_legacy_channels();
            ata_log_channel_config();

            ata_clear_detected_drive_state();
            physical_count = ata_probe_standard_slots_with_health();
            legacy_succeeded = (physical_count > 0) ? 1 : 0;
            if (physical_count > 0) {
                printf("[ata] pci ide: legacy compatibility channels promoted after native demotion\n");
                selected_succeeded = 1;
            }
        }
    }

    if (physical_count == 0 && g_ata_ide_secondary_candidate.found) {
        printf("[ata] pci ide: selected candidate yielded no drives, trying secondary candidate\n");
        ata_apply_pci_ide_candidate(&g_ata_ide_secondary_candidate);

        ata_clear_detected_drive_state();
        physical_count = ata_probe_standard_slots_with_health();
        if (physical_count > 0) {
            g_ata_ide_selected_candidate = g_ata_ide_secondary_candidate;
            memset(&g_ata_ide_secondary_candidate, 0, sizeof(g_ata_ide_secondary_candidate));
            printf("[ata] pci ide: secondary candidate promoted\n");
            selected_succeeded = 1;
        } else {
            printf("[ata] pci ide: secondary candidate also yielded no drives\n");
            selected_succeeded = 0;
        }
    }

    if (selected_attempted && g_ata_ide_selected_candidate.found) {
        g_ata_prev_selected_candidate = g_ata_ide_selected_candidate;
        if (selected_succeeded) {
            g_ata_prev_selected_weight += 8;
        } else {
            g_ata_prev_selected_weight -= 16;
        }
        if (g_ata_prev_selected_weight > 32) g_ata_prev_selected_weight = 32;
        if (g_ata_prev_selected_weight < -64) g_ata_prev_selected_weight = -64;
    }

    if (legacy_attempted) {
        if (legacy_succeeded) {
            g_ata_prev_legacy_weight += 4;
        } else {
            g_ata_prev_legacy_weight -= 8;
        }
        if (g_ata_prev_legacy_weight > 16) g_ata_prev_legacy_weight = 16;
        if (g_ata_prev_legacy_weight < -32) g_ata_prev_legacy_weight = -32;
    }

    printf("[ata] pci ide: persistence weights selected=%d legacy=%d\n",
           g_ata_prev_selected_weight,
           g_ata_prev_legacy_weight);

    ata_run_lba48_smoke();
    
    // initialize logical drive mapping after detection
    init_logical_drive_mapping();

    {
        physical_count = ata_count_detected_physical_drives();
        printf("[ata] probe complete: physical=%u logical=%u\n",
               (unsigned)physical_count,
               (unsigned)num_logical_drives);
    }
}

int ata_identify(uint8 drive, uint16* identify_data) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;
    uint16 io_base = ata_drive_io_base(drive);
    uint16 ctrl_base = ata_drive_ctrl_base(drive);
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset drive
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(ctrl_base);
    ata_soft_reset(ctrl_base);
    
    // Clear registers
    outportb(io_base + ATA_REG_SECCOUNT0, 0);
    outportb(io_base + ATA_REG_LBA0, 0);
    outportb(io_base + ATA_REG_LBA1, 0);
    outportb(io_base + ATA_REG_LBA2, 0);
    
    // Send IDENTIFY command
    outportb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(ctrl_base);
    
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    
    if (status == 0 || status == 0xFF) {
        return -1;
    }
    
    // Older drives can take longer to clear BSY after IDENTIFY.
    int timeout = ATA_IDENTIFY_BSY_TIMEOUT;
    while ((inportb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) {
        return -1;
    }

    // If IDENTIFY errored, it may be an ATAPI device. Try IDENTIFY PACKET.
    status = inportb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) {
        uint8 lba1 = inportb(io_base + ATA_REG_LBA1);
        uint8 lba2 = inportb(io_base + ATA_REG_LBA2);
        if ((lba1 == 0x14 && lba2 == 0xEB) || (lba1 == 0x69 && lba2 == 0x96)) {
            outportb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
            ata_io_wait(ctrl_base);
        } else {
            return -1;
        }
    }
    
    // Give legacy devices time to assert DRQ after IDENTIFY/IDENTIFY PACKET.
    timeout = ATA_IDENTIFY_DRQ_TIMEOUT;
    while (!(inportb(io_base + ATA_REG_STATUS) & ATA_SR_DRQ) && --timeout);
    if (timeout == 0) {
        return -1;
    }
    
    // Read identify data
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(io_base + ATA_REG_DATA);
    }
    
    return 0;
}

int ata_read_sector(uint8 drive, uint32 lba, uint8* buf) {
    if (drive >= 8 || !detected_drives[drive].present) {
        return -1;
    }
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;

    /*
     * SECURITY-INVARIANT: Re-entrancy guard -- must be checked before touching
     * any controller register.
     *
     * Why: A synchronous page fault can fire at any point in this function,
     * including mid-PIO (e.g. on the BSY-polling branch or on a stack access).
     * The #PF handler runs synchronously on the same CPU stack and may call
     * swap_read_page -> ata_read_sector or swap_write_page -> ata_write_sector,
     * re-entering before this call has completed its command.  arch_irq_save()
     * only prevents asynchronous IRQ delivery; it cannot prevent this.
     *
     * Protocol: spin-wait until g_ata_busy is clear, then atomically claim it.
     * "Atomic" here is safe because x86 single-word stores/loads from the same
     * CPU are serialized; the PF is synchronous, so no true concurrency exists.
     * The spin is needed for the IRQ case (timer fires, schedules another read).
     * For the PF case the outer caller will never make progress while inside the
     * PF handler, so g_ata_busy will clear quickly (the outer PIO is done before
     * the PF fires if IRQs are disabled, or the IRQ guard ensures it completes).
     *
     * If the flag is still set when we check, we spin briefly then proceed --
     * the outer call must be nearly done (it holds IRQs off for its PIO).
     * A hard timeout prevents livelock if the outer call was abandoned.
     * Breakage if removed: nested PF swap-reads corrupt controller state and
     * cause BSY timeouts -> swap-in failure -> process SIGSEGV.
     * ABI-sensitive: No.  Security-critical: No.
     */
    {
        int spin = 2000000;
        while (g_ata_busy && --spin);
    }
    g_ata_busy = 1;

    uint16 io_base = ata_drive_io_base(drive);
    uint16 ctrl_base = ata_drive_ctrl_base(drive);

    for (int attempt = 1; attempt <= ATA_PIO_MAX_ATTEMPTS; ++attempt) {
        uint8 cmd = ATA_CMD_READ_PIO;
        if (ata_program_sector_address(drive, io_base, ctrl_base, lba, &cmd, 0) != 0) {
            g_ata_busy = 0;
            return -1;
        }

        /*
         * ABI-INVARIANT: Interrupts must be disabled from command-send through
         * data-read to prevent timer-IRQ-triggered re-entrant ATA access.
         */
        arch_irq_state_t irq_state = arch_irq_save();

        // Send read command
        outportb(io_base + ATA_REG_COMMAND, cmd);
        ata_io_wait(ctrl_base);

        int timeout = ATA_PIO_BSY_TIMEOUT;
        uint8 status = 0;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "read",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              "bsy-timeout",
                                              status,
                                              -1) == 0) {
                continue;
            }
            g_ata_busy = 0;
            printf("ATA read timeout: Drive %d LBA %d - BSY timeout\n", (int)drive, (int)lba);
            return -1;
        }

        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "read",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              (status & ATA_SR_DF) ? "device-fault" : "status-error",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            if (status & ATA_SR_ERR) {
                printf("ATA read error: Drive %d LBA %d - Error 0x%x\n", (int)drive, (int)lba, error_code);
            } else {
                printf("ATA read fault: Drive %d LBA %d - Status 0x%x\n", (int)drive, (int)lba, (int)status);
            }
            return -1;
        }

        timeout = ATA_PIO_DRQ_TIMEOUT;
        while (timeout > 0) {
            status = inportb(io_base + ATA_REG_STATUS);
            if (status & ATA_SR_DRQ) break;
            if (status & (ATA_SR_ERR | ATA_SR_DF)) break;
            --timeout;
        }

        if (!(status & ATA_SR_DRQ)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "read",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              (timeout == 0) ? "drq-timeout" : "drq-error",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            if (timeout == 0) {
                printf("ATA read timeout: Drive %d LBA %d - DRQ timeout\n", (int)drive, (int)lba);
            } else {
                printf("ATA read error: Drive %d LBA %d - Status 0x%x\n", (int)drive, (int)lba, (int)status);
            }
            return -1;
        }

        // Read data
        for (int i = 0; i < 256; i++) {
            uint16 data = inw(io_base + ATA_REG_DATA);
            buf[i * 2] = data & 0xFF;
            buf[i * 2 + 1] = (data >> 8) & 0xFF;
        }

        ata_io_wait(ctrl_base);
        arch_irq_restore(irq_state);
        ata_note_io_success(drive);
        g_ata_busy = 0;
        return 0;
    }

    g_ata_busy = 0;
    return -1;
}

int ata_write_sector(uint8 drive, uint32 lba, const uint8* buf) {
    if (drive >= 8 || !detected_drives[drive].present) {
        return -1;
    }
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;

    /* Re-entrancy guard -- see ata_read_sector for full rationale. */
    {
        int spin = 2000000;
        while (g_ata_busy && --spin);
    }
    g_ata_busy = 1;

    uint16 io_base = ata_drive_io_base(drive);
    uint16 ctrl_base = ata_drive_ctrl_base(drive);

    for (int attempt = 1; attempt <= ATA_PIO_MAX_ATTEMPTS; ++attempt) {
        uint8 cmd = ATA_CMD_WRITE_PIO;
        if (ata_program_sector_address(drive, io_base, ctrl_base, lba, &cmd, 1) != 0) {
            g_ata_busy = 0;
            return -1;
        }

        /* Same IRQ guard as ata_read_sector -- see comment there. */
        arch_irq_state_t irq_state = arch_irq_save();

        // Send write command
        outportb(io_base + ATA_REG_COMMAND, cmd);
        ata_io_wait(ctrl_base);

        int timeout = ATA_PIO_BSY_TIMEOUT;
        uint8 status = 0;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              "bsy-timeout",
                                              status,
                                              -1) == 0) {
                continue;
            }
            g_ata_busy = 0;
            printf("ATA write timeout: Drive %d LBA %d - BSY timeout\n", (int)drive, (int)lba);
            return -1;
        }

        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              (status & ATA_SR_DF) ? "device-fault" : "status-error",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            if (status & ATA_SR_ERR) {
                printf("ATA write error: Drive %d LBA %d - Error 0x%x\n", (int)drive, (int)lba, error_code);
            } else {
                printf("ATA write fault: Drive %d LBA %d - Status 0x%x\n", (int)drive, (int)lba, (int)status);
            }
            return -1;
        }

        timeout = ATA_PIO_DRQ_TIMEOUT;
        while (timeout > 0) {
            status = inportb(io_base + ATA_REG_STATUS);
            if (status & ATA_SR_DRQ) break;
            if (status & (ATA_SR_ERR | ATA_SR_DF)) break;
            --timeout;
        }

        if (!(status & ATA_SR_DRQ)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              (timeout == 0) ? "drq-timeout" : "drq-error",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            if (timeout == 0) {
                printf("ATA write timeout: Drive %d LBA %d - DRQ timeout\n", (int)drive, (int)lba);
            } else {
                printf("ATA write error: Drive %d LBA %d - Status 0x%x\n", (int)drive, (int)lba, (int)status);
            }
            return -1;
        }

        // Write the data
        for (int i = 0; i < 256; i++) {
            uint16 data = buf[i * 2] | (buf[i * 2 + 1] << 8);
            outw(io_base + ATA_REG_DATA, data);
        }

        ata_io_wait(ctrl_base);

        timeout = ATA_PIO_POST_WRITE_BSY_TIMEOUT;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              "post-write-bsy-timeout",
                                              status,
                                              -1) == 0) {
                continue;
            }
            g_ata_busy = 0;
            return -1;
        }

        timeout = ATA_PIO_POST_WRITE_DRDY_TIMEOUT;
        while (timeout > 0) {
            status = inportb(io_base + ATA_REG_STATUS);
            if (status & ATA_SR_DRDY) break;
            if (status & (ATA_SR_ERR | ATA_SR_DF)) break;
            --timeout;
        }
        if (!(status & ATA_SR_DRDY)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              (timeout == 0) ? "post-write-drdy-timeout" : "post-write-status-error",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            return -1;
        }

        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            int error_code = (status & ATA_SR_ERR) ? (int)inportb(io_base + ATA_REG_ERROR) : -1;
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
                                              ctrl_base,
                                              "write",
                                              lba,
                                              attempt,
                                              ATA_PIO_MAX_ATTEMPTS,
                                              "post-write-final-status",
                                              status,
                                              error_code) == 0) {
                continue;
            }
            g_ata_busy = 0;
            return -1;
        }

        arch_irq_restore(irq_state);
        ata_note_io_success(drive);
        g_ata_busy = 0;
        return 0;
    }

    g_ata_busy = 0;
    return -1;
}

// Get drive information
drive_info_t* ata_get_drive_info(uint8 drive) {
    if (drive >= 8) return NULL;
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return NULL;
    return &detected_drives[drive];
}

void ata_identify_drive(uint8 drive, char* model, uint32* sectors) {
    if (model) model[0] = '\0';
    if (sectors) *sectors = 0;

    if (drive >= 8) return;
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;

    if (detected_drives[drive].present) {
        if (model) {
            strncpy(model, detected_drives[drive].model, 40);
            model[40] = '\0';
        }
        if (sectors) *sectors = detected_drives[drive].sectors;
        return;
    }

    uint16 id[256];
    if (ata_identify(drive, id) != 0) return;

    if (model) {
        for (int i = 0; i < 20; i++) {
            model[i * 2] = (id[27 + i] >> 8) & 0xFF;
            model[i * 2 + 1] = id[27 + i] & 0xFF;
        }
        model[40] = '\0';
    }
    if (sectors) {
        uint16 caps49 = id[49];
        int has_lba = (caps49 & (1u << 9)) ? 1 : 0;
        uint32 lba28 = (uint32)id[60] | ((uint32)id[61] << 16);
        if (has_lba && lba28) {
            *sectors = lba28;
        } else {
            uint16 chs_cyl = id[1];
            uint16 chs_heads = id[3];
            uint16 chs_spt = id[6];
            *sectors = (uint32)chs_cyl * (uint32)chs_heads * (uint32)chs_spt;
        }
    }
}

// Check if drive is present
int ata_drive_present(uint8 drive) {
    if (drive >= 8) return 0;
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return 0;
    return detected_drives[drive].present;
}

// Enhanced read with retry mechanism
int ata_read_sector_retry(uint8 drive, uint32 lba, uint8* buf, int max_retries) {
    int retries = 0;
    int result;
    
    while (retries < max_retries) {
        if ((retries & 0x3) == 0) ata_ctx_account(SCHED_COST_FS);
        result = ata_read_sector(drive, lba, buf);
        if (result == 0) {
            return 0; // Success
        }
        
        retries++;
        if (retries < max_retries) {
            printf("ATA read retry %d/%d for drive %d LBA %d\n", retries, max_retries, drive, lba);
            // Small delay before retry
            for (int i = 0; i < 10000; i++) {
                asm volatile("nop");
            }
        }
    }
    
    printf("ATA read failed after %d retries for drive %d LBA %d\n", max_retries, drive, lba);
    return -1;
}

// Enhanced write with retry mechanism
int ata_write_sector_retry(uint8 drive, uint32 lba, const uint8* buf, int max_retries) {
    int retries = 0;
    int result;
    
    while (retries < max_retries) {
        if ((retries & 0x3) == 0) ata_ctx_account(SCHED_COST_FS);
        result = ata_write_sector(drive, lba, buf);
        if (result == 0) {
            return 0; // Success
        }
        
        retries++;
        if (retries < max_retries) {
            printf("ATA write retry %d/%d for drive %d LBA %d\n", retries, max_retries, drive, lba);
            // Small delay before retry
            for (int i = 0; i < 10000; i++) {
                asm volatile("nop");
            }
        }
    }
    
    printf("ATA write failed after %d retries for drive %d LBA %d\n", max_retries, drive, lba);
    return -1;
}

// Get drive status with detailed information
int ata_get_drive_status(uint8 drive, char* status_buffer, int buffer_size) {
    if (drive >= 8 || !detected_drives[drive].present || !status_buffer) {
        return -1;
    }
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;
    
    drive_info_t* info = &detected_drives[drive];
    
    // Simple string formatting without sprintf
    strcpy(status_buffer, "Drive ");
    // Convert drive number to string (simple approach)
    if (drive < 10) {
        status_buffer[6] = '0' + drive;
        status_buffer[7] = '\0';
    } else {
        status_buffer[6] = '0' + (drive / 10);
        status_buffer[7] = '0' + (drive % 10);
        status_buffer[8] = '\0';
    }
    
    strcat(status_buffer, ": ");
    strcat(status_buffer, info->model);
    strcat(status_buffer, " (");
    strcat(status_buffer, (info->type == 1) ? "SATA" : "IDE");
    strcat(status_buffer, ", ");
    
    // Convert size to string (simplified)
    char size_str[32];
    int size = info->size_mb;
    int size_pos = 0;
    if (size >= 1000) {
        size_str[size_pos++] = '0' + (size / 1000);
        size = size % 1000;
    }
    if (size >= 100) {
        size_str[size_pos++] = '0' + (size / 100);
        size = size % 100;
    }
    if (size >= 10) {
        size_str[size_pos++] = '0' + (size / 10);
        size = size % 10;
    }
    size_str[size_pos++] = '0' + size;
    size_str[size_pos] = '\0';
    
    strcat(status_buffer, size_str);
    strcat(status_buffer, " MB)");
    
    return 0;
}

// List all detected drives
void ata_list_drives(void) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    printf("Detected ATA/SATA drives:\n");
    for (int i = 0; i < 8; i++) {
        if (detected_drives[i].present) {
            char status[256];
            if (ata_get_drive_status(i, status, sizeof(status)) == 0) {
                printf("  %s\n", status);
            }
        }
    }
}

// initialize logical drive mapping
static void init_logical_drive_mapping(void) {
    num_logical_drives = 0;
    
    // clear all mappings
    for (int i = 0; i < 8; i++) {
        logical_to_physical_map[i] = 0xFF;  // invalid mapping
        physical_to_logical_map[i] = 0xFF;  // invalid mapping
    }
    
    // map present drives to logical numbers
    for (int physical = 0; physical < 8; physical++) {
        if (detected_drives[physical].present) {
            logical_to_physical_map[num_logical_drives] = physical;
            physical_to_logical_map[physical] = num_logical_drives;
            num_logical_drives++;
        }
    }
}

// get physical drive number from logical drive number
uint8 ata_logical_to_physical(uint8 logical_drive) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return 0xFF;
    if (logical_drive >= num_logical_drives) {
        return 0xFF;  // invalid
    }
    return logical_to_physical_map[logical_drive];
}

// get logical drive number from physical drive number
uint8 ata_physical_to_logical(uint8 physical_drive) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return 0xFF;
    if (physical_drive >= 8) {
        return 0xFF;  // invalid
    }
    return physical_to_logical_map[physical_drive];
}

// get number of logical drives
uint8 ata_get_num_logical_drives(void) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return 0;
    return num_logical_drives;
}

// check if logical drive is present
int ata_logical_drive_present(uint8 logical_drive) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return 0;
    if (logical_drive >= num_logical_drives) {
        return 0;
    }
    uint8 physical = logical_to_physical_map[logical_drive];
    return detected_drives[physical].present;
} 