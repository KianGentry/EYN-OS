#include <native_exec.h>
#include <eynfs.h>
#include <util.h>
#include <string.h>
#include <kernel_api.h>
#include <linux_syscalls.h>
#include <vga.h>
#include <kb.h>
#include <drivers/flat_exe_format.h>
#include <utilities/linker.h>
#include <misc/sched.h>
#include <context.h>

// Process management
#define MAX_NATIVE_PROCESSES 8
#define USER_CODE_BASE 0x2000000    // 32MB
#define USER_DATA_BASE 0x3000000    // 48MB  
#define USER_STACK_BASE 0x4000000   // 64MB
#define USER_HEAP_BASE  0x5000000   // 80MB

// Global process table
static native_process_t g_processes[MAX_NATIVE_PROCESSES] CACHE_ALIGNED_32;
static native_process_t* g_current_process = NULL;
static int g_next_pid = 1;
static uint32 g_user_heap_ptr = USER_HEAP_BASE;
static int g_current_index = -1;
// Verbose tracing control (set to 1 to enable detailed trace prints)
static int native_verbose = 0;

static int native_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Minimal Linux i386 user stack initializer: argc=1, argv[0]=filename, envp={"TERM=eyn"}, auxv terminator
static void prepare_linux_user_stack(native_process_t* p, const char* filename) {
    if (!p || !p->stack_start || !p->stack_size) return;
    uint32_t stack_top = p->stack_start + p->stack_size;
    uint32_t sp_strings = stack_top;
    // Place strings at the very top, growing downward
    uint32_t argv0_addr = 0;
    if (filename && filename[0]) {
        size_t len = strlen(filename) + 1;
        if (len > 200) len = 200; // cap filename length copied
        sp_strings -= (uint32_t)len;
        memcpy((void*)sp_strings, filename, len);
        argv0_addr = sp_strings;
    } else {
        // Default name
        const char* d = "program";
        size_t len = strlen(d) + 1;
        sp_strings -= (uint32_t)len;
        memcpy((void*)sp_strings, d, len);
        argv0_addr = sp_strings;
    }
    const char* env0 = "TERM=eyn";
    uint32_t env0_addr = 0;
    {
        size_t len = strlen(env0) + 1;
        sp_strings -= (uint32_t)len;
        memcpy((void*)sp_strings, env0, len);
        env0_addr = sp_strings;
    }
    // Align strings area to 16 bytes for sanity
    sp_strings &= ~0xF;
    // Build pointer tables just below strings area
    uint32_t sp = sp_strings;
    // Reserve a small guard gap
    if (sp > p->stack_start + 256) sp -= 256;
    // Write layout: argc, argv[0], 0, envp[0], 0, auxv(AT_NULL)
    uint32_t* wp = (uint32_t*)sp;
    *wp++ = 1;            // argc
    *wp++ = argv0_addr;   // argv[0]
    *wp++ = 0;            // argv terminator
    *wp++ = env0_addr;    // envp[0]
    *wp++ = 0;            // envp terminator
    *wp++ = 0;            // auxv AT_NULL type
    *wp++ = 0;            // auxv AT_NULL val
    // Set ESP to point at argc
    p->esp = (uint32_t)sp;
}

// Initialize native execution system
void native_exec_init(void) {
    memset(g_processes, 0, sizeof(g_processes));
    g_current_process = NULL;
    g_next_pid = 1;
    g_user_heap_ptr = USER_HEAP_BASE;
    g_current_index = -1;
    // Execution system initialized
}

// Load a program from EYNFS
exec_result_t native_load_program(const char* filename, native_process_t* process) {
    // Loading program

    if (!native_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return EXEC_ERROR_INVALID_FORMAT;
    
    // Read EYNFS superblock
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        // No supported filesystem found
        return EXEC_ERROR_INVALID_FORMAT;
    }
    
    // Find file (support absolute and relative-like paths)
    eynfs_dir_entry_t entry; uint32_t pb = 0, ei = 0;
    if (eynfs_traverse_path(0, &sb, filename, &entry, &pb, &ei) != 0) {
        // Fallback: try root directory by name only (legacy behavior)
        if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, filename, &entry, 0) != 0) {
            return EXEC_ERROR_INVALID_FORMAT;
        }
    }
    
    // Read file
    uint32_t size = entry.size;
    if (!native_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return EXEC_ERROR_MEMORY_ALLOC;
    char* buf = (char*)malloc(size);
    if (!buf) {
        // Out of memory
        return EXEC_ERROR_MEMORY_ALLOC;
    }
    
    int n = eynfs_read_file(0, &sb, &entry, buf, size, 0);
    if (n < 0) {
        // Failed to read file
        free(buf);
        return EXEC_ERROR_INVALID_FORMAT;
    }

    /* Check for ELF32 executable (little-endian). If detected, perform a
       minimal PT_LOAD mapping into a contiguous heap buffer and set up the
       native_process structure so the existing emulator can run it. This is a
       conservative, user-space buffer-based loader (no paging/user-mode
       switch) intended to allow simple statically-linked ELF32 binaries to
       run under the current native execution model. */
    if (size >= 16 && (uint8_t)buf[0] == ELFMAG0 && (uint8_t)buf[1] == ELFMAG1 && (uint8_t)buf[2] == ELFMAG2 && (uint8_t)buf[3] == ELFMAG3) {
        printf("[NATIVE_EXEC] Detected ELF32 file, checking for i386...\n");
        // Candidate ELF file
        if ((uint8_t)buf[EI_CLASS] == ELFCLASS32 && (uint8_t)buf[5] == ELFDATA2LSB) {
            // Parse header bounds-safely
            if (size >= sizeof(Elf32_Ehdr)) {
                Elf32_Ehdr* eh = (Elf32_Ehdr*)buf;
                // Check machine
                if (eh->e_machine == EM_386) {
                    printf("[NATIVE_EXEC] Loading i386 ELF binary: %s (entry=%p)\n", filename, eh->e_entry);
                    uint32_t min_vaddr = 0xffffffffu;
                    uint32_t max_vaddr = 0;
                    int load_count = 0;
                    char interpreter_path[256];
                    interpreter_path[0] = '\0';

                    if (eh->e_phoff + (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize <= size) {
                        Elf32_Phdr* ph = (Elf32_Phdr*)(buf + eh->e_phoff);
                        for (int i = 0; i < eh->e_phnum; i++) {
                            if (ph->p_type == PT_INTERP && ph->p_offset + ph->p_filesz <= size) {
                                uint32_t interp_len = ph->p_filesz;
                                if (interp_len > sizeof(interpreter_path) - 1) interp_len = sizeof(interpreter_path) - 1;
                                memcpy(interpreter_path, buf + ph->p_offset, interp_len);
                                interpreter_path[interp_len] = '\0';
                                break;
                            }
                            ph = (Elf32_Phdr*)((char*)ph + eh->e_phentsize);
                        }
                    }

                    if (interpreter_path[0] != '\0') {
                        printf("[NATIVE_EXEC] PT_INTERP detected (%s); use run/user_elf path for dynamic ELF execution\n", interpreter_path);
                        free(buf);
                        return EXEC_ERROR_INVALID_FORMAT;
                    }

                    if (eh->e_phoff + (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize <= size) {
                        Elf32_Phdr* ph = (Elf32_Phdr*)(buf + eh->e_phoff);
                        for (int i = 0; i < eh->e_phnum; i++) {
                            if (ph->p_type == PT_LOAD) {
                                if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
                                uint32_t end = ph->p_vaddr + ph->p_memsz;
                                if (end > max_vaddr) max_vaddr = end;
                                load_count++;
                            }
                            ph = (Elf32_Phdr*)((char*)ph + eh->e_phentsize);
                        }
                    }

                    if (load_count > 0 && min_vaddr != 0xffffffffu && max_vaddr > min_vaddr) {
                        memset(process, 0, sizeof(native_process_t));
                        process->pid = g_next_pid++;
                        process->segment_count = 0;

                        Elf32_Phdr* ph = (Elf32_Phdr*)(buf + eh->e_phoff);
                        uint32_t first_exec_vaddr = 0;
                        uint32_t first_exec_mem = 0;
                        for (int i = 0; i < eh->e_phnum; i++) {
                            if (ph->p_type == PT_LOAD) {
                                if (process->segment_count >= 8) break;
                                uint32_t memsz = ph->p_memsz;
                                uint32_t filesz = ph->p_filesz;
                                void* segmem = malloc(memsz);
                                if (!segmem) {
                                    for (int j = 0; j < process->segment_count; j++) {
                                        free(process->segments[j].mem);
                                        process->segments[j].mem = NULL;
                                    }
                                    free(buf);
                                    return EXEC_ERROR_MEMORY_ALLOC;
                                }
                                memset(segmem, 0, memsz);
                                if (ph->p_offset + filesz <= (uint32_t)size && filesz > 0) {
                                    memcpy(segmem, buf + ph->p_offset, filesz);
                                }

                                process->segments[process->segment_count].vaddr = ph->p_vaddr;
                                process->segments[process->segment_count].memsz = memsz;
                                process->segments[process->segment_count].filesz = filesz;
                                process->segments[process->segment_count].mem = segmem;
                                process->segments[process->segment_count].flags = ph->p_flags;
                                process->segment_count++;

                                if ((ph->p_flags & 0x1) && first_exec_vaddr == 0) {
                                    first_exec_vaddr = ph->p_vaddr;
                                    first_exec_mem = (uint32)segmem;
                                }
                            }
                            ph = (Elf32_Phdr*)((char*)ph + eh->e_phentsize);
                        }

                        if (process->segment_count == 0) {
                            free(buf);
                            return EXEC_ERROR_INVALID_FORMAT;
                        }

                        process->elf_vaddr_min = min_vaddr;
                        process->elf_vaddr_max = max_vaddr;

                        if (first_exec_vaddr != 0) {
                            process->code_start = first_exec_mem;
                            for (int s = 0; s < process->segment_count; s++) {
                                if (process->segments[s].vaddr == first_exec_vaddr) {
                                    process->code_size = process->segments[s].memsz;
                                    break;
                                }
                            }
                        } else {
                            uint32_t lowv = 0xffffffffu;
                            void* lowmem = NULL;
                            uint32_t lowsz = 0;
                            for (int s = 0; s < process->segment_count; s++) {
                                if (process->segments[s].vaddr < lowv) {
                                    lowv = process->segments[s].vaddr;
                                    lowmem = process->segments[s].mem;
                                    lowsz = process->segments[s].memsz;
                                }
                            }
                            process->code_start = (uint32)lowmem;
                            process->code_size = lowsz;
                        }

                        process->stack_start = (uint32_t)malloc(0x10000);
                        if (!process->stack_start) {
                            for (int j = 0; j < process->segment_count; j++) free(process->segments[j].mem);
                            free(buf);
                            return EXEC_ERROR_MEMORY_ALLOC;
                        }
                        process->stack_size = 0x10000;

                        uint32_t e_entry = eh->e_entry;
                        uint32_t entry_addr = 0;
                        for (int s = 0; s < process->segment_count; s++) {
                            uint32_t seg_v = process->segments[s].vaddr;
                            uint32_t seg_m = process->segments[s].memsz;
                            if (e_entry >= seg_v && e_entry < seg_v + seg_m) {
                                entry_addr = (uint32)process->segments[s].mem + (e_entry - seg_v);
                                break;
                            }
                        }
                        if (entry_addr == 0) {
                            entry_addr = process->code_start;
                        }

                        process->entry_point = entry_addr;
                        process->linux_compat_mode = 1;

                        prepare_linux_user_stack(process, filename);
                        if (!process->esp) process->esp = process->stack_start + process->stack_size - 4;
                        process->eip = process->entry_point;
                        process->active = 1;
                        safe_strcpy(process->name, filename, sizeof(process->name));
                        
                        // Initialize standard file descriptors for Linux programs
                        linux_init_stdio(process);

                        free(buf);
                        return EXEC_SUCCESS;
                    }
                }
            }
        }
        // If ELF detection fails to fully validate, fall through to other loaders
    }
    
    // Try to parse EYN executable header first
    if (size >= sizeof(struct eyn_exe_header)) {
        struct eyn_exe_header* hdr = (struct eyn_exe_header*)buf;
        if (hdr->magic[0] == 'E' && hdr->magic[1] == 'Y' && hdr->magic[2] == 'N' && hdr->magic[3] == '\0') {
            // EYN executable loaded
            printf("[native_exec] Loaded '%s' (code=%d, data=%d, entry_off=%d)\n", filename, hdr->code_size, hdr->data_size, hdr->entry_point);

            // Basic sanity checks to avoid malformed headers
            const uint32_t MAX_CODE = 1024 * 1024; // 1MB
            const uint32_t MAX_DATA = 1024 * 1024; // 1MB
            if (hdr->code_size == 0 || hdr->code_size > MAX_CODE) { free(buf); return EXEC_ERROR_INVALID_FORMAT; }
            if (hdr->data_size > MAX_DATA) { free(buf); return EXEC_ERROR_INVALID_FORMAT; }

            // Initialize process structure
            memset(process, 0, sizeof(native_process_t));
            process->pid = g_next_pid++;

            // Allocate memory for code and data sections
            process->code_start = (uint32_t)malloc(hdr->code_size);
            if (!process->code_start) {
                free(buf);
                return EXEC_ERROR_MEMORY_ALLOC;
            }
            process->code_size = hdr->code_size;

            process->data_start = (uint32_t)malloc(hdr->data_size + 0x1000); // Extra space for safety
            if (!process->data_start) {
                free((void*)process->code_start);
                free(buf);
                return EXEC_ERROR_MEMORY_ALLOC;
            }
            process->data_size = hdr->data_size;

            process->stack_start = (uint32_t)malloc(0x10000); // 64KB stack
            if (!process->stack_start) {
                free((void*)process->code_start);
                free((void*)process->data_start);
                free(buf);
                return EXEC_ERROR_MEMORY_ALLOC;
            }
            process->stack_size = 0x10000;

            process->entry_point = process->code_start + hdr->entry_point;

            // Memory allocated and entry point calculated
            // Initialize Linux-like argc/argv/envp on the user stack
            prepare_linux_user_stack(process, filename);
            if (!process->esp) process->esp = process->stack_start + process->stack_size - 4; // fallback
            process->eip = process->entry_point;
            process->active = 1;

            safe_strcpy(process->name, filename, sizeof(process->name));

            // Copy code section
            if (hdr->code_size > 0) {
                uint8_t* code = (uint8_t*)(buf + sizeof(struct eyn_exe_header));
                memcpy((void*)process->code_start, code, hdr->code_size);
                // Tiny dump of first bytes for sanity
                uint32_t preview = hdr->code_size < 8 ? hdr->code_size : 8;
                printf("[native_exec] Code preview:");
                for (uint32_t i = 0; i < preview; i++) { printf(" %02d", code[i]); }
                printf("\n");
            }

            // Copy data section and perform EYN-specific relocations
            if (hdr->data_size > 0) {
                uint8_t* data = (uint8_t*)(buf + sizeof(struct eyn_exe_header) + hdr->code_size);
                memcpy((void*)process->data_start, data, hdr->data_size);

                // Fix up 32-bit immediates inside the code that look like addresses/offsets.
                // Two common patterns are:
                // 1) assembler-emitted data addresses in the 0x02001000 range
                // 2) linker-emitted offsets when the binary was linked at base 0 (small values)
                uint32_t actual_data_addr = process->data_start;
                uint8_t* code_ptr = (uint8_t*)process->code_start;
                for (uint32_t i = 0; i < process->code_size - 4; i++) {
                    uint32_t* addr_ptr = (uint32_t*)(code_ptr + i);
                    uint32_t addr = *addr_ptr;
                    // assembler convention: 0x02001000 + offset (always safe to rebase)
                    if (addr >= 0x02001000 && addr < 0x02002000) {
                        uint32_t offset = addr - 0x02001000;
                        *addr_ptr = actual_data_addr + offset;
                    } else {
                        /* Conservative rebase for small offsets:
                           - Only rebase if the dword is 4-byte aligned within the code
                           - And avoid the initial crt0 prologue (first 64 bytes) where relative
                             call/jmp immediates are common and should not be rewritten.
                           This avoids corrupting instruction immediates such as call rel32. */
                        if ((i % 4) == 0 && i >= 64 && addr < process->code_size) {
                            *addr_ptr = (uint32_t)process->code_start + addr;
                        }
                    }
                }
            }

            free(buf);
            return EXEC_SUCCESS;
        }
    }

    // If we reach here, file is not a valid EYN executable. Try FLAT header then raw binary.
    // Check for FLAT header
    if (size >= sizeof(struct flat_exe_header)) {
        struct flat_exe_header* fh = (struct flat_exe_header*)buf;
        if (fh->magic[0] == 'F' && fh->magic[1] == 'L' && fh->magic[2] == 'A' && fh->magic[3] == 'T') {
            // Sanity checks for FLAT
            const uint32_t MAX_CODE_F = 1024 * 1024;
            const uint32_t MAX_DATA_F = 1024 * 1024;
            if (fh->code_size == 0 || fh->code_size > MAX_CODE_F) { free(buf); return EXEC_ERROR_INVALID_FORMAT; }
            if (fh->data_size > MAX_DATA_F) { free(buf); return EXEC_ERROR_INVALID_FORMAT; }
            printf("[native_exec] Loaded FLAT binary '%s' (code=%d, data=%d, entry_off=%d)\n", filename, fh->code_size, fh->data_size, fh->entry_point);

            memset(process, 0, sizeof(native_process_t));
            process->pid = g_next_pid++;

            // Allocate code and data sections
            process->code_start = (uint32_t)malloc(fh->code_size);
            if (!process->code_start) { free(buf); return EXEC_ERROR_MEMORY_ALLOC; }
            process->code_size = fh->code_size;

            process->data_start = 0;
            process->data_size = 0;
            if (fh->data_size > 0) {
                process->data_start = (uint32_t)malloc(fh->data_size + 0x1000);
                if (!process->data_start) { free((void*)process->code_start); free(buf); return EXEC_ERROR_MEMORY_ALLOC; }
                process->data_size = fh->data_size;
            }

            process->stack_start = (uint32_t)malloc(0x10000);
            if (!process->stack_start) { if (process->data_start) free((void*)process->data_start); free((void*)process->code_start); free(buf); return EXEC_ERROR_MEMORY_ALLOC; }
            process->stack_size = 0x10000;

            // Copy code and data from file buffer
            uint8_t* code = (uint8_t*)(buf + sizeof(struct flat_exe_header));
            memcpy((void*)process->code_start, code, fh->code_size);
            if (fh->data_size > 0) {
                uint8_t* data = (uint8_t*)(buf + sizeof(struct flat_exe_header) + fh->code_size);
                memcpy((void*)process->data_start, data, fh->data_size);

                // Patch 32-bit immediates inside code which may reference assembler/data
                uint32_t actual_data_addr = process->data_start;
                uint8_t* code_ptr = (uint8_t*)process->code_start;
                for (uint32_t i = 0; i < process->code_size - 4; i++) {
                    uint32_t* addr_ptr = (uint32_t*)(code_ptr + i);
                    uint32_t addr = *addr_ptr;
                    if (addr >= 0x02001000 && addr < 0x02002000) {
                        uint32_t offset = addr - 0x02001000;
                        *addr_ptr = actual_data_addr + offset;
                    } else {
                        if ((i % 4) == 0 && i >= 64 && addr < process->code_size) {
                            *addr_ptr = (uint32_t)process->code_start + addr;
                        }
                    }
                }
            }

            process->entry_point = process->code_start + fh->entry_point;
            prepare_linux_user_stack(process, filename);
            if (!process->esp) process->esp = process->stack_start + process->stack_size - 4;
            process->eip = process->entry_point;
            process->active = 1;
            safe_strcpy(process->name, filename, sizeof(process->name));

         // Diagnostic: print memory layout for the loaded process
         printf("[native_exec] proc layout: code=%d..%d data=%d..%d stack=%d..%d entry=%d\n",
             process->code_start, process->code_start + process->code_size,
             process->data_start, process->data_start + process->data_size,
             process->stack_start, process->stack_start + process->stack_size,
             process->entry_point - process->code_start);

            free(buf);
            return EXEC_SUCCESS;
        }
    }

    // Fallback: treat as raw flat binary with no header
    printf("[native_exec] File '%s' is not EYN/FLAT format - treating as raw flat i386 binary (size=%u bytes)\n", filename, size);

    // Initialize process structure for raw flat binary
    memset(process, 0, sizeof(native_process_t));
    process->pid = g_next_pid++;

    process->code_start = (uint32_t)malloc(size);
    if (!process->code_start) { free(buf); return EXEC_ERROR_MEMORY_ALLOC; }
    process->code_size = size;

    process->data_start = 0;
    process->data_size = 0;

    process->stack_start = (uint32_t)malloc(0x10000); // 64KB stack
    if (!process->stack_start) { free((void*)process->code_start); free(buf); return EXEC_ERROR_MEMORY_ALLOC; }
    process->stack_size = 0x10000;

    memcpy((void*)process->code_start, buf, size);

    process->entry_point = process->code_start;
    prepare_linux_user_stack(process, filename);
    if (!process->esp) process->esp = process->stack_start + process->stack_size - 4;
    process->eip = process->entry_point;
    process->active = 1;
    safe_strcpy(process->name, filename, sizeof(process->name));

    free(buf);
    return EXEC_SUCCESS;
}

// Execute a process natively
exec_result_t native_run_process(native_process_t* process) {
    if (!process || !process->active) {
        return EXEC_ERROR_EXECUTION_FAILED;
    }
    
    // Starting execution (single-step emulator model)
    
    // Safety check: ensure entry point is valid
    if (process->entry_point == 0) {
        process->active = 0;
        g_current_process = NULL;
        return EXEC_ERROR_INVALID_ENTRY;
    }
    
    // Set up kernel API for user program
    // The user program will have access to the kernel API through g_kernel_api
    g_current_process = process;
    
    // For now, we'll use a simplified execution model
    // In a full implementation, this would involve:
    // 1. Setting up proper memory protection
    // 2. Switching to user mode
    // 3. Setting up proper stack and registers
    // 4. Jumping to the entry point
    
    // Simplified execution: call the entry point as a function
    // This is not ideal but works for demonstration
    typedef void (*entry_func_t)(void);
    entry_func_t entry = (entry_func_t)process->entry_point;
    
    // For now, use a safer approach - simulate execution instead of direct call
    // This avoids the issues with calling user code directly from kernel space
    
    // Check if the code contains syscalls and handle them
    uint8_t* code_ptr = (uint8_t*)process->code_start;
    uint32_t pc = 0;
    /* Start execution at the entry offset within the mapped image. For
       flat/raw binaries this will be zero; for ELF executables the entry
       point will typically be non-zero. Guard against invalid entries. */
    if (process->entry_point >= process->code_start && process->entry_point < process->code_start + process->code_size) {
        pc = process->entry_point - process->code_start;
    } else {
        pc = 0;
    }
    uint32_t regs[8] = {0}; // eax, ecx, edx, ebx, esp, ebp, esi, edi

    // Initialize emulated stack pointer from process allocation so user code has a valid stack
    regs[4] = process->esp;
    
    // Simple instruction simulation for basic instructions
    uint32_t __max_steps = process->code_size * 16 + 1024;
    if (__max_steps < 2048) __max_steps = 2048;
    uint32_t __steps = 0;
    while (process->active && pc < process->code_size && __steps++ < __max_steps) { // Limit to prevent infinite loops
        uint8_t opcode = code_ptr[pc];
        if (native_verbose) {
            /* Lightweight trace for initial steps to help debug why programs exit immediately */
            if (__steps < 200) {
                printf("[native_exec:trace] step=%d pc=%d opcode=%d\n", __steps, pc, opcode);
            }
        }
        
        // Debug output removed for clean program execution
        
        if (opcode == 0xB8 || opcode == 0xB9 || opcode == 0xBA || opcode == 0xBB || 
            opcode == 0xBC || opcode == 0xBD || opcode == 0xBE || opcode == 0xBF) {
            // mov reg, imm32
            uint8_t reg = opcode - 0xB8;
            if (pc + 4 < process->code_size) {
                uint32_t imm = *(uint32_t*)(code_ptr + pc + 1);
                regs[reg] = imm;
                // mov reg, imm32 executed
                pc += 5;
            } else {
                break;
            }
        } else if (opcode == 0xCD) {
                            // int imm8 - syscall
            if (pc + 1 < process->code_size) {
                uint8_t imm = code_ptr[pc + 1];
                // Syscall handling
                // int syscall executed
                
                if (imm == 0x80) {
                    if (process->linux_compat_mode) {
                        // Linux-style syscall dispatch for Linux-compat processes only.
                        int dispatched = linux_syscall_dispatch(process, regs);
                        if (dispatched != -38) { // -ENOSYS means unknown; otherwise handled
                            if (process->linux_execve_pending) {
                                process->linux_execve_pending = 0;
                                code_ptr = (uint8_t*)process->code_start;
                                pc = 0;
                                if (process->entry_point >= process->code_start &&
                                    process->entry_point < process->code_start + process->code_size) {
                                    pc = process->entry_point - process->code_start;
                                }
                                memset(regs, 0, sizeof(regs));
                                regs[4] = process->esp;
                                continue;
                            }
                            pc += 2;
                            continue;
                        }
                    }
                    if (native_verbose) printf("[native_exec:syscall-legacy] eax=%d ebx=%d ecx=%d edx=%d esp=%d\n", regs[0], regs[3], regs[1], regs[2], regs[4]);

                    if (regs[0] == 1) { // WRITE
                        if (regs[3] == 1 && regs[2] > 0 && regs[2] <= 512) {
                            /* stdout, reasonable length
                               Accept buffers located in the process code, data or stack
                               regions. Many freestanding binaries place string literals
                               in the code section or use stack buffers for I/O. */
                            uint32_t buffer_addr = regs[1];
                            uint32_t len = regs[2];

                            uint8_t* buffer = NULL;
                            uint32_t region_end = 0;
                            // Translate potential ELF virtual addresses into the mapped image
                            // If the program was loaded from ELF, user-space pointers will
                            // likely be ELF virtual addresses (e.g. 0x08049000...). Convert
                            // those into our image region addresses before validating.
                            uint32_t translated = buffer_addr;
                            // If ELF-loaded, map via segment table
                            if (process->segment_count && buffer_addr >= process->elf_vaddr_min && buffer_addr < process->elf_vaddr_max) {
                                for (int s = 0; s < process->segment_count; s++) {
                                    uint32_t sv = process->segments[s].vaddr;
                                    uint32_t sm = process->segments[s].memsz;
                                    if (buffer_addr >= sv && buffer_addr + len <= sv + sm) {
                                        translated = (uint32_t)process->segments[s].mem + (buffer_addr - sv);
                                        break;
                                    }
                                }
                            }

                            if (translated >= process->code_start && translated + len <= process->code_start + process->code_size) {
                                buffer = (uint8_t*)translated;
                                region_end = process->code_start + process->code_size;
                            } else if (process->data_size > 0 && translated >= process->data_start && translated + len <= process->data_start + process->data_size) {
                                buffer = (uint8_t*)translated;
                                region_end = process->data_start + process->data_size;
                            } else if (buffer_addr >= process->stack_start && buffer_addr + len <= process->stack_start + process->stack_size) {
                                // stack addresses are allocated in kernel-space layout and are
                                // already direct addresses into the process->stack_start buffer
                                buffer = (uint8_t*)buffer_addr;
                                region_end = process->stack_start + process->stack_size;
                            }

                            if (buffer) {
                                char output_buffer[101];
                                uint32_t output_pos = 0;
                                for (uint32_t i = 0; i < len && output_pos < 100; i++) {
                                    unsigned char ch = buffer[i];
                                    if (ch >= 32 && ch <= 126) {
                                        output_buffer[output_pos++] = (char)ch;
                                    } else if (ch == '\n') {
                                        output_buffer[output_pos++] = '\n';
                                    }
                                }
                                output_buffer[output_pos] = '\0';
                                printf("%s", output_buffer);
                                regs[0] = len; // Return bytes written
                            } else {
                                // Try fallback: the requested buffer may cross multiple
                                // PT_LOAD segments. Assemble bytes by querying segments
                                // per-address. This handles cases where the string is not
                                // contained within a single segment allocation.
                                if (process->elf_vaddr_min && buffer_addr >= process->elf_vaddr_min && buffer_addr + len <= process->elf_vaddr_max) {
                                    char output_buffer[101];
                                    uint32_t output_pos = 0;
                                    for (uint32_t i = 0; i < len && output_pos < 100; i++) {
                                        uint8_t b = 0;
                                        int ok = 0;
                                        // search segments for this byte
                                        for (int ss = 0; ss < process->segment_count; ss++) {
                                            uint32_t sv = process->segments[ss].vaddr;
                                            uint32_t sm = process->segments[ss].memsz;
                                            if (buffer_addr + i >= sv && buffer_addr + i < sv + sm) {
                                                uint8_t* ptr = (uint8_t*)process->segments[ss].mem + (buffer_addr + i - sv);
                                                b = *ptr;
                                                ok = 1;
                                                break;
                                            }
                                        }
                                        if (!ok) break;
                                        if (b >= 32 && b <= 126) output_buffer[output_pos++] = (char)b;
                                        else if (b == '\n') output_buffer[output_pos++] = '\n';
                                    }
                                    output_buffer[output_pos] = '\0';
                                    if (output_pos > 0) printf("%s", output_buffer);
                                    regs[0] = (int)len;
                                } else {
                                    if (native_verbose) printf("[native_exec:syscall] WRITE buffer %d not in code/data/stack (code=%d..%d data=%d..%d stack=%d..%d)\n",
                                        buffer_addr,
                                        process->code_start, process->code_start + process->code_size,
                                        process->data_start, process->data_start + process->data_size,
                                        process->stack_start, process->stack_start + process->stack_size);
                                    regs[0] = -1;
                                }
                            }
                        } else {
                            regs[0] = -1;
                        }
                    } else if (regs[0] == 2) { // EXIT
                        break;
                    } else if (regs[0] == 3) { // READ
                        // fd in EBX (regs[3]), buf in ECX (regs[1]), len in EDX (regs[2])
                        if (regs[3] == 0 && regs[1] && regs[2] > 0) {
                            string s = readStr();
                            if (s) {
                                int slen = (int)strlen(s);
                                int maxcpy = (int)regs[2];
                                int n = slen;
                                if (n > maxcpy - 1) n = maxcpy - 1;
                                uint32_t buffer_addr = regs[1];

                                /* Allow the destination buffer to be in data or stack
                                   (and also code, though writing into code is uncommon).
                                   Ensure the copy stays within bounds. */
                                int wrote = 0;
                                uint32_t translated = buffer_addr;
                                if (process->segment_count && buffer_addr >= process->elf_vaddr_min && buffer_addr < process->elf_vaddr_max) {
                                    for (int s = 0; s < process->segment_count; s++) {
                                        uint32_t sv = process->segments[s].vaddr;
                                        uint32_t sm = process->segments[s].memsz;
                                        if (buffer_addr >= sv && buffer_addr + n + 1 <= sv + sm) {
                                            translated = (uint32_t)process->segments[s].mem + (buffer_addr - sv);
                                            break;
                                        }
                                    }
                                }
                                if (process->data_size > 0 && translated >= process->data_start && translated + n + 1 <= process->data_start + process->data_size && n >= 0) {
                                    memcpy((void*)translated, s, n);
                                    ((char*)translated)[n] = '\0';
                                    regs[0] = n;
                                    wrote = 1;
                                } else if (buffer_addr >= process->stack_start && buffer_addr + n + 1 <= process->stack_start + process->stack_size && n >= 0) {
                                    memcpy((void*)buffer_addr, s, n);
                                    ((char*)buffer_addr)[n] = '\0';
                                    regs[0] = n;
                                    wrote = 1;
                                } else if (translated >= process->code_start && translated + n + 1 <= process->code_start + process->code_size && n >= 0) {
                                    /* Writing into code is allowed but unusual; permit for simple programs. */
                                    memcpy((void*)translated, s, n);
                                    ((char*)translated)[n] = '\0';
                                    regs[0] = n;
                                    wrote = 1;
                                } else if (process->segment_count && buffer_addr >= process->elf_vaddr_min && buffer_addr + n + 1 <= process->elf_vaddr_max) {
                                    /* Fallback: if the destination spans segments, attempt
                                       to write per-byte into the appropriate segment
                                       allocation. */
                                    int ok = 1;
                                    for (int i = 0; i < n; i++) {
                                        uint32_t addr = buffer_addr + i;
                                        int found = 0;
                                        for (int ss = 0; ss < process->segment_count; ss++) {
                                            uint32_t sv = process->segments[ss].vaddr;
                                            uint32_t sm = process->segments[ss].memsz;
                                            if (addr >= sv && addr < sv + sm) {
                                                uint8_t* dst = (uint8_t*)process->segments[ss].mem + (addr - sv);
                                                dst[0] = ((uint8_t*)s)[i];
                                                found = 1; break;
                                            }
                                        }
                                        if (!found) { ok = 0; break; }
                                    }
                                    if (ok) { ((char*)buffer_addr)[n] = '\0'; regs[0] = n; wrote = 1; }
                                }

                                if (!wrote) {
                     printf("[native_exec:syscall] READ destination %d not in data/stack/code (code=%d..%d data=%d..%d stack=%d..%d)\n",
                         buffer_addr,
                         process->code_start, process->code_start + process->code_size,
                         process->data_start, process->data_start + process->data_size,
                         process->stack_start, process->stack_start + process->stack_size);
                                    regs[0] = -1;
                                }
                            } else {
                                regs[0] = -1;
                            }
                        } else {
                            regs[0] = -1;
                        }
                    } else {
                        regs[0] = -1;
                    }
                }
                pc += 2;
            } else {
                break;
            }
        } else if (opcode == 0x8D) {
            // lea r32, m
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                uint8_t dst = (modrm >> 3) & 7;
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm  = modrm & 7;
                uint32_t ea = 0; // effective address (only absolute disp32 handled)
                int step = 2;
                if (mod == 0 && rm == 5) {
                    // [disp32] absolute
                    if (pc + 6 <= process->code_size) {
                        ea = *(uint32_t*)(code_ptr + pc + 2);
                        step = 6;
                    }
                } else if (mod == 0 && rm == 4) {
                    // SIB present; handle special case [disp32] via SIB base=5
                    if (pc + 3 <= process->code_size) {
                        uint8_t sib = code_ptr[pc + 2];
                        step = 3;
                        uint8_t base = sib & 7;
                        if (base == 5) {
                            if (pc + 7 <= process->code_size) {
                                ea = *(uint32_t*)(code_ptr + pc + 3);
                                step = 7;
                            }
                        }
                    }
                } else if (mod == 2) {
                    // disp32 with base register (we don't compute full EA here)
                    step = 6; // skip over disp32
                } else if (mod == 1) {
                    step = 3; // skip over disp8
                } else {
                    step = 2; // register-direct; treat as NOP for LEA
                }
                if (ea != 0) {
                    // If this looks like a data address pre-patch, it has already been rebased by loader
                    // so just assign it; otherwise assign as-is.
                    regs[dst] = ea;
                }
                pc += step;
            } else {
                pc++;
            }
        } else if (opcode == 0x0F) {
            // extended opcodes
            if (pc + 1 < process->code_size) {
                uint8_t op2 = code_ptr[pc + 1];
                if (op2 == 0xB6 || op2 == 0xBE) {
                    // movzx/movsx r32, r/m8 (support reg8 and simple [disp32])
                    int is_zx = (op2 == 0xB6);
                    if (pc + 2 < process->code_size) {
                        uint8_t modrm = code_ptr[pc + 2];
                        uint8_t dst = (modrm >> 3) & 7;
                        uint8_t mod = (modrm >> 6) & 3;
                        uint8_t rm  = modrm & 7;
                        uint32_t v = 0;
                        int step = 3;
                        if (mod == 3) {
                            // reg8 source from low/high 8 bits of regs
                            uint8_t src8 = rm; // al=0..bh=7
                            uint32_t src32 = regs[src8 & 3];
                            if (src8 < 4) v = src32 & 0xFF; else v = (src32 >> 8) & 0xFF;
                        } else if (mod == 0 && rm == 5 && pc + 6 <= process->code_size) {
                            // [disp32]
                            uint32_t ea = *(uint32_t*)(code_ptr + pc + 3);
                            step = 7;
                            if (ea >= process->data_start && ea < process->data_start + process->data_size) {
                                v = *(uint8_t*)ea;
                            } else {
                                v = 0;
                            }
                        }
                        if (!is_zx) {
                            int8_t sv = (int8_t)(v & 0xFF);
                            regs[dst] = (uint32_t)(int32_t)sv;
                        } else {
                            regs[dst] = (uint32_t)(v & 0xFF);
                        }
                        pc += step;
                    } else {
                        pc += 2;
                    }
                } else {
                    pc += 2; // unhandled 0F xx
                }
            } else {
                pc++;
            }
        } else if (opcode == 0x89) {
            // mov r/m32, r32 (handle reg,reg)
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                if ((modrm >> 6) == 3) {
                    uint8_t src = (modrm >> 3) & 7;
                    uint8_t dst = modrm & 7;
                    regs[dst] = regs[src];
                }
                pc += 2;
            } else {
                pc++;
            }
        } else if (opcode == 0x8B) {
            // mov r32, r/m32 (handle reg,reg)
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                if ((modrm >> 6) == 3) {
                    uint8_t dst = (modrm >> 3) & 7;
                    uint8_t src = modrm & 7;
                    regs[dst] = regs[src];
                }
                pc += 2;
            } else {
                pc++;
            }
        } else if (opcode == 0x31 || opcode == 0x33) {
            // xor r/m32, r32 (0x31) or xor r32, r/m32 (0x33) for reg,reg
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                if ((modrm >> 6) == 3) {
                    uint8_t reg = (modrm >> 3) & 7;
                    uint8_t rm  = modrm & 7;
                    if (opcode == 0x31) regs[rm] ^= regs[reg]; else regs[reg] ^= regs[rm];
                }
                pc += 2;
            } else { pc++; }
        } else if (opcode == 0x01) {
            // add reg, reg (32-bit)
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                uint8_t dst_reg = (modrm >> 3) & 7;
                uint8_t src_reg = modrm & 7;
                // add instruction executed
                regs[dst_reg] += regs[src_reg];
                pc += 2;
            } else {
                pc++;
            }
        } else if (opcode == 0x29) {
            // sub reg, reg (32-bit)
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                uint8_t dst_reg = (modrm >> 3) & 7;
                uint8_t src_reg = modrm & 7;
                regs[dst_reg] -= regs[src_reg];
                pc += 2;
            } else {
                pc++;
            }
        } else if (opcode == 0xF7) {
            // mul reg (32-bit)
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                if ((modrm & 0xF8) == 0xE0) { // mul reg
                    uint8_t reg = modrm & 7;
                    uint64_t result = (uint64_t)regs[0] * regs[reg];
                    regs[0] = (uint32_t)result;
                    regs[2] = (uint32_t)(result >> 32); // edx gets high part
                }
                pc += 2;
            } else {
                pc++;
            }
        } else if (opcode == 0xF6) {
            // div reg (8-bit) - simplified to 32-bit
            if (pc + 1 < process->code_size) {
                uint8_t modrm = code_ptr[pc + 1];
                if ((modrm & 0xF8) == 0xF0) { // div reg
                    uint8_t reg = modrm & 7;
                    if (regs[reg] != 0) {
                        uint32_t quotient = regs[0] / regs[reg];
                        uint32_t remainder = regs[0] % regs[reg];
                        regs[0] = quotient;
                        regs[2] = remainder; // edx gets remainder
                    }
                }
                pc += 2;
            } else {
                pc++;
            }
        } else if ((opcode >= 0x50 && opcode <= 0x57) || (opcode >= 0x58 && opcode <= 0x5F) || opcode == 0xE8 || opcode == 0xE9 || opcode == 0xC3 || opcode == 0x39 || opcode == 0x3B || opcode == 0x74 || opcode == 0x75) {
            // push/pop/call/jmp/ret/cmp/je/jne support
                if (opcode >= 0x50 && opcode <= 0x57) {
                // push r32
                uint8_t reg = opcode - 0x50;
                regs[4] -= 4;
                /* Allow writes to the process stack allocation regardless of global USER_* ranges */
                if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                    *(uint32_t*)regs[4] = regs[reg];
                }
                pc++;
            } else if (opcode >= 0x58 && opcode <= 0x5F) {
                // pop r32
                uint8_t reg = opcode - 0x58;
                if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                    regs[reg] = *(uint32_t*)regs[4];
                }
                regs[4] += 4;
                pc++;
            } else if (opcode == 0xE8) {
                // call rel32
                if (pc + 4 < process->code_size) {
                    int32_t rel = *(int32_t*)(code_ptr + pc + 1);
                    uint32_t ret = pc + 5;
                    regs[4] -= 4;
                    if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) *(uint32_t*)regs[4] = ret;
                    uint32_t new_pc = (uint32_t)((int32_t)(pc + 5) + rel);
                    uint32_t memv = 0;
                    unsigned char b0=0,b1=0,b2=0,b3=0;
                    if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                        memv = *(uint32_t*)regs[4];
                        unsigned char* bp = (unsigned char*)regs[4];
                        b0 = bp[0]; b1 = bp[1]; b2 = bp[2]; b3 = bp[3];
                    }
                    printf("[native_exec:trace] CALL rel=%d -> new_pc=%d (ret=%d) sp=%d mem_at_sp=%d bytes=%d,%d,%d,%d\n", rel, new_pc, ret, regs[4], memv, b0, b1, b2, b3);
                    pc = new_pc;
                } else pc++;
            } else if (opcode == 0xE9) {
                // jmp rel32
                if (pc + 4 < process->code_size) {
                    int32_t rel = *(int32_t*)(code_ptr + pc + 1);
                    pc = (uint32_t)((int32_t)(pc + 5) + rel);
                } else pc++;
            } else if (opcode == 0xC9) {
                // leave: mov esp, ebp; pop ebp
                // Set esp to ebp, then pop ebp from stack
                regs[4] = regs[5];
                if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                    regs[5] = *(uint32_t*)regs[4];
                    regs[4] += 4;
                } else {
                    break;
                }
                pc++;
            } else if (opcode == 0xC3) {
                // ret: pop return address from process stack and jump
                uint32_t mem_before = 0;
                unsigned char bb0=0,bb1=0,bb2=0,bb3=0;
                if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                    unsigned char* bp = (unsigned char*)regs[4];
                    bb0 = bp[0]; bb1 = bp[1]; bb2 = bp[2]; bb3 = bp[3];
                    mem_before = *(uint32_t*)regs[4];
                }
                printf("[native_exec:trace] RET about to pop sp=%d mem_before=%d bytes=%d,%d,%d,%d\n", regs[4], mem_before, bb0, bb1, bb2, bb3);
                if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                    uint32_t ret = *(uint32_t*)regs[4];
                    regs[4] += 4;
                    uint32_t mem_after = 0;
                    unsigned char aa0=0,aa1=0,aa2=0,aa3=0;
                    if (regs[4] >= process->stack_start && regs[4] + 4 <= process->stack_start + process->stack_size) {
                        unsigned char* ap = (unsigned char*)(regs[4]-4);
                        aa0 = ap[0]; aa1 = ap[1]; aa2 = ap[2]; aa3 = ap[3];
                        mem_after = *(uint32_t*)(regs[4]-4);
                    }
                    printf("[native_exec:trace] RET popped=%d -> jumping to %d mem_after=%d bytes=%d,%d,%d,%d\n", ret, ret, mem_after, aa0, aa1, aa2, aa3);
                    if (ret < process->code_size) pc = ret; else break;
                } else break;
            } else if (opcode == 0x39 || opcode == 0x3B) {
                // cmp r/m32, r32  (0x39) or cmp r32, r/m32 (0x3B) - only reg,reg supported
                if (pc + 1 < process->code_size) {
                    uint8_t modrm = code_ptr[pc + 1];
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t reg = (modrm >> 3) & 7;
                    uint8_t rm  = modrm & 7;
                    if (mod == 3) {
                        int32_t left = (opcode == 0x39) ? (int32_t)regs[rm] : (int32_t)regs[reg];
                        int32_t right = (opcode == 0x39) ? (int32_t)regs[reg] : (int32_t)regs[rm];
                        int32_t res = left - right;
                        // set flags
                        // store flags in regs[7] low bits? Use local static-ish; simpler: store in global vars (but avoid globals)
                        // We'll reuse regs[7] (EDI) low bits: bit0=ZF, bit1=SF
                        regs[7] &= ~0x3;
                        if (res == 0) regs[7] |= 0x1; // ZF
                        if (res < 0) regs[7] |= 0x2;  // SF
                    }
                    pc += 2;
                } else pc++;
            } else if (opcode == 0x74 || opcode == 0x75) {
                // JE (0x74) / JNE (0x75) short
                if (pc + 1 < process->code_size) {
                    int8_t rel = *(int8_t*)(code_ptr + pc + 1);
                    int ZF = (regs[7] & 0x1) != 0;
                    if ((opcode == 0x74 && ZF) || (opcode == 0x75 && !ZF)) pc = (uint32_t)((int32_t)(pc + 2) + rel);
                    else pc += 2;
                } else pc++;
            }
        } else {
            // Unknown opcode - skip
            pc++;
        }
    }
    
    // Process execution completed. Do not free resources here; centralize
    // cleanup in native_cleanup_process so both transient (execute_program)
    // runs and table-owned processes (spawn) follow the same rules and avoid
    // double-free races.
    process->active = 0;
    g_current_process = NULL;
    return EXEC_SUCCESS;
}

// Main function to execute a program
exec_result_t native_execute_program(const char* filename) {
    native_process_t process;
    exec_result_t result;
    
    // Load the program
    result = native_load_program(filename, &process);
    if (result != EXEC_SUCCESS) {
        return result;
    }
    // Mark this stack-allocated process as not owned by the global table so
    // cleanup will free its allocations when done.
    process.owned = 0;

    // Run the process
    result = native_run_process(&process);

    // Clean up transient process resources
    native_cleanup_process(&process);
    
    return result;
}

// Clean up process resources
void native_cleanup_process(native_process_t* process) {
    if (!process) return;
    
    // Cleaning up process
    
    // In a full implementation, this would also clean up file handles and
    // notify the parent. For now, free any allocated memory belonging to the
    // process. To avoid double-free when the process struct is owned by the
    // global process table (shallow-copied via native_spawn), we only free
    // memory when the process is not table-owned (owned == 0) or when the
    // pointer was separately allocated (data_start not part of code_start image).

    // Free per-segment allocations first (done below). After that, free
    // code_start only if it was not one of the segment allocations (to avoid
    // double-free).

    // Free data section if separately allocated (and owned)
    if (process->data_start) {
        int data_is_offset_in_code = 0;
        if (process->code_start && process->data_start >= process->code_start && process->data_start < process->code_start + process->code_size) {
            data_is_offset_in_code = 1;
        }
        if (!data_is_offset_in_code) {
            if (!process->owned) {
                free((void*)process->data_start);
                process->data_start = 0;
            }
        }
    }

    // Free stack if owned
    if (process->stack_start) {
        if (!process->owned) {
            free((void*)process->stack_start);
            process->stack_start = 0;
        }
    }

    // Free any per-segment allocations for ELF-loaded processes
    if (process->segment_count) {
        // remember whether code_start matched any segment mem
        int code_is_segment = 0;
        for (int s = 0; s < process->segment_count; s++) {
            if (process->segments[s].mem) {
                if ((uint32)process->segments[s].mem == process->code_start) code_is_segment = 1;
                if (!process->owned) {
                    free(process->segments[s].mem);
                }
                process->segments[s].mem = NULL;
            }
        }
        process->segment_count = 0;

        // If code_start pointed into a segment we already freed above,
        // avoid freeing it again. Otherwise free it now if we own it.
        if (process->code_start && !code_is_segment) {
            if (!process->owned) {
                free((void*)process->code_start);
            }
            process->code_start = 0;
        } else {
            process->code_start = 0;
        }
    } else {
        // No segments: free code_start normally
        if (process->code_start) {
            if (!process->owned) {
                free((void*)process->code_start);
            }
            process->code_start = 0;
        }
    }

    // Remove from global process table if present
    for (int i = 0; i < MAX_NATIVE_PROCESSES; i++) {
        if (&g_processes[i] == process) {
            // Clear record and release ownership only when removing
            memset(&g_processes[i], 0, sizeof(native_process_t));
            if (g_current_index == i) g_current_index = -1;
            break;
        }
    }

    // Clean up VMM address space if allocated (must do before memset)
    if (process->address_space) {
        extern void destroy_address_space(address_space_t* as);
        destroy_address_space(process->address_space);
        process->address_space = NULL;
    }

    process->active = 0;
    process->owned = 0;
    g_current_process = NULL;
}

// Process management functions
native_process_t* native_get_current_process(void) {
    return g_current_process;
}

void native_set_current_process(native_process_t* process) {
    g_current_process = process;
}

int native_get_process_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_NATIVE_PROCESSES; i++) {
        if (g_processes[i].active) count++;
    }
    return count;
}

int native_process_is_active(uint32 pid) {
    if (pid == 0) return 0;
    for (int i = 0; i < MAX_NATIVE_PROCESSES; i++) {
        if (g_processes[i].pid == pid) {
            return g_processes[i].active ? 1 : 0;
        }
    }
    return 0;
}

// round-robin: return next active process index or -1
static int next_active_index(int start_after) {
    for (int offset = 1; offset <= MAX_NATIVE_PROCESSES; offset++) {
        int idx = (start_after + offset) % MAX_NATIVE_PROCESSES;
        if (g_processes[idx].active) return idx;
    }
    return -1;
}

// simple spawn: load into first free slot and mark active
exec_result_t native_spawn(const char* filename, uint32* out_pid) {
    int slot = -1;
    for (int i = 0; i < MAX_NATIVE_PROCESSES; i++) {
        if (!g_processes[i].active && g_processes[i].pid == 0) { slot = i; break; }
    }
    if (slot == -1) {
        // reuse inactive slot
        for (int i = 0; i < MAX_NATIVE_PROCESSES; i++) {
            if (!g_processes[i].active) { slot = i; break; }
        }
    }
    if (slot == -1) return EXEC_ERROR_EXECUTION_FAILED;

    native_process_t temp;
    exec_result_t r = native_load_program(filename, &temp);
    if (r != EXEC_SUCCESS) return r;

    g_processes[slot] = temp; // shallow copy ok: pointers now owned by table
    g_processes[slot].owned = 1;
    if (out_pid) *out_pid = g_processes[slot].pid;
    if (g_current_index == -1) g_current_index = slot;
    return EXEC_SUCCESS;
}

void native_exit(int code) {
    (void)code;
    if (!g_current_process) return;
    // free resources
    // Cleanup will free resources if this process record is transient. For
    // table-owned processes, native_cleanup_process will clear the table entry
    // as well. Ensure pid is cleared to indicate termination.
    native_cleanup_process(g_current_process);
    g_current_process->pid = 0;
}

// scheduler hook called on timeslice end
void sched_on_timeslice_end(void) {
    if (sched_work_on_timeslice_end()) {
        return;
    }
    if (g_current_index < 0) return;
    int next = next_active_index(g_current_index);
    if (next >= 0 && next != g_current_index) {
        g_current_index = next;
        g_current_process = &g_processes[g_current_index];
        // for now, run a short step window using emulator loop
        native_run_process(g_current_process);
    }
}

// Memory management for user programs
void* native_user_alloc(uint32 size) {
    if (size == 0) return NULL;
    
    // Simple heap allocation
    uint32 ptr = g_user_heap_ptr;
    g_user_heap_ptr += size;
    
    // Check for heap overflow
    if (g_user_heap_ptr > USER_HEAP_BASE + 0x100000) { // 1MB heap limit
        // User heap overflow
        return NULL;
    }
    
    return (void*)ptr;
}

void native_user_free(void* ptr) {
    // Will implement later, no actual freeing yet.
    (void)ptr; // Suppress unused parameter warning
}

int native_validate_user_memory(uint32 addr, uint32 size) {
    // Check if address is in user space
    if (addr < USER_CODE_BASE || addr + size > USER_HEAP_BASE + 0x100000) {
        return 0;
    }
    return 1;
}