#include <predictive_memory.h>
#include <shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <context.h>
#include <misc/sched.h>

// Command handlers for predictive memory management
void predict_cmd(const shell_args_t* args);
void mmap_cmd(const shell_args_t* args);
void munmap_cmd(const shell_args_t* args);
void msync_cmd(const shell_args_t* args);
void memory_stats_cmd(const shell_args_t* args);

static int pred_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Register commands
REGISTER_SHELL_COMMAND(predict_cmd_info, "predict", predict_cmd, CMD_STREAMING, "Predictive memory management", "predict [stats|reset|optimize]");
REGISTER_SHELL_COMMAND(mmap_cmd_info, "mmap", mmap_cmd, CMD_STREAMING, "Memory map a file for zero-copy access", "mmap <filename>");
REGISTER_SHELL_COMMAND(munmap_cmd_info, "munmap", munmap_cmd, CMD_STREAMING, "Unmap a memory-mapped file", "munmap <address>");
REGISTER_SHELL_COMMAND(msync_cmd_info, "msync", msync_cmd, CMD_STREAMING, "Synchronize memory-mapped file to disk", "msync <address>");
REGISTER_SHELL_COMMAND(memory_stats_cmd_info, "memory_stats", memory_stats_cmd, CMD_STREAMING, "Show predictive memory statistics", "memory_stats");

// Predictive memory command
void predict_cmd(const shell_args_t* args) {
    if (!pred_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return;
    if (!args || args->argc < 2) {
        printf("%cUsage: predict [stats|reset|optimize|init]\n", 255, 255, 255);
        printf("%c  stats    - Show prediction statistics\n", 255, 255, 255);
        printf("%c  reset    - Reset prediction statistics\n", 255, 255, 255);
        printf("%c  optimize - Run memory optimization\n", 255, 255, 255);
        printf("%c  init     - Initialize predictive memory system\n", 255, 255, 255);
        return;
    }

    const char* subcmd = args->argv[1];
    if (strcmp(subcmd, "stats") == 0) {
        print_prediction_stats();
    } else if (strcmp(subcmd, "reset") == 0) {
        reset_prediction_stats();
        printf("%cPrediction statistics reset\n", 0, 255, 0);
    } else if (strcmp(subcmd, "optimize") == 0) {
        predictive_memory_optimize();
        printf("%cMemory optimization completed\n", 0, 255, 0);
    } else if (strcmp(subcmd, "init") == 0) {
        predictive_memory_init();
        printf("%cPredictive memory system initialized\n", 0, 255, 0);
    } else {
        printf("%cUnknown subcommand: %s\n", 255, 0, 0, subcmd ? subcmd : "");
    }
}

// Memory mapping command
void mmap_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2) {
        printf("%cUsage: mmap <filename> [readonly]\n", 255, 255, 255);
        printf("%cExample: mmap test.txt\n", 255, 255, 255);
        printf("%cExample: mmap data.bin readonly\n", 255, 255, 255);
        return;
    }

    const char* filename = args->argv[1];
    
    // Check for readonly flag
    uint8_t read_only = 0;
    if (args->argc >= 3 && strcmp(args->argv[2], "readonly") == 0) {
        read_only = 1;
    }

    uint32 caps = read_only ? CAP_READ_FS : (CAP_READ_FS | CAP_WRITE_FS);
    if (!pred_ctx_allow(caps, SCHED_COST_FS)) return;
    
    void* addr;
    size_t size;
    
    if (eynfs_mmap(filename, &addr, &size, read_only) == 0) {
        printf("%cFile mapped successfully\n", 0, 255, 0);
        printf("%cAddress: 0x%X\n", 255, 255, 255, (uint32_t)addr);
        printf("%cSize: %d bytes\n", 255, 255, 255, (int)size);
        printf("%cMode: %s\n", 255, 255, 255, read_only ? "read-only" : "read-write");
    } else {
        printf("%cFailed to map file: %s\n", 255, 0, 0, filename);
    }
}

static uint32_t parse_hex_u32(const char* s) {
    if (!s) return 0;
    uint32_t v = 0;
    uint32_t i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    for (; s[i]; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

// Memory unmapping command
void munmap_cmd(const shell_args_t* args) {
    if (!pred_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return;
    if (!args || args->argc < 2) {
        printf("%cUsage: munmap <address>\n", 255, 255, 255);
        printf("%cExample: munmap 0x12345678\n", 255, 255, 255);
        return;
    }

    uint32_t addr = parse_hex_u32(args->argv[1]);
    
    if (eynfs_munmap((void*)addr, 0) == 0) {
        printf("%cFile unmapped successfully\n", 0, 255, 0);
    } else {
        printf("%cFailed to unmap address 0x%X\n", 255, 0, 0, addr);
    }
}

// Memory synchronization command
void msync_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2) {
        printf("%cUsage: msync <address>\n", 255, 255, 255);
        printf("%cExample: msync 0x12345678\n", 255, 255, 255);
        return;
    }

    if (!pred_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    

    uint32_t addr = parse_hex_u32(args->argv[1]);
    
    if (eynfs_msync((void*)addr, 0) == 0) {
        printf("%cFile synchronized successfully\n", 0, 255, 0);
    } else {
        printf("%cFailed to sync address 0x%X\n", 255, 0, 0, addr);
    }
}

// Memory statistics command
void memory_stats_cmd(const shell_args_t* args) {
    (void)args;
    printf("%c=== Memory Management Statistics ===\n", 255, 255, 255);
    
    // Show predictive memory stats
    print_prediction_stats();
    
    // Show regular memory stats
    printf("%c\n=== Regular Memory Statistics ===\n", 255, 255, 255);
    print_memory_stats();
    
    // Show memory mapping info
    printf("%c\n=== Memory Mapping Information ===\n", 255, 255, 255);
    printf("%cTotal memory accesses: %d\n", 255, 255, 255, get_memory_access_count());
    printf("%cPrediction accuracy: %d%%\n", 255, 255, 255, get_prediction_hit_rate());
} 