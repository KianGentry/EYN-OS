#include <capabilities.h>
#include <string.h>

/*
 * SECURITY-INVARIANT: Upper bound for live registered capability objects.
 *
 * Why: The registry is scanned linearly; this bound caps worst-case validation
 * latency and bounds kernel memory for capability metadata.
 * Invariant: All registration/lookup logic assumes entries live in
 * g_cap_registry[0..CAP_REGISTRY_MAX).
 * Breakage if changed:
 *   - Increasing raises fixed kernel memory usage and validation scan cost.
 *   - Decreasing can cause registration failures for workloads that previously
 *     succeeded.
 * ABI-sensitive: No (kernel-internal), but behavior-sensitive under load.
 * Security-critical: Yes (resource exhaustion/latency control).
 */
#define CAP_REGISTRY_MAX 64

typedef struct cap_entry_t {
    void* obj;
    uint32 type;
    uint32 epoch;
    uint32 flags;
} cap_entry_t;

typedef struct cap_secret_t {
    uint32 s0;
    uint32 s1;
} cap_secret_t;

static cap_entry_t g_cap_registry[CAP_REGISTRY_MAX];
static cap_secret_t g_cap_secret;

static uint32 cap_fold_ptr(uintptr value) {
    uint32 lo = (uint32)value;
#if EYNOS_POINTER_BITS == 64
    uint32 hi = (uint32)(value >> 32);
    return lo ^ hi;
#else
    return lo;
#endif
}

static int cap_ptr_to_obj32(void* obj, uint32* out_obj32) {
    if (!obj || !out_obj32) {
        return -1;
    }

    uintptr raw = (uintptr)obj;
    uint32 encoded = (uint32)raw;
    if ((uintptr)encoded != raw) {
        return -1;
    }

    *out_obj32 = encoded;
    return 0;
}

static inline void cap_cli(void) {
    __asm__ __volatile__("cli");
}

static inline void cap_sti(void) {
    __asm__ __volatile__("sti");
}

static uint64 cap_mix64(uint64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64 cap_make_tag(uint32 obj32, uint32 type, uint32 rights, uint32 epoch) {
    uint64 x = (uint64)obj32;
    x |= ((uint64)type << 32);
    x ^= ((uint64)rights << 16);
    x ^= ((uint64)epoch << 48);
    x ^= ((uint64)g_cap_secret.s0) | ((uint64)g_cap_secret.s1 << 32);
    return cap_mix64(x);
}

static cap_entry_t* cap_find_entry(void* obj, uint32 type) {
    for (int i = 0; i < CAP_REGISTRY_MAX; ++i) {
        if (g_cap_registry[i].obj == obj && g_cap_registry[i].type == type) {
            return &g_cap_registry[i];
        }
    }
    return 0;
}

void cap_init(void) {
    uint32 tsc_lo = 0;
    uint32 tsc_hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));

    g_cap_secret.s0 = tsc_lo ^ cap_fold_ptr((uintptr)&g_cap_registry);
    g_cap_secret.s1 = tsc_hi ^ cap_fold_ptr((uintptr)&g_cap_secret) ^ 0x9e3779b9u;

    memset(g_cap_registry, 0, sizeof(g_cap_registry));
}

int cap_register_object(void* obj, uint32 type) {
    uint32 obj32 = 0;

    if (!obj || type == CAP_OBJ_NONE) {
        return -1;
    }

    if (cap_ptr_to_obj32(obj, &obj32) != 0) {
        return -1;
    }

    cap_cli();

    cap_entry_t* existing = cap_find_entry(obj, type);
    if (existing) {
        cap_sti();
        return 0;
    }

    for (int i = 0; i < CAP_REGISTRY_MAX; ++i) {
        if (!g_cap_registry[i].obj) {
            g_cap_registry[i].obj = obj;
            g_cap_registry[i].type = type;
            g_cap_registry[i].epoch = 1;
            g_cap_registry[i].flags = 0;
            cap_sti();
            return 0;
        }
    }

    cap_sti();
    return -1;
}

int cap_revoke_object(void* obj, uint32 type) {
    if (!obj || type == CAP_OBJ_NONE) {
        return -1;
    }

    cap_cli();
    cap_entry_t* entry = cap_find_entry(obj, type);
    if (entry) {
        entry->epoch++;
        cap_sti();
        return 0;
    }
    cap_sti();
    return -1;
}

int cap_mint(cap_t* out, void* obj, uint32 type, uint32 rights) {
    uint32 obj32 = 0;

    if (!out || !obj || type == CAP_OBJ_NONE) {
        return -1;
    }

    if (cap_ptr_to_obj32(obj, &obj32) != 0) {
        return -1;
    }

    if (cap_register_object(obj, type) != 0) {
        return -1;
    }

    cap_entry_t* entry = cap_find_entry(obj, type);
    if (!entry) {
        return -1;
    }

    uint64 tag = cap_make_tag(obj32, type, rights, entry->epoch);
    out->obj = obj32;
    out->type = type;
    out->rights = rights;
    out->epoch = entry->epoch;
    out->tag_lo = (uint32)(tag & 0xFFFFFFFFu);
    out->tag_hi = (uint32)((tag >> 32) & 0xFFFFFFFFu);
    return 0;
}

int cap_validate(const cap_t* cap, void* obj, uint32 type, uint32 required_rights) {
    uint32 obj32 = 0;

    if (!cap || !obj || type == CAP_OBJ_NONE) {
        return 0;
    }
    if (cap_ptr_to_obj32(obj, &obj32) != 0) {
        return 0;
    }
    if (cap->obj != obj32) {
        return 0;
    }
    if (cap->type != type) {
        return 0;
    }
    if ((cap->rights & required_rights) != required_rights) {
        return 0;
    }

    cap_entry_t* entry = cap_find_entry(obj, type);
    if (!entry) {
        return 0;
    }
    if (entry->epoch != cap->epoch) {
        return 0;
    }

    uint64 expected = cap_make_tag(obj32, type, cap->rights, cap->epoch);
    uint32 exp_lo = (uint32)(expected & 0xFFFFFFFFu);
    uint32 exp_hi = (uint32)((expected >> 32) & 0xFFFFFFFFu);
    if (cap->tag_lo != exp_lo || cap->tag_hi != exp_hi) {
        return 0;
    }

    return 1;
}
