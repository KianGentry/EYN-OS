#include <cpu/user_elf.h>

#include <misc/types.h>
#include <string.h>
#include <util.h>
#include <vga.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <cpu/gdt.h>
#include <cpu/segdom.h>
#include <tile_manager.h>
#include <terminals.h>
#include <isr.h>
#include <context.h>
#include <misc/sched.h>
#include <watchdog.h>

// Defined in src/boot/kernel.asm; this is the top of the kernel stack.
extern uint32 stack_space;

volatile uint16 g_user_segdom_cs = GDT_USER_CS;
volatile uint16 g_user_segdom_ds = GDT_USER_DS;
#if !defined(EYNOS_ARCH_AMD64)
static segdom_t g_user_segdom;
#endif

// PID bookkeeping for spawned tasks crossing non-local abort-to-kernel flow.
static volatile int g_user_task_pending_pid = 0;
static volatile int g_user_task_running_pid = 0;

static int user_elf_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static inline void* user_elf_kphys_alias_ptr(uint32 phys_addr) {
    return (void*)((uintptr)KERNEL_BASE + (uintptr)phys_addr);
}

// Minimal ELF32 structures for parsing 32-bit little-endian ELF files
typedef struct {
    unsigned char e_ident[16];
    uint16 e_type;
    uint16 e_machine;
    uint32 e_version;
    uint32 e_entry;
    uint32 e_phoff;
    uint32 e_shoff;
    uint32 e_flags;
    uint16 e_ehsize;
    uint16 e_phentsize;
    uint16 e_phnum;
    uint16 e_shentsize;
    uint16 e_shnum;
    uint16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32 p_type;
    uint32 p_offset;
    uint32 p_vaddr;
    uint32 p_paddr;
    uint32 p_filesz;
    uint32 p_memsz;
    uint32 p_flags;
    uint32 p_align;
} Elf32_Phdr;

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA  5

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EM_386 3

#define PT_LOAD 1

/*
 * ABI-INVARIANT: Maximum ELF file size the loader will read into kernel heap.
 *
 * Why: The loader reads the entire ELF into a malloc'd buffer before parsing.
 *      This bounds the kernel heap spike during program loading.
 *
 * Value: 16 MB accommodates chibicc-compiled binaries that store BSS as zeros
 *        in the data segment (e.g., chibicc-DOOM is ~8.5 MB).  With DOOM's
 *        128 MB QEMU config, the transient heap usage is well within budget.
 *
 * Breakage if decreased: programs whose on-disk size exceeds the limit will
 *   be rejected at load time ("ELF too large").
 * Breakage if increased past physical RAM: malloc will fail and the loader
 *   will report "out of memory" before doing any work.
 *
 * ABI-sensitive: No (internal loader policy, not exposed to user ABI).
 * Security-critical: Yes -- bounds worst-case kernel heap usage from untrusted
 *   ELF files loaded from disk.
 */
#define USER_ELF_MAX_FILE_BYTES (16u * 1024u * 1024u)

/*
 * ABI-INVARIANT: Initial user stack mapping size.
 *
 * Why: Bounds the physical frames we must allocate up-front before entering ring3.
 * Invariant: The VMM can grow the stack on-demand via #PF, but growth is limited
 *            to one page below the current stack_bottom to keep faults bounded.
 * Breakage if changed:
 *   - Increasing: can exhaust frames on low-RAM configs and prevent any user
 *     program from starting.
 *   - Decreasing: may cause programs that reserve a large stack frame early to
 *     fault below stack_bottom and segfault.
 */
#define USER_ELF_INITIAL_STACK_PAGES 8u  // 32KB

#if defined(EYNOS_ARCH_AMD64)
#define USER_ELF_AMD64_STACK_TOP 0x02000000u
#endif

static inline uint32 align_down(uint32 v, uint32 a) { return v & ~(a - 1); }
static inline uint32 align_up(uint32 v, uint32 a) { return (v + a - 1) & ~(a - 1); }

static inline void* user_elf_user_ptr(uint32 address) {
    return (void*)(uintptr)address;
}

static inline uint32 user_elf_ptr_to_u32(const void* pointer) {
    uintptr raw = (uintptr)pointer;
    uint32 narrowed = (uint32)raw;
    if ((uintptr)narrowed != raw) {
        return 0;
    }
    return narrowed;
}

// Bounds for argv copying to user stack. Keep small for low-memory configs.
#define USER_ELF_MAX_ARGC 32
#define USER_ELF_MAX_ARG_BYTES 2048

static uint32 user_stack_build_argv(uint32 user_stack_top,
                                   uint32 user_stack_floor,
                                   const char* prog_abspath,
                                   int argc, const char* const* argv) {
    // Build a SysV-like initial stack:
    //   argc
    //   argv[0..argc-1]
    //   NULL
    // Strings live below.
    // Returns new user ESP, or 0 on failure.

    const char* local_argv[USER_ELF_MAX_ARGC];
    int local_argc = 0;

    // Always provide argv[0].
    local_argv[local_argc++] = (prog_abspath && prog_abspath[0]) ? prog_abspath : "";

    if (argc > 0 && argv) {
        for (int i = 0; i < argc && local_argc < USER_ELF_MAX_ARGC; i++) {
            if (!argv[i]) continue;
            local_argv[local_argc++] = argv[i];
        }
    }

    uint32 sp = user_stack_top;
    uint32 argv_ptrs[USER_ELF_MAX_ARGC];
    uint32 total_bytes = 0;

    // Copy strings top-down.
    for (int i = local_argc - 1; i >= 0; i--) {
        const char* s = local_argv[i];
        uint32 len = 0;
        while (s[len]) len++;
        len += 1; // NUL

        total_bytes += len;
        if (total_bytes > USER_ELF_MAX_ARG_BYTES) return 0;

        sp -= len;
        if (sp < user_stack_floor) return 0;
        memcpy(user_elf_user_ptr(sp), s, len);
        argv_ptrs[i] = sp;
    }

    // Align for pointer pushes.
    sp = align_down(sp, 4);

    // Push argv NULL terminator.
    sp -= 4;
    if (sp < user_stack_floor) return 0;
    *(uint32*)user_elf_user_ptr(sp) = 0;

    // Push argv pointers.
    for (int i = local_argc - 1; i >= 0; i--) {
        sp -= 4;
        if (sp < user_stack_floor) return 0;
        *(uint32*)user_elf_user_ptr(sp) = argv_ptrs[i];
    }

    // Push argc.
    sp -= 4;
    if (sp < user_stack_floor) return 0;
    *(uint32*)user_elf_user_ptr(sp) = (uint32)local_argc;

    return sp;
}

int user_elf_run_argv(uint8 drive, const char* abspath, int argc, const char* const* argv) {
    if (!abspath || !abspath[0]) return -1;

    if (!user_elf_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;

    vfs_stat_t st;
    if (vfs_stat(drive, abspath, &st) != 0 || st.type != VFS_NODE_FILE || st.size <= 0) {
        printf("%cError: file not found: %s\n", 255, 0, 0, abspath);
        return -1;
    }

    // Keep it small for now; avoids big allocations in low-memory configs.
    if ((uint32)st.size > USER_ELF_MAX_FILE_BYTES) {
        printf("%cError: ELF too large (max %u KB for now).\n", 255, 0, 0, (unsigned)(USER_ELF_MAX_FILE_BYTES / 1024u));
        return -1;
    }

    if (!user_elf_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    uint8* file = (uint8*)malloc((size_t)st.size);
    if (!file) {
        printf("%cError: out of memory.\n", 255, 0, 0);
        return -1;
    }

    if (!user_elf_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) {
        free(file);
        return -1;
    }
    int n = vfs_read_file(drive, abspath, file, (int)st.size);
    if (n < 0) {
        printf("%cError: failed to read ELF.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    if ((uint32)n < sizeof(Elf32_Ehdr)) {
        printf("%cError: invalid ELF (too small).\n", 255, 0, 0);
        free(file);
        return -1;
    }

    Elf32_Ehdr* eh = (Elf32_Ehdr*)file;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 || eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        printf("%cError: not an ELF file.\n", 255, 0, 0);
        free(file);
        return -1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_386) {
        printf("%cError: unsupported ELF (need i386 ELF32 LSB).\n", 255, 0, 0);
        free(file);
        return -1;
    }

    if (eh->e_phoff == 0 || eh->e_phentsize < sizeof(Elf32_Phdr) || eh->e_phnum == 0) {
        printf("%cError: ELF missing program headers.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    uint32 ph_end = eh->e_phoff + (uint32)eh->e_phnum * (uint32)eh->e_phentsize;
    if (ph_end > (uint32)n) {
        printf("%cError: ELF program headers out of range.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    // Compute a single contiguous mapping range that covers all PT_LOAD segments.
    uint32 min_vaddr = 0xFFFFFFFFu;
    uint32 max_vaddr = 0;
    int load_count = 0;

    for (uint16 i = 0; i < eh->e_phnum; ++i) {
        Elf32_Phdr* ph = (Elf32_Phdr*)(file + eh->e_phoff + (uint32)i * (uint32)eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        uint32 end = ph->p_vaddr + ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
        load_count++;
    }

    if (load_count == 0 || min_vaddr == 0xFFFFFFFFu || max_vaddr <= min_vaddr) {
        printf("%cError: ELF has no loadable segments.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    // Restrict to user space range.
    uint32 map_start = align_down(min_vaddr, PAGE_SIZE);
    uint32 map_end = align_up(max_vaddr, PAGE_SIZE);

    if (map_start < USER_CODE_BASE || map_end >= USER_STACK_BASE) {
        printf("%cError: ELF vaddr range not supported.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    uint32 map_size = map_end - map_start;
    uint32 pages = map_size / PAGE_SIZE;

    /*
     * ABI-INVARIANT: Maximum PT_LOAD virtual span (in pages) for a user ELF.
     *
     * Why: Bounds the number of page-table entries and physical frames the
     *      loader may allocate up-front (file-backed pages) or register as
     *      demand-zero (BSS) before jumping to ring 3.
     *
     * Value: 16384 pages = 64 MB virtual span.  Demand-zero BSS pages are
     *        only backed by physical frames when written, so a large virtual
     *        span does not imply physical RAM usage.  64 MB accommodates
     *        programs with large static heap buffers (e.g., chibicc's 32 MB
     *        internal allocator arena) while remaining well below
     *        USER_STACK_BASE (0xB0000000) for ELFs loaded at 0x00400000.
     *
     * Breakage if decreased below a program's PT_LOAD span: that program
     *   will be rejected at load time with "ELF mapping size too large".
     * Breakage if increased beyond free virtual space: loader would attempt
     *   to register PTEs past USER_STACK_BASE; the range check above catches
     *   this before we reach here, so the guard is redundant but kept for
     *   defence-in-depth.
     *
     * ABI-sensitive: Yes -- this is the effective user-program BSS ceiling.
     * Security-critical: Yes -- prevents an untrusted ELF from exhausting the
     *   page-table allocation budget or mapping into kernel space.
     */
    if (pages == 0 || pages > 16384) {
        printf("%cError: ELF mapping size too large.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    // Clean up any previous user-task mappings first.
    user_task_cleanup_mappings();

    // Reset per-task syscall state.
    if (!syscall_get_user_fd_inherit_mode()) {
        syscall_reset_user_fds();
        syscall_reset_user_stdio_fds();
    }
    syscall_reset_user_streams();
    syscall_reset_user_guis();

    g_user_interrupt = 0;
    g_user_task_active = 1;
    g_user_task_term = tile_is_tiling_active() ? tile_get_focused() : -1;
    if (g_user_task_term < 0) g_user_task_term = 0;
    g_user_task_ui_dirty = 1;

    // Default user program output colour to white (programs can change it).
    extern volatile int g_user_task_colour_r;
    extern volatile int g_user_task_colour_g;
    extern volatile int g_user_task_colour_b;
    extern volatile uint8 g_user_task_colour_state;
    extern volatile uint8 g_user_task_icon_state;
    g_user_task_colour_r = 255;
    g_user_task_colour_g = 255;
    g_user_task_colour_b = 255;
    g_user_task_colour_state = 0;
    g_user_task_icon_state = 0;
    
    // Clear the stdin buffer for this terminal so the user task starts fresh
    vterm_stdin_clear(g_user_task_term);

    // Record mappings incrementally so any mid-loop OOM can be cleaned up.
    user_task_set_current_mapping_state(map_start, 0, 0);
    vmm_kernel_as.stack_bottom = USER_STACK_TOP - PAGE_SIZE;

    /* Low-RAM-friendly program image mapping:
     * - Pre-create PTEs for the full PT_LOAD range as demand-zero (not present).
     * - Allocate/map/initialize only the pages that contain file-backed bytes.
     * This avoids eagerly allocating large .bss regions (e.g. userland malloc's
     * 1MB static heap) while still providing correct zero-fill semantics.
     */
    for (uint32 pi = 0; pi < pages; ++pi) {
        uint32 va = map_start + pi * PAGE_SIZE;
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 1);
        if (!pte) {
            printf("%cError: failed to create PTEs for user image.\n", 255, 0, 0);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        /* Ensure PDE is user-accessible when we eventually fault/map. */
        pde_t* pde = &vmm_kernel_as.pd->entries[PDE_INDEX(va)];
        *pde |= PTE_USER;
        *pte = PTE_DEMAND | PTE_USER | PTE_RW;
    }
    user_task_set_current_mapping_state(map_start, pages, 0);

    // Map user stack (initial N pages; can grow further on page faults).
    const uint32 user_stack_pages = USER_ELF_INITIAL_STACK_PAGES;
#if defined(EYNOS_ARCH_AMD64)
    const uint32 user_stack_limit = USER_ELF_AMD64_STACK_TOP;
#else
    const uint32 user_stack_limit = USER_STACK_TOP;
#endif
    const uint32 user_stack_page = user_stack_limit - user_stack_pages * PAGE_SIZE;
    const uint32 user_stack_top = user_stack_limit - 0x10;

    // Enable VMM stack growth for the current address space.
    vmm_kernel_as.stack_bottom = user_stack_page;
    user_task_set_current_mapping_state(map_start, pages, user_stack_page);

    for (uint32 spi = 0; spi < user_stack_pages; ++spi) {
        uint32 va = user_stack_page + spi * PAGE_SIZE;
        uint32 frame = frame_alloc();
        if (frame == 0) {
            printf("%cError: out of physical frames (free=%u/%u).\n",
                   255, 0, 0, (unsigned)vmm_get_free_frames(), (unsigned)vmm_get_total_frames());
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }

        memset(user_elf_kphys_alias_ptr(frame), 0, PAGE_SIZE);

        if (vmm_map_page(&vmm_kernel_as, va, frame, PTE_PRESENT | PTE_USER | PTE_RW) != 0) {
            printf("%cError: failed to map user stack.\n", 255, 0, 0);
            frame_free(frame);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }

        /* Pin during argv/stack construction. */
        clock_remove_page(frame);
    }

    // Map/initialize only the file-backed portions of PT_LOAD segments.
    for (uint16 i = 0; i < eh->e_phnum; ++i) {
        Elf32_Phdr* ph = (Elf32_Phdr*)(file + eh->e_phoff + (uint32)i * (uint32)eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz) {
            printf("%cError: ELF segment filesz > memsz.\n", 255, 0, 0);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        if (ph->p_offset + ph->p_filesz > (uint32)n) {
            printf("%cError: ELF segment out of range.\n", 255, 0, 0);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }

        /* Allocate and map each page that contains file bytes. */
        uint32 seg_file_start = ph->p_vaddr;
        uint32 seg_file_end = ph->p_vaddr + ph->p_filesz;
        uint32 page_start = align_down(seg_file_start, PAGE_SIZE);
        uint32 page_end = align_up(seg_file_end, PAGE_SIZE);
        for (uint32 va = page_start; va < page_end; va += PAGE_SIZE) {
            uint32 frame = frame_alloc();
            if (frame == 0) {
                printf("%cError: out of physical frames (free=%u/%u).\n",
                       255, 0, 0, (unsigned)vmm_get_free_frames(), (unsigned)vmm_get_total_frames());
                user_task_cleanup_mappings();
                free(file);
                return -1;
            }

            /* Zero via kernel mapping so we don't touch user VAs unnecessarily. */
            memset(user_elf_kphys_alias_ptr(frame), 0, PAGE_SIZE);

            if (vmm_map_page(&vmm_kernel_as, va, frame, PTE_PRESENT | PTE_USER | PTE_RW) != 0) {
                printf("%cError: failed to map ELF segment page.\n", 255, 0, 0);
                frame_free(frame);
                user_task_cleanup_mappings();
                free(file);
                return -1;
            }

            /* Pin during load so eviction can't swap out the page before memcpy. */
            clock_remove_page(frame);
        }

        /* Copy file-backed bytes into the now-present mappings. */
        if (ph->p_filesz) {
            memcpy(user_elf_user_ptr(ph->p_vaddr), file + ph->p_offset, ph->p_filesz);
        }
    }

    /* Stack pages were zeroed via KERNEL_BASE mapping during allocation. */

    // Save entry before releasing the ELF buffer.
    uint32 entry = eh->e_entry;
    if (entry == 0 || entry < map_start || entry >= map_end) {
        printf("%cError: invalid ELF entrypoint: 0x%X\n", 255, 0, 0, (unsigned)entry);
        user_task_cleanup_mappings();
        free(file);
        return -1;
    }
    free(file);

    // Build initial user stack with argv.
    uint32 user_esp = user_stack_build_argv(user_stack_top, user_stack_page, abspath, argc, argv);
    if (user_esp == 0) {
        printf("%cError: argv too large.\n", 255, 0, 0);
        user_task_cleanup_mappings();
        return -1;
    }

    /* Done writing into user VAs from CPL0: re-add pages to the clock so ring3 can
     * fault/swap them normally.
     */
    for (uint32 pi = 0; pi < pages; ++pi) {
        uint32 va = map_start + pi * PAGE_SIZE;
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            clock_add_page(*pte & PTE_FRAME_MASK, pte, va, &vmm_kernel_as);
        }
    }
    for (uint32 spi = 0; spi < user_stack_pages; ++spi) {
        uint32 va = user_stack_page + spi * PAGE_SIZE;
        pte_t* pte = vmm_walk_page_tables(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            clock_add_page(*pte & PTE_FRAME_MASK, pte, va, &vmm_kernel_as);
        }
    }

#if defined(EYNOS_ARCH_AMD64)
    g_user_segdom_cs = GDT_USER_CS;
    g_user_segdom_ds = GDT_USER_DS;
#else
    uint32 seg_base = 0;
    uint32 seg_limit = USER_STACK_TOP;
    segdom_init(&g_user_segdom, seg_base, seg_limit);
    g_user_segdom_cs = g_user_segdom.user_cs;
    g_user_segdom_ds = g_user_segdom.user_ds;
    segdom_load(&g_user_segdom);
#endif

    // Commit pending spawn PID (if any) so SYSCALL_EXIT/abort can report
    // completion to waitpid slot tracking.
    g_user_task_running_pid = g_user_task_pending_pid;
    g_user_task_pending_pid = 0;

    // Enter ring3 at ELF entry.
    // printf("%c[elfrun] entering user mode: %s (entry=0x%X)\n", 0, 255, 0, abspath, (unsigned)entry);
    uint32 kernel_stack_u32 = user_elf_ptr_to_u32(&stack_space);
    if (kernel_stack_u32 == 0) {
        printf("%cError: kernel stack pointer exceeds 32-bit TSS ABI.\n", 255, 0, 0);
        user_task_cleanup_mappings();
        return -1;
    }
    tss_set_kernel_stack(kernel_stack_u32);
#if defined(EYNOS_ARCH_AMD64)
    enter_user_mode(entry, user_esp);
#else
    enter_user_mode_segdom(entry, user_esp, g_user_segdom_cs, g_user_segdom_ds);
#endif

    return 0;
}

int user_elf_run(uint8 drive, const char* abspath) {
    return user_elf_run_argv(drive, abspath, 0, NULL);
}

typedef struct {
    uint32 code_base;
    uint32 code_pages;
    uint32 stack_page;
} user_task_runtime_t;

#define USER_TASK_MAX_SPAWN_ARGC 16

typedef struct {
    uint8 drive;
    int argc;
    char* path;
    const char* argv[USER_TASK_MAX_SPAWN_ARGC];
    char* arg_storage[USER_TASK_MAX_SPAWN_ARGC];
} user_task_image_t;

typedef enum {
    USER_TASK_STATE_UNUSED = 0,
    USER_TASK_STATE_RUNNABLE = 1,
    USER_TASK_STATE_RUNNING = 2,
    USER_TASK_STATE_BLOCKED = 3,
    USER_TASK_STATE_ZOMBIE = 4,
} user_task_state_t;

typedef struct {
    int used;
    int pid;
    int status;
    int term_idx;
    user_task_state_t state;
    uint8 sched_class;
    uint8 _sched_pad0;
    uint16 base_tickets;
    uint16 donated_tickets;
    uint16 effective_tickets;
    int wait_target_pid;
    int donation_target_pid;
    uint16 donation_tickets_out;
    uint16 _sched_pad1;
    user_task_runtime_t runtime;
    user_task_image_t* image;
    int has_syscall_frame;
    regs_t last_syscall_frame;
} user_task_slot_t;

/*
 * ABI-INVARIANT: Maximum tracked spawned user tasks.
 *
 * Why: Bounds static kernel state for task lifecycle bookkeeping.
 * Invariant: PIDs are tracked in a fixed slot table; waitpid relies on this.
 * Breakage if changed:
 *   - Increasing grows .bss linearly.
 *   - Decreasing reduces number of concurrent tracked tasks.
 * ABI-sensitive: Yes (spawn failure behavior under load).
 * Security-critical: Yes (resource exhaustion boundary).
 */
#define USER_TASK_MAX 16

/*
 * ABI-INVARIANT: Stratified lottery scheduling classes for ring3 tasks.
 *
 * Why: Keeps scheduling policy simple while preserving broad urgency tiers.
 * Invariant: Class IDs are stable and must remain in [0, USER_TASK_SCHED_CLASS_COUNT).
 * ABI-sensitive: Yes (user-visible run-order behavior under contention).
 */
#define USER_TASK_SCHED_CLASS_INTERACTIVE 0u
#define USER_TASK_SCHED_CLASS_NORMAL      1u
#define USER_TASK_SCHED_CLASS_BACKGROUND  2u
#define USER_TASK_SCHED_CLASS_COUNT       3u

/*
 * SECURITY-INVARIANT: Per-task ticket bounds for lottery selection.
 *
 * Why: Prevents runaway ticket inflation from dominating selection and keeps
 * integer accounting bounded on low-RAM builds.
 * Invariant: Effective tickets are clamped to [USER_TASK_TICKETS_MIN, USER_TASK_TICKETS_MAX].
 * ABI-sensitive: Yes (priority behavior is ticket-proportional).
 */
#define USER_TASK_TICKETS_MIN      1u
#define USER_TASK_TICKETS_DEFAULT  32u
#define USER_TASK_TICKETS_MAX      1024u

/*
 * ABI-INVARIANT: Cross-class service floors for stratified lottery.
 *
 * Why: Prevents strict-class starvation while preserving strong preference for
 * higher classes.
 * Invariant: If lower classes are runnable continuously, class 0 yields at
 * least one slot every (USER_TASK_CLASS0_BURST_MAX + 1) selections, and class 1
 * yields to class 2 at least one slot every (USER_TASK_CLASS1_BURST_MAX + 1)
 * selections.
 * ABI-sensitive: Yes (user-visible latency bounds by class).
 */
#define USER_TASK_CLASS0_BURST_MAX 8u
#define USER_TASK_CLASS1_BURST_MAX 4u

static user_task_slot_t g_user_tasks[USER_TASK_MAX];
static int g_user_task_next_pid = 1;
static user_task_slot_t* g_user_task_active_slot = NULL;
static volatile int g_user_task_schedule_request = 0;
static int g_user_task_focus_term = -1;
static uint16 g_user_task_class0_burst_remaining = USER_TASK_CLASS0_BURST_MAX;
static uint16 g_user_task_class1_burst_remaining = USER_TASK_CLASS1_BURST_MAX;

static user_task_image_t* user_task_image_build(uint8 drive,
                                                const char* abspath,
                                                int argc,
                                                const char* const* argv) {
    if (!abspath || !abspath[0]) return NULL;
    if (argc < 0) return NULL;
    if (argc > 0 && !argv) return NULL;
    if (argc > USER_TASK_MAX_SPAWN_ARGC) argc = USER_TASK_MAX_SPAWN_ARGC;

    user_task_image_t* image = (user_task_image_t*)malloc(sizeof(user_task_image_t));
    if (!image) return NULL;
    memset(image, 0, sizeof(*image));
    image->drive = drive;
    image->argc = argc;

    size_t path_len = strlen(abspath) + 1;
    image->path = (char*)malloc(path_len);
    if (!image->path) {
        free(image);
        return NULL;
    }
    memcpy(image->path, abspath, path_len);

    for (int i = 0; i < argc; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        image->arg_storage[i] = (char*)malloc(len);
        if (!image->arg_storage[i]) {
            for (int j = 0; j < i; ++j) free(image->arg_storage[j]);
            free(image->path);
            free(image);
            return NULL;
        }
        memcpy(image->arg_storage[i], s, len);
        image->argv[i] = image->arg_storage[i];
    }

    return image;
}

static void user_task_image_free(user_task_image_t* image) {
    if (!image) return;
    for (int i = 0; i < image->argc && i < USER_TASK_MAX_SPAWN_ARGC; ++i) {
        if (image->arg_storage[i]) free(image->arg_storage[i]);
    }
    if (image->path) free(image->path);
    free(image);
}

static int user_task_launch_slot(user_task_slot_t* slot) {
    if (!slot || !slot->image) return -1;

    slot->state = USER_TASK_STATE_RUNNING;
    slot->wait_target_pid = 0;
    if (slot->term_idx >= 0) {
        g_user_task_term = slot->term_idx;
    }
    g_user_task_active_slot = slot;
    g_user_task_pending_pid = slot->pid;

    int rc = user_elf_run_argv(slot->image->drive,
                               slot->image->path,
                               slot->image->argc,
                               slot->image->argv);

    // Returning here means load failed before entering ring3.
    g_user_task_active_slot = NULL;
    if (g_user_task_pending_pid == slot->pid) g_user_task_pending_pid = 0;
    if (g_user_task_running_pid == slot->pid) g_user_task_running_pid = 0;
    slot->state = USER_TASK_STATE_ZOMBIE;
    slot->status = (rc == 0) ? 0 : -1;
    return rc;
}

void user_task_request_schedule(void) {
    g_user_task_schedule_request = 1;
}

void user_task_note_focus_term(int term_idx) {
    if (term_idx < 0) {
        g_user_task_focus_term = -1;
    } else {
        g_user_task_focus_term = term_idx;
    }

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used) continue;
        if (slot->state == USER_TASK_STATE_UNUSED || slot->state == USER_TASK_STATE_ZOMBIE) continue;
        if (g_user_task_focus_term >= 0 && slot->term_idx == g_user_task_focus_term) {
            slot->sched_class = (uint8)USER_TASK_SCHED_CLASS_INTERACTIVE;
        } else {
            slot->sched_class = (uint8)USER_TASK_SCHED_CLASS_NORMAL;
        }
    }
}

void user_task_on_timeslice_end(void) {
    if (!g_user_task_active) return;
    if ((int)g_user_task_running_pid <= 0) return;
    user_task_request_schedule();
}

int user_task_get_running_pid(void) {
    return (int)g_user_task_running_pid;
}

void user_task_get_current_mapping_state(uint32* base, uint32* pages, uint32* stack_page) {
    if (g_user_task_active_slot) {
        if (base) *base = g_user_task_active_slot->runtime.code_base;
        if (pages) *pages = g_user_task_active_slot->runtime.code_pages;
        if (stack_page) *stack_page = g_user_task_active_slot->runtime.stack_page;
        return;
    }
    if (base) *base = g_user_code_base;
    if (pages) *pages = g_user_code_pages;
    if (stack_page) *stack_page = g_user_stack_page;
}

void user_task_set_current_mapping_state(uint32 base, uint32 pages, uint32 stack_page) {
    if (g_user_task_active_slot) {
        g_user_task_active_slot->runtime.code_base = base;
        g_user_task_active_slot->runtime.code_pages = pages;
        g_user_task_active_slot->runtime.stack_page = stack_page;
    }
    g_user_code_base = base;
    g_user_code_pages = pages;
    g_user_stack_page = stack_page;
}

void user_task_clear_current_mapping_state(void) {
    user_task_set_current_mapping_state(0, 0, 0);
}

static user_task_slot_t* user_task_find_slot_by_pid(int pid) {
    if (pid <= 0) return NULL;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        if (g_user_tasks[i].used && g_user_tasks[i].pid == pid) return &g_user_tasks[i];
    }
    return NULL;
}

static user_task_slot_t* user_task_alloc_slot(void) {
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        if (!g_user_tasks[i].used) return &g_user_tasks[i];
    }
    return NULL;
}

static int user_task_runtime_matches_live(const user_task_runtime_t* rt) {
    if (!rt) return 0;
    return (rt->code_base == g_user_code_base &&
            rt->code_pages == g_user_code_pages &&
            rt->stack_page == g_user_stack_page) ? 1 : 0;
}

static uint32 user_task_sched_class_index(const user_task_slot_t* slot) {
    if (!slot) return USER_TASK_SCHED_CLASS_NORMAL;
    uint32 cls = (uint32)slot->sched_class;
    if (cls >= USER_TASK_SCHED_CLASS_COUNT) return USER_TASK_SCHED_CLASS_NORMAL;
    return cls;
}

static uint32 user_task_clamp_tickets(uint32 tickets) {
    if (tickets < USER_TASK_TICKETS_MIN) return USER_TASK_TICKETS_MIN;
    if (tickets > USER_TASK_TICKETS_MAX) return USER_TASK_TICKETS_MAX;
    return tickets;
}

static uint32 user_task_effective_tickets(user_task_slot_t* slot) {
    if (!slot) return USER_TASK_TICKETS_MIN;
    uint32 base = slot->base_tickets ? (uint32)slot->base_tickets : USER_TASK_TICKETS_DEFAULT;
    uint32 donated = (uint32)slot->donated_tickets;
    uint32 total = base + donated;
    uint32 clamped = user_task_clamp_tickets(total);
    slot->effective_tickets = (uint16)clamped;
    return clamped;
}

static void user_task_clear_donation(user_task_slot_t* donor) {
    if (!donor) return;

    int target_pid = donor->donation_target_pid;
    uint32 donated_out = (uint32)donor->donation_tickets_out;
    if (target_pid > 0 && donated_out > 0) {
        user_task_slot_t* target = user_task_find_slot_by_pid(target_pid);
        if (target && target->used) {
            uint32 current = (uint32)target->donated_tickets;
            target->donated_tickets = (uint16)((current > donated_out) ? (current - donated_out) : 0u);
            (void)user_task_effective_tickets(target);
        }
    }

    donor->donation_target_pid = 0;
    donor->donation_tickets_out = 0;
    (void)user_task_effective_tickets(donor);
}

static void user_task_clear_donations_target_pid(int target_pid) {
    if (target_pid <= 0) return;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* donor = &g_user_tasks[i];
        if (!donor->used) continue;
        if (donor->donation_target_pid != target_pid) continue;
        user_task_clear_donation(donor);
    }
}

static void user_task_donate_tickets(user_task_slot_t* donor,
                                     user_task_slot_t* target,
                                     uint32 tickets) {
    if (!donor) return;

    user_task_clear_donation(donor);

    if (!target || !target->used) return;
    if (target->state == USER_TASK_STATE_UNUSED) return;
    if (target->pid <= 0 || donor->pid <= 0) return;
    if (target->pid == donor->pid) return;
    if (tickets == 0) return;

    uint32 grant = user_task_clamp_tickets(tickets);
    uint32 room = USER_TASK_TICKETS_MAX - (uint32)target->donated_tickets;
    if (room == 0) return;
    if (grant > room) grant = room;

    target->donated_tickets = (uint16)((uint32)target->donated_tickets + grant);
    donor->donation_target_pid = target->pid;
    donor->donation_tickets_out = (uint16)grant;

    (void)user_task_effective_tickets(target);
    (void)user_task_effective_tickets(donor);
}

int user_task_donate_running_to_pid(int target_pid, uint16 tickets) {
    int running_pid = (int)g_user_task_running_pid;
    if (running_pid <= 0 || target_pid <= 0) return -1;

    user_task_slot_t* donor = user_task_find_slot_by_pid(running_pid);
    user_task_slot_t* target = user_task_find_slot_by_pid(target_pid);
    if (!donor || !target) return -1;
    if (donor == target) return -1;

    user_task_donate_tickets(donor, target, (uint32)tickets);
    return 0;
}

void user_task_clear_running_donation(void) {
    int running_pid = (int)g_user_task_running_pid;
    if (running_pid <= 0) return;
    user_task_slot_t* donor = user_task_find_slot_by_pid(running_pid);
    if (!donor) return;
    user_task_clear_donation(donor);
}

static int user_task_slot_eligible(user_task_slot_t* slot,
                                   int exclude_pid,
                                   int require_syscall_frame) {
    if (!slot || !slot->used) return 0;
    if (slot->state != USER_TASK_STATE_RUNNABLE) return 0;
    if (exclude_pid > 0 && slot->pid == exclude_pid) return 0;

    if (require_syscall_frame) {
        if (!slot->has_syscall_frame) return 0;
        if (!user_task_runtime_matches_live(&slot->runtime)) return 0;
    }

    return 1;
}

static int user_task_class_has_runnable(uint32 sched_class,
                                        int exclude_pid,
                                        int require_syscall_frame) {
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!user_task_slot_eligible(slot, exclude_pid, require_syscall_frame)) continue;
        if (user_task_sched_class_index(slot) != sched_class) continue;
        return 1;
    }
    return 0;
}

static int user_task_select_sched_class_with_floor(int exclude_pid,
                                                    int require_syscall_frame,
                                                    uint32* out_class) {
    int has0 = user_task_class_has_runnable(USER_TASK_SCHED_CLASS_INTERACTIVE,
                                            exclude_pid,
                                            require_syscall_frame);
    int has1 = user_task_class_has_runnable(USER_TASK_SCHED_CLASS_NORMAL,
                                            exclude_pid,
                                            require_syscall_frame);
    int has2 = user_task_class_has_runnable(USER_TASK_SCHED_CLASS_BACKGROUND,
                                            exclude_pid,
                                            require_syscall_frame);

    if (!has0) g_user_task_class0_burst_remaining = USER_TASK_CLASS0_BURST_MAX;
    if (!has1) g_user_task_class1_burst_remaining = USER_TASK_CLASS1_BURST_MAX;

    if (has0) {
        if (has1 || has2) {
            if (g_user_task_class0_burst_remaining == 0) {
                g_user_task_class0_burst_remaining = USER_TASK_CLASS0_BURST_MAX;
                if (has1) {
                    *out_class = USER_TASK_SCHED_CLASS_NORMAL;
                    return 1;
                }
                *out_class = USER_TASK_SCHED_CLASS_BACKGROUND;
                return 1;
            }
            g_user_task_class0_burst_remaining--;
        }

        *out_class = USER_TASK_SCHED_CLASS_INTERACTIVE;
        return 1;
    }

    if (has1) {
        if (has2) {
            if (g_user_task_class1_burst_remaining == 0) {
                g_user_task_class1_burst_remaining = USER_TASK_CLASS1_BURST_MAX;
                *out_class = USER_TASK_SCHED_CLASS_BACKGROUND;
                return 1;
            }
            g_user_task_class1_burst_remaining--;
        }

        *out_class = USER_TASK_SCHED_CLASS_NORMAL;
        return 1;
    }

    if (has2) {
        *out_class = USER_TASK_SCHED_CLASS_BACKGROUND;
        return 1;
    }

    return 0;
}

static user_task_slot_t* user_task_pick_runnable_slot(int exclude_pid,
                                                       int require_syscall_frame) {
    uint32 cls = USER_TASK_SCHED_CLASS_NORMAL;
    if (!user_task_select_sched_class_with_floor(exclude_pid, require_syscall_frame, &cls)) {
        return NULL;
    }

    uint32 total_tickets = 0;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!user_task_slot_eligible(slot, exclude_pid, require_syscall_frame)) continue;
        if (user_task_sched_class_index(slot) != cls) continue;
        total_tickets += user_task_effective_tickets(slot);
    }

    if (total_tickets == 0) return NULL;

    uint32 draw = sched_rng_bounded(total_tickets);
    uint32 acc = 0;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!user_task_slot_eligible(slot, exclude_pid, require_syscall_frame)) continue;
        if (user_task_sched_class_index(slot) != cls) continue;

        acc += user_task_effective_tickets(slot);
        if (draw < acc) {
            return slot;
        }
    }

    return NULL;
}

static int user_task_try_switch_to_saved_frame(regs_t* regs,
                                               user_task_slot_t* current,
                                               int current_pid) {
    if (!regs || !current) return 0;

    user_task_slot_t* target = user_task_pick_runnable_slot(current_pid, 1);
    if (!target) return 0;

    current->last_syscall_frame = *regs;
    current->has_syscall_frame = 1;
    current->state = USER_TASK_STATE_RUNNABLE;

    *regs = target->last_syscall_frame;
    target->state = USER_TASK_STATE_RUNNING;
    g_user_task_active_slot = target;
    g_user_task_running_pid = target->pid;
    if (target->term_idx >= 0) {
        g_user_task_term = target->term_idx;
    }
    g_user_task_schedule_request = 0;
    return 1;
}

int user_task_try_preempt_from_irq(regs_t* regs) {
    if (!regs) return 0;
    if (!g_user_task_schedule_request) return 0;
    if (!g_user_task_active) return 0;

    int current_pid = (int)g_user_task_running_pid;
    user_task_slot_t* current = user_task_find_slot_by_pid(current_pid);
    if (!current) return 0;

    return user_task_try_switch_to_saved_frame(regs, current, current_pid);
}

int user_task_try_resume_from_syscall(regs_t* regs) {
    if (!regs) return 0;
    if (!g_user_task_schedule_request) return 0;
    if (!g_user_task_active) return 0;

    int current_pid = (int)g_user_task_running_pid;
    user_task_slot_t* current = user_task_find_slot_by_pid(current_pid);
    if (!current) return 0;

    if (user_task_try_switch_to_saved_frame(regs, current, current_pid)) {
        return 1;
    }

    /*
     * SECURITY-INVARIANT: Do not launch a fresh ring3 image from an active
     * task's syscall return path.
     *
     * Why: user_elf_run_argv() tears down and rebuilds the singleton live
     * user mapping. Doing that while another task is still active corrupts the
     * current task's execution context and can deadlock scheduling.
     *
     * Consequence: syscall-path reschedule is frame-switch-only. Tasks without
     * a captured syscall frame remain queued and are launched when no ring3
     * task is active (abort/poll scheduler path).
     */
    return 0;
}

int user_task_spawn_argv(uint8 drive, const char* abspath, int argc, const char* const* argv) {
    user_task_slot_t* slot = user_task_alloc_slot();
    if (!slot) return -1;

    user_task_image_t* image = user_task_image_build(drive, abspath, argc, argv);
    if (!image) return -1;

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->pid = g_user_task_next_pid++;
    slot->image = image;
    slot->state = USER_TASK_STATE_RUNNABLE;
    slot->term_idx = -1;
    slot->sched_class = (uint8)USER_TASK_SCHED_CLASS_NORMAL;
    slot->base_tickets = (uint16)USER_TASK_TICKETS_DEFAULT;
    slot->donated_tickets = 0;
    slot->effective_tickets = (uint16)USER_TASK_TICKETS_DEFAULT;
    slot->wait_target_pid = 0;
    slot->donation_target_pid = 0;
    slot->donation_tickets_out = 0;
    if (g_user_task_term >= 0) {
        slot->term_idx = g_user_task_term;
    } else if (tile_is_tiling_active()) {
        slot->term_idx = tile_get_focused_term();
    }
    if (g_user_task_next_pid <= 0) g_user_task_next_pid = 1;

    /*
     * Scheduler handoff model:
     * - If no ring3 task is active, launch immediately.
     * - If a task is active, leave this slot queued; abort continuation will
     *   launch it when the current task exits.
     */
    if (!g_user_task_active) {
        if (user_task_launch_slot(slot) != 0) {
            return slot->pid;
        }
    } else {
        user_task_request_schedule();
    }

    return slot->pid;
}

int user_task_continue_or_schedule(void) {
    // Prefer runnable queued tasks over UI fallback.
    user_task_slot_t* slot = user_task_pick_runnable_slot(0, 0);
    if (!slot) return 0;
    (void)user_task_launch_slot(slot);
    return 1;
}

int user_task_poll_scheduler(void) {
    if (!g_user_task_schedule_request) return 0;
    if (g_user_task_active) return 0;

    int ran = user_task_continue_or_schedule();
    if (!ran) {
        g_user_task_schedule_request = 0;
    }
    return ran;
}

void user_task_capture_syscall_frame(const regs_t* regs) {
    if (!regs) return;
    int pid = (int)g_user_task_running_pid;
    if (pid <= 0) return;

    user_task_slot_t* slot = user_task_find_slot_by_pid(pid);
    if (!slot) return;

    slot->last_syscall_frame = *regs;
    slot->has_syscall_frame = 1;
    if (g_user_task_term >= 0) {
        slot->term_idx = g_user_task_term;
    }
}

void user_task_notify_exit(int status) {
    int pid = (int)g_user_task_running_pid;
    if (pid > 0) {
        user_task_slot_t* slot = user_task_find_slot_by_pid(pid);
        if (slot) {
            user_task_clear_donation(slot);
            user_task_clear_donations_target_pid(pid);
            slot->state = USER_TASK_STATE_ZOMBIE;
            slot->status = status;
        }
    }
    g_user_task_active_slot = NULL;
    g_user_task_running_pid = 0;
    g_user_task_pending_pid = 0;
    user_task_request_schedule();
}

int user_task_waitpid(int pid, int* out_status, int flags) {
    user_task_slot_t* slot = user_task_find_slot_by_pid(pid);
    if (!slot) return -1;

    int waiter_pid = (int)g_user_task_running_pid;
    user_task_slot_t* waiter = user_task_find_slot_by_pid(waiter_pid);
    if (waiter && waiter != slot) {
        uint32 donation = waiter->base_tickets ? (uint32)waiter->base_tickets : USER_TASK_TICKETS_DEFAULT;
        user_task_donate_tickets(waiter, slot, donation);
    }

    while (slot->state != USER_TASK_STATE_ZOMBIE) {
        // If any runnable user task exists, execute it now. This allows
        // parent tasks blocked in waitpid() to make progress on spawned
        // children even though only one ring3 task runs at a time.
        if (user_task_continue_or_schedule()) {
            slot = user_task_find_slot_by_pid(pid);
            if (!slot) {
                if (waiter) user_task_clear_donation(waiter);
                return -1;
            }
            continue;
        }

        if (flags & USER_TASK_WAIT_NOHANG) {
            if (waiter) user_task_clear_donation(waiter);
            return 0;
        }
        watchdog_kick("waitpid");
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");

        slot = user_task_find_slot_by_pid(pid);
        if (!slot) {
            if (waiter) user_task_clear_donation(waiter);
            return -1;
        }
    }

    if (waiter) user_task_clear_donation(waiter);

    if (out_status) *out_status = slot->status;
    user_task_clear_donation(slot);
    user_task_clear_donations_target_pid(slot->pid);
    user_task_image_free(slot->image);
    slot->image = NULL;
    slot->used = 0;
    slot->pid = 0;
    slot->state = USER_TASK_STATE_UNUSED;
    slot->term_idx = -1;
    slot->status = 0;
    slot->sched_class = (uint8)USER_TASK_SCHED_CLASS_NORMAL;
    slot->base_tickets = (uint16)USER_TASK_TICKETS_DEFAULT;
    slot->donated_tickets = 0;
    slot->effective_tickets = (uint16)USER_TASK_TICKETS_DEFAULT;
    slot->wait_target_pid = 0;
    slot->donation_target_pid = 0;
    slot->donation_tickets_out = 0;
    slot->has_syscall_frame = 0;
    user_task_request_schedule();
    return pid;
}
