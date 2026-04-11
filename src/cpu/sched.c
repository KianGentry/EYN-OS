#include <misc/sched.h>
#include <system.h>
#include <irq.h>
#include <watchdog.h>
#include <stddef.h>
#include <cpu/user_elf.h>

#define SCHED_WORK_MAX 64
/*
 * 386-OPT: 32 priority levels packed into a single ready bitmap.
 * Highest priority selection becomes O(1) via BSR instead of a descending loop.
 */
#define SCHED_WORK_PRIOS 32

#ifndef CONFIG_SCHED_USE_BSR_ASM
#define CONFIG_SCHED_USE_BSR_ASM 1
#endif

typedef struct sched_queue {
    int head;
    int tail;
} sched_queue_t;

static sched_work_t g_work[SCHED_WORK_MAX] CACHE_ALIGNED_32;
static sched_queue_t g_runq[SCHED_WORK_PRIOS] CACHE_ALIGNED_32;
static int g_sched_work_inflight = 0;
static uint32 g_ready_bitmap = 0;

static volatile uint32 g_ticks = 0;
static uint32 g_tick_hz = 100;
static uint32 g_sched_rng_state = 0x53A9D37Du;
/*
 * ABI-INVARIANT: Scheduler timeslice length in ticks.
 *
 * Why: Controls how often sched_on_timeslice_end() fires, which runs
 *      deferred work items (UI events, network polling, etc.).
 * Invariant: timeslice_ticks × tick_period_ms = timeslice_real_ms.
 *   At 1000 Hz (1 ms/tick): 100 ticks = 100 ms timeslice.
 *   At  100 Hz (10 ms/tick):  10 ticks = 100 ms timeslice.
 *   At   50 Hz (20 ms/tick):   5 ticks = 100 ms timeslice.
 * Breakage if changed: reducing the timeslice starves deferred work;
 *   increasing makes the system less responsive to background tasks.
 * ABI-sensitive: No.  Scheduler-internal only.
 */
static uint32 g_timeslice_ticks = 100; // 100 ms at 1000 Hz
static uint32 g_current_slice = 0;
static volatile uint32 g_idle_hlt_count = 0; // counts ticks elapsed while idling (not raw HLTs)

#define DET_QUEUE_CAP 256

typedef struct det_event_t {
    uint8 type;
    uint8 irq;
    uint16 _pad;
} det_event_t;

enum {
    DET_EVENT_IRQ = 1,
};

static det_event_t g_det_queue[DET_QUEUE_CAP];
static volatile uint16 g_det_head = 0;
static volatile uint16 g_det_tail = 0;
static volatile int g_det_enabled = 0;
static volatile int g_det_processing = 0;

/*
 * ABI-INVARIANT: Default scheduler PRNG seed.
 *
 * Why: Provides a deterministic baseline sequence across boots and test runs.
 * Invariant: The scheduler-owned PRNG stream is independent from userland RNG.
 * ABI-sensitive: Yes when deterministic mode is enabled.
 */
#define SCHED_RNG_DEFAULT_SEED 0x53A9D37Du

static void sched_irq0_handler(void) {
    sched_tick();
}

static inline void sched_cli(void) {
    __asm__ __volatile__("cli");
}

static inline void sched_sti(void) {
    __asm__ __volatile__("sti");
}

void sched_init(void) {
    g_ticks = 0;
    sched_rng_seed(SCHED_RNG_DEFAULT_SEED);
    // register tick handler on IRQ0
    register_interrupt_handler(0, sched_irq0_handler);
    sched_work_init();
}

void sched_yield(void) {
    // cooperative yield: no-op for now
    g_current_slice = 0; // force timeslice end handling on next tick
    (void)user_task_poll_scheduler();
}

void sched_sleep_us(uint32 microseconds) {
    if (g_tick_hz == 0) {
        extern void eyn_kernel_delay(uint32 microseconds);
        eyn_kernel_delay(microseconds);
        return;
    }
    // convert requested microseconds to ticks using integer ceil
    uint32 tick_us = 1000000U / g_tick_hz;
    if (tick_us == 0) tick_us = 1;
    uint32 needed_ticks = (microseconds + tick_us - 1) / tick_us;
    uint32 start_ticks = g_ticks;
    uint32 target_ticks = start_ticks + needed_ticks;
    while ((uint32)g_ticks < target_ticks) {
        (void)user_task_poll_scheduler();
        // halt until next interrupt to save cpu; track for idle time estimation
        __asm__ __volatile__("hlt");
    }
    // Accumulate idle time in ticks that elapsed during this sleep
    uint32 end_ticks = g_ticks;
    if (end_ticks > start_ticks) {
        g_idle_hlt_count += (end_ticks - start_ticks);
    }
}

void sched_tick(void) {
    if (g_det_enabled) {
        (void)sched_det_queue_irq(0);
        return;
    }
    g_ticks++;
    // Update watchdog each tick
    watchdog_on_tick();
    if (++g_current_slice >= g_timeslice_ticks) {
        g_current_slice = 0;
        extern void sched_on_timeslice_end(void);
        sched_on_timeslice_end();
    }
}

void sched_set_tick_hz(uint32 hz) {
    g_tick_hz = (hz == 0) ? 100 : hz;
}

void sched_set_timeslice_ticks(uint32 ticks) {
    if (ticks == 0) ticks = 1;
    g_timeslice_ticks = ticks;
    if (g_current_slice >= g_timeslice_ticks) g_current_slice = 0;
}

// default weak hook: does nothing; will be implemented with real context switches later
__attribute__((weak)) void sched_on_timeslice_end(void) {}

static void sched_tick_det(void) {
    g_ticks++;
    watchdog_on_tick();
    if (++g_current_slice >= g_timeslice_ticks) {
        g_current_slice = 0;
        extern void sched_on_timeslice_end(void);
        sched_on_timeslice_end();
    }
}

static inline uint32 sched_prio_index(uint32 priority) {
    /* Avoid modulo/division in the scheduler path. */
    if (priority >= (uint32)SCHED_WORK_PRIOS) return (uint32)(SCHED_WORK_PRIOS - 1);
    return priority;
}

static inline int sched_bsr32(uint32 v) {
    if (v == 0) return -1;
#if CONFIG_SCHED_USE_BSR_ASM
    int idx;
    __asm__ __volatile__("bsr %1, %0" : "=r"(idx) : "r"(v));
    return idx;
#else
    for (int i = 31; i >= 0; --i) {
        if (v & (1u << (uint32)i)) return i;
    }
    return -1;
#endif
}

static void sched_work_enqueue(int idx) {
    if (idx < 0 || idx >= SCHED_WORK_MAX) return;
    sched_work_t* w = &g_work[idx];
    if (w->in_queue) return;
    uint32 prio = sched_prio_index(w->priority);
    sched_queue_t* q = &g_runq[prio];
    int was_empty = (q->head < 0);
    w->next = -1;
    if (q->tail >= 0) {
        g_work[q->tail].next = idx;
        q->tail = idx;
    } else {
        q->head = idx;
        q->tail = idx;
    }
    w->in_queue = 1;

    if (was_empty) {
        g_ready_bitmap |= (1u << prio);
    }
}

static int sched_work_dequeue_prio(uint32 prio) {
    if (prio >= SCHED_WORK_PRIOS) return -1;
    sched_queue_t* q = &g_runq[prio];
    int idx = q->head;
    if (idx < 0) return -1;
    sched_work_t* w = &g_work[idx];
    q->head = w->next;
    if (q->head < 0) {
        q->tail = -1;
        g_ready_bitmap &= ~(1u << prio);
    }
    w->next = -1;
    w->in_queue = 0;
    return idx;
}

static sched_work_t* sched_work_get(uint32 id) {
    if (id == 0) return NULL;
    uint32 idx = id - 1;
    if (idx >= SCHED_WORK_MAX) return NULL;
    if (g_work[idx].id != id) return NULL;
    return &g_work[idx];
}

void sched_work_init(void) {
    for (int i = 0; i < SCHED_WORK_MAX; ++i) {
        g_work[i].id = 0;
        g_work[i].run = NULL;
        g_work[i].userdata = NULL;
        g_work[i].priority = 0;
        g_work[i].affinity_mask = SCHED_WORK_CPU_ANY;
        g_work[i].budget_ticks = 0;
        g_work[i].budget_left = 0;
        g_work[i].cache_hint = 0;
        g_work[i].next = -1;
        g_work[i].in_queue = 0;
    }
    for (int p = 0; p < SCHED_WORK_PRIOS; ++p) {
        g_runq[p].head = -1;
        g_runq[p].tail = -1;
    }
    g_ready_bitmap = 0;
    g_sched_work_inflight = 0;
}

int sched_work_register(void (*run)(sched_work_t*), void* userdata,
                        uint32 priority, uint32 affinity_mask,
                        uint32 budget_ticks, uint32 cache_hint) {
    if (!run) return -1;
    for (int i = 0; i < SCHED_WORK_MAX; ++i) {
        if (g_work[i].id == 0) {
            uint32 id = (uint32)(i + 1);
            g_work[i].id = id;
            g_work[i].run = run;
            g_work[i].userdata = userdata;
            g_work[i].priority = priority;
            g_work[i].affinity_mask = affinity_mask ? affinity_mask : SCHED_WORK_CPU_ANY;
            g_work[i].budget_ticks = budget_ticks;
            g_work[i].budget_left = budget_ticks;
            g_work[i].cache_hint = cache_hint;
            g_work[i].next = -1;
            g_work[i].in_queue = 0;
            if (budget_ticks) sched_work_enqueue(i);
            return (int)id;
        }
    }
    return -1;
}

int sched_work_set_ready(uint32 id) {
    sched_work_t* w = sched_work_get(id);
    if (!w) return -1;
    if (w->budget_left == 0 && w->budget_ticks != 0) {
        w->budget_left = w->budget_ticks;
    }
    int idx = (int)(id - 1);
    sched_work_enqueue(idx);
    return 0;
}

int sched_work_unready(uint32 id) {
    sched_work_t* w = sched_work_get(id);
    if (!w) return -1;
    w->budget_left = 0;
    return 0;
}

int sched_work_update_budget(uint32 id, uint32 budget_ticks) {
    sched_work_t* w = sched_work_get(id);
    if (!w) return -1;
    w->budget_ticks = budget_ticks;
    if (w->budget_left > budget_ticks) w->budget_left = budget_ticks;
    return 0;
}

int sched_work_on_timeslice_end(void) {
    if (g_sched_work_inflight) return 0;
    g_sched_work_inflight = 1;

    int ran = 0;
    uint32 ready = g_ready_bitmap;
    while (ready) {
        int prio = sched_bsr32(ready);
        if (prio < 0) break;
        int idx = sched_work_dequeue_prio((uint32)prio);
        if (idx < 0) {
            /* Defensive: bitmap/queue got out of sync; clear and continue. */
            ready &= ~(1u << (uint32)prio);
            g_ready_bitmap &= ~(1u << (uint32)prio);
            continue;
        }
        sched_work_t* w = &g_work[idx];
        if (!w->run || w->budget_left == 0) continue;
        if ((w->affinity_mask & 1u) == 0) {
            sched_work_enqueue(idx);
            continue;
        }
        w->run(w);
        if (w->budget_left > 0) w->budget_left--;
        if (w->budget_left > 0) {
            sched_work_enqueue(idx);
        }
        ran = 1;
        break;
    }

    g_sched_work_inflight = 0;
    return ran;
}

void sched_rng_seed(uint32 seed) {
    if (seed == 0) seed = SCHED_RNG_DEFAULT_SEED;
    g_sched_rng_state = seed;
}

uint32 sched_rng_next_u32(void) {
    /* Numerical Recipes LCG constants: fast and adequate for scheduling draws. */
    g_sched_rng_state = g_sched_rng_state * 1664525u + 1013904223u;
    return g_sched_rng_state;
}

uint32 sched_rng_bounded(uint32 upper_exclusive) {
    if (upper_exclusive <= 1u) return 0;

    /*
     * Map uniformly with multiply-high to avoid modulo bias for non-powers of two.
     */
    uint64 product = (uint64)sched_rng_next_u32() * (uint64)upper_exclusive;
    return (uint32)(product >> 32);
}

void sched_det_enable(int enabled) {
    g_det_enabled = enabled ? 1 : 0;
    if (g_det_enabled) {
        sched_cli();
        g_det_head = 0;
        g_det_tail = 0;
        sched_sti();
        sched_rng_seed(SCHED_RNG_DEFAULT_SEED);
    }
}

int sched_det_is_enabled(void) {
    return g_det_enabled != 0;
}

int sched_det_queue_irq(int irq) {
    if (!g_det_enabled) return -1;

    sched_cli();
    /* DET_QUEUE_CAP is a power-of-two, so wrap with a mask (no division/modulo). */
    uint16 next = (uint16)((g_det_tail + 1u) & (DET_QUEUE_CAP - 1u));
    if (next == g_det_head) {
        sched_sti();
        return -1;
    }
    g_det_queue[g_det_tail].type = DET_EVENT_IRQ;
    g_det_queue[g_det_tail].irq = (uint8)irq;
    g_det_tail = next;
    sched_sti();
    return 0;
}

int sched_det_step(uint32 max_events) {
    if (!g_det_enabled) return 0;
    if (g_det_processing) return 0;
    g_det_processing = 1;

    if (max_events == 0) max_events = 1;

    uint32 processed = 0;
    while (processed < max_events) {
        sched_cli();
        if (g_det_head == g_det_tail) {
            sched_sti();
            break;
        }
        det_event_t ev = g_det_queue[g_det_head];
        g_det_head = (uint16)((g_det_head + 1u) & (DET_QUEUE_CAP - 1u));
        sched_sti();

        if (ev.type == DET_EVENT_IRQ) {
            if (ev.irq == 0) {
                sched_tick_det();
            } else {
                irq_dispatch_deferred((int)ev.irq);
            }
        }
        processed++;
    }

    g_det_processing = 0;
    return (int)processed;
}

void scheduler_account(sched_work_t* w, uint32 cost) {
    if (!w || cost == 0) return;
    if (w->budget_left > cost) {
        w->budget_left -= cost;
    } else {
        w->budget_left = 0;
    }
}

void scheduler_yield_if_needed(sched_work_t* w) {
    if (!w) return;
    if (w->budget_ticks != 0 && w->budget_left == 0) {
        sched_yield();
    }
}

uint32 sched_get_tick_count(void) { return g_ticks; }
uint32 sched_get_tick_hz(void) { return g_tick_hz ? g_tick_hz : 100; }
uint32 sched_get_idle_hlt_count(void) { return g_idle_hlt_count; }


