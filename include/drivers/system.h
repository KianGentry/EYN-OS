#ifndef SYSTEM_H
#define SYSTEM_H
#include <misc/types.h>
#include <stdint.h>
#include <ata.h>

uint8 inportb (uint16 _port);
void outportb (uint16 _port, uint8 _data);
void sleep(uint8 times);
void Shutdown(void);
uint16 inw(uint16 _port);
void outw(uint16 _port, uint16 _data);

// 32-bit port I/O is required by some PC hardware interfaces.
// Primary current use: PCI config mechanism #1 (0xCF8/0xCFC) for device discovery.
uint32 inl(uint16 _port);
void outl(uint16 _port, uint32 _data);

#endif