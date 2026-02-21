#include <mm/slab.h>
#include <mm/vmm.h>
#include <string.h>
#include <vga.h>

#define SLAB_MAGIC 0x534C4142u /* 'SLAB' */

/*
 * Size classes: powers of two only.
 * 16..2048 bytes by default keeps per-page object counts reasonable and avoids
 * ballooning internal fragmentation for larger allocations.
 */
#define SLAB_MIN_SHIFT 4u  /* 2^4 = 16 */
#define SLAB_MAX_SHIFT 11u /* 2^11 = 2048 */
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1u)

#if CONFIG_SLAB_POISON
#define SLAB_FREE_POISON_BYTE 0xA5u
#endif

typedef struct slab_header slab_header_t;

struct slab_header {
    uint32 magic;
    uint16 class_index;
    uint16 obj_size;
    uint16 total_objs;
    uint16 free_objs;
    void* free_list;
    slab_header_t* next; /* per-class list of slabs that have free objects */
};

typedef struct slab_class {
    slab_header_t* partial;
#if CONFIG_SLAB_TRACKING
    uint32 alloc_count;
    uint32 free_count;
    uint32 slowpath_newslab;
#endif
} slab_class_t;

static slab_class_t g_slab_classes[SLAB_CLASS_COUNT];
static int g_slab_inited = 0;

static inline uint32 slab_align_up_u32(uint32 v, uint32 align) {
    return (v + align - 1u) & ~(align - 1u);
}

/* Returns 0..31 for non-zero v. Undefined for v==0 (caller guards). */
static inline uint32 slab_bsr32(uint32 v) {
#if defined(__GNUC__)
    uint32 idx;
    __asm__ __volatile__("bsr %1, %0" : "=r"(idx) : "r"(v));
    return idx;
#else
    uint32 idx = 0;
    while (v >>= 1u) idx++;
    return idx;
#endif
}

/* ceil(log2(size)) for size in 1..0x80000000. */
static inline uint32 slab_ceil_log2_u32(uint32 size) {
    if (size <= 1u) return 0u;
    uint32 v = size - 1u;
    return slab_bsr32(v) + 1u;
}

static inline int slab_is_pow2_u32(uint32 v) {
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

static inline uint32 slab_class_index_for_size(uint32 size) {
    uint32 shift = slab_ceil_log2_u32(size);
    if (shift < SLAB_MIN_SHIFT) shift = SLAB_MIN_SHIFT;
    if (shift > SLAB_MAX_SHIFT) return 0xFFFFFFFFu;
    return shift - SLAB_MIN_SHIFT;
}

static slab_header_t* slab_new_page(uint32 class_index, uint32 obj_size) {
    void* page = vmm_kmalloc_page();
    if (!page) return NULL;

    slab_header_t* h = (slab_header_t*)page;
    uint32 header_bytes = slab_align_up_u32((uint32)sizeof(*h), 4u);
    uint32 usable = PAGE_SIZE - header_bytes;

    /* Slow path: compute object count (division is OK here). */
    uint32 total = usable / obj_size;
    if (total == 0u) {
        vmm_kfree_page(page);
        return NULL;
    }

    h->magic = SLAB_MAGIC;
    h->class_index = (uint16)class_index;
    h->obj_size = (uint16)obj_size;
    h->total_objs = (uint16)total;
    h->free_objs = (uint16)total;
    h->next = NULL;

    /* Build freelist (LIFO) inside objects. */
    uint8* base = (uint8*)page + header_bytes;
    void* free_list = NULL;
    for (uint32 i = 0; i < total; ++i) {
        void* obj = (void*)(base + i * obj_size);
#if CONFIG_SLAB_POISON
        memset(obj, (int)SLAB_FREE_POISON_BYTE, obj_size);
#endif
        *(void**)obj = free_list;
        free_list = obj;
    }
    h->free_list = free_list;

    return h;
}

void slab_init(void) {
    if (g_slab_inited) return;
    memset(g_slab_classes, 0, sizeof(g_slab_classes));
    g_slab_inited = 1;
}

int slab_owns(const void* ptr) {
    if (!ptr) return 0;
    uint32 va = (uint32)ptr;
    if (va < KERNEL_BASE) return 0;
    uint32 page_base = va & PAGE_MASK;
    const slab_header_t* h = (const slab_header_t*)page_base;

    if (h->magic != SLAB_MAGIC) return 0;
    if (h->class_index >= SLAB_CLASS_COUNT) return 0;
    if (!slab_is_pow2_u32((uint32)h->obj_size)) return 0;
    if (h->obj_size < (1u << SLAB_MIN_SHIFT) || h->obj_size > (1u << SLAB_MAX_SHIFT)) return 0;
    return 1;
}

void* slab_alloc(size_t size) {
    if (!g_slab_inited) slab_init();

    // Slabs are page-backed; require the frame allocator to be initialized.
    // If VMM isn't ready yet, the caller should fall back to the legacy heap.
    if (vmm_get_total_frames() == 0) {
        return NULL;
    }

    uint32 sz = (uint32)size;
    if (sz == 0u) return NULL;

    uint32 class_index = slab_class_index_for_size(sz);
    if (class_index == 0xFFFFFFFFu) return NULL;

    slab_class_t* c = &g_slab_classes[class_index];
    uint32 obj_size = 1u << (SLAB_MIN_SHIFT + class_index);

    slab_header_t* h = c->partial;
    if (!h || h->free_list == NULL) {
        /* Slow path: allocate a new slab page. */
        h = slab_new_page(class_index, obj_size);
        if (!h) return NULL;
        h->next = c->partial;
        c->partial = h;
#if CONFIG_SLAB_TRACKING
        c->slowpath_newslab++;
#endif
    }

    /* Fast path: pop one object from the slab's freelist. */
    void* obj = h->free_list;
    h->free_list = *(void**)obj;
    if (h->free_objs) h->free_objs--;

    /* If slab is now full, remove it from the partial list head (O(1)). */
    if (h->free_list == NULL) {
        c->partial = h->next;
        h->next = NULL;
    }

#if CONFIG_SLAB_TRACKING
    c->alloc_count++;
#endif

    return obj;
}

int slab_free(void* ptr) {
    if (!ptr) return 1;
    if (!slab_owns(ptr)) return 0;

    uint32 va = (uint32)ptr;
    slab_header_t* h = (slab_header_t*)(va & PAGE_MASK);
    uint32 class_index = (uint32)h->class_index;
    if (class_index >= SLAB_CLASS_COUNT) return 0;

    slab_class_t* c = &g_slab_classes[class_index];
    uint32 obj_size = (uint32)h->obj_size;

#if CONFIG_SLAB_POISON
    memset(ptr, (int)SLAB_FREE_POISON_BYTE, obj_size);
#endif

    *(void**)ptr = h->free_list;
    h->free_list = ptr;
    if (h->free_objs < h->total_objs) h->free_objs++;

    /* If slab was previously full, re-add to partial list head (O(1)). */
    if (h->free_objs == 1u) {
        h->next = c->partial;
        c->partial = h;
    }

#if CONFIG_SLAB_TRACKING
    c->free_count++;
#endif

    return 1;
}
