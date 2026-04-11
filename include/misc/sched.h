#ifndef SCHED_H
#define SCHED_H

#include <misc/types.h>

// simple cooperative scheduler stubs

// initialize scheduler state
void sched_init(void);

// yield the cpu cooperatively
void sched_yield(void);

// sleep for approximately microseconds (busy-wait fallback)
void sched_sleep_us(uint32 microseconds);

// tick hook for future timer integration
void sched_tick(void);

// inform scheduler of tick frequency (hz)
void sched_set_tick_hz(uint32 hz);

// optional: set timeslice length in ticks for preemption hinting
void sched_set_timeslice_ticks(uint32 ticks);

// called when a timeslice ends to trigger a context switch (stub)
void sched_on_timeslice_end(void);

// Object-centric scheduler support
#define SCHED_WORK_CPU_ANY 0xFFFFFFFFu

typedef struct sched_work sched_work_t;
struct sched_work {
	uint32 id;
	uint32 priority;
	int next;
	uint8 in_queue;
	uint8 _pad[3];
	void (*run)(sched_work_t* w);
	void* userdata;
	uint32 affinity_mask;
	uint32 budget_ticks;
	uint32 budget_left;
	uint32 cache_hint;
};

void sched_work_init(void);
int sched_work_register(void (*run)(sched_work_t*), void* userdata,
						uint32 priority, uint32 affinity_mask,
						uint32 budget_ticks, uint32 cache_hint);
int sched_work_set_ready(uint32 id);
int sched_work_unready(uint32 id);
int sched_work_update_budget(uint32 id, uint32 budget_ticks);
int sched_work_on_timeslice_end(void);

// Deterministic execution mode
void sched_det_enable(int enabled);
int sched_det_is_enabled(void);
int sched_det_queue_irq(int irq);
int sched_det_step(uint32 max_events);

/*
 * ABI-INVARIANT: Scheduler PRNG interface.
 *
 * Why: Scheduling policies that use randomness (e.g. lottery selection)
 * require a kernel-owned PRNG stream that can be made reproducible in
 * deterministic mode.
 * Invariant: Calls to sched_rng_next_u32()/sched_rng_bounded() advance a
 * scheduler-local sequence only; they do not depend on userland RNG state.
 * ABI-sensitive: Yes (DET mode userspace tests rely on repeatable ordering).
 */
void sched_rng_seed(uint32 seed);
uint32 sched_rng_next_u32(void);
uint32 sched_rng_bounded(uint32 upper_exclusive);

// Lightweight timing/usage getters ---
// Monotonic scheduler tick counter (increments in IRQ0 handler)
uint32 sched_get_tick_count(void);
// Configured tick frequency in Hz
uint32 sched_get_tick_hz(void);
// Count of idle halts performed inside sched_sleep_us (used to estimate idle time)
uint32 sched_get_idle_hlt_count(void);

// Scheduler accounting helpers
#define SCHED_COST_CONSOLE 1u
#define SCHED_COST_FS      1u
#define SCHED_COST_ALLOC   1u

void scheduler_account(sched_work_t* w, uint32 cost);
void scheduler_yield_if_needed(sched_work_t* w);

#endif // SCHED_H


