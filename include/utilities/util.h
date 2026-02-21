#ifndef UTIL_H
#define UTIL_H

#include <misc/types.h>
#include <stddef.h>

// Global interrupt flag
extern volatile int g_user_interrupt;

// Minimal user-task control (prototype)
extern volatile int g_user_task_active;
extern volatile int g_abort_to_shell;

// When a ring3 user task is active under the tiling manager, pin its stdout to
// the virtual terminal that launched it (so output doesn't follow focus).
extern volatile int g_user_task_term;

// While a ring3 task is active, IRQ0 drives a limited UI loop. This flag is set
// when something changes (user stdout, keypress, focus) and a repaint is needed.
extern volatile int g_user_task_ui_dirty;

// Last user-task mapping info (for cleanup on exit/abort)
extern volatile uint32 g_user_code_base;
extern volatile uint32 g_user_code_pages;
// Bottom (lowest VA) of the mapped user stack region.
// Historically this was a single page at USER_STACK_TOP-PAGE_SIZE.
extern volatile uint32 g_user_stack_page;

void user_task_cleanup_mappings(void);

// Return control to the interactive UI after aborting a user task.
// Chooses the tiling-manager UI when available; otherwise falls back to the classic shell.
void ui_return_from_user_task(void);

// Memory management (standardized names, standard signatures)
void *malloc(size_t nbytes);
void free(void *ptr);
void *realloc(void *ptr, size_t new_size);
void *calloc(size_t count, size_t size);
void init_memory_manager(void);

// Memory detection
uint32 detect_available_memory(void);

// Diagnostics and stats
void print_memory_stats(void);
void check_stack_overflow(void);
int get_memory_error_count(void);
int get_stack_overflow_status(void);
uint32 get_current_stack_pointer(void);
uint32 get_heap_size(void);
// Return approximate used heap bytes
uint32 get_heap_used(void);
// Total physical RAM in bytes (from multiboot memory map, cached)
uint32 get_total_ram(void);
void putchar(char c);

// Hint for static analyzers: treat `p` as escaping the current function.
// Runtime behavior is a no-op.
void util_escape_ptr(const void* p);

#endif
