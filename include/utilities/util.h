#ifndef UTIL_H
#define UTIL_H

#include <types.h>
#include <stddef.h>

// Global interrupt flag
extern volatile int g_user_interrupt;

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
void putchar(char c);

#endif
