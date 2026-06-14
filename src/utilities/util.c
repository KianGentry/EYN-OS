#include <types.h>
#include <util.h>
#include <string.h>
#include <vga.h>
#include <stdint.h>
#include <multiboot.h>
#include <tile_manager.h>
#include <shell.h>
#include <mm/vmm.h>
#include <isr.h>
#include <cpu/segdom.h>
#include <cpu/gdt.h>
#include <context.h>
#include <misc/sched.h>
#include <rust_alloc.h>
#include <mm/slab.h>
#include <utilities/shell/pipeline.h>
#include <cpu/user_elf.h>

volatile int g_user_interrupt = 0;
volatile int g_user_task_active = 0;
volatile int g_abort_to_shell = 0;
volatile int g_user_task_term = -1;
volatile int g_user_task_ui_dirty = 0;

// User-task console colour state (used by ring3 stdout/stderr). Defaults to white.
volatile int g_user_task_colour_r = 255;
volatile int g_user_task_colour_g = 255;
volatile int g_user_task_colour_b = 255;
// Parser state for 0xFF,r,g,b control sequences that may be split across writes.
volatile uint8 g_user_task_colour_state = 0;
volatile uint8 g_user_task_colour_bytes[3] = {0, 0, 0};

// Parser state for ring3 stdout control sequences.
//
// - 0xFF,r,g,b sets the current user-task RGB text colour.
// - 0xFE,<16 bytes> registers a GUI icon key for the current output line.
//   The key is a NUL-terminated string padded with zeros up to 16 bytes,
//   matching entries in /icons (e.g. "file_txt", "dir_full").
volatile uint8 g_user_task_icon_state = 0;
volatile uint8 g_user_task_icon_bytes[16] = {0};

volatile uint32 g_user_code_base = 0;
volatile uint32 g_user_code_pages = 0;
volatile uint32 g_user_stack_page = 0;

void user_task_cleanup_mappings(void) {
    // Best-effort cleanup; safe to call repeatedly.
    uint32 base = 0;
    uint32 pages = 0;
    uint32 stack_page = 0;
    user_task_get_current_mapping_state(&base, &pages, &stack_page);
    uint32 stack_bottom = vmm_kernel_as.stack_bottom;

    vm_tlb_defer_begin();

    if (base && pages) {
        for (uint32 i = 0; i < pages; ++i) {
            (void)vmm_unmap_page(&vmm_kernel_as, base + i * PAGE_SIZE);
        }
    }

    // Unmap user stack pages. The VMM can grow the stack on-demand, so we use
    // the current stack_bottom as the lower bound when it looks valid.
    if (stack_bottom >= USER_STACK_BASE && stack_bottom < USER_STACK_TOP) {
        for (uint32 va = stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
            (void)vmm_unmap_page(&vmm_kernel_as, va);
        }
    } else if (stack_page) {
        // Back-compat: older callers only tracked a single stack page.
        (void)vmm_unmap_page(&vmm_kernel_as, stack_page);
    }

    user_task_clear_current_mapping_state();
    // Legacy mirrors for code paths still reading singleton globals.
    g_user_code_base = 0;
    g_user_code_pages = 0;
    g_user_stack_page = 0;

    g_user_segdom_cs = GDT_USER_CS;
    g_user_segdom_ds = GDT_USER_DS;

    // Reset stack metadata for the kernel address space.
    vmm_kernel_as.stack_bottom = USER_STACK_TOP - PAGE_SIZE;

    vm_tlb_defer_end();

    // Keep inherited user FDs alive while a multi-stage shell pipeline is
    // active; the pipeline runtime will close and reset them when it finishes.
    if (!pipeline_is_runtime_active()) {
        syscall_reset_user_fds();
    }
    syscall_reset_user_streams();
}

void user_task_abort_continue(void) {
    // Best-effort cleanup and clear state before re-entering the UI.
    command_context_clear();
    // If a UELF task exited or crashed while a redirect was active (e.g. the
    // tiling-manager terminal wraps commands in start/stop_shell_redirect and
    // an abrupt exit bypasses stop_shell_redirect), shell_redirect_active can
    // be left as 1.  Reset it here so subsequent kernel printf calls go to VGA
    // instead of the capture buffer+serial.
    stop_shell_redirect();
    g_shell_capture_mode = 0;
    user_task_cleanup_mappings();
    g_abort_to_shell = 0;
    g_user_task_active = 0;
    g_user_task_term = -1;
    g_user_task_ui_dirty = 0;

    g_user_task_colour_r = 255;
    g_user_task_colour_g = 255;
    g_user_task_colour_b = 255;
    g_user_task_colour_state = 0;
    g_user_task_icon_state = 0;

    // Scheduler-first continuation: run queued spawned tasks before UI fallback.
    if (user_task_continue_or_schedule()) {
        // If a queued task is launched this path does not normally return;
        // if it does (load failure), continue to pipeline/UI fallback.
    }

    // Continue any pipeline stage that was armed before this user task exited.
    // This path is reached via non-local abort return from SYSCALL_EXIT.
    (void)pipeline_resume_pending();

    // Prefer the graphical tiling-manager shell when it's been initialized.
    if (tile_is_tiling_active()) {
        start_tiling_manager();
    } else {
        launch_shell(0);
    }

    // Shouldn't return; if it does, stop safely.
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

void ui_return_from_user_task(void) {
    user_task_abort_continue();
}

static int mem_ctx_allow(void) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, CAP_ALLOC_MEMORY)) {
        return 0;
    }
    return 1;
}

static void mem_ctx_account(void) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, SCHED_COST_ALLOC);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

uint32_t __stack_chk_fail(){
    return 0;
}

// Standard C library memory functions (optimized implementations)
__attribute__((weak)) void *memcpy(void *dest, const void *src, size_t n) {
    if (n <= 0) return dest;
    
    // Check if we can do word-aligned copying (both pointers aligned to 4-byte boundary)
    uintptr src_addr = (uintptr)src;
    uintptr dest_addr = (uintptr)dest;
    
    // Copy unaligned bytes at the beginning
    size_t unaligned_start = 0;
    if (src_addr % 4 != 0 || dest_addr % 4 != 0) {
        unaligned_start = 4 - (src_addr % 4);
        if (unaligned_start > n) unaligned_start = n;
        
        for (size_t i = 0; i < unaligned_start; i++) {
            ((char*)dest)[i] = ((char*)src)[i];
        }
    }
    
    // Copy 4-byte words when possible
    size_t words = (n - unaligned_start) / 4;
    if (words > 0) {
        uint32_t *src32 = (uint32_t*)((char*)src + unaligned_start);
        uint32_t *dst32 = (uint32_t*)((char*)dest + unaligned_start);
        
        for (size_t i = 0; i < words; i++) {
            dst32[i] = src32[i];
        }
    }
    
    // Copy remaining bytes
    size_t remaining = n - unaligned_start - (words * 4);
    if (remaining > 0) {
        size_t offset = unaligned_start + (words * 4);
        for (size_t i = 0; i < remaining; i++) {
            ((char*)dest)[offset + i] = ((char*)src)[offset + i];
        }
    }
    
    return dest;
}

__attribute__((weak)) void *memset(void *s, int c, size_t n) {
    if (n <= 0) return s;
    
    uint8_t *ptr = (uint8_t*)s;
    uint8_t val = (uint8_t)c;
    
    // Check if we can do word-aligned setting
    uintptr addr = (uintptr)ptr;
    size_t unaligned_start = 0;
    
    if (addr % 4 != 0) {
        unaligned_start = 4 - (addr % 4);
        if (unaligned_start > n) unaligned_start = n;
        
        for (size_t i = 0; i < unaligned_start; i++) {
            ptr[i] = val;
        }
    }
    
    // Set 4-byte words when possible
    size_t words = (n - unaligned_start) / 4;
    if (words > 0) {
        uint32_t *ptr32 = (uint32_t*)(ptr + unaligned_start);
        uint32_t word_val = (val << 24) | (val << 16) | (val << 8) | val;
        
        for (size_t i = 0; i < words; i++) {
            ptr32[i] = word_val;
        }
    }
    
    // Set remaining bytes
    size_t remaining = n - unaligned_start - (words * 4);
    if (remaining > 0) {
        size_t offset = unaligned_start + (words * 4);
        for (size_t i = 0; i < remaining; i++) {
            ptr[offset + i] = val;
        }
    }
    
    return s;
}

void util_escape_ptr(const void* p) {
    __asm__ __volatile__(/*! escape */ "" : : "g"(p) : "memory");
}

// EYN-OS specific memory functions
// Provide POSIX-like names for allocators to standardize usage

void *memmove(void *dest, const void *src, size_t n) {
    if (n <= 0) return dest;
    
    // If source and destination overlap and source is before destination,
    // we need to copy backwards to avoid overwriting source data
    if (src < dest && (char*)src + n > (char*)dest) {
        // Copy backwards for overlapping ranges.
        char *d = (char*)dest;
        const char *s = (const char*)src;
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
        return dest;
    }

    // Use memcpy for non-overlapping or forward copy.
    return memcpy(dest, src, n);
}

/**
 * K&R implementation
 */
string int_to_ascii(int n, char str[]) {          
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';         
    } while ((n /= 10) > 0);

    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    // Note: String reversal is handled in int_to_string function
    return str;
}
// String conversion functions moved to string.c for standardization

// String validation functions moved to string.c for standardization

// Robust Memory Manager with Enhanced Error Handling ---
// Heap start must be after the kernel image (.text/.rodata/.data/.bss).
// Using a fixed address (like 0x200000) breaks as soon as .bss grows.
extern uint8 __kernel_end;
#define HEAP_SIZE_DEFAULT 0x100000    // 1MB default heap size (increased for assembler)
#define HEAP_SIZE_MIN 0x40000         // 256KB minimum heap size
#define HEAP_SIZE_MAX 0x1000000       // 16MB maximum heap size
#define MIN_BLOCK_SIZE 16     // Reduced from 32
#define NO_BLOCK 0xFFFFFFFF
#define MAGIC_NUMBER 0xDEADBEEF

typedef struct {
    uint32 size;        // Size of the block (including header)
    uint32 used;        // 1 if used, 0 if free
    uint32 next;        // Offset to next block (NO_BLOCK if last)
    uint32 magic;       // Magic number for corruption detection
} block_header_t;

// Must match block_header_t layout. If this is smaller than sizeof(block_header_t),
// malloc() will hand out pointers into the header and corrupt allocator metadata.
#define BLOCK_HEADER_SIZE ((uint32)sizeof(block_header_t))

static uint8* heap_start = (uint8*)0;
static uint32 heap_phys_start = 0;
static uint32 heap_size = HEAP_SIZE_DEFAULT;  // Dynamic heap size
static uint32 first_block = 0;
static int memory_initialized = 0;
static uint32 memory_errors = 0;
static uint32 allocation_count = 0;
static uint32 free_count = 0;

/*
 * RESOURCE-INVARIANT: Minimum physical memory to keep outside the legacy heap.
 *
 * Why: The heap reserves physical frames up-front via vmm_reserve_phys_range().
 *      On low-RAM targets this can otherwise drive the frame allocator to 0 and
 *      effectively brick paging, page-table allocation, slab growth, and user
 *      program startup.
 * Breakage if changed:
 *   - Increasing: smaller heap, but more reliable user task + VMM operation.
 *   - Decreasing: higher risk of "out of physical frames" and silent failures.
 */
#define HEAP_PHYS_RESERVE_BYTES (512u * 1024u) /* 512KB */

static inline uint32 align_up_u32(uint32 v, uint32 align) {
    if (align == 0) return v;
    return (v + align - 1) & ~(align - 1);
}

// Stack overflow protection - ultra lightweight
static volatile int stack_overflow_detected = 0;

// Dynamic memory detection (safer implementation)
uint32 detect_available_memory() {
    extern multiboot_info_t* g_mbi;

    if (!g_mbi) {
        return 32u * 1024u * 1024u; // Conservative fallback
    }

    // Prefer the basic mem_lower/mem_upper fields when available.
    // They are simple and robust and avoid relying on the mmap buffer being
    // mapped/accessible after paging is enabled.
    uint32 total_basic = 0;
    if (g_mbi->flags & MULTIBOOT_INFO_MEMORY) {
        uint64 total_kb = (uint64)g_mbi->mem_lower + (uint64)g_mbi->mem_upper;
        uint64 total_bytes = total_kb * 1024ull;
        if (total_bytes > 0xFFFFFFFFull) total_bytes = 0xFFFFFFFFull;
        total_basic = (uint32)total_bytes;
    }

    // If a full memory map is available, walk it using the multiboot-defined
    // variable-sized entry format.
    uint32 total_mmap = 0;
    if ((g_mbi->flags & MULTIBOOT_INFO_MEM_MAP) && g_mbi->mmap_addr && g_mbi->mmap_length) {
        const uint8* p = (const uint8*)(uintptr_t)g_mbi->mmap_addr;
        const uint8* end = p + (uint32)g_mbi->mmap_length;

        uint64 max_usable_end = 0;
        uint32 guard = 0;
        while (p + sizeof(multiboot_memory_map_t) <= end && guard++ < 256) {
            const multiboot_memory_map_t* e = (const multiboot_memory_map_t*)(const void*)p;

            // Entry size includes everything after the 'size' field.
            uint32 entry_bytes = (uint32)e->size + (uint32)sizeof(e->size);
            if (entry_bytes < sizeof(multiboot_memory_map_t) || p + entry_bytes > end) {
                break;
            }

            if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->len) {
                uint64 start = e->addr;
                uint64 region_end = e->addr + e->len;
                // Clamp to 32-bit physical for this kernel.
                if (start < 0x100000000ull) {
                    if (region_end > 0x100000000ull) region_end = 0x100000000ull;
                    if (region_end > max_usable_end) max_usable_end = region_end;
                }
            }

            p += entry_bytes;
        }

        if (max_usable_end > 0xFFFFFFFFull) max_usable_end = 0xFFFFFFFFull;
        total_mmap = (uint32)max_usable_end;
    }

    uint32 total = total_basic;
    if (total_mmap > total) total = total_mmap;

    // Final sanity fallback: if we couldn't detect anything plausible, pick a
    // conservative default so the system can still boot.
    if (total < 1u * 1024u * 1024u) {
        return 32u * 1024u * 1024u;
    }
    return total;
}

// Cached total RAM accessor for consumers like stats GUI
uint32 get_total_ram(void) {
    static uint32 cached_total = 0;
    if (cached_total == 0) {
        uint32 v = detect_available_memory();
        // Basic sanity clamp to avoid zero
        if (v == 0) v = 32 * 1024 * 1024;
        cached_total = v;
    }
    return cached_total;
}

// Adaptive heap sizing based on available memory (safer)
static void calculate_optimal_heap_size() {
    uint32 available_ram = detect_available_memory();
    
    // Use more generous heap sizing for assembler and zero-copy operations
    if (available_ram <= 4 * 1024 * 1024) {        // 4MB or less
        heap_size = 0x100000;                        // 1MB (increased for assembler)
    } else if (available_ram <= 16 * 1024 * 1024) { // 16MB or less
        heap_size = 0x200000;                        // 2MB (increased for assembler)
    } else if (available_ram <= 64 * 1024 * 1024) { // 64MB or less
        heap_size = 0x400000;                        // 4MB (increased for assembler)
    } else if (available_ram <= 256 * 1024 * 1024) { // 256MB or less
        heap_size = 0x800000;                        // 8MB (doubled)
    } else {                                         // 256MB+
        heap_size = 0x2000000;                       // 32MB (doubled)
    }
    
    // Ensure heap doesn't exceed available memory
    // Be more generous on low-RAM systems to support tools like the assembler
    if (available_ram <= 8 * 1024 * 1024) {
        // Up to half of RAM as heap when total RAM <= 8MB
        if (heap_size > available_ram / 2) heap_size = available_ram / 2;
    } else {
        // Otherwise cap at a quarter of RAM
        if (heap_size > available_ram / 4) heap_size = available_ram / 4;
    }
    
    // Ensure minimum heap size
    if (heap_size < HEAP_SIZE_MIN) {
        heap_size = HEAP_SIZE_MIN;
    }
}

// Ultra-lightweight checksum calculation
static uint32 calculate_checksum(uint8* data, uint32 size) {
    uint32 checksum = 0;
    for (uint32 i = 0; i < size && i < 16; i++) { // Only check first 16 bytes
        checksum = (checksum << 1) + data[i];
    }
    return checksum;
}

// Enhanced block validation with better error reporting
static int validate_block(block_header_t* block, uint32 offset) {
#if CONFIG_RUST_ALLOCATOR
    rust_heap_validation_result_t vr = rust_validate_heap_block(
        (const rust_heap_block_header_t*)block,
        offset,
        heap_start,
        heap_size,
        HEAP_SIZE_MIN,
        BLOCK_HEADER_SIZE);

    if (vr != RUST_HEAP_VALIDATION_OK) {
        uintptr heap_begin = (uintptr)heap_start;
        uintptr heap_end = heap_begin + heap_size;
        uintptr block_addr = (uintptr)block;

        switch (vr) {
            case RUST_HEAP_VALIDATION_OK:
                break;
            case RUST_HEAP_VALIDATION_NULL_BLOCK:
                printf("%c[MEMORY] Null block pointer at offset 0x%X\n", 255, 0, 0, offset);
                break;
            case RUST_HEAP_VALIDATION_BAD_HEAP_BASE:
                printf("%c[MEMORY] Heap base corrupt (heap_start=0x%X heap_size=%d)\n", 255, 0, 0, heap_begin, heap_size);
                break;
            case RUST_HEAP_VALIDATION_HEAP_OVERFLOW:
                printf("%c[MEMORY] Heap bounds overflow (heap_start=0x%X heap_size=%d)\n", 255, 0, 0, heap_begin, heap_size);
                break;
            case RUST_HEAP_VALIDATION_BLOCK_OUTSIDE_HEAP:
                printf("%c[MEMORY] Block pointer out of heap (block=0x%X heap=[0x%X..0x%X))\n", 255, 0, 0, block_addr, heap_begin, heap_end);
                break;
            case RUST_HEAP_VALIDATION_OFFSET_MISMATCH:
                printf("%c[MEMORY] Block pointer mismatch (offset=0x%X block=0x%X expected=0x%X)\n", 255, 0, 0, offset, block_addr, heap_begin + offset);
                break;
            case RUST_HEAP_VALIDATION_BLOCK_RANGE_INVALID:
                printf("%c[MEMORY] Block out of bounds at offset 0x%X (size: %d, heap: %d)\n", 255, 0, 0, offset, block ? block->size : 0, heap_size);
                break;
            case RUST_HEAP_VALIDATION_BLOCK_SIZE_INVALID:
                printf("%c[MEMORY] Invalid block size at offset 0x%X: %d (min: %d, max: %d)\n", 255, 0, 0, offset, block ? block->size : 0, BLOCK_HEADER_SIZE, heap_size);
                break;
            default:
                printf("%c[MEMORY] Unknown heap validation error at offset 0x%X\n", 255, 0, 0, offset);
                break;
        }

        memory_errors++;
        return 0;
    }
#else
    if (!block) {
        printf("%c[MEMORY] Null block pointer at offset 0x%X\n", 255, 0, 0, offset);
        memory_errors++;
        return 0;
    }

    // Validate heap_start/heap_size before touching any block fields.
    uintptr heap_begin = (uintptr)heap_start;
    if (!heap_begin || heap_begin < 0x100000 || heap_size < HEAP_SIZE_MIN) {
        printf("%c[MEMORY] Heap base corrupt (heap_start=0x%X heap_size=%d)\n", 255, 0, 0, heap_begin, heap_size);
        memory_errors++;
        return 0;
    }
    uintptr heap_end = heap_begin + heap_size;
    if (heap_end <= heap_begin) {
        printf("%c[MEMORY] Heap bounds overflow (heap_start=0x%X heap_size=%d)\n", 255, 0, 0, heap_begin, heap_size);
        memory_errors++;
        return 0;
    }

    // Ensure the block pointer itself is within the heap before dereferencing.
    uintptr block_addr = (uintptr)block;
    if (block_addr < heap_begin || block_addr + sizeof(block_header_t) > heap_end) {
        printf("%c[MEMORY] Block pointer out of heap (block=0x%X heap=[0x%X..0x%X))\n", 255, 0, 0, block_addr, heap_begin, heap_end);
        memory_errors++;
        return 0;
    }

    // Sanity: offset should match the pointer.
    if (block_addr != heap_begin + offset) {
        printf("%c[MEMORY] Block pointer mismatch (offset=0x%X block=0x%X expected=0x%X)\n", 255, 0, 0, offset, block_addr, heap_begin + offset);
        memory_errors++;
        return 0;
    }
    
    // Check if block is within heap bounds first
    // (safe now that block pointer is validated)
    if (offset >= heap_size || offset + BLOCK_HEADER_SIZE > heap_size || offset + block->size > heap_size) {
        printf("%c[MEMORY] Block out of bounds at offset 0x%X (size: %d, heap: %d)\n", 255, 0, 0, offset, block->size, heap_size);
        memory_errors++;
        return 0;
    }
    
    // Check for reasonable block size
    if (block->size < BLOCK_HEADER_SIZE || block->size > heap_size || block->size == 0) {
        printf("%c[MEMORY] Invalid block size at offset 0x%X: %d (min: %d, max: %d)\n", 255, 0, 0, offset, block->size, BLOCK_HEADER_SIZE, heap_size);
        memory_errors++;
        return 0;
    }
#endif
    
    // Temporarily disable magic number checking to eliminate false positives
    // This will be re-enabled once we have better corruption detection
    /*
    if (block->used && block->magic != MAGIC_NUMBER && block->size > BLOCK_HEADER_SIZE) {
        printf("%c[MEMORY] Block corruption detected at offset 0x%X (magic: 0x%X)\n", 255, 0, 0, offset, block->magic);
        memory_errors++;
        return 0;
    }
    */
    
    return 1;
}

// Check for stack overflow (ultra lightweight)
void check_stack_overflow() {
    // Temporarily disabled to avoid false positives
    return;
}

static void* heap_malloc(size_t nbytes);
static void heap_free(void* ptr);

void init_memory_manager() {
    if (memory_initialized) return;

    // Choose a heap start that cannot overlap the VMM's boot-time allocations
    // (page tables, etc.). IMPORTANT: user tasks are mapped into the *low* user
    // range (starting at USER_CODE_BASE = 0x00400000). If the kernel heap also
    // lives in the low identity range, loading a user task can overwrite/unmap
    // heap pages and corrupt the allocator.
    //
    // To avoid that, we keep the heap in the kernel high-half by using the
    // KERNEL_BASE alias of low physical memory.
    if (heap_phys_start == 0) {
        uint32 boot_end = vmm_get_boot_alloc_end();
        if (boot_end != 0) {
            heap_phys_start = boot_end;
        } else {
            // Fallback (should only happen if malloc is used before vmm_init())
            uint32 end = (uint32)(uintptr)&__kernel_end;
            heap_phys_start = align_up_u32(end + 0x10000, 0x1000);
        }
    }
    if (!heap_start || heap_start == (uint8*)0) {
        uintptr heap_alias = (uintptr)KERNEL_BASE + (uintptr)heap_phys_start;
        heap_start = (uint8*)heap_alias;
    }
    
    // Try to detect available memory and adjust heap size accordingly
    uint32 available_ram = detect_available_memory();
    
    // Use the new generous heap sizing for zero-copy operations
    calculate_optimal_heap_size();

    // Clamp heap_size against the *actual* current free-frame budget.
    // detect_available_memory() reports total physical RAM, but does not account
    // for reserved ranges (BIOS low 1MB, kernel image, boot allocations, etc.).
    // The heap reserves frames up-front, so it must not consume all free frames.
    uint32 free_frames_before = vmm_get_free_frames();
    if (free_frames_before != 0) {
        uint32 free_bytes_before = free_frames_before * PAGE_SIZE;
        uint32 heap_cap = 0;

        if (free_bytes_before > HEAP_PHYS_RESERVE_BYTES) {
            heap_cap = free_bytes_before - HEAP_PHYS_RESERVE_BYTES;
        }

        // Keep at least a minimally useful heap, but never exceed the cap.
        // If the cap is too small, prefer a tiny heap over starving the system.
        if (heap_cap >= PAGE_SIZE) {
            if (heap_size > heap_cap) heap_size = heap_cap;
        } else {
            heap_size = PAGE_SIZE;
        }

        // Keep heap page-aligned for vmm_reserve_phys_range().
        heap_size &= PAGE_MASK;

        // Enforce the existing min/max bounds (min may be relaxed on extreme low RAM).
        if (heap_size > HEAP_SIZE_MAX) heap_size = HEAP_SIZE_MAX;
        if (heap_size < HEAP_SIZE_MIN) {
            // Only force the minimum if it still leaves a reasonable reserve.
            if (free_bytes_before > (HEAP_PHYS_RESERVE_BYTES + HEAP_SIZE_MIN)) {
                heap_size = HEAP_SIZE_MIN;
            }
        }
    }
    
    // Basic sanity check (physical)
    if (heap_phys_start < 0x100000) {
        return;
    }
    
    // Initialize heap with calculated size.
    // Reserve the physical region so paging/frame_alloc will never hand out
    // frames that overlap the heap.
    vmm_reserve_phys_range(heap_phys_start, heap_phys_start + heap_size);

    memset(heap_start, 0, heap_size);
    
    // Verify the memory was set properly
    uint8* test_ptr = heap_start;
    if (*test_ptr != 0) {
        return;
    }
    
    block_header_t* first = (block_header_t*)heap_start;
    first->size = heap_size;
    first->used = 0;
    first->next = NO_BLOCK;
    first->magic = MAGIC_NUMBER;
    first_block = 0;
    memory_initialized = 1;
    memory_errors = 0;
    stack_overflow_detected = 0;
    allocation_count = 0;
    free_count = 0;

    // Heap-only smoke test: avoid calling malloc() here because malloc() may
    // call ensure_memory_initialized(), which would recurse back into
    // init_memory_manager().
    void* test_alloc = heap_malloc(16);
    if (test_alloc) {
        heap_free(test_alloc);
    } else {
        memory_initialized = 0;
    }
}

// Lazy memory initialization - only initialize when first allocation is needed
static void ensure_memory_initialized() {
    if (memory_initialized) return;

    // If vmm_init() has not run yet, the fallback heap placement can overlap
    // boot-time early_alloc() page-table memory and corrupt allocator metadata.
    // Fail closed here; callers should retry after VMM initialization.
    if (vmm_get_boot_alloc_end() == 0) {
        return;
    }

    init_memory_manager();
}

static uint32 find_free_block(uint32 size) {
    uint32 current = first_block;
    uint32 best_fit = NO_BLOCK;
    uint32 best_size = 0xFFFFFFFF;
    int blocks_checked = 0;
    
    while (current != NO_BLOCK && blocks_checked < 100) { // Prevent infinite loops
        block_header_t* block = (block_header_t*)(heap_start + current);
        blocks_checked++;
        if ((blocks_checked & 0x7) == 0) {
            mem_ctx_account();
        }
        
        // Validate block integrity. If this fails, the heap metadata is unsafe
        // to traverse (we cannot safely read block->next), so fail fast.
        if (!validate_block(block, current)) {
            printf("%c[MEMORY] Heap traversal aborted (corruption near offset 0x%X)\n", 255, 0, 0, current);
            return NO_BLOCK;
        }
        
        if (!block->used) {
#if CONFIG_RUST_ALLOCATOR
            rust_heap_best_fit_result_t fit_res = rust_heap_best_fit_update(
                block->size,
                best_size,
                size);
            if (fit_res == RUST_HEAP_BEST_FIT_UPDATE) {
                best_size = block->size;
                best_fit = current;
            }
#else
            if (block->size >= size && block->size < best_size) {
                best_size = block->size;
                best_fit = current;
            }
#endif
        }
        current = block->next;
    }
    
    if (best_fit == NO_BLOCK) {
        printf("%c[MEMORY] No suitable free block found for size %d (checked %d blocks)\n", 255, 165, 0, size, blocks_checked);
    }
    
    return best_fit;
}

static void split_block(uint32 block_offset, uint32 needed_size) {
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
#if CONFIG_RUST_ALLOCATOR
    uint32 new_block_offset = 0;
    uint32 new_block_size = 0;
    rust_heap_math_result_t split_res = rust_heap_plan_split(
        block_offset,
        block->size,
        needed_size,
        BLOCK_HEADER_SIZE,
        MIN_BLOCK_SIZE,
        &new_block_offset,
        &new_block_size);
    if (split_res != RUST_HEAP_MATH_OK) {
        return; // Don't split if the remainder would be too small
    }
#else
    if (block->size < needed_size + BLOCK_HEADER_SIZE + MIN_BLOCK_SIZE) {
        return; // Don't split if the remainder would be too small
    }
    uint32 new_block_offset = block_offset + needed_size;
    uint32 new_block_size = block->size - needed_size;
#endif

    block_header_t* new_block = (block_header_t*)(heap_start + new_block_offset);
    new_block->size = new_block_size;
    new_block->used = 0;
    new_block->next = block->next;
    new_block->magic = MAGIC_NUMBER;
    
    block->size = needed_size;
    block->next = new_block_offset;
}

static void merge_free_blocks() {
    uint32 current = first_block;
    int blocks_checked = 0;
    while (current != NO_BLOCK) {
        block_header_t* block = (block_header_t*)(heap_start + current);
        blocks_checked++;
        if ((blocks_checked & 0x7) == 0) {
            mem_ctx_account();
        }
        
        // Validate block integrity
        if (!validate_block(block, current)) {
            return;
        }
        
        while (block->next != NO_BLOCK) {
            block_header_t* next_block = (block_header_t*)(heap_start + block->next);
            
            // Validate next block
            if (!validate_block(next_block, block->next)) {
                return;
            }
            
            uint32 merged_size = 0;
#if CONFIG_RUST_ALLOCATOR
            rust_heap_coalesce_result_t coalesce_res = rust_heap_plan_coalesce(
                block->used,
                next_block->used,
                block->size,
                next_block->size,
                &merged_size);

            if (coalesce_res == RUST_HEAP_COALESCE_MERGE) {
                block->size = merged_size;
                block->next = next_block->next;
            } else if (coalesce_res == RUST_HEAP_COALESCE_OVERFLOW) {
                printf("%c[MEMORY] Coalesce overflow at offset 0x%X\n", 255, 0, 0, current);
                memory_errors++;
                return;
            } else {
                break;
            }
#else
            if (!block->used && !next_block->used) {
                block->size += next_block->size;
                block->next = next_block->next;
            } else {
                break;
            }
#endif
        }
        current = block->next;
    }
}

static void* heap_malloc(size_t nbytes) {
    // Lazy initialization
    ensure_memory_initialized();
    if (!memory_initialized) {
        return NULL;
    }
    
    // Check for stack overflow
    check_stack_overflow();
    
    if (nbytes <= 0) {
        printf("%c[MEMORY] Invalid allocation request: %d bytes\n", 255, 0, 0, nbytes);
        return NULL;
    }
    
    // Increased limit for larger files like .rei images, assembler, and zero-copy operations
    uint32 max_allocation = 0;
#if CONFIG_RUST_ALLOCATOR
    rust_heap_math_result_t max_res = rust_heap_compute_max_allocation(
        heap_size,
        9,
        10,
        &max_allocation);
    if (max_res != RUST_HEAP_MATH_OK) {
        printf("%c[MEMORY] Failed to compute max allocation threshold\n", 255, 0, 0);
        memory_errors++;
        return NULL;
    }
#else
    max_allocation = heap_size * 9 / 10; // 90% of heap allowed for single allocation
#endif
    if (nbytes > max_allocation) {
        printf("%c[MEMORY] Request too large: %d bytes (heap: %d KB, max: %d bytes)\n", 255, 0, 0, nbytes, heap_size / 1024, max_allocation);
        return NULL;
    }

    if (nbytes > 0xFFFFFFFFu) {
        printf("%c[MEMORY] Allocation size overflow: %d\n", 255, 0, 0, nbytes);
        return NULL;
    }

    uint32 total_size = 0;
#if CONFIG_RUST_ALLOCATOR
    rust_heap_math_result_t total_res = rust_heap_compute_total_size(
        (uint32)nbytes,
        BLOCK_HEADER_SIZE,
        4,
        &total_size);
    if (total_res != RUST_HEAP_MATH_OK) {
        printf("%c[MEMORY] Allocation size computation failed for %d bytes\n", 255, 0, 0, nbytes);
        memory_errors++;
        return NULL;
    }
#else
    total_size = ((uint32)nbytes + BLOCK_HEADER_SIZE + 3u) & ~3u; // 4-byte alignment
#endif
    
    // Safety check: ensure heap_start is valid
    if (!heap_start || heap_start == (uint8*)0) {
        printf("%c[MEMORY] Critical: heap_start is corrupted (0x%X)\n", 255, 0, 0, (uint32)(uintptr)heap_start);
        return NULL;
    }
    
    uint32 block_offset = find_free_block(total_size);
    if (block_offset == NO_BLOCK) {
        printf("%c[MEMORY] Out of memory (requested %d bytes, heap: %d KB, allocations: %d)\n", 255, 0, 0, nbytes, heap_size / 1024, allocation_count);
        return NULL;
    }
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    block->used = 1;
    block->magic = MAGIC_NUMBER;
    split_block(block_offset, total_size);
    
    allocation_count++;
    
    // Return pointer to the data area (after header)
    return (void*)(heap_start + block_offset + BLOCK_HEADER_SIZE);
}

static void heap_free(void* ptr) {
    if (!ptr) return;
    
    // Lazy initialization check
    if (!memory_initialized) {
        printf("%c[MEMORY] Attempting to free before initialization\n", 255, 0, 0);
        return;
    }
    
    // Check for stack overflow
    check_stack_overflow();
    
    // Safety check: ensure heap_start is valid
    if (!heap_start || heap_start == (uint8*)0) {
        printf("%c[MEMORY] Critical: heap_start is corrupted (0x%X)\n", 255, 0, 0, (uint32)(uintptr)heap_start);
        memory_errors++;
        return;
    }

    uint32 block_offset = 0;
#if CONFIG_RUST_ALLOCATOR
    rust_heap_ptr_result_t ptr_res = rust_heap_compute_block_offset(
        (uintptr)ptr,
        (uintptr)heap_start,
        BLOCK_HEADER_SIZE,
        heap_size,
        &block_offset);
    if (ptr_res != RUST_HEAP_PTR_OK) {
        printf("%c[MEMORY] Invalid pointer: 0x%X (heap_start: 0x%X, offset: %d)\n", 255, 0, 0, (uint32)(uintptr)ptr, (uint32)(uintptr)heap_start, block_offset);
        memory_errors++;
        return;
    }
#else
    uint8* data_ptr = (uint8*)ptr;
    block_offset = (uint32)(data_ptr - heap_start - BLOCK_HEADER_SIZE);
    if (block_offset >= heap_size) {
        printf("%c[MEMORY] Invalid pointer: 0x%X (heap_start: 0x%X, offset: %d)\n", 255, 0, 0, (uint32)(uintptr)ptr, (uint32)(uintptr)heap_start, block_offset);
        memory_errors++;
        return;
    }
#endif
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    
    // Validate block integrity
    if (!validate_block(block, block_offset)) {
        return;
    }
    
    if (!block->used) {
        printf("%c[MEMORY] Double free detected: 0x%X\n", 255, 0, 0, (uint32)(uintptr)ptr);
        memory_errors++;
        return;
    }
    
    block->used = 0;
    merge_free_blocks();
    free_count++;
}

void* malloc(size_t nbytes) {
    if (!mem_ctx_allow()) {
        return NULL;
    }
    mem_ctx_account();

    // IMPORTANT: reserve and initialize the legacy heap before allowing the
    // slab allocator to allocate pages from the frame allocator.
    //
    // Without this, early malloc() calls can allocate slab pages from physical
    // frames that later become part of the heap region. When init_memory_manager()
    // runs, it reserves+memsets the heap physical range and writes heap headers
    // (MAGIC_NUMBER), which would silently overwrite slab metadata and cause
    // freelist/header corruption (observed as free_list=0xDEADBEEF).
    if (!memory_initialized && vmm_get_boot_alloc_end() == 0) {
        return NULL;
    }
    ensure_memory_initialized();

    if (nbytes == 0) return NULL;

    // Fast path: try slab allocator for small allocations.
    // Large allocations fall back to the existing heap allocator.
    void* p = slab_alloc(nbytes);
    if (p) return p;
    return heap_malloc(nbytes);
}

void free(void* ptr) {
    if (!mem_ctx_allow()) return;
    mem_ctx_account();
    if (!ptr) return;
    if (slab_free(ptr)) return;
    heap_free(ptr);
}

void* realloc(void* ptr, size_t new_size) {
    if (!mem_ctx_allow()) {
        return NULL;
    }
    mem_ctx_account();
    if (!ptr) return malloc(new_size);
    if (new_size <= 0) {
        free(ptr);
        return NULL;
    }
    
    // Lazy initialization (heap uses this; slab does not require it)
    ensure_memory_initialized();
    
    // Check for stack overflow
    check_stack_overflow();
    
    uint32 block_offset = 0;
#if CONFIG_RUST_ALLOCATOR
    rust_heap_ptr_result_t ptr_res = rust_heap_compute_block_offset(
        (uintptr)ptr,
        (uintptr)heap_start,
        BLOCK_HEADER_SIZE,
        heap_size,
        &block_offset);
    if (ptr_res != RUST_HEAP_PTR_OK) {
        printf("%c[MEMORY] Invalid pointer in realloc: 0x%X\n", 255, 0, 0, (uint32)(uintptr)ptr);
        memory_errors++;
        return NULL;
    }
#else
    uint8* data_ptr = (uint8*)ptr;
    block_offset = (uint32)(data_ptr - heap_start - BLOCK_HEADER_SIZE);
    if (block_offset >= heap_size) {
        printf("%c[MEMORY] Invalid pointer in realloc: 0x%X\n", 255, 0, 0, (uint32)(uintptr)ptr);
        memory_errors++;
        return NULL;
    }
#endif
    
    block_header_t* block = (block_header_t*)(heap_start + block_offset);
    
    // Validate block integrity
    if (!validate_block(block, block_offset)) {
        return NULL;
    }
    
    uint32 copy_size = 0;
    uint32 do_realloc = 0;
#if CONFIG_RUST_ALLOCATOR
    rust_heap_math_result_t realloc_res = rust_heap_realloc_copy_size(
        block->size,
        BLOCK_HEADER_SIZE,
        (uint32)new_size,
        &copy_size,
        &do_realloc);
    if (realloc_res != RUST_HEAP_MATH_OK) {
        printf("%c[MEMORY] Realloc size computation failed\n", 255, 0, 0);
        memory_errors++;
        return NULL;
    }
#else
    uint32 current_size = block->size - BLOCK_HEADER_SIZE;
    copy_size = (current_size < (uint32)new_size) ? current_size : (uint32)new_size;
    do_realloc = ((uint32)new_size > current_size) ? 1u : 0u;
#endif

    if (!do_realloc) {
        return ptr; // No need to reallocate
    }
    void* new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy((char*)new_ptr, (char*)ptr, copy_size);
    free(ptr);
    return new_ptr;
}

void* calloc(size_t count, size_t size) {
    if (!mem_ctx_allow()) return NULL;
    mem_ctx_account();
    // Lazy initialization
    ensure_memory_initialized();
    
    int total_size = count * size;
    void* ptr = malloc(total_size);
    if (ptr) {
        memset((uint8*)ptr, 0, total_size);
    }
    return ptr;
}

// Enhanced memory statistics with error reporting
void print_memory_stats() {
    if (!memory_initialized) {
        printf("%c[MEMORY] Memory manager not initialized\n", 255, 0, 0);
        return;
    }
    
    uint32 total_free = 0;
    uint32 total_used = 0;
    uint32 block_count = 0;
    uint32 corrupted_blocks = 0;
    uint32 current = first_block;
    
    while (current != NO_BLOCK) {
        block_header_t* block = (block_header_t*)(heap_start + current);
        
        if (!validate_block(block, current)) {
            corrupted_blocks++;
            // Heap is unsafe to traverse further.
            break;
        } else {
            if (block->used) {
                total_used += block->size;
            } else {
                total_free += block->size;
            }
        }
        block_count++;
        current = block->next;
    }
    
    printf("%cMemory Statistics:\n", 255, 255, 255);
    printf("%c  Total Heap: %d KB\n", 255, 255, 255, heap_size / 1024);
    {
        uint32 free_frames = vmm_get_free_frames();
        uint32 total_frames = vmm_get_total_frames();
        uint32 free_kb = free_frames * (PAGE_SIZE / 1024);
        printf("%c  Phys Frames: %u/%u free (%u KB free)\n",
               255, 255, 255, (unsigned)free_frames, (unsigned)total_frames, (unsigned)free_kb);
    }
    printf("%c  Used: %d bytes (%d%%)\n", 255, 255, 255, total_used, (total_used * 100) / heap_size);
    printf("%c  Free: %d bytes (%d%%)\n", 255, 255, 255, total_free, (total_free * 100) / heap_size);
    printf("%c  Blocks: %d\n", 255, 255, 255, block_count);
    printf("%c  Allocations: %d\n", 255, 255, 255, allocation_count);
    printf("%c  Frees: %d\n", 255, 255, 255, free_count);
    printf("%c  Memory Errors: %d\n", 255, 255, 255, memory_errors);
    printf("%c  Corrupted Blocks: %d\n", 255, 255, 255, corrupted_blocks);
    
    if (stack_overflow_detected) {
        printf("%c  STACK OVERFLOW DETECTED!\n", 255, 0, 0);
    }
    
    if (memory_errors > 0) {
        printf("%c  WARNING: Memory corruption detected!\n", 255, 165, 0);
    }
    
    if (allocation_count > free_count + 10) {
        printf("%c  WARNING: Possible heap leak (or long-lived allocations): %d outstanding\n",
               255, 165, 0, (int)(allocation_count - free_count));
        printf("%c  Note: this counter tracks heap_malloc/heap_free only (not slab).\n", 200, 200, 200);
    }
}

// Get memory error count for shell commands
int get_memory_error_count() {
    return memory_errors;
}

int get_stack_overflow_status() {
    return stack_overflow_detected;
}

// Debug function to show current stack pointer
uint32 get_current_stack_pointer() {
    volatile uint32 esp;
    __asm__ __volatile__("mov %%esp, %0" : "=r" (esp));
    return esp;
}

// Get current heap size for shell commands
uint32 get_heap_size() {
    return heap_size;
}

void putchar(char c) {
    drawText(c, 255, 255, 255);
}

// Compute total used bytes in heap by scanning blocks
uint32 get_heap_used(void) {
    if (!memory_initialized) return 0;
    uint32 total_used = 0;
    uint32 current = first_block;
    int guard = 0;
    while (current != NO_BLOCK && guard++ < 20000) {
        block_header_t* block = (block_header_t*)(heap_start + current);
        if ((guard & 0xFF) == 0) {
            mem_ctx_account();
        }
        if (!validate_block(block, current)) {
            break;
        }
        if (block->used) {
            total_used += block->size;
        }
        if (block->next == current || block->next >= heap_size) break; // safety
        current = block->next;
    }
    // Exclude header overhead to present closer to payload usage
    if (total_used >= BLOCK_HEADER_SIZE) {
        // Roughly subtract one header per average block (approx): keep as-is to avoid negative
    }
    return total_used;
}