#include <kernel_api.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <sched.h>
#include <context.h>

// Global kernel API structure
eynos_kernel_api_t g_kernel_api_struct;
eynos_kernel_api_t* g_kernel_api = &g_kernel_api_struct;

static int kernel_api_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Kernel API function implementations
void eyn_kernel_output(const char* str, uint32 len) {
    if (!str || len == 0) return;
    if (!kernel_api_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) return;
    
    // Use printf for output (it handles the VGA driver internally)
    // Create a null-terminated string for printf
    char* temp_str = (char*)malloc(len + 1);
    if (!temp_str) return;
    
    memcpy(temp_str, str, len);
    temp_str[len] = '\0';
    
    printf("%s", temp_str);
    free(temp_str);
}

uint8 eyn_kernel_input(void) {
    // For now, return 0 (no input available)
    // This can be enhanced later with proper keyboard input handling
    return 0;
}

uint32 eyn_kernel_system(uint32 function, uint32 var1, uint32 var2) {
    switch (function) {
        case EYN_SYSTEM_TIMECOUNTER:
            // Return a simple time counter (can be enhanced with real timer)
            return (uint32)(var1 + var2); // Placeholder
            
        case EYN_SYSTEM_FREE_MEMORY:
            // Return available memory (placeholder)
            return 1024 * 1024; // 1MB placeholder
            
        case EYN_SYSTEM_GET_SCREEN_X:
            return 80; // Default VGA text mode width
            
        case EYN_SYSTEM_GET_SCREEN_Y:
            return 25; // Default VGA text mode height
            
        case EYN_SYSTEM_GET_SCREEN_BPP:
            return 32; // Assume 32-bit color
            
        case EYN_SYSTEM_GET_LFB:
            return 0xB8000; // VGA text mode framebuffer address
            
        case EYN_SYSTEM_DEBUG_DUMP:
            // Dump memory at address var1, length var2
            if (!kernel_api_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) {
                return (uint32)-1;
            }
            if (var1 && var2 && var2 <= 256) {
                printf("[DEBUG] Memory dump at 0x%X, length %d:\n", var1, var2);
                uint8* ptr = (uint8*)var1;
                for (uint32 i = 0; i < var2; i++) {
                    if (i % 16 == 0) printf("\n%04X: ", i);
                    printf("%02X ", ptr[i]);
                }
                printf("\n");
            }
            return 0;
            
        case EYN_SYSTEM_REBOOT:
            if (!kernel_api_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) {
                return (uint32)-1;
            }
            printf("[SYSTEM] Reboot requested\n");
            // In a real implementation, this would trigger a reboot
            return 0;
            
        case EYN_SYSTEM_SHUTDOWN:
            if (!kernel_api_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) {
                return (uint32)-1;
            }
            printf("[SYSTEM] Shutdown requested\n");
            // In a real implementation, this would trigger a shutdown
            return 0;
            
        default:
            if (!kernel_api_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) {
                return (uint32)-1;
            }
            printf("[SYSTEM] Unknown system function: %d\n", function);
            return (uint32)-1;
    }
}

void eyn_kernel_delay(uint32 microseconds) {
    // Simple delay loop (can be enhanced with proper timer)
    volatile uint32 count = microseconds * 1000; // Rough approximation
    while (count--) {
        asm volatile("nop");
    }
}

void eyn_kernel_yield(void) {
    sched_yield();
}

void eyn_kernel_sleep_us(uint32 microseconds) {
    sched_sleep_us(microseconds);
}

void eyn_kernel_api_init(void) {
    // Initialize the kernel API function table
    g_kernel_api_struct.output = eyn_kernel_output;
    g_kernel_api_struct.input = eyn_kernel_input;
    g_kernel_api_struct.system = eyn_kernel_system;
    g_kernel_api_struct.delay = eyn_kernel_delay;
    g_kernel_api_struct.yield = eyn_kernel_yield;
    g_kernel_api_struct.sleep_us = eyn_kernel_sleep_us;
    
//    printf("[KERNEL] API initialized (version %d)\n", EYNOS_API_VERSION);
}
