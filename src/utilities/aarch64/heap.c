#include <misc/types.h>
#include <stddef.h>
#include <stdint.h>
#include <utilities/string.h>

#include <utilities/aarch64/heap.h>

/*
 * AArch64 bring-up heap allocator
 *
 * Constraints:
 * - Freestanding (no libc).
 * - Alignment-safe for AArch64 (16-byte alignment for returned pointers).
 * - Small and predictable; meant to unblock porting of existing subsystems.
 */

extern uint8 __kernel_end;

#define HEAP_MAGIC 0xE1A0A64FULL
#define HEAP_ALIGN 16u
#define MIN_SPLIT  (HEAP_ALIGN * 2u)

typedef struct heap_block {
    uint64 magic;
    uint64 size;              /* total bytes including header */
    struct heap_block* next;
    uint32 used;
    uint32 _pad;
} heap_block_t;

static heap_block_t* g_heap_first;
static uint8* g_heap_start;
static uint8* g_heap_end;
static int g_heap_ready;

static inline uintptr_t align_up(uintptr_t v, uintptr_t a) {
    if (a == 0) return v;
    return (v + a - 1u) & ~(a - 1u);
}

static inline size_t align_up_sz(size_t v, size_t a) {
    if (a == 0) return v;
    return (v + a - 1u) & ~(a - 1u);
}

static void heap_init_default(void) {
    if (g_heap_ready) return;

    uintptr_t start = (uintptr_t)&__kernel_end;
    start = align_up(start, HEAP_ALIGN);

    /*
     * If we weren't given DT RAM bounds yet, pick a conservative window.
     * QEMU virt typically has plenty of RAM; keep this small to match the
     * project's low-memory expectations.
     */
    uintptr_t end = start + (2u * 1024u * 1024u); /* 2 MiB */

    g_heap_start = (uint8*)start;
    g_heap_end = (uint8*)end;

    g_heap_first = (heap_block_t*)g_heap_start;
    g_heap_first->magic = HEAP_MAGIC;
    g_heap_first->size = (uint64)(g_heap_end - g_heap_start);
    g_heap_first->next = NULL;
    g_heap_first->used = 0;

    g_heap_ready = 1;
}

void aarch64_heap_init(uint64 ram_base, uint64 ram_size) {
    uintptr_t start = (uintptr_t)&__kernel_end;
    start = align_up(start, HEAP_ALIGN);

    uintptr_t ram_end = 0;
    if (ram_base != 0 && ram_size != 0) {
        ram_end = (uintptr_t)(ram_base + ram_size);
    }

    /* Default to a small heap; grow up to 8 MiB if RAM bounds allow. */
    size_t desired = 2u * 1024u * 1024u;
    size_t max_desired = 8u * 1024u * 1024u;

    uintptr_t end = start + desired;
    if (ram_end != 0) {
        uintptr_t max_end = start + max_desired;
        if (max_end > ram_end) max_end = ram_end;
        if (end > max_end) end = max_end;
    }

    /* Ensure we have enough space for at least one block header + payload. */
    if (end <= start + sizeof(heap_block_t) + HEAP_ALIGN) {
        /* Fall back to default, even if tiny; malloc will then fail cleanly. */
        heap_init_default();
        return;
    }

    g_heap_start = (uint8*)start;
    g_heap_end = (uint8*)end;

    g_heap_first = (heap_block_t*)g_heap_start;
    g_heap_first->magic = HEAP_MAGIC;
    g_heap_first->size = (uint64)(g_heap_end - g_heap_start);
    g_heap_first->next = NULL;
    g_heap_first->used = 0;

    g_heap_ready = 1;
}

static void heap_coalesce(void) {
    for (heap_block_t* b = g_heap_first; b && b->next; b = b->next) {
        if (!b->used && !b->next->used) {
            heap_block_t* n = b->next;
            if (n->magic != HEAP_MAGIC) break;
            b->size += n->size;
            b->next = n->next;
            /* restart coalescing at current block */
            continue;
        }
    }
}

void* malloc(size_t size) {
    if (size == 0) return NULL;
    if (!g_heap_ready) heap_init_default();

    size_t payload = align_up_sz(size, HEAP_ALIGN);
    size_t need = payload + sizeof(heap_block_t);

    for (heap_block_t* b = g_heap_first; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) return NULL;
        if (b->used) continue;
        if ((size_t)b->size < need) continue;

        uint64 remaining = b->size - (uint64)need;
        if (remaining >= (uint64)(sizeof(heap_block_t) + MIN_SPLIT)) {
            heap_block_t* nb = (heap_block_t*)((uint8*)b + need);
            nb->magic = HEAP_MAGIC;
            nb->size = remaining;
            nb->next = b->next;
            nb->used = 0;

            b->size = (uint64)need;
            b->next = nb;
        }

        b->used = 1;
        return (void*)((uint8*)b + sizeof(heap_block_t));
    }

    return NULL;
}

void free(void* ptr) {
    if (!ptr) return;
    if (!g_heap_ready) return;

    heap_block_t* b = (heap_block_t*)((uint8*)ptr - sizeof(heap_block_t));
    if (b->magic != HEAP_MAGIC) return;

    b->used = 0;
    heap_coalesce();
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    /* Best-effort overflow check */
    size_t total = nmemb * size;
    if (size != 0 && total / size != nmemb) return NULL;

    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    heap_block_t* b = (heap_block_t*)((uint8*)ptr - sizeof(heap_block_t));
    if (b->magic != HEAP_MAGIC) return NULL;

    size_t old_payload = (size_t)b->size;
    if (old_payload >= sizeof(heap_block_t)) old_payload -= sizeof(heap_block_t);
    else old_payload = 0;

    size_t new_payload = align_up_sz(size, HEAP_ALIGN);
    if (new_payload <= old_payload) {
        /* Keep the same block; we could split, but not necessary for bring-up. */
        return ptr;
    }

    void* np = malloc(size);
    if (!np) return NULL;

    memcpy(np, ptr, old_payload);
    free(ptr);
    return np;
}
