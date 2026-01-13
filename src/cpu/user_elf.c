#include <cpu/user_elf.h>

#include <types.h>
#include <string.h>
#include <util.h>
#include <vga.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <cpu/gdt.h>
#include <tile_manager.h>
#include <terminals.h>
#include <isr.h>

// Defined in src/boot/kernel.asm; this is the top of the kernel stack.
extern uint32 stack_space;

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

// Keep the loader simple and memory-bounded: we currently read the full ELF into RAM.
// This cap is intentionally conservative for low-memory QEMU configs.
#define USER_ELF_MAX_FILE_BYTES (2u * 1024u * 1024u)

// Initial user stack mapping. The VMM can still grow the stack on-demand via #PF.
#define USER_ELF_INITIAL_STACK_PAGES 32u  // 128KB

static inline uint32 align_down(uint32 v, uint32 a) { return v & ~(a - 1); }
static inline uint32 align_up(uint32 v, uint32 a) { return (v + a - 1) & ~(a - 1); }

// Bounds for argv copying to user stack. Keep small for low-memory configs.
#define USER_ELF_MAX_ARGC 32
#define USER_ELF_MAX_ARG_BYTES 2048

static uint32 user_stack_build_argv(uint32 user_stack_top, const char* prog_abspath,
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
        if (sp < USER_STACK_BASE) return 0;
        memcpy((void*)sp, s, len);
        argv_ptrs[i] = sp;
    }

    // Align for pointer pushes.
    sp = align_down(sp, 4);

    // Push argv NULL terminator.
    sp -= 4;
    if (sp < USER_STACK_BASE) return 0;
    *(uint32*)sp = 0;

    // Push argv pointers.
    for (int i = local_argc - 1; i >= 0; i--) {
        sp -= 4;
        if (sp < USER_STACK_BASE) return 0;
        *(uint32*)sp = argv_ptrs[i];
    }

    // Push argc.
    sp -= 4;
    if (sp < USER_STACK_BASE) return 0;
    *(uint32*)sp = (uint32)local_argc;

    return sp;
}

int user_elf_run_argv(uint8 drive, const char* abspath, int argc, const char* const* argv) {
    if (!abspath || !abspath[0]) return -1;

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

    uint8* file = (uint8*)malloc((size_t)st.size);
    if (!file) {
        printf("%cError: out of memory.\n", 255, 0, 0);
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
    if (pages == 0 || pages > 1024) {
        printf("%cError: ELF mapping size too large.\n", 255, 0, 0);
        free(file);
        return -1;
    }

    // Clean up any previous user-task mappings first.
    user_task_cleanup_mappings();

    // Reset per-task syscall state.
    syscall_reset_user_fds();
    syscall_reset_user_guis();

    g_user_interrupt = 0;
    g_user_task_active = 1;
    g_user_task_term = tile_is_tiling_active() ? tile_get_focused() : -1;
    if (g_user_task_term < 0) g_user_task_term = 0;
    g_user_task_ui_dirty = 1;

    // Default user program output color to white (programs can change it).
    extern volatile int g_user_task_color_r;
    extern volatile int g_user_task_color_g;
    extern volatile int g_user_task_color_b;
    extern volatile uint8 g_user_task_color_state;
    g_user_task_color_r = 255;
    g_user_task_color_g = 255;
    g_user_task_color_b = 255;
    g_user_task_color_state = 0;
    
    // Clear the stdin buffer for this terminal so the user task starts fresh
    vterm_stdin_clear(g_user_task_term);

    // Map code/data region pages.
    for (uint32 pi = 0; pi < pages; ++pi) {
        uint32 va = map_start + pi * PAGE_SIZE;
        uint32 frame = frame_alloc();
        if (frame == 0) {
            printf("%cError: out of physical frames.\n", 255, 0, 0);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        if (vmm_map_page(&vmm_kernel_as, va, frame, PTE_PRESENT | PTE_USER | PTE_RW) != 0) {
            printf("%cError: failed to map user pages.\n", 255, 0, 0);
            frame_free(frame);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        invalidate_tlb_entry(va);
    }

    // Map user stack (initial N pages; can grow further on page faults).
    const uint32 user_stack_pages = USER_ELF_INITIAL_STACK_PAGES;
    const uint32 user_stack_page = USER_STACK_TOP - user_stack_pages * PAGE_SIZE;
    const uint32 user_stack_top = USER_STACK_TOP - 0x10;
    for (uint32 spi = 0; spi < user_stack_pages; ++spi) {
        uint32 va = user_stack_page + spi * PAGE_SIZE;
        uint32 frame = frame_alloc();
        if (frame == 0) {
            printf("%cError: out of physical frames.\n", 255, 0, 0);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        if (vmm_map_page(&vmm_kernel_as, va, frame, PTE_PRESENT | PTE_USER | PTE_RW) != 0) {
            printf("%cError: failed to map user stack.\n", 255, 0, 0);
            frame_free(frame);
            user_task_cleanup_mappings();
            free(file);
            return -1;
        }
        invalidate_tlb_entry(va);
    }

    // Enable VMM stack growth for the current address space.
    vmm_kernel_as.stack_bottom = user_stack_page;

    // Record mappings for cleanup on exit/abort.
    g_user_code_base = map_start;
    g_user_code_pages = pages;
    // Bottom of mapped stack region (not necessarily a single page anymore).
    g_user_stack_page = user_stack_page;

    // Zero the full mapped region and apply PT_LOAD contents.
    memset((void*)map_start, 0, map_size);

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
        memcpy((void*)ph->p_vaddr, file + ph->p_offset, ph->p_filesz);
    }

    memset((void*)user_stack_page, 0, user_stack_pages * PAGE_SIZE);

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
    uint32 user_esp = user_stack_build_argv(user_stack_top, abspath, argc, argv);
    if (user_esp == 0) {
        printf("%cError: argv too large.\n", 255, 0, 0);
        user_task_cleanup_mappings();
        return -1;
    }

    // Enter ring3 at ELF entry.
    printf("%c[elfrun] entering user mode: %s (entry=0x%X)\n", 0, 255, 0, abspath, (unsigned)entry);
    tss_set_kernel_stack((uint32)&stack_space);
    enter_user_mode(entry, user_esp);

    return 0;
}

int user_elf_run(uint8 drive, const char* abspath) {
    return user_elf_run_argv(drive, abspath, 0, NULL);
}
