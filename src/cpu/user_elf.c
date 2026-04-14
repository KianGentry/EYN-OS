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

typedef enum {
    USER_TASK_BLOCK_NONE = 0,
    USER_TASK_BLOCK_SLEEP = 1,
    USER_TASK_BLOCK_WAITPID = 2,
    USER_TASK_BLOCK_GUI_EVENT = 3,
} user_task_block_reason_t;

typedef struct {
    int used;
    int pid;
    int status;
    user_task_state_t state;
    int wait_target_pid;
    int wait_gui_handle;
    uint32 wake_tick;
    uint8 block_reason;
    uint8 mlfq_level;
    uint8 in_runq;
    uint16 _pad0;
    int runq_next;
    uint32 mlfq_slice_left;
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

static user_task_slot_t g_user_tasks[USER_TASK_MAX];
static int g_user_task_next_pid = 1;
static user_task_slot_t* g_user_task_active_slot = NULL;
static volatile int g_user_task_schedule_request = 0;

typedef struct {
    int head;
    int tail;
} user_task_runq_t;

static user_task_runq_t g_user_task_runq[SCHED_MLFQ_LEVELS];
static int g_user_task_runq_initialized = 0;
static uint32 g_user_task_last_boost_tick = 0;

static inline uint32 user_task_level_quantum_ticks(uint32 level) {
    uint32 q = sched_mlfq_level_quantum_ticks(level);
    if (q == 0) q = 1;
    return q;
}

static void user_task_runq_bootstrap(void) {
    if (!sched_mlfq_is_enabled()) return;
    if (g_user_task_runq_initialized) return;

    for (uint32 i = 0; i < SCHED_MLFQ_LEVELS; ++i) {
        g_user_task_runq[i].head = -1;
        g_user_task_runq[i].tail = -1;
    }

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        g_user_tasks[i].runq_next = -1;
        g_user_tasks[i].in_runq = 0;
        if (g_user_tasks[i].used && g_user_tasks[i].state == USER_TASK_STATE_RUNNABLE) {
            g_user_tasks[i].mlfq_level = SCHED_MLFQ_LEVEL_HIGH;
            g_user_tasks[i].mlfq_slice_left = user_task_level_quantum_ticks(SCHED_MLFQ_LEVEL_HIGH);
        }
    }

    g_user_task_last_boost_tick = sched_get_tick_count();
    g_user_task_runq_initialized = 1;
}

static void user_task_runq_enqueue(user_task_slot_t* slot) {
    if (!slot || !sched_mlfq_is_enabled()) return;
    if (!slot->used || slot->state != USER_TASK_STATE_RUNNABLE) return;
    user_task_runq_bootstrap();
    if (slot->in_runq) return;

    uint32 level = slot->mlfq_level;
    if (level >= SCHED_MLFQ_LEVELS) level = SCHED_MLFQ_LEVEL_HIGH;

    int idx = (int)(slot - g_user_tasks);
    if (idx < 0 || idx >= USER_TASK_MAX) return;

    user_task_runq_t* q = &g_user_task_runq[level];
    slot->runq_next = -1;
    if (q->tail >= 0) {
        g_user_tasks[q->tail].runq_next = idx;
        q->tail = idx;
    } else {
        q->head = idx;
        q->tail = idx;
    }
    slot->in_runq = 1;
}

static void user_task_runq_remove(user_task_slot_t* slot) {
    if (!slot || !sched_mlfq_is_enabled()) return;
    if (!slot->in_runq) return;
    user_task_runq_bootstrap();

    int idx = (int)(slot - g_user_tasks);
    if (idx < 0 || idx >= USER_TASK_MAX) return;

    for (uint32 level = 0; level < SCHED_MLFQ_LEVELS; ++level) {
        user_task_runq_t* q = &g_user_task_runq[level];
        int prev = -1;
        int cur = q->head;
        while (cur >= 0) {
            int next = g_user_tasks[cur].runq_next;
            if (cur == idx) {
                if (prev >= 0) {
                    g_user_tasks[prev].runq_next = next;
                } else {
                    q->head = next;
                }
                if (q->tail == cur) {
                    q->tail = prev;
                }
                slot->in_runq = 0;
                slot->runq_next = -1;
                return;
            }
            prev = cur;
            cur = next;
        }
    }

    slot->in_runq = 0;
    slot->runq_next = -1;
}

static user_task_slot_t* user_task_runq_pop_highest(void) {
    if (!sched_mlfq_is_enabled()) return NULL;
    user_task_runq_bootstrap();

    for (int level = (int)SCHED_MLFQ_LEVELS - 1; level >= 0; --level) {
        user_task_runq_t* q = &g_user_task_runq[(uint32)level];
        int idx = q->head;
        if (idx < 0) continue;

        user_task_slot_t* slot = &g_user_tasks[idx];
        q->head = slot->runq_next;
        if (q->head < 0) q->tail = -1;

        slot->runq_next = -1;
        slot->in_runq = 0;
        return slot;
    }
    return NULL;
}

static void user_task_runq_rebuild(void) {
    if (!sched_mlfq_is_enabled()) return;
    user_task_runq_bootstrap();

    for (uint32 i = 0; i < SCHED_MLFQ_LEVELS; ++i) {
        g_user_task_runq[i].head = -1;
        g_user_task_runq[i].tail = -1;
    }

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        g_user_tasks[i].runq_next = -1;
        g_user_tasks[i].in_runq = 0;
    }

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used || slot->state != USER_TASK_STATE_RUNNABLE) continue;
        user_task_runq_enqueue(slot);
    }
}

static void user_task_mlfq_boost_if_due(void) {
    if (!sched_mlfq_is_enabled()) return;
    user_task_runq_bootstrap();

    uint32 interval = sched_mlfq_boost_interval_ticks();
    if (interval == 0) return;

    uint32 now = sched_get_tick_count();
    if ((uint32)(now - g_user_task_last_boost_tick) < interval) return;

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used) continue;
        if (slot->state != USER_TASK_STATE_RUNNABLE && slot->state != USER_TASK_STATE_RUNNING) continue;
        slot->mlfq_level = SCHED_MLFQ_LEVEL_HIGH;
        slot->mlfq_slice_left = user_task_level_quantum_ticks(SCHED_MLFQ_LEVEL_HIGH);
    }

    user_task_runq_rebuild();
    g_user_task_last_boost_tick = now;
}

static void user_task_mlfq_account_preempt(user_task_slot_t* slot) {
    if (!slot || !sched_mlfq_is_enabled()) return;
    if (slot->mlfq_slice_left > 0) {
        slot->mlfq_slice_left--;
    }
    if (slot->mlfq_slice_left == 0) {
        if (slot->mlfq_level > SCHED_MLFQ_LEVEL_LOW) {
            slot->mlfq_level--;
        }
        slot->mlfq_slice_left = user_task_level_quantum_ticks(slot->mlfq_level);
    }
}

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

    if (sched_mlfq_is_enabled()) {
        user_task_runq_remove(slot);
        if (slot->mlfq_slice_left == 0) {
            slot->mlfq_slice_left = user_task_level_quantum_ticks(slot->mlfq_level);
        }
    }

    slot->state = USER_TASK_STATE_RUNNING;
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = -1;
    slot->wake_tick = 0;
    slot->block_reason = USER_TASK_BLOCK_NONE;
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

static user_task_slot_t* user_task_current_slot(void) {
    int pid = (int)g_user_task_running_pid;
    if (pid <= 0) return NULL;
    return user_task_find_slot_by_pid(pid);
}

void user_task_scheduler_tick(void) {
    uint32 now = sched_get_tick_count();
    int current_pid = (int)g_user_task_running_pid;

    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used || slot->state != USER_TASK_STATE_BLOCKED) continue;
        if (slot->block_reason != USER_TASK_BLOCK_SLEEP) continue;
        if ((int32)(now - slot->wake_tick) < 0) continue;

        slot->wake_tick = 0;
        slot->block_reason = USER_TASK_BLOCK_NONE;
        if (slot->pid == current_pid) {
            slot->state = USER_TASK_STATE_RUNNING;
        } else {
            slot->state = USER_TASK_STATE_RUNNABLE;
            user_task_runq_enqueue(slot);
            user_task_request_schedule();
        }
    }
}

void user_task_block_current_sleep_until(uint32 wake_tick) {
    user_task_slot_t* slot = user_task_current_slot();
    if (!slot) return;
    slot->state = USER_TASK_STATE_BLOCKED;
    slot->wake_tick = wake_tick;
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = -1;
    slot->block_reason = USER_TASK_BLOCK_SLEEP;
    user_task_runq_remove(slot);
    user_task_request_schedule();
}

void user_task_block_current_waitpid(int target_pid) {
    user_task_slot_t* slot = user_task_current_slot();
    if (!slot) return;
    slot->state = USER_TASK_STATE_BLOCKED;
    slot->wake_tick = 0;
    slot->wait_target_pid = target_pid;
    slot->wait_gui_handle = -1;
    slot->block_reason = USER_TASK_BLOCK_WAITPID;
    user_task_runq_remove(slot);
    user_task_request_schedule();
}

void user_task_block_current_gui_wait(int gui_handle) {
    user_task_slot_t* slot = user_task_current_slot();
    if (!slot) return;
    slot->state = USER_TASK_STATE_BLOCKED;
    slot->wake_tick = 0;
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = gui_handle;
    slot->block_reason = USER_TASK_BLOCK_GUI_EVENT;
    user_task_runq_remove(slot);
    user_task_request_schedule();
}

void user_task_unblock_current(void) {
    user_task_slot_t* slot = user_task_current_slot();
    if (!slot) return;
    if (slot->state == USER_TASK_STATE_BLOCKED) {
        slot->state = USER_TASK_STATE_RUNNING;
    }
    slot->wake_tick = 0;
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = -1;
    slot->block_reason = USER_TASK_BLOCK_NONE;
}

int user_task_current_is_blocked(void) {
    user_task_slot_t* slot = user_task_current_slot();
    if (!slot) return 0;
    return slot->state == USER_TASK_STATE_BLOCKED ? 1 : 0;
}

void user_task_wake_waiters_for_pid(int pid) {
    int current_pid = (int)g_user_task_running_pid;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used || slot->state != USER_TASK_STATE_BLOCKED) continue;
        if (slot->block_reason != USER_TASK_BLOCK_WAITPID) continue;
        if (slot->wait_target_pid != pid) continue;

        slot->wait_target_pid = 0;
        slot->wait_gui_handle = -1;
        slot->wake_tick = 0;
        slot->block_reason = USER_TASK_BLOCK_NONE;
        if (slot->pid == current_pid) {
            slot->state = USER_TASK_STATE_RUNNING;
        } else {
            slot->state = USER_TASK_STATE_RUNNABLE;
            user_task_runq_enqueue(slot);
        }
        user_task_request_schedule();
    }
}

void user_task_wake_gui_waiters(int gui_handle) {
    int current_pid = (int)g_user_task_running_pid;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used || slot->state != USER_TASK_STATE_BLOCKED) continue;
        if (slot->block_reason != USER_TASK_BLOCK_GUI_EVENT) continue;
        if (slot->wait_gui_handle != gui_handle) continue;

        slot->wait_target_pid = 0;
        slot->wait_gui_handle = -1;
        slot->wake_tick = 0;
        slot->block_reason = USER_TASK_BLOCK_NONE;
        if (slot->pid == current_pid) {
            slot->state = USER_TASK_STATE_RUNNING;
        } else {
            slot->state = USER_TASK_STATE_RUNNABLE;
            user_task_runq_enqueue(slot);
        }
        user_task_request_schedule();
    }
}

static int user_task_runtime_matches_live(const user_task_runtime_t* rt);

#if defined(EYNOS_ARCH_AMD64)
typedef struct user_task_irq_frame64_t {
    uint64 rax;
    uint64 rcx;
    uint64 rdx;
    uint64 rbx;
    uint64 rbp;
    uint64 rsi;
    uint64 rdi;
    uint64 r8;
    uint64 r9;
    uint64 r10;
    uint64 r11;
    uint64 r12;
    uint64 r13;
    uint64 r14;
    uint64 r15;
    uint64 rip;
    uint64 cs;
    uint64 rflags;
    uint64 user_rsp;
    uint64 user_ss;
} user_task_irq_frame64_t;

static void user_task_regs_from_irq_frame(const user_task_irq_frame64_t* irqf, regs_t* out) {
    if (!irqf || !out) return;
    memset(out, 0, sizeof(*out));
    out->edi = (uint32)irqf->rdi;
    out->esi = (uint32)irqf->rsi;
    out->ebp = (uint32)irqf->rbp;
    out->esp = (uint32)irqf->user_rsp;
    out->ebx = (uint32)irqf->rbx;
    out->edx = (uint32)irqf->rdx;
    out->ecx = (uint32)irqf->rcx;
    out->eax = (uint32)irqf->rax;
    out->eip = (uint32)irqf->rip;
    out->cs = (uint32)irqf->cs;
    out->eflags = (uint32)irqf->rflags;
    out->useresp = (uint32)irqf->user_rsp;
    out->ss = (uint32)irqf->user_ss;
}

static void user_task_regs_to_irq_frame(user_task_irq_frame64_t* irqf, const regs_t* regs) {
    if (!irqf || !regs) return;
    irqf->rdi = (uint64)(uint32)regs->edi;
    irqf->rsi = (uint64)(uint32)regs->esi;
    irqf->rbp = (uint64)(uint32)regs->ebp;
    irqf->rbx = (uint64)(uint32)regs->ebx;
    irqf->rdx = (uint64)(uint32)regs->edx;
    irqf->rcx = (uint64)(uint32)regs->ecx;
    irqf->rax = (uint64)(uint32)regs->eax;
    irqf->rip = (uint64)(uint32)regs->eip;
    irqf->cs = (uint64)(uint32)regs->cs;
    irqf->rflags = (uint64)(uint32)regs->eflags;
    irqf->user_rsp = (uint64)(uint32)regs->useresp;
    irqf->user_ss = (uint64)(uint32)regs->ss;
}
#else
typedef struct user_task_irq_frame32_t {
    uint32 edi;
    uint32 esi;
    uint32 ebp;
    uint32 esp;
    uint32 ebx;
    uint32 edx;
    uint32 ecx;
    uint32 eax;
    uint32 eip;
    uint32 cs;
    uint32 eflags;
    uint32 useresp;
    uint32 ss;
} user_task_irq_frame32_t;

static void user_task_regs_from_irq_frame(const user_task_irq_frame32_t* irqf, regs_t* out) {
    if (!irqf || !out) return;
    memset(out, 0, sizeof(*out));
    out->edi = irqf->edi;
    out->esi = irqf->esi;
    out->ebp = irqf->ebp;
    out->esp = irqf->esp;
    out->ebx = irqf->ebx;
    out->edx = irqf->edx;
    out->ecx = irqf->ecx;
    out->eax = irqf->eax;
    out->eip = irqf->eip;
    out->cs = irqf->cs;
    out->eflags = irqf->eflags;
    out->useresp = irqf->useresp;
    out->ss = irqf->ss;
}

static void user_task_regs_to_irq_frame(user_task_irq_frame32_t* irqf, const regs_t* regs) {
    if (!irqf || !regs) return;
    irqf->edi = regs->edi;
    irqf->esi = regs->esi;
    irqf->ebp = regs->ebp;
    irqf->esp = regs->esp;
    irqf->ebx = regs->ebx;
    irqf->edx = regs->edx;
    irqf->ecx = regs->ecx;
    irqf->eax = regs->eax;
    irqf->eip = regs->eip;
    irqf->cs = regs->cs;
    irqf->eflags = regs->eflags;
    irqf->useresp = regs->useresp;
    irqf->ss = regs->ss;
}
#endif

int user_task_try_preempt_from_irq(void* frame) {
    if (!sched_mlfq_irq_preempt_enabled()) return 0;
    if (!g_user_task_active) return 0;

    if (sched_mlfq_is_enabled()) {
        user_task_runq_bootstrap();
        user_task_mlfq_boost_if_due();
    }

    int current_pid = (int)g_user_task_running_pid;
    user_task_slot_t* current = user_task_find_slot_by_pid(current_pid);
    if (!current) return 0;

    if (sched_mlfq_is_enabled()) {
        uint32 slice_before = current->mlfq_slice_left;
        user_task_mlfq_account_preempt(current);
        if (slice_before == 1) {
            user_task_request_schedule();
        }
    }

    if (!g_user_task_schedule_request) return 0;

#if defined(EYNOS_ARCH_AMD64)
    if (!frame) return 0;
    if ((((const user_task_irq_frame64_t*)frame)->cs & 3u) != 3u) return 0;

    user_task_slot_t* target = NULL;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used) continue;
        if (slot->pid == current_pid) continue;
        if (slot->state != USER_TASK_STATE_RUNNABLE) continue;
        if (!slot->has_syscall_frame) continue;
        if (!user_task_runtime_matches_live(&slot->runtime)) continue;
        if (!target) {
            target = slot;
            continue;
        }
        if (sched_mlfq_is_enabled() && slot->mlfq_level > target->mlfq_level) {
            target = slot;
        }
    }

    if (!target) return 0;

    regs_t live_regs;
    user_task_regs_from_irq_frame((const user_task_irq_frame64_t*)frame, &live_regs);
    current->last_syscall_frame = live_regs;
    current->has_syscall_frame = 1;

    if (sched_mlfq_is_enabled()) {
        current->state = USER_TASK_STATE_RUNNABLE;
        user_task_runq_enqueue(current);
        user_task_runq_remove(target);
        if (target->mlfq_slice_left == 0) {
            target->mlfq_slice_left = user_task_level_quantum_ticks(target->mlfq_level);
        }
    } else {
        current->state = USER_TASK_STATE_RUNNABLE;
    }

    user_task_regs_to_irq_frame((user_task_irq_frame64_t*)frame, &target->last_syscall_frame);
    target->state = USER_TASK_STATE_RUNNING;
    g_user_task_active_slot = target;
    g_user_task_running_pid = target->pid;
    g_user_task_schedule_request = 0;
    return 1;
#else
    if (!frame) return 0;
    if ((((const user_task_irq_frame32_t*)frame)->cs & 3u) != 3u) return 0;

    user_task_slot_t* target = NULL;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used) continue;
        if (slot->pid == current_pid) continue;
        if (slot->state != USER_TASK_STATE_RUNNABLE) continue;
        if (!slot->has_syscall_frame) continue;
        if (!user_task_runtime_matches_live(&slot->runtime)) continue;
        if (!target) {
            target = slot;
            continue;
        }
        if (sched_mlfq_is_enabled() && slot->mlfq_level > target->mlfq_level) {
            target = slot;
        }
    }

    if (!target) return 0;

    regs_t live_regs;
    user_task_regs_from_irq_frame((const user_task_irq_frame32_t*)frame, &live_regs);
    current->last_syscall_frame = live_regs;
    current->has_syscall_frame = 1;

    if (sched_mlfq_is_enabled()) {
        current->state = USER_TASK_STATE_RUNNABLE;
        user_task_runq_enqueue(current);
        user_task_runq_remove(target);
        if (target->mlfq_slice_left == 0) {
            target->mlfq_slice_left = user_task_level_quantum_ticks(target->mlfq_level);
        }
    } else {
        current->state = USER_TASK_STATE_RUNNABLE;
    }

    user_task_regs_to_irq_frame((user_task_irq_frame32_t*)frame, &target->last_syscall_frame);
    target->state = USER_TASK_STATE_RUNNING;
    g_user_task_active_slot = target;
    g_user_task_running_pid = target->pid;
    g_user_task_schedule_request = 0;
    return 1;
#endif
}

static int user_task_runtime_matches_live(const user_task_runtime_t* rt) {
    if (!rt) return 0;
    return (rt->code_base == g_user_code_base &&
            rt->code_pages == g_user_code_pages &&
            rt->stack_page == g_user_stack_page) ? 1 : 0;
}

int user_task_try_resume_from_syscall(regs_t* regs) {
    if (!regs) return 0;
    if (!g_user_task_schedule_request) return 0;
    if (!g_user_task_active) return 0;

    if (sched_mlfq_is_enabled()) {
        user_task_runq_bootstrap();
        user_task_mlfq_boost_if_due();
    }

    int current_pid = (int)g_user_task_running_pid;
    user_task_slot_t* current = user_task_find_slot_by_pid(current_pid);
    if (!current) return 0;

    user_task_slot_t* target = NULL;
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used) continue;
        if (slot->pid == current_pid) continue;
        if (slot->state != USER_TASK_STATE_RUNNABLE) continue;
        if (!slot->has_syscall_frame) continue;
        if (!user_task_runtime_matches_live(&slot->runtime)) continue;
        if (!target) {
            target = slot;
            continue;
        }
        if (sched_mlfq_is_enabled() && slot->mlfq_level > target->mlfq_level) {
            target = slot;
        }
    }

    if (!target) return 0;

    current->last_syscall_frame = *regs;
    current->has_syscall_frame = 1;
    if (sched_mlfq_is_enabled()) {
        user_task_mlfq_account_preempt(current);
        current->state = USER_TASK_STATE_RUNNABLE;
        user_task_runq_enqueue(current);
        user_task_runq_remove(target);
        if (target->mlfq_slice_left == 0) {
            target->mlfq_slice_left = user_task_level_quantum_ticks(target->mlfq_level);
        }
    } else {
        current->state = USER_TASK_STATE_RUNNABLE;
    }

    *regs = target->last_syscall_frame;
    target->state = USER_TASK_STATE_RUNNING;
    g_user_task_active_slot = target;
    g_user_task_running_pid = target->pid;
    g_user_task_schedule_request = 0;
    return 1;
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
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = -1;
    slot->wake_tick = 0;
    slot->block_reason = USER_TASK_BLOCK_NONE;
    slot->mlfq_level = SCHED_MLFQ_LEVEL_HIGH;
    slot->mlfq_slice_left = user_task_level_quantum_ticks(SCHED_MLFQ_LEVEL_HIGH);
    slot->in_runq = 0;
    slot->runq_next = -1;
    if (g_user_task_next_pid <= 0) g_user_task_next_pid = 1;

    if (sched_mlfq_is_enabled()) {
        user_task_runq_bootstrap();
        user_task_runq_enqueue(slot);
    }

    /*
     * Scheduler handoff model:
     * - If no ring3 task is active, launch immediately.
     * - If a task is active, leave this slot queued; abort continuation will
     *   launch it when the current task exits.
     */
    if (!g_user_task_active) {
        if (sched_mlfq_is_enabled()) {
            (void)user_task_continue_or_schedule();
        } else {
            if (user_task_launch_slot(slot) != 0) {
                return slot->pid;
            }
        }
    } else {
        user_task_request_schedule();
    }

    return slot->pid;
}

int user_task_continue_or_schedule(void) {
    if (sched_mlfq_is_enabled()) {
        user_task_runq_bootstrap();
        user_task_mlfq_boost_if_due();

        for (;;) {
            user_task_slot_t* slot = user_task_runq_pop_highest();
            if (!slot) break;
            if (!slot->used || slot->state != USER_TASK_STATE_RUNNABLE) continue;
            if (slot->mlfq_slice_left == 0) {
                slot->mlfq_slice_left = user_task_level_quantum_ticks(slot->mlfq_level);
            }
            (void)user_task_launch_slot(slot);
            return 1;
        }
        return 0;
    }

    // Prefer runnable queued tasks over UI fallback.
    for (int i = 0; i < USER_TASK_MAX; ++i) {
        user_task_slot_t* slot = &g_user_tasks[i];
        if (!slot->used || slot->state != USER_TASK_STATE_RUNNABLE) continue;
        (void)user_task_launch_slot(slot);
        return 1;
    }
    return 0;
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
}

void user_task_notify_exit(int status) {
    int pid = (int)g_user_task_running_pid;
    if (pid > 0) {
        user_task_slot_t* slot = user_task_find_slot_by_pid(pid);
        if (slot) {
            user_task_runq_remove(slot);
            slot->state = USER_TASK_STATE_ZOMBIE;
            slot->status = status;
        }
        user_task_wake_waiters_for_pid(pid);
    }
    g_user_task_active_slot = NULL;
    g_user_task_running_pid = 0;
    g_user_task_pending_pid = 0;
    user_task_request_schedule();
}

int user_task_waitpid(int pid, int* out_status, int flags) {
    user_task_slot_t* slot = user_task_find_slot_by_pid(pid);
    if (!slot) return -1;

    if (slot->state != USER_TASK_STATE_ZOMBIE && !(flags & USER_TASK_WAIT_NOHANG)) {
        user_task_block_current_waitpid(pid);
    }

    while (slot->state != USER_TASK_STATE_ZOMBIE) {
        // If any runnable user task exists, execute it now. This allows
        // parent tasks blocked in waitpid() to make progress on spawned
        // children even though only one ring3 task runs at a time.
        if (user_task_continue_or_schedule()) {
            slot = user_task_find_slot_by_pid(pid);
            if (!slot) return -1;
            continue;
        }

        if (flags & USER_TASK_WAIT_NOHANG) {
            user_task_unblock_current();
            return 0;
        }
        watchdog_kick("waitpid");
        __asm__ __volatile__("sti");
        __asm__ __volatile__("hlt");

        slot = user_task_find_slot_by_pid(pid);
        if (!slot) return -1;
    }

    user_task_unblock_current();

    if (out_status) *out_status = slot->status;
    user_task_runq_remove(slot);
    user_task_image_free(slot->image);
    slot->image = NULL;
    slot->used = 0;
    slot->pid = 0;
    slot->state = USER_TASK_STATE_UNUSED;
    slot->status = 0;
    slot->wait_target_pid = 0;
    slot->wait_gui_handle = -1;
    slot->wake_tick = 0;
    slot->block_reason = USER_TASK_BLOCK_NONE;
    slot->mlfq_level = SCHED_MLFQ_LEVEL_HIGH;
    slot->mlfq_slice_left = user_task_level_quantum_ticks(SCHED_MLFQ_LEVEL_HIGH);
    slot->in_runq = 0;
    slot->runq_next = -1;
    slot->has_syscall_frame = 0;
    user_task_request_schedule();
    return pid;
}
