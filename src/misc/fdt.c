#include <misc/fdt.h>

/* FDT tokens (big-endian u32) */
#define FDT_BEGIN_NODE  0x1u
#define FDT_END_NODE    0x2u
#define FDT_PROP        0x3u
#define FDT_NOP         0x4u
#define FDT_END         0x9u

typedef struct __attribute__((packed)) {
    uint32 magic;
    uint32 totalsize;
    uint32 off_dt_struct;
    uint32 off_dt_strings;
    uint32 off_mem_rsvmap;
    uint32 version;
    uint32 last_comp_version;
    uint32 boot_cpuid_phys;
    uint32 size_dt_strings;
    uint32 size_dt_struct;
} fdt_header_t;

static inline uint32 be32_to_cpu(uint32 v) {
    return __builtin_bswap32(v);
}

static int streq(const char* a, const char* b) {
    if (a == 0 || b == 0) return 0;
    while (*a && *b) {
        if (*a++ != *b++) return 0;
    }
    return (*a == 0 && *b == 0);
}

static uint32 align4_u32(uint32 x) {
    return (x + 3u) & ~3u;
}

static uint64 read_cells_as_u64(const uint32* be_cells, uint32 cell_count) {
    /* cell_count is 1 or 2 for our usage. */
    if (cell_count == 1) {
        return (uint64)be32_to_cpu(be_cells[0]);
    }

    if (cell_count == 2) {
        uint64 hi = (uint64)be32_to_cpu(be_cells[0]);
        uint64 lo = (uint64)be32_to_cpu(be_cells[1]);
        return (hi << 32) | lo;
    }

    /* Unsupported in this minimal parser. */
    return 0;
}

int fdt_parse_memory(uint64 dtb_ptr, uint64* out_base, uint64* out_size) {
    if (!dtb_ptr || !out_base || !out_size) {
        return -1;
    }

    *out_base = 0;
    *out_size = 0;

    const fdt_header_t* hdr = (const fdt_header_t*)(uint64)dtb_ptr;
    if (be32_to_cpu(hdr->magic) != FDT_MAGIC) {
        return -1;
    }

    uint32 off_struct  = be32_to_cpu(hdr->off_dt_struct);
    uint32 off_strings = be32_to_cpu(hdr->off_dt_strings);
    uint32 size_struct = be32_to_cpu(hdr->size_dt_struct);

    const uint8* base = (const uint8*)(uint64)dtb_ptr;
    const uint32* p = (const uint32*)(const void*)(base + off_struct);
    const uint32* struct_end = (const uint32*)(const void*)(base + off_struct + size_struct);
    const char* strings = (const char*)(const void*)(base + off_strings);

    /* Default per spec if properties are missing. */
    uint32 address_cells = 2;
    uint32 size_cells = 1;

    /* Track whether we're currently inside the memory node. */
    int in_memory_node = 0;

    /* Track depth to know when we leave the memory node. */
    int depth = 0;
    int memory_node_depth = -1;

    while (p < struct_end) {
        uint32 token = be32_to_cpu(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(const void*)p;

            /* Node name is NUL-terminated and padded to 4 bytes. */
            uint32 len = 0;
            while (name[len] != '\0') {
                len++;
            }

            /* Determine if this node is the memory node.
             * Most DTBs name it "memory@...". */
            if (depth == 0) {
                /* root node */
            }

            if (len >= 6) {
                /* Check prefix "memory" */
                int is_mem = 1;
                const char* mem = "memory";
                for (uint32 i = 0; i < 6; i++) {
                    if (name[i] != mem[i]) {
                        is_mem = 0;
                        break;
                    }
                }

                if (is_mem) {
                    in_memory_node = 1;
                    memory_node_depth = depth;
                }
            }

            depth++;

            /* Advance p past the name string (including NUL, padded). */
            p = (const uint32*)((const uint8*)p + align4_u32(len + 1));
            continue;
        }

        if (token == FDT_END_NODE) {
            depth--;
            if (in_memory_node && depth <= memory_node_depth) {
                in_memory_node = 0;
                memory_node_depth = -1;
            }
            continue;
        }

        if (token == FDT_PROP) {
            uint32 len = be32_to_cpu(*p++);
            uint32 nameoff = be32_to_cpu(*p++);
            const char* pname = strings + nameoff;

            const uint8* value = (const uint8*)(const void*)p;
            p = (const uint32*)(const void*)((const uint8*)p + align4_u32(len));

            /* Root-level address/size cells affect how we parse /memory reg. */
            if (depth == 1) {
                if (streq(pname, "#address-cells") && len >= 4) {
                    address_cells = be32_to_cpu(*(const uint32*)(const void*)value);
                } else if (streq(pname, "#size-cells") && len >= 4) {
                    size_cells = be32_to_cpu(*(const uint32*)(const void*)value);
                }
            }

            if (in_memory_node) {
                /* Prefer a device_type="memory" check when present. */
                if (streq(pname, "device_type")) {
                    const char* v = (const char*)(const void*)value;
                    if (len >= 6 && v[0] == 'm' && v[1] == 'e' && v[2] == 'm' && v[3] == 'o' && v[4] == 'r' && v[5] == 'y') {
                        /* ok */
                    } else {
                        /* Not actually memory; ignore this node. */
                        in_memory_node = 0;
                        memory_node_depth = -1;
                    }
                }

                if (streq(pname, "reg")) {
                    uint32 tuple_cells = address_cells + size_cells;
                    if (tuple_cells == 0) {
                        return -1;
                    }

                    /* reg is an array of tuples; parse only the first. */
                    if (len < tuple_cells * 4) {
                        return -1;
                    }

                    const uint32* cells = (const uint32*)(const void*)value;
                    uint64 base_addr = read_cells_as_u64(cells, address_cells);
                    uint64 size = read_cells_as_u64(cells + address_cells, size_cells);

                    if (base_addr != 0 && size != 0) {
                        *out_base = base_addr;
                        *out_size = size;
                        return 0;
                    }
                }
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        /* Unknown token */
        return -1;
    }

    return -1;
}
