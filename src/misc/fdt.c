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

static int mem_has_exact_str(const uint8* data, uint32 len, const char* needle) {
    /* DTB string lists (like "compatible") are NUL-separated.
     * Accept either an exact match of an entry or needle as the only string.
     */
    if (!data || !needle) return 0;

    uint32 i = 0;
    while (i < len) {
        const char* s = (const char*)(const void*)(data + i);

        uint32 slen = 0;
        while ((i + slen) < len && s[slen] != '\0') {
            slen++;
        }

        /* Compare exact entry. */
        uint32 j = 0;
        while (needle[j] != '\0' && j < slen) {
            if (needle[j] != s[j]) {
                break;
            }
            j++;
        }
        if (needle[j] == '\0' && j == slen) {
            return 1;
        }

        /* Advance to next string (skip NUL). */
        i += slen;
        while (i < len && data[i] == 0) {
            i++;
        }
    }

    return 0;
}

static uint32 gic_irq_id_from_spec(uint32 int_type, uint32 int_num) {
    /* GIC binding:
     *  - SPI: type=0, IDs start at 32
     *  - PPI: type=1, IDs start at 16
     */
    if (int_type == 0) {
        return 32u + int_num;
    }
    if (int_type == 1) {
        return 16u + int_num;
    }
    return 0;
}

static int parse_timer_irq_from_interrupts_extended(const uint8* data, uint32 len, uint32* out_irq_id) {
    /* Minimal interrupts-extended parser for GIC (#interrupt-cells=3).
     * Each entry: <phandle type number flags> (4 cells).
     */
    if (!data || !out_irq_id) return -1;
    if (len < 16) return -1;

    const uint32* cells = (const uint32*)(const void*)data;
    uint32 cell_count = len / 4u;

    for (uint32 i = 0; (i + 3u) < cell_count; i += 4u) {
        uint32 int_type = be32_to_cpu(cells[i + 1u]);
        uint32 int_num  = be32_to_cpu(cells[i + 2u]);
        uint32 irq_id = gic_irq_id_from_spec(int_type, int_num);
        if (irq_id != 0) {
            *out_irq_id = irq_id;
            return 0;
        }
    }

    return -1;
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

int fdt_parse_gicv2(uint64 dtb_ptr, uint64* out_gicd_base, uint64* out_gicc_base) {
    if (!dtb_ptr || !out_gicd_base || !out_gicc_base) {
        return -1;
    }

    *out_gicd_base = 0;
    *out_gicc_base = 0;

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

    /* Per spec default. These are bus values inherited by children. */
    uint32 addr_cells_stack[32];
    uint32 size_cells_stack[32];
    uint8 gic_compat_stack[32];
    uint8 intc_stack[32];

    for (int i = 0; i < 32; i++) {
        addr_cells_stack[i] = 2;
        size_cells_stack[i] = 1;
        gic_compat_stack[i] = 0;
        intc_stack[i] = 0;
    }

    int depth = 0;

    while (p < struct_end) {
        uint32 token = be32_to_cpu(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(const void*)p;
            uint32 len = 0;
            while (name[len] != '\0') {
                len++;
            }

            int parent_depth = depth;
            depth++;
            if (depth >= 32) {
                return -1;
            }

            /* Inherit bus cells from parent. */
            addr_cells_stack[depth] = addr_cells_stack[parent_depth];
            size_cells_stack[depth] = size_cells_stack[parent_depth];
            gic_compat_stack[depth] = 0;
            intc_stack[depth] = 0;

            /* Advance p past name. */
            p = (const uint32*)((const uint8*)p + align4_u32(len + 1));
            continue;
        }

        if (token == FDT_END_NODE) {
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32 len = be32_to_cpu(*p++);
            uint32 nameoff = be32_to_cpu(*p++);
            const char* pname = strings + nameoff;

            const uint8* value = (const uint8*)(const void*)p;
            p = (const uint32*)(const void*)((const uint8*)p + align4_u32(len));

            /* These properties define the bus format for *children* of this node. */
            if (streq(pname, "#address-cells") && len >= 4) {
                addr_cells_stack[depth] = be32_to_cpu(*(const uint32*)(const void*)value);
                continue;
            }

            if (streq(pname, "#size-cells") && len >= 4) {
                size_cells_stack[depth] = be32_to_cpu(*(const uint32*)(const void*)value);
                continue;
            }

            if (streq(pname, "interrupt-controller")) {
                /* Boolean property (often len=0). */
                intc_stack[depth] = 1;
                continue;
            }

            if (streq(pname, "compatible")) {
                if (mem_has_exact_str(value, len, "arm,cortex-a15-gic") ||
                    mem_has_exact_str(value, len, "arm,gic-400") ||
                    mem_has_exact_str(value, len, "arm,gic-410") ||
                    mem_has_exact_str(value, len, "arm,gic-420")) {
                    gic_compat_stack[depth] = 1;
                }
                continue;
            }

            if (streq(pname, "reg")) {
                if (!gic_compat_stack[depth] || !intc_stack[depth]) {
                    continue;
                }

                /* reg encoding uses the parent bus cell sizes. */
                uint32 parent_depth = (depth > 0) ? (uint32)(depth - 1) : 0;
                uint32 address_cells = addr_cells_stack[parent_depth];
                uint32 size_cells = size_cells_stack[parent_depth];
                uint32 tuple_cells = address_cells + size_cells;
                if (tuple_cells == 0) {
                    return -1;
                }

                /* Need at least two reg entries: (GICD, GICC). */
                if (len < (2u * tuple_cells * 4u)) {
                    continue;
                }

                const uint32* cells = (const uint32*)(const void*)value;
                uint64 gicd = read_cells_as_u64(cells, address_cells);
                uint64 gicc = read_cells_as_u64(cells + tuple_cells, address_cells);

                if (gicd != 0 && gicc != 0) {
                    *out_gicd_base = gicd;
                    *out_gicc_base = gicc;
                    return 0;
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

        return -1;
    }

    return -1;
}

int fdt_parse_armv8_timer_virtual_irq(uint64 dtb_ptr, uint32* out_irq_id) {
    if (!dtb_ptr || !out_irq_id) {
        return -1;
    }

    *out_irq_id = 0;

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

    uint8 timer_match_stack[32];
    for (int i = 0; i < 32; i++) {
        timer_match_stack[i] = 0;
    }

    int depth = 0;

    while (p < struct_end) {
        uint32 token = be32_to_cpu(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(const void*)p;
            uint32 len = 0;
            while (name[len] != '\0') {
                len++;
            }

            depth++;
            if (depth >= 32) {
                return -1;
            }
            timer_match_stack[depth] = 0;

            p = (const uint32*)((const uint8*)p + align4_u32(len + 1));
            continue;
        }

        if (token == FDT_END_NODE) {
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32 len = be32_to_cpu(*p++);
            uint32 nameoff = be32_to_cpu(*p++);
            const char* pname = strings + nameoff;

            const uint8* value = (const uint8*)(const void*)p;
            p = (const uint32*)(const void*)((const uint8*)p + align4_u32(len));

            if (streq(pname, "compatible")) {
                if (mem_has_exact_str(value, len, "arm,armv8-timer")) {
                    timer_match_stack[depth] = 1;
                }
                continue;
            }

            if (streq(pname, "interrupts")) {
                if (!timer_match_stack[depth]) {
                    continue;
                }

                /* For arm,armv8-timer: 4 interrupt specifiers, each 3 cells.
                 * Order per binding: secure, non-secure, virtual, hypervisor.
                 * We want the virtual timer (CNTV): the 3rd triplet (index 2).
                 */
                if (len < (9u * 4u)) {
                    return -1;
                }

                const uint32* cells = (const uint32*)(const void*)value;
                uint32 int_type = be32_to_cpu(cells[6]);
                uint32 int_num  = be32_to_cpu(cells[7]);
                uint32 irq_id = gic_irq_id_from_spec(int_type, int_num);
                if (irq_id == 0) {
                    return -1;
                }

                *out_irq_id = irq_id;
                return 0;
            }

            if (streq(pname, "interrupts-extended")) {
                if (!timer_match_stack[depth]) {
                    continue;
                }

                if (parse_timer_irq_from_interrupts_extended(value, len, out_irq_id) == 0) {
                    return 0;
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

        return -1;
    }

    return -1;
}

int fdt_parse_cpus_mpidr(uint64 dtb_ptr, uint64* out_mpidrs, uint32 max_cpus, uint32* out_count) {
    if (!dtb_ptr || !out_mpidrs || !out_count || max_cpus == 0) {
        return -1;
    }

    *out_count = 0;

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

    uint32 addr_cells_stack[32];
    uint32 size_cells_stack[32];
    for (int i = 0; i < 32; i++) {
        addr_cells_stack[i] = 2;
        size_cells_stack[i] = 1;
    }

    int depth = 0;
    int cpus_depth = -1;
    int cpu_node_depth = -1;
    int cpu_anywhere_depth = -1;
    int cpu_device_ok = 1;
    int cpu_psci_ok = 1;

    while (p < struct_end) {
        uint32 token = be32_to_cpu(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(const void*)p;
            uint32 len = 0;
            while (name[len] != '\0') {
                len++;
            }

            int parent_depth = depth;
            depth++;
            if (depth >= 32) {
                return -1;
            }

            /* Inherit bus cells. */
            addr_cells_stack[depth] = addr_cells_stack[parent_depth];
            size_cells_stack[depth] = size_cells_stack[parent_depth];

            /* Identify /cpus node (accept "cpus" or "cpus@..."). */
            if (parent_depth == 0 && len >= 4 &&
                name[0] == 'c' && name[1] == 'p' && name[2] == 'u' && name[3] == 's') {
                cpus_depth = depth;
            }

            /* CPU nodes are children of /cpus. */
            if (cpus_depth != -1 && parent_depth == cpus_depth) {
                cpu_node_depth = depth;
                cpu_device_ok = 1;
                cpu_psci_ok = 1;
            }

            /* Fallback: accept nodes named "cpu@..." anywhere. */
            if (len >= 3 && name[0] == 'c' && name[1] == 'p' && name[2] == 'u') {
                cpu_anywhere_depth = depth;
                cpu_device_ok = 1;
                cpu_psci_ok = 1;
            }

            p = (const uint32*)((const uint8*)p + align4_u32(len + 1));
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == cpu_node_depth) {
                cpu_node_depth = -1;
                cpu_device_ok = 1;
                cpu_psci_ok = 1;
            }
            if (depth == cpu_anywhere_depth) {
                cpu_anywhere_depth = -1;
            }
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32 len = be32_to_cpu(*p++);
            uint32 nameoff = be32_to_cpu(*p++);
            const char* pname = strings + nameoff;

            const uint8* value = (const uint8*)(const void*)p;
            p = (const uint32*)(const void*)((const uint8*)p + align4_u32(len));

            /* Bus cell sizes (apply to children). */
            if (streq(pname, "#address-cells") && len >= 4) {
                addr_cells_stack[depth] = be32_to_cpu(*(const uint32*)(const void*)value);
                continue;
            }

            if (streq(pname, "#size-cells") && len >= 4) {
                size_cells_stack[depth] = be32_to_cpu(*(const uint32*)(const void*)value);
                continue;
            }

            /* CPU node properties. */
            if ((cpu_node_depth != -1 && depth == cpu_node_depth) || (cpu_anywhere_depth != -1 && depth == cpu_anywhere_depth)) {
                if (streq(pname, "device_type")) {
                    const char* v = (const char*)(const void*)value;
                    if (len >= 3 && v[0] == 'c' && v[1] == 'p' && v[2] == 'u') {
                        cpu_anywhere_depth = depth;
                    } else {
                        cpu_device_ok = 0;
                    }
                    continue;
                }

                if (streq(pname, "enable-method")) {
                    if (!mem_has_exact_str(value, len, "psci")) {
                        cpu_psci_ok = 0;
                    }
                    continue;
                }

                if (streq(pname, "reg")) {
                    if (!cpu_device_ok || !cpu_psci_ok) {
                        continue;
                    }

                    uint32 parent_depth = (depth > 0) ? (uint32)(depth - 1) : 0;
                    uint32 address_cells = addr_cells_stack[parent_depth];
                    if (address_cells == 0) {
                        address_cells = 1;
                    }

                    if (len < (address_cells * 4u)) {
                        if (len >= 8) {
                            address_cells = 2;
                        } else if (len >= 4) {
                            address_cells = 1;
                        } else {
                            continue;
                        }
                    }

                    if (*out_count >= max_cpus) {
                        continue;
                    }

                    const uint32* cells = (const uint32*)(const void*)value;
                    uint64 mpidr = read_cells_as_u64(cells, address_cells);
                    out_mpidrs[*out_count] = mpidr;
                    (*out_count)++;
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

        return -1;
    }

    return (*out_count > 0) ? 0 : -1;
}

int fdt_parse_psci_method(uint64 dtb_ptr, uint32* out_use_hvc) {
    if (!dtb_ptr || !out_use_hvc) {
        return -1;
    }

    *out_use_hvc = 0;

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

    int depth = 0;
    int psci_node_depth = -1;
    int psci_node_match = 0;

    while (p < struct_end) {
        uint32 token = be32_to_cpu(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char* name = (const char*)(const void*)p;
            uint32 len = 0;
            while (name[len] != '\0') {
                len++;
            }

            int parent_depth = depth;
            depth++;

            if (parent_depth == 0 && len >= 4 &&
                name[0] == 'p' && name[1] == 's' && name[2] == 'c' && name[3] == 'i') {
                psci_node_depth = depth;
                psci_node_match = 1;
            }

            p = (const uint32*)((const uint8*)p + align4_u32(len + 1));
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == psci_node_depth) {
                psci_node_depth = -1;
                psci_node_match = 0;
            }
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32 len = be32_to_cpu(*p++);
            uint32 nameoff = be32_to_cpu(*p++);
            const char* pname = strings + nameoff;

            const uint8* value = (const uint8*)(const void*)p;
            p = (const uint32*)(const void*)((const uint8*)p + align4_u32(len));

            if (streq(pname, "compatible")) {
                if (mem_has_exact_str(value, len, "arm,psci-0.2") ||
                    mem_has_exact_str(value, len, "arm,psci-1.0")) {
                    psci_node_match = 1;
                    psci_node_depth = depth;
                }
            }

            if (streq(pname, "method")) {
                if (psci_node_match && depth != psci_node_depth) {
                    /* If method appears outside the matched node, ignore it. */
                    continue;
                }
                    const char* m = (const char*)(const void*)value;
                    if (len >= 3 && m[0] == 'h' && m[1] == 'v' && m[2] == 'c') {
                        *out_use_hvc = 1;
                        return 0;
                    }
                    if (len >= 3 && m[0] == 's' && m[1] == 'm' && m[2] == 'c') {
                        *out_use_hvc = 0;
                        return 0;
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

        return -1;
    }

    return -1;
}
