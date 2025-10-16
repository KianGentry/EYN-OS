#ifndef FLAT_EXE_FORMAT_H
#define FLAT_EXE_FORMAT_H

#include <stdint.h>

#define FLAT_MAGIC "FLAT"

struct flat_exe_header {
    char     magic[4];       // "FLAT"
    uint32_t entry_point;    // Offset to entry point (from code base)
    uint32_t code_size;      // Code section size
    uint32_t data_size;      // Data section size
};

#endif // FLAT_EXE_FORMAT_H
