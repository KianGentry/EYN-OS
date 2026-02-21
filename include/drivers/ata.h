#ifndef ATA_H
#define ATA_H

#include <types.h>

/*
 * Legacy ATA/IDE (PIO) driver interface.
 *
 * Drives are addressed by a small physical index:
 *   0 = primary master, 1 = primary slave, 2 = secondary master, 3 = secondary slave
 * The implementation currently supports up to 8 slots, but only 0-3 map to classic IDE.
 */

#define ATA_MAX_DRIVES 8

typedef struct {
    uint8 present;
    uint8 type; // historical: 0=IDE, 1=SATA, 2=RAID (do not rely on this for transport) 
    char model[41];
    uint32 sectors; /* LBA28 sector count when available */
    uint32 size_mb;
} drive_info_t;

void ata_init_drives(void);
int ata_detect_drive(uint8 drive);

int ata_identify(uint8 drive, uint16* identify_data);

int ata_read_sector(uint8 drive, uint32 lba, uint8* buf);
int ata_write_sector(uint8 drive, uint32 lba, const uint8* buf);

int ata_read_sector_retry(uint8 drive, uint32 lba, uint8* buf, int max_retries);
int ata_write_sector_retry(uint8 drive, uint32 lba, const uint8* buf, int max_retries);

int ata_drive_present(uint8 drive);

drive_info_t* ata_get_drive_info(uint8 drive);
void ata_list_drives(void);

// Compatibility helper used by some shell tooling.
void ata_identify_drive(uint8 drive, char* model, uint32* sectors);

// Logical drive mapping helpers
uint8 ata_logical_to_physical(uint8 logical_drive);
uint8 ata_physical_to_logical(uint8 physical_drive);
uint8 ata_get_num_logical_drives(void);
int ata_logical_drive_present(uint8 logical_drive);

#endif // ATA_H
