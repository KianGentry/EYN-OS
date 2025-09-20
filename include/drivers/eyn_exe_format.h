#ifndef EYN_EXE_FORMAT_H
#define EYN_EXE_FORMAT_H

#include <stdint.h>

#define EYN_MAGIC "EYN\0"
#define EYN_EXE_VERSION 1

// Flags for EYN executable
#define EYN_FLAG_DYNAMIC   0x01 // Uses dynamic linking
#define EYN_FLAG_RELOC     0x02 // Contains relocations

struct eyn_exe_header {
    char     magic[4];         // "EYN\0"
    uint8_t  version;          // Format version
    uint8_t  flags;            // Flags (bitfield)
    uint16_t reserved;         // Reserved for alignment/future
    uint32_t entry_point;      // Offset to entry point (from code base)
    uint32_t code_size;        // Code section size
    uint32_t data_size;        // Data section size
    uint32_t bss_size;         // BSS (uninitialized data) size
    uint32_t dyn_table_off;    // Offset to dynamic linking table
    uint32_t dyn_table_size;   // Size of dynamic linking table
};

#endif // EYN_EXE_FORMAT_H 