#ifndef SYSTEM_H
#define SYSTEM_H
#include <types.h>
#include <stdint.h>

uint8 inportb (uint16 _port);
void outportb (uint16 _port, uint8 _data);
void sleep(uint8 times);
void Shutdown(void);
int ata_read_sector(uint8 drive, uint32 lba, uint8* buf);
int ata_write_sector(uint8 drive, uint32 lba, const uint8* buf);
int ata_identify(uint8 drive, uint16* identify_data);
void ata_init_drives(void);
int ata_detect_drive(uint8 drive);
int ata_drive_present(uint8 drive);
uint8 ata_logical_to_physical(uint8 logical_drive);
uint8 ata_physical_to_logical(uint8 physical_drive);
uint8 ata_get_num_logical_drives(void);
int ata_logical_drive_present(uint8 logical_drive);
uint16 inw(uint16 _port);
void outw(uint16 _port, uint16 _data);

// 32-bit port I/O is required by some PC hardware interfaces.
// Primary current use: PCI config mechanism #1 (0xCF8/0xCFC) for device discovery.
uint32 inl(uint16 _port);
void outl(uint16 _port, uint32 _data);



#endif