#include <misc/types.h>
#include <misc/fdt.h>
#include <drivers/aarch64/virtio_input.h>

/* Virtio MMIO register offsets (virtio-mmio v2). */
#define VIRTIO_MMIO_MAGIC_VALUE       0x000
#define VIRTIO_MMIO_VERSION           0x004
#define VIRTIO_MMIO_DEVICE_ID         0x008
#define VIRTIO_MMIO_VENDOR_ID         0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE   0x028 /* legacy (version 1) */
#define VIRTIO_MMIO_QUEUE_SEL         0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034
#define VIRTIO_MMIO_QUEUE_NUM         0x038
#define VIRTIO_MMIO_QUEUE_ALIGN       0x03c /* legacy (version 1) */
#define VIRTIO_MMIO_QUEUE_PFN         0x040 /* legacy (version 1) */
#define VIRTIO_MMIO_QUEUE_READY       0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS  0x060
#define VIRTIO_MMIO_INTERRUPT_ACK     0x064
#define VIRTIO_MMIO_STATUS            0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW    0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH   0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW   0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH  0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW    0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH   0x0a4

#define VIRTIO_MMIO_MAGIC 0x74726976u /* 'virt' little-endian */

/* Virtio device IDs */
#define VIRTIO_DEVICE_ID_INPUT 18u

/* Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED      0x80u

/* Virtqueue */
#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

/* Linux input event types we care about */
#define EV_SYN 0u
#define EV_KEY 1u

/* Selected key codes (Linux input-event-codes.h) */
#define KEY_BACKSPACE 14u
#define KEY_ENTER     28u
#define KEY_LEFTSHIFT 42u
#define KEY_RIGHTSHIFT 54u
#define KEY_SPACE     57u

static inline void mmio_w32(uint64 base, uint64 off, uint32 v) {
    *(volatile uint32*)(uint64)(base + off) = v;
}

static inline uint32 mmio_r32(uint64 base, uint64 off) {
    return *(volatile uint32*)(uint64)(base + off);
}

static inline uint64 make_u64(uint32 lo, uint32 hi) {
    return ((uint64)hi << 32) | (uint64)lo;
}

static inline uint32 aarch64_dcache_line_size_bytes(void) {
    uint64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint32 dminline = (uint32)((ctr >> 16) & 0xFu);
    return 4u << dminline;
}

static void aarch64_dcache_clean_poc_range(const void* addr, uint64 len) {
    if (!addr || len == 0) return;

    uint32 line = aarch64_dcache_line_size_bytes();
    uint64 start = (uint64)(const void*)addr;
    uint64 end = start + len;
    uint64 mask = (uint64)line - 1u;
    start &= ~mask;
    end = (end + mask) & ~mask;

    for (uint64 p = start; p < end; p += (uint64)line) {
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

static void aarch64_dcache_clean_invalidate_poc_range(const void* addr, uint64 len) {
    if (!addr || len == 0) return;

    uint32 line = aarch64_dcache_line_size_bytes();
    uint64 start = (uint64)(const void*)addr;
    uint64 end = start + len;
    uint64 mask = (uint64)line - 1u;
    start &= ~mask;
    end = (end + mask) & ~mask;

    for (uint64 p = start; p < end; p += (uint64)line) {
        asm volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

typedef struct {
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next;
} __attribute__((packed, aligned(16))) virtq_desc_t;

typedef struct {
    uint16 flags;
    uint16 idx;
    uint16 ring[16];
    uint16 used_event; /* only used with EVENT_IDX, kept for layout compatibility */
} __attribute__((packed, aligned(16))) virtq_avail_t;

typedef struct {
    uint32 id;
    uint32 len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16 flags;
    uint16 idx;
    virtq_used_elem_t ring[16];
    uint16 avail_event; /* only used with EVENT_IDX, kept for layout compatibility */
} __attribute__((packed, aligned(16))) virtq_used_t;

typedef struct {
    uint16 type;
    uint16 code;
    int32 value;
} __attribute__((packed, aligned(8))) virtio_input_event_t;

static uint64 g_input_base;
static uint32 g_ready;
static uint16 g_last_used_idx;
static uint16 g_avail_idx;
static uint32 g_shift;

static virtq_desc_t* g_desc_p;
static virtq_avail_t* g_avail_p;
static virtq_used_t* g_used_p;

/* Optional bring-up diagnostics (wired to the existing UART). */
void uart_pl011_write(const char* s);
void uart_pl011_write_hex64(uint64 v);

static void dbg_write(const char* s) {
    if (s) uart_pl011_write(s);
}

static void dbg_hex(uint64 v) {
    uart_pl011_write_hex64(v);
}

static virtq_desc_t g_desc[16] __attribute__((aligned(4096)));
static virtq_avail_t g_avail __attribute__((aligned(4096)));
static virtq_used_t g_used __attribute__((aligned(4096)));
static virtio_input_event_t g_events[16] __attribute__((aligned(4096)));

/* Legacy (virtio-mmio version 1) requires the vring to be described via a PFN.
 * The vring layout aligns the used ring to QUEUE_ALIGN (commonly 4096), which
 * effectively means we need >=2 pages even for small queue sizes.
 */
static uint8 g_legacy_ring_q0[8192] __attribute__((aligned(4096)));

static inline uint64 align_up_u64(uint64 v, uint64 a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void legacy_ring_layout(uint8* ring,
                              virtq_desc_t** out_desc,
                              virtq_avail_t** out_avail,
                              virtq_used_t** out_used) {
    uint64 desc_sz = (uint64)sizeof(virtq_desc_t) * 16u;
    uint64 avail_sz = (uint64)sizeof(virtq_avail_t);
    uint64 used_off = align_up_u64(desc_sz + avail_sz, 4096u);

    *out_desc = (virtq_desc_t*)(void*)ring;
    *out_avail = (virtq_avail_t*)(void*)(ring + desc_sz);
    *out_used = (virtq_used_t*)(void*)(ring + used_off);
}

static char map_keycode(uint16 code, uint32 shift) {
    /*
     * Linux evdev keycodes are positional (QWERTY), not alphabetical ranges.
     * Map a minimal US layout explicitly.
     */
    switch (code) {
        /* Letters (QWERTY) */
        case 16: return shift ? 'Q' : 'q';
        case 17: return shift ? 'W' : 'w';
        case 18: return shift ? 'E' : 'e';
        case 19: return shift ? 'R' : 'r';
        case 20: return shift ? 'T' : 't';
        case 21: return shift ? 'Y' : 'y';
        case 22: return shift ? 'U' : 'u';
        case 23: return shift ? 'I' : 'i';
        case 24: return shift ? 'O' : 'o';
        case 25: return shift ? 'P' : 'p';

        case 30: return shift ? 'A' : 'a';
        case 31: return shift ? 'S' : 's';
        case 32: return shift ? 'D' : 'd';
        case 33: return shift ? 'F' : 'f';
        case 34: return shift ? 'G' : 'g';
        case 35: return shift ? 'H' : 'h';
        case 36: return shift ? 'J' : 'j';
        case 37: return shift ? 'K' : 'k';
        case 38: return shift ? 'L' : 'l';

        case 44: return shift ? 'Z' : 'z';
        case 45: return shift ? 'X' : 'x';
        case 46: return shift ? 'C' : 'c';
        case 47: return shift ? 'V' : 'v';
        case 48: return shift ? 'B' : 'b';
        case 49: return shift ? 'N' : 'n';
        case 50: return shift ? 'M' : 'm';

        /* Numbers row */
        case 2:  return shift ? '!' : '1';
        case 3:  return shift ? '@' : '2';
        case 4:  return shift ? '#' : '3';
        case 5:  return shift ? '$' : '4';
        case 6:  return shift ? '%' : '5';
        case 7:  return shift ? '^' : '6';
        case 8:  return shift ? '&' : '7';
        case 9:  return shift ? '*' : '8';
        case 10: return shift ? '(' : '9';
        case 11: return shift ? ')' : '0';

        /* Whitespace/control */
        case KEY_SPACE: return ' ';
        case 15: return '\t';
        case KEY_ENTER: return '\n';
        case KEY_BACKSPACE: return '\b';

        /* Punctuation */
        case 12: return shift ? '_' : '-';
        case 13: return shift ? '+' : '=';
        case 26: return shift ? '{' : '[';
        case 27: return shift ? '}' : ']';
        case 39: return shift ? ':' : ';';
        case 40: return shift ? '"' : '\'';
        case 41: return shift ? '~' : '`';
        case 43: return shift ? '|' : '\\';
        case 51: return shift ? '<' : ',';
        case 52: return shift ? '>' : '.';
        case 53: return shift ? '?' : '/';
        default: return 0;
    }
}

static int virtio_input_try_consume_event(char* out_c) {
    if (!g_ready) return -1;

    if (!g_used_p || !g_avail_p) return -1;

    /* Invalidate used ring so we see device updates. */
    aarch64_dcache_clean_invalidate_poc_range(g_used_p, sizeof(*g_used_p));

    uint16 used_idx = g_used_p->idx;
    while (g_last_used_idx != used_idx) {
        uint16 i = (uint16)(g_last_used_idx & 0xF);
        uint32 desc_id = g_used_p->ring[i].id;

        if (desc_id < 16) {
            aarch64_dcache_clean_invalidate_poc_range(&g_events[desc_id], sizeof(g_events[desc_id]));
            virtio_input_event_t ev = g_events[desc_id];

            /* Requeue descriptor */
            g_avail_p->ring[g_avail_idx & 0xF] = (uint16)desc_id;
            g_avail_idx++;
            g_avail_p->idx = g_avail_idx;
            aarch64_dcache_clean_poc_range(g_avail_p, sizeof(*g_avail_p));
            mmio_w32(g_input_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

            if (ev.type == EV_KEY) {
                if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                    g_shift = (ev.value != 0) ? 1u : 0u;
                } else if (ev.value == 1) {
                    char c = map_keycode(ev.code, g_shift);
                    if (c != 0 && out_c) {
                        *out_c = c;
                        g_last_used_idx++;
                        return 0;
                    }
                }
            }
        }

        g_last_used_idx++;
    }

    return -1;
}

int virtio_input_ready(void) {
    return (int)g_ready;
}

uint64 virtio_input_base(void) {
    return g_input_base;
}

int virtio_input_getc_nonblock(char* out_c) {
    return virtio_input_try_consume_event(out_c);
}

static int virtio_input_init_at_base(uint64 base) {
    if (mmio_r32(base, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) return -1;
    uint32 ver = mmio_r32(base, VIRTIO_MMIO_VERSION);
    if (mmio_r32(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_INPUT) return -1;

    /* Reset */
    mmio_w32(base, VIRTIO_MMIO_STATUS, 0);

    uint32 status = 0;
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_DRIVER;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    /* Negotiate features: accept none for now. */
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);

    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    /* Re-read to ensure device accepted. */
    status = mmio_r32(base, VIRTIO_MMIO_STATUS);
    if ((status & VIRTIO_STATUS_FEATURES_OK) == 0) {
        status |= VIRTIO_STATUS_FAILED;
        mmio_w32(base, VIRTIO_MMIO_STATUS, status);
        return -1;
    }

    /* Queue 0: event queue */
    mmio_w32(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32 qmax = mmio_r32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < 16u) return -1;

    /* We use fixed-size 16 structures. */
    mmio_w32(base, VIRTIO_MMIO_QUEUE_NUM, 16);

    /* Initialize shared state */
    g_last_used_idx = 0;
    g_avail_idx = 0;
    g_shift = 0;

    if (ver >= 2u) {
        g_desc_p = g_desc;
        g_avail_p = &g_avail;
        g_used_p = &g_used;

        uint64 desc_pa = (uint64)(const void*)g_desc;
        uint64 avail_pa = (uint64)(const void*)&g_avail;
        uint64 used_pa = (uint64)(const void*)&g_used;

        mmio_w32(base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32)(desc_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32)(desc_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32)(avail_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32)(avail_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32)(used_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32)(used_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_READY, 1);

        g_avail_p->flags = 0;
        g_avail_p->idx = 0;
        g_avail_p->used_event = 0;
        g_used_p->flags = 0;
        g_used_p->idx = 0;
        g_used_p->avail_event = 0;

        for (uint16 i = 0; i < 16; i++) {
            g_desc[i].addr = (uint64)(const void*)&g_events[i];
            g_desc[i].len = (uint32)sizeof(virtio_input_event_t);
            g_desc[i].flags = VIRTQ_DESC_F_WRITE;
            g_desc[i].next = 0;
            g_avail_p->ring[i] = i;
        }

        g_avail_idx = 16;
        g_avail_p->idx = 16;

        aarch64_dcache_clean_poc_range(g_desc_p, sizeof(g_desc));
        aarch64_dcache_clean_poc_range(g_avail_p, sizeof(*g_avail_p));
        aarch64_dcache_clean_poc_range(g_used_p, sizeof(*g_used_p));
        aarch64_dcache_clean_poc_range(g_events, sizeof(g_events));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    } else {
        /* Legacy (version 1) PFN-based queue setup. */
        mmio_w32(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096u);
        mmio_w32(base, VIRTIO_MMIO_QUEUE_ALIGN, 4096u);

        virtq_desc_t* desc;
        virtq_avail_t* avail;
        virtq_used_t* used;
        legacy_ring_layout(g_legacy_ring_q0, &desc, &avail, &used);

        g_desc_p = desc;
        g_avail_p = avail;
        g_used_p = used;

        /* Point our globals at the legacy layout so the poller works unchanged. */
        for (uint16 i = 0; i < 16; i++) {
            desc[i].addr = (uint64)(const void*)&g_events[i];
            desc[i].len = (uint32)sizeof(virtio_input_event_t);
            desc[i].flags = VIRTQ_DESC_F_WRITE;
            desc[i].next = 0;
            avail->ring[i] = i;
        }
        avail->flags = 0;
        avail->idx = 16;
        avail->used_event = 0;
        used->flags = 0;
        used->idx = 0;
        used->avail_event = 0;

        g_avail_idx = 16;

        aarch64_dcache_clean_poc_range(g_legacy_ring_q0, sizeof(g_legacy_ring_q0));
        aarch64_dcache_clean_poc_range(g_events, sizeof(g_events));

        uint64 ring_pa = (uint64)(const void*)g_legacy_ring_q0;
        mmio_w32(base, VIRTIO_MMIO_QUEUE_PFN, (uint32)(ring_pa >> 12));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    return 0;
}

int virtio_input_init(uint64 dtb_ptr) {
    g_ready = 0;
    g_input_base = 0;

    uint64 bases[64];
    uint32 count = 0;
    for (int i = 0; i < 64; i++) bases[i] = 0;

    if (fdt_parse_virtio_mmio(dtb_ptr, bases, 64, &count) != 0 || count == 0) {
        dbg_write("[VIRTIO_INPUT] no virtio-mmio nodes\n");
        return -1;
    }

    dbg_write("[VIRTIO_INPUT] virtio-mmio bases ");
    dbg_hex((uint64)count);
    dbg_write("\n");

    for (uint32 i = 0; i < count; i++) {
        uint32 did = mmio_r32(bases[i], VIRTIO_MMIO_DEVICE_ID);
        if (did == 0) {
            continue;
        }

        uint32 ver = mmio_r32(bases[i], VIRTIO_MMIO_VERSION);
        dbg_write("[VIRTIO_INPUT] slot base ");
        dbg_hex(bases[i]);
        dbg_write(" ver ");
        dbg_hex((uint64)ver);
        dbg_write(" did ");
        dbg_hex((uint64)did);
        dbg_write("\n");

        if ((uint32)did == VIRTIO_DEVICE_ID_INPUT && virtio_input_init_at_base(bases[i]) == 0) {
            g_input_base = bases[i];
            g_ready = 1;

            dbg_write("[VIRTIO_INPUT] ready @ ");
            dbg_hex(g_input_base);
            dbg_write("\n");
            return 0;
        }
    }

    dbg_write("[VIRTIO_INPUT] no virtio-input device found\n");

    return -1;
}
