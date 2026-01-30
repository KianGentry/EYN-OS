#include <ata.h>
#include <drivers/aarch64/virtio_blk.h>

/*
 * AArch64 block I/O bridge.
 *
 * The existing filesystem stack (VFS/EYNFS/FAT32) expects the legacy ATA API
 * (ata_read_sector/ata_write_sector). On QEMU 'virt' there is no IDE/ATA; the
 * standard disk device is virtio-blk over virtio-mmio.
 *
 * This file provides a minimal ATA-compatible implementation backed by
 * virtio-blk, allowing the existing filesystem code to run unchanged.
 */

static drive_info_t g_drive0;
static int g_inited;
static int g_present;

static int ensure_ready(void) {
    if (!g_inited) {
        /* Kernel is expected to call virtio_blk_init(dtb_ptr) during boot.
         * If it didn't, we will simply report "no drive".
         */
        g_inited = 1;
    }
    return g_present ? 0 : -1;
}

/* Called by the AArch64 boot path once virtio-blk is initialized. */
void ata_init_drives(void) {
    g_inited = 1;
    g_present = 1;

    g_drive0.present = 1;
    g_drive0.type = 0;
    /*
     * Model string must be NUL-terminated.
     *
     * Note: drive_info_t starts with two uint8 fields, so model[] is not
     * naturally word-aligned. On AArch64, the compiler may emit unaligned
     * wide stores (e.g. STP) when optimizing simple copies, which can fault.
     * Force byte stores via volatile.
     */
    const char* model = "virtio-blk";
    volatile char* dst = (volatile char*)g_drive0.model;
    for (int i = 0; i < 40; i++) {
        char c = model[i];
        dst[i] = c ? c : ' ';
        if (!c) {
            for (int j = i + 1; j < 40; j++) dst[j] = ' ';
            break;
        }
    }
    dst[40] = '\0';

    /*
     * Unknown; virtio-blk can report capacity but we don't need it yet.
     * Force 32-bit stores: these fields sit at a 4-byte aligned offset.
     */
    volatile uint32* p32 = (volatile uint32*)(void*)&g_drive0.sectors;
    p32[0] = 0;
    p32[1] = 0;
}

int ata_detect_drive(uint8 drive) {
    if (drive != 0) return -1;
    return ensure_ready();
}

int ata_identify(uint8 drive, uint16* identify_data) {
    (void)drive;
    (void)identify_data;
    return -1;
}

int ata_read_sector(uint8 drive, uint32 lba, uint8* buf) {
    if (drive != 0 || !buf) return -1;
    if (ensure_ready() != 0) return -1;
    return virtio_blk_read_sector(lba, buf);
}

int ata_write_sector(uint8 drive, uint32 lba, const uint8* buf) {
    if (drive != 0 || !buf) return -1;
    if (ensure_ready() != 0) return -1;
    return virtio_blk_write_sector(lba, buf);
}

int ata_read_sector_retry(uint8 drive, uint32 lba, uint8* buf, int max_retries) {
    if (max_retries <= 0) max_retries = 1;
    for (int i = 0; i < max_retries; i++) {
        if (ata_read_sector(drive, lba, buf) == 0) return 0;
    }
    return -1;
}

int ata_write_sector_retry(uint8 drive, uint32 lba, const uint8* buf, int max_retries) {
    if (max_retries <= 0) max_retries = 1;
    for (int i = 0; i < max_retries; i++) {
        if (ata_write_sector(drive, lba, buf) == 0) return 0;
    }
    return -1;
}

int ata_drive_present(uint8 drive) {
    if (drive != 0) return 0;
    return (ensure_ready() == 0) ? 1 : 0;
}

drive_info_t* ata_get_drive_info(uint8 drive) {
    if (drive != 0) return 0;
    return (ensure_ready() == 0) ? &g_drive0 : 0;
}

void ata_list_drives(void) {
    /* Not used by the AArch64 bring-up shell yet. */
}

void ata_identify_drive(uint8 drive, char* model, uint32* sectors) {
    if (model) {
        model[0] = 0;
    }
    if (sectors) {
        *sectors = 0;
    }

    drive_info_t* di = ata_get_drive_info(drive);
    if (!di) return;

    if (model) {
        int i = 0;
        for (; i < 40 && di->model[i]; i++) model[i] = di->model[i];
        model[i] = 0;
    }
    if (sectors) {
        *sectors = di->sectors;
    }
}

uint8 ata_logical_to_physical(uint8 logical_drive) {
    return logical_drive;
}

uint8 ata_physical_to_logical(uint8 physical_drive) {
    return physical_drive;
}

uint8 ata_get_num_logical_drives(void) {
    return (ensure_ready() == 0) ? 1 : 0;
}

int ata_logical_drive_present(uint8 logical_drive) {
    return (logical_drive == 0 && ensure_ready() == 0) ? 1 : 0;
}
