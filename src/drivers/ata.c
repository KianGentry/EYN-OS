#include <misc/types.h>
#include <system.h>
#include <vga.h>
#include <string.h>
#include <ata.h>
#include <context.h>
#include <misc/sched.h>
#include <cpu/arch.h>

// Define NULL if not available
#ifndef NULL
#define NULL ((void*)0)
#endif

#define ATA_PRIMARY_IO 0x1F0
#define ATA_SECONDARY_IO 0x170
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_CTRL 0x376

// SATA ports (common on Dell Optiplex 755)
#define SATA_PRIMARY_IO 0x1F0
#define SATA_SECONDARY_IO 0x170
#define SATA_PRIMARY_CTRL 0x3F6
#define SATA_SECONDARY_CTRL 0x376

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

#ifndef CONFIG_ATA_LBA48_SMOKE
#define CONFIG_ATA_LBA48_SMOKE 0
#endif

#define ATA_LBA28_MAX_SECTOR 0x0FFFFFFFu
#define ATA_LBA48_SMOKE_TARGET_LBA 0x10000000u

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

static void ata_io_wait(uint16 io_base) {
    for (int i = 0; i < 4; i++) inportb(io_base + ATA_REG_ALTSTATUS);
}

static void ata_soft_reset(uint16 io_base) {
    /* Legacy control port is io_base + 0x206 (0x3F6/0x376). */
    uint16 ctrl = io_base + ATA_REG_ALTSTATUS;
    outportb(ctrl, 0x04); /* SRST */
    ata_io_wait(io_base);
    outportb(ctrl, 0x00);
    ata_io_wait(io_base);
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

static int ata_try_recover_after_failure(uint8 drive,
                                         uint16 io_base,
                                         const char* op,
                                         uint32 lba,
                                         int attempt,
                                         int max_attempts,
                                         const char* reason,
                                         uint8 status,
                                         int error_code) {
    if (attempt >= max_attempts) {
        return -1;
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

    ata_soft_reset(io_base);
    outportb(io_base + ATA_REG_HDDEVSEL, (drive & 1u) ? 0xB0 : 0xA0);
    ata_io_wait(io_base);
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

static int ata_program_sector_address(uint8 drive, uint16 io_base, uint32 lba, uint8* out_cmd, int is_write) {
    if (!out_cmd) return -1;

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
            ata_io_wait(io_base);

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
        ata_io_wait(io_base);
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
    ata_io_wait(io_base);
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
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset the drive first
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(io_base);
    ata_soft_reset(io_base);
    
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

    // Clear drive info
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
    
    // Probe classic primary/secondary master/slave.
    for (int drive = 0; drive < 4; drive++) {
        ata_detect_drive(drive);
    }

    ata_run_lba48_smoke();
    
    // initialize logical drive mapping after detection
    init_logical_drive_mapping();

    {
        uint8 physical_count = 0;
        for (int i = 0; i < 8; i++) {
            if (detected_drives[i].present) physical_count++;
        }
        printf("[ata] probe complete: physical=%u logical=%u\n",
               (unsigned)physical_count,
               (unsigned)num_logical_drives);
    }
}

int ata_identify(uint8 drive, uint16* identify_data) {
    if (!ata_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return -1;
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset drive
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(io_base);
    ata_soft_reset(io_base);
    
    // Clear registers
    outportb(io_base + ATA_REG_SECCOUNT0, 0);
    outportb(io_base + ATA_REG_LBA0, 0);
    outportb(io_base + ATA_REG_LBA1, 0);
    outportb(io_base + ATA_REG_LBA2, 0);
    
    // Send IDENTIFY command
    outportb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(io_base);
    
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
            ata_io_wait(io_base);
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

    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;

    for (int attempt = 1; attempt <= ATA_PIO_MAX_ATTEMPTS; ++attempt) {
        uint8 cmd = ATA_CMD_READ_PIO;
        if (ata_program_sector_address(drive, io_base, lba, &cmd, 0) != 0) {
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
        ata_io_wait(io_base);

        int timeout = ATA_PIO_BSY_TIMEOUT;
        uint8 status = 0;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
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

        ata_io_wait(io_base);
        arch_irq_restore(irq_state);
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

    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;

    for (int attempt = 1; attempt <= ATA_PIO_MAX_ATTEMPTS; ++attempt) {
        uint8 cmd = ATA_CMD_WRITE_PIO;
        if (ata_program_sector_address(drive, io_base, lba, &cmd, 1) != 0) {
            g_ata_busy = 0;
            return -1;
        }

        /* Same IRQ guard as ata_read_sector -- see comment there. */
        arch_irq_state_t irq_state = arch_irq_save();

        // Send write command
        outportb(io_base + ATA_REG_COMMAND, cmd);
        ata_io_wait(io_base);

        int timeout = ATA_PIO_BSY_TIMEOUT;
        uint8 status = 0;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
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

        ata_io_wait(io_base);

        timeout = ATA_PIO_POST_WRITE_BSY_TIMEOUT;
        while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (--timeout == 0) break;
        }
        if (timeout == 0) {
            arch_irq_restore(irq_state);
            if (ata_try_recover_after_failure(drive,
                                              io_base,
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