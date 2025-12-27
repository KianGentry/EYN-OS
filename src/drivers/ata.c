#include <types.h>
#include <system.h>
#include <vga.h>
#include <string.h>

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

// SATA specific features
#define ATA_FEATURE_SATA_ENABLE 0x10
#define ATA_FEATURE_SATA_DISABLE 0x90

// Drive detection structure
typedef struct {
    uint8 present;
    uint8 type;  // 0=IDE, 1=SATA, 2=RAID
    char model[41];
    uint32 sectors;
    uint32 size_mb;
} drive_info_t;

static drive_info_t detected_drives[8];

// logical drive mapping system
static uint8 logical_to_physical_map[8];  // maps logical drive (0,1,2...) to physical drive (0,1,2,3,4,5,6,7)
static uint8 physical_to_logical_map[8];  // maps physical drive to logical drive (0xFF = not mapped)
static uint8 num_logical_drives = 0;

// function declarations
static void init_logical_drive_mapping(void);

static void ata_io_wait(uint16 io_base) {
    for (int i = 0; i < 4; i++) inportb(io_base + ATA_REG_ALTSTATUS);
}

static int ata_poll(uint16 io_base) {
    for (int i = 0; i < 100000; i++) {
        uint8 status = inportb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) return 0;
    }
    return -1;
}

// Enhanced drive detection for SATA compatibility
int ata_detect_drive(uint8 drive) {
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset the drive first
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(io_base);
    
    // Try to detect if drive is present
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    if (status == 0) {
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
        
        // Determine drive type based on identify data
        if (identify_data[83] & 0x0400) {
            detected_drives[drive].type = 1; // SATA
        } else {
            detected_drives[drive].type = 0; // IDE
        }
        
        return 0;
    }
    
    return -1;
}

// Initialize all drives during system startup
void ata_init_drives() {
    // Clear drive info
    for (int i = 0; i < 8; i++) {
        detected_drives[i].present = 0;
        detected_drives[i].type = 0;
        detected_drives[i].sectors = 0;
        detected_drives[i].size_mb = 0;
        detected_drives[i].model[0] = '\0';
    }
    
    // Probe only primary drives (0,1) first for faster boot
    // Secondary drives (2,3) are less common and can be detected on-demand
    for (int drive = 0; drive < 2; drive++) {
        ata_detect_drive(drive);
    }
    
    // initialize logical drive mapping after detection
    init_logical_drive_mapping();
}

int ata_identify(uint8 drive, uint16* identify_data) {
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xB0 : 0xA0;
    
    // Reset drive
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit);
    ata_io_wait(io_base);
    
    // Clear registers
    outportb(io_base + ATA_REG_SECCOUNT0, 0);
    outportb(io_base + ATA_REG_LBA0, 0);
    outportb(io_base + ATA_REG_LBA1, 0);
    outportb(io_base + ATA_REG_LBA2, 0);
    
    // Send IDENTIFY command
    outportb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(io_base);
    
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    
    if (status == 0) {
        return -1;
    }
    
    // Wait for BSY to clear (reduced timeout for faster boot)
    int timeout = 10000; // Reduced from 1000000
    while ((inportb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) { 
        return -1; 
    }
    
    // Wait for DRQ to set (reduced timeout for faster boot)
    timeout = 10000; // Reduced from 1000000
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
    
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xF0 : 0xE0;
    
    // Set up LBA addressing
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit | ((lba >> 24) & 0x0F));
    outportb(io_base + ATA_REG_SECCOUNT0, 1);
    outportb(io_base + ATA_REG_LBA0, (uint8)(lba & 0xFF));
    outportb(io_base + ATA_REG_LBA1, (uint8)((lba >> 8) & 0xFF));
    outportb(io_base + ATA_REG_LBA2, (uint8)((lba >> 16) & 0xFF));
    
    // Send read command
    outportb(io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    ata_io_wait(io_base);
    
    // Wait for BSY to clear with better error handling
    int timeout = 100000;
    while ((inportb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) { 
        // vga printf formatter does not support %u
        printf("ATA read timeout: Drive %d LBA %d - BSY timeout\n", (int)drive, (int)lba);
        return -1; 
    }
    
    // Check for errors before waiting for DRQ
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) {
        uint8 error = inportb(io_base + ATA_REG_ERROR);
        // vga printf formatter does not support width/padding or %X
        printf("ATA read error: Drive %d LBA %d - Error 0x%x\n", (int)drive, (int)lba, (int)error);
        return -1;
    }
    
    // Wait for DRQ to set
    timeout = 100000;
    while (!(inportb(io_base + ATA_REG_STATUS) & ATA_SR_DRQ) && --timeout);
    if (timeout == 0) { 
        printf("ATA read timeout: Drive %d LBA %d - DRQ timeout\n", (int)drive, (int)lba);
        return -1; 
    }
    
    // Read data
    for (int i = 0; i < 256; i++) {
        uint16 data = inw(io_base + ATA_REG_DATA);
        buf[i*2] = data & 0xFF;
        buf[i*2+1] = (data >> 8) & 0xFF;
    }
    
    ata_io_wait(io_base);
    return 0;
}

int ata_write_sector(uint8 drive, uint32 lba, const uint8* buf) {
    if (drive >= 8 || !detected_drives[drive].present) {
        return -1;
    }
    
    uint16 io_base = (drive & 2) ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
    uint8 slavebit = (drive & 1) ? 0xF0 : 0xE0;
    
    // Set up LBA addressing
    outportb(io_base + ATA_REG_HDDEVSEL, slavebit | ((lba >> 24) & 0x0F));
    outportb(io_base + ATA_REG_SECCOUNT0, 1);
    outportb(io_base + ATA_REG_LBA0, (uint8)(lba & 0xFF));
    outportb(io_base + ATA_REG_LBA1, (uint8)((lba >> 8) & 0xFF));
    outportb(io_base + ATA_REG_LBA2, (uint8)((lba >> 16) & 0xFF));
    
    // Send write command
    outportb(io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    ata_io_wait(io_base);

    // Wait for BSY to clear with better error handling
    int timeout = 100000;
    while ((inportb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) { 
        printf("ATA write timeout: Drive %d LBA %d - BSY timeout\n", (int)drive, (int)lba);
        return -1; 
    }
    
    // Check for errors before waiting for DRQ
    uint8 status = inportb(io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) {
        uint8 error = inportb(io_base + ATA_REG_ERROR);
        printf("ATA write error: Drive %d LBA %d - Error 0x%x\n", (int)drive, (int)lba, (int)error);
        return -1;
    }
    
    // Wait for DRQ to set
    timeout = 100000;
    while (!(inportb(io_base + ATA_REG_STATUS) & ATA_SR_DRQ) && --timeout);
    if (timeout == 0) { 
        printf("ATA write timeout: Drive %d LBA %d - DRQ timeout\n", (int)drive, (int)lba);
        return -1; 
    }

    // Write the data
    for (int i = 0; i < 256; i++) {
        uint16 data = buf[i*2] | (buf[i*2+1] << 8);
        outw(io_base + ATA_REG_DATA, data);
    }

    ata_io_wait(io_base);

    // Wait for BSY to clear and DRDY to set after writing
    timeout = 1000000;
    while ((inportb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) { 
        return -1; 
    }
    
    timeout = 1000000;
    while (!(inportb(io_base + ATA_REG_STATUS) & ATA_SR_DRDY) && --timeout);
    if (timeout == 0) { 
        return -1; 
    }

    // Check for errors
    uint8 final_status = inportb(io_base + ATA_REG_STATUS);
    if (final_status & (ATA_SR_ERR | ATA_SR_DF)) {
        return -1;
    }

    return 0;
}

// Get drive information
drive_info_t* ata_get_drive_info(uint8 drive) {
    if (drive >= 8) return NULL;
    return &detected_drives[drive];
}

// Check if drive is present
int ata_drive_present(uint8 drive) {
    if (drive >= 8) return 0;
    return detected_drives[drive].present;
}

// Enhanced read with retry mechanism
int ata_read_sector_retry(uint8 drive, uint32 lba, uint8* buf, int max_retries) {
    int retries = 0;
    int result;
    
    while (retries < max_retries) {
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
    if (logical_drive >= num_logical_drives) {
        return 0xFF;  // invalid
    }
    return logical_to_physical_map[logical_drive];
}

// get logical drive number from physical drive number
uint8 ata_physical_to_logical(uint8 physical_drive) {
    if (physical_drive >= 8) {
        return 0xFF;  // invalid
    }
    return physical_to_logical_map[physical_drive];
}

// get number of logical drives
uint8 ata_get_num_logical_drives(void) {
    return num_logical_drives;
}

// check if logical drive is present
int ata_logical_drive_present(uint8 logical_drive) {
    if (logical_drive >= num_logical_drives) {
        return 0;
    }
    uint8 physical = logical_to_physical_map[logical_drive];
    return detected_drives[physical].present;
} 