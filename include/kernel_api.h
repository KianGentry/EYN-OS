#ifndef KERNEL_API_H
#define KERNEL_API_H

#include <types.h>

// Kernel API version
#define EYNOS_API_VERSION 1

// Function pointer types for kernel API
typedef void (*eyn_output_func)(const char* str, uint32 len);
typedef uint8 (*eyn_input_func)(void);
typedef uint32 (*eyn_system_func)(uint32 function, uint32 var1, uint32 var2);
typedef void (*eyn_delay_func)(uint32 microseconds);
typedef void (*eyn_yield_func)(void);
typedef void (*eyn_sleep_us_func)(uint32 microseconds);

// Kernel API structure - similar to BareMetal's function table
typedef struct {
    eyn_output_func output;      // 0x00 - Output string to console
    eyn_input_func input;        // 0x04 - Get input character
    eyn_system_func system;      // 0x08 - System functions
    eyn_delay_func delay;        // 0x0C - Delay execution
    eyn_yield_func yield;        // 0x10 - Cooperative yield
    eyn_sleep_us_func sleep_us;  // 0x14 - Sleep in microseconds
    // Future API functions can be added here
} eynos_kernel_api_t;

// System function indices (similar to BareMetal's b_system indices)
#define EYN_SYSTEM_TIMECOUNTER    0x00
#define EYN_SYSTEM_FREE_MEMORY    0x01
#define EYN_SYSTEM_GET_SCREEN_X   0x02
#define EYN_SYSTEM_GET_SCREEN_Y   0x03
#define EYN_SYSTEM_GET_SCREEN_BPP 0x04
#define EYN_SYSTEM_GET_LFB        0x05
#define EYN_SYSTEM_DEBUG_DUMP     0x06
#define EYN_SYSTEM_REBOOT         0x07
#define EYN_SYSTEM_SHUTDOWN       0x08

// Global kernel API pointer - will be set by kernel initialization
extern eynos_kernel_api_t* g_kernel_api;

// Convenience macros for user programs to call kernel functions
#define eyn_output(str, len) g_kernel_api->output(str, len)
#define eyn_input() g_kernel_api->input()
#define eyn_system(func, var1, var2) g_kernel_api->system(func, var1, var2)
#define eyn_delay(us) g_kernel_api->delay(us)
#define eyn_yield() g_kernel_api->yield()
#define eyn_sleep_us(us) g_kernel_api->sleep_us(us)

// Function declarations for kernel implementation
void eyn_kernel_output(const char* str, uint32 len);
uint8 eyn_kernel_input(void);
uint32 eyn_kernel_system(uint32 function, uint32 var1, uint32 var2);
void eyn_kernel_delay(uint32 microseconds);
void eyn_kernel_yield(void);
void eyn_kernel_sleep_us(uint32 microseconds);

// Initialize the kernel API
void eyn_kernel_api_init(void);

#endif // KERNEL_API_H
