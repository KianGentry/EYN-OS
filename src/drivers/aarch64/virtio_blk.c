#include <drivers/aarch64/virtio_blk.h>

#include <misc/fdt.h>
#include <string.h>

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

#define VIRTIO_DEVICE_ID_BLOCK 2u

/* Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED      0x80u

/* Virtqueue */
#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

/* Feature bits */
#define VIRTIO_F_VERSION_1 (1ull << 32)
#define VIRTIO_F_ACCESS_PLATFORM (1ull << 33)

/* virtio-blk request types */
#define VIRTIO_BLK_T_IN  0u
#define VIRTIO_BLK_T_OUT 1u

/* virtio-blk status */
#define VIRTIO_BLK_S_OK 0u

#define VIRTIO_BLK_QUEUE_SIZE 8u

static inline void mmio_w32(uint64 base, uint64 off, uint32 v) {
    *(volatile uint32*)(uint64)(base + off) = v;
}

static inline uint32 mmio_r32(uint64 base, uint64 off) {
    return *(volatile uint32*)(uint64)(base + off);
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
    uint16 ring[VIRTIO_BLK_QUEUE_SIZE];
    uint16 used_event; /* kept for layout compatibility */
} __attribute__((packed, aligned(16))) virtq_avail_t;

typedef struct {
    uint32 id;
    uint32 len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16 flags;
    uint16 idx;
    virtq_used_elem_t ring[VIRTIO_BLK_QUEUE_SIZE];
    uint16 avail_event; /* kept for layout compatibility */
} __attribute__((packed, aligned(16))) virtq_used_t;

typedef struct {
    uint32 type;
    uint32 reserved;
    uint64 sector;
} __attribute__((packed, aligned(16))) virtio_blk_req_hdr_t;

static uint64 g_blk_base;
static uint32 g_blk_ver;
static uint32 g_ready;

static virtq_desc_t* g_desc_p;
static virtq_avail_t* g_avail_p;
static virtq_used_t* g_used_p;

static uint16 g_avail_idx;
static uint16 g_last_used_idx;

static virtq_desc_t g_desc[VIRTIO_BLK_QUEUE_SIZE] __attribute__((aligned(4096)));
static virtq_avail_t g_avail __attribute__((aligned(4096)));
static virtq_used_t g_used __attribute__((aligned(4096)));

static uint8 g_legacy_ring_q0[8192] __attribute__((aligned(4096)));

static virtio_blk_req_hdr_t g_req_hdr __attribute__((aligned(64)));
static uint8 g_req_status __attribute__((aligned(64)));
static uint8 g_bounce[512] __attribute__((aligned(64)));

static inline uint64 align_up_u64(uint64 v, uint64 a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void legacy_ring_layout(uint8* ring,
                              virtq_desc_t** out_desc,
                              virtq_avail_t** out_avail,
                              virtq_used_t** out_used) {
    uint64 desc_sz = (uint64)sizeof(virtq_desc_t) * (uint64)VIRTIO_BLK_QUEUE_SIZE;
    uint64 avail_sz = (uint64)sizeof(virtq_avail_t);
    uint64 used_off = align_up_u64(desc_sz + avail_sz, 4096u);

    *out_desc = (virtq_desc_t*)(void*)ring;
    *out_avail = (virtq_avail_t*)(void*)(ring + desc_sz);
    *out_used = (virtq_used_t*)(void*)(ring + used_off);
}

static int virtio_blk_init_at_base(uint64 base) {
    if (mmio_r32(base, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) return -1;
    g_blk_ver = mmio_r32(base, VIRTIO_MMIO_VERSION);
    if (mmio_r32(base, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_ID_BLOCK) return -1;

    /* Reset */
    mmio_w32(base, VIRTIO_MMIO_STATUS, 0);

    uint32 status = 0;
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    status |= VIRTIO_STATUS_DRIVER;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    /* Feature negotiation */
    uint64 dev_features = 0;
    mmio_w32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    dev_features |= (uint64)mmio_r32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_w32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    dev_features |= ((uint64)mmio_r32(base, VIRTIO_MMIO_DEVICE_FEATURES) << 32);

    uint64 wanted = 0;
    if (g_blk_ver >= 2u) {
        /* Modern virtio requires VERSION_1. */
        if ((dev_features & VIRTIO_F_VERSION_1) == 0) {
            status |= VIRTIO_STATUS_FAILED;
            mmio_w32(base, VIRTIO_MMIO_STATUS, status);
            return -2;
        }
        wanted |= VIRTIO_F_VERSION_1;

        /* We don't support ACCESS_PLATFORM (IOMMU path) in bring-up. */
        if (dev_features & VIRTIO_F_ACCESS_PLATFORM) {
            /* Leave it unnegotiated; device should accept. */
        }
    }

    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32)(wanted & 0xFFFFFFFFu));
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_w32(base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32)(wanted >> 32));

    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    status = mmio_r32(base, VIRTIO_MMIO_STATUS);
    if ((status & VIRTIO_STATUS_FEATURES_OK) == 0) {
        status |= VIRTIO_STATUS_FAILED;
        mmio_w32(base, VIRTIO_MMIO_STATUS, status);
        return -3;
    }

    /* Queue 0 */
    mmio_w32(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32 qmax = mmio_r32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < VIRTIO_BLK_QUEUE_SIZE) return -4;
    mmio_w32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_BLK_QUEUE_SIZE);

    g_avail_idx = 0;
    g_last_used_idx = 0;

    if (g_blk_ver >= 2u) {
        g_desc_p = g_desc;
        g_avail_p = &g_avail;
        g_used_p = &g_used;

        uint64 desc_pa = (uint64)(const void*)g_desc_p;
        uint64 avail_pa = (uint64)(const void*)g_avail_p;
        uint64 used_pa = (uint64)(const void*)g_used_p;

        mmio_w32(base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32)(desc_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32)(desc_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32)(avail_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32)(avail_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32)(used_pa & 0xFFFFFFFFu));
        mmio_w32(base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32)(used_pa >> 32));

        mmio_w32(base, VIRTIO_MMIO_QUEUE_READY, 1);

        memset(g_desc_p, 0, sizeof(g_desc));
        memset(g_avail_p, 0, sizeof(g_avail));
        memset(g_used_p, 0, sizeof(g_used));
    } else {
        /* Legacy (version 1) PFN-based queue setup. */
        mmio_w32(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096u);
        mmio_w32(base, VIRTIO_MMIO_QUEUE_ALIGN, 4096u);

        virtq_desc_t* desc;
        virtq_avail_t* avail;
        virtq_used_t* used;
        legacy_ring_layout(g_legacy_ring_q0, &desc, &avail, &used);
        memset(g_legacy_ring_q0, 0, sizeof(g_legacy_ring_q0));

        g_desc_p = desc;
        g_avail_p = avail;
        g_used_p = used;

        uint64 ring_pa = (uint64)(const void*)g_legacy_ring_q0;
        mmio_w32(base, VIRTIO_MMIO_QUEUE_PFN, (uint32)(ring_pa >> 12));
    }

    /* Driver OK */
    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_w32(base, VIRTIO_MMIO_STATUS, status);

    /* Clear pending interrupts */
    mmio_w32(base, VIRTIO_MMIO_INTERRUPT_ACK, 0x3);

    return 0;
}

static int virtio_blk_submit(uint32 type, uint32 lba, void* buf512) {
    if (!g_ready || !g_desc_p || !g_avail_p || !g_used_p) return -1;
    if (!buf512) return -1;

    g_req_hdr.type = type;
    g_req_hdr.reserved = 0;
    g_req_hdr.sector = (uint64)lba;
    g_req_status = 0xFFu;

    int is_write = (type == VIRTIO_BLK_T_OUT);
    if (is_write) {
        memcpy(g_bounce, buf512, 512);
    }

    /* Build 3-descriptor chain: header, data, status */
    g_desc_p[0].addr = (uint64)(const void*)&g_req_hdr;
    g_desc_p[0].len = (uint32)sizeof(g_req_hdr);
    g_desc_p[0].flags = VIRTQ_DESC_F_NEXT;
    g_desc_p[0].next = 1;

    g_desc_p[1].addr = (uint64)(const void*)(is_write ? g_bounce : buf512);
    g_desc_p[1].len = 512;
    g_desc_p[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0u : VIRTQ_DESC_F_WRITE);
    g_desc_p[1].next = 2;

    g_desc_p[2].addr = (uint64)(const void*)&g_req_status;
    g_desc_p[2].len = 1;
    g_desc_p[2].flags = VIRTQ_DESC_F_WRITE;
    g_desc_p[2].next = 0;

    /* For reads, ensure destination buffer won't be later written back over DMA data. */
    if (!is_write) {
        aarch64_dcache_clean_invalidate_poc_range(buf512, 512);
    } else {
        aarch64_dcache_clean_poc_range(g_bounce, 512);
    }

    aarch64_dcache_clean_poc_range(&g_req_hdr, sizeof(g_req_hdr));
    aarch64_dcache_clean_poc_range(&g_req_status, sizeof(g_req_status));
    aarch64_dcache_clean_poc_range(g_desc_p, sizeof(virtq_desc_t) * VIRTIO_BLK_QUEUE_SIZE);

    uint16 slot = (uint16)(g_avail_idx & (VIRTIO_BLK_QUEUE_SIZE - 1u));
    g_avail_p->ring[slot] = 0;
    g_avail_idx++;
    g_avail_p->idx = g_avail_idx;
    aarch64_dcache_clean_poc_range(g_avail_p, sizeof(*g_avail_p));

    mmio_w32(g_blk_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion. */
    for (;;) {
        aarch64_dcache_clean_invalidate_poc_range(g_used_p, sizeof(*g_used_p));
        uint16 used_idx = g_used_p->idx;
        if (used_idx != g_last_used_idx) {
            g_last_used_idx = used_idx;
            break;
        }
    }

    aarch64_dcache_clean_invalidate_poc_range(&g_req_status, sizeof(g_req_status));
    if (!is_write) {
        aarch64_dcache_clean_invalidate_poc_range(buf512, 512);
    }

    /* Ack interrupts */
    uint32 isr = mmio_r32(g_blk_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr) {
        mmio_w32(g_blk_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    }

    return (g_req_status == VIRTIO_BLK_S_OK) ? 0 : -2;
}

int virtio_blk_init(uint64 dtb_ptr) {
    g_ready = 0;
    g_blk_base = 0;
    g_blk_ver = 0;
    g_desc_p = NULL;
    g_avail_p = NULL;
    g_used_p = NULL;

    uint64 bases[64];
    uint32 count = 0;
    for (int i = 0; i < 64; i++) bases[i] = 0;

    if (fdt_parse_virtio_mmio(dtb_ptr, bases, 64, &count) != 0 || count == 0) {
        return -1;
    }

    for (uint32 i = 0; i < count; i++) {
        uint32 did = mmio_r32(bases[i], VIRTIO_MMIO_DEVICE_ID);
        if (did == 0) continue;
        if (did != VIRTIO_DEVICE_ID_BLOCK) continue;

        if (virtio_blk_init_at_base(bases[i]) == 0) {
            g_blk_base = bases[i];
            g_ready = 1;
            return 0;
        }
    }

    return -1;
}

int virtio_blk_read_sector(uint32 lba, void* buf512) {
    return virtio_blk_submit(VIRTIO_BLK_T_IN, lba, buf512);
}

int virtio_blk_write_sector(uint32 lba, const void* buf512) {
    return virtio_blk_submit(VIRTIO_BLK_T_OUT, lba, (void*)buf512);
}
