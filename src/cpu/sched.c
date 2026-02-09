#include <sched.h>
#include <system.h>
#include <irq.h>
#include <watchdog.h>

#define SCHED_WORK_MAX 64
#define SCHED_WORK_PRIOS 8

typedef struct sched_queue {
    int head;
    int tail;
} sched_queue_t;

static sched_work_t g_work[SCHED_WORK_MAX];
static sched_queue_t g_runq[SCHED_WORK_PRIOS];
static int g_sched_work_inflight = 0;

static volatile uint32 g_ticks = 0;
static uint32 g_tick_hz = 100;
static uint32 g_timeslice_ticks = 5; // ~50ms at 100Hz
static uint32 g_current_slice = 0;
static volatile uint32 g_idle_hlt_count = 0; // counts ticks elapsed while idling (not raw HLTs)

static void sched_irq0_handler(void) {
    sched_tick();
}

void sched_init(void) {
    g_ticks = 0;
    // register tick handler on IRQ0
    register_interrupt_handler(0, sched_irq0_handler);
    sched_work_init();
}

void sched_yield(void) {
    // cooperative yield: no-op for now
    g_current_slice = 0; // force timeslice end handling on next tick
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

static void sched_work_enqueue(int idx) {
    if (idx < 0 || idx >= SCHED_WORK_MAX) return;
    sched_work_t* w = &g_work[idx];
    if (w->in_queue) return;
    uint32 prio = w->priority % SCHED_WORK_PRIOS;
    sched_queue_t* q = &g_runq[prio];
    w->next = -1;
    if (q->tail >= 0) {
        g_work[q->tail].next = idx;
        q->tail = idx;
    } else {
        q->head = idx;
        q->tail = idx;
    }
    w->in_queue = 1;
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
    for (int prio = SCHED_WORK_PRIOS - 1; prio >= 0; --prio) {
        int idx = sched_work_dequeue_prio((uint32)prio);
        if (idx < 0) continue;
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

uint32 sched_get_tick_count(void) { return g_ticks; }
uint32 sched_get_tick_hz(void) { return g_tick_hz ? g_tick_hz : 100; }
uint32 sched_get_idle_hlt_count(void) { return g_idle_hlt_count; }


