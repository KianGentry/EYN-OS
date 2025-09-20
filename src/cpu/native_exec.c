#include <native_exec.h>
#include <eynfs.h>
#include <util.h>
#include <string.h>
#include <kernel_api.h>
#include <vga.h>
#include <kb.h>

// Process management
#define MAX_NATIVE_PROCESSES 8
#define USER_CODE_BASE 0x2000000    // 32MB
#define USER_DATA_BASE 0x3000000    // 48MB  
#define USER_STACK_BASE 0x4000000   // 64MB
#define USER_HEAP_BASE  0x5000000   // 80MB

// Global process table
static native_process_t g_processes[MAX_NATIVE_PROCESSES];
static native_process_t* g_current_process = NULL;
static int g_next_pid = 1;
static uint32 g_user_heap_ptr = USER_HEAP_BASE;
static int g_current_index = -1;

// EYNFS constants
#define EYNFS_SUPERBLOCK_LBA 2048

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
    
    // Read EYNFS superblock
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        // No supported filesystem found
        return EXEC_ERROR_INVALID_FORMAT;
    }
    
    // Find file
    eynfs_dir_entry_t entry;
    if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, filename, &entry, 0) != 0) {
        // File not found
        return EXEC_ERROR_INVALID_FORMAT;
    }
    
    // Read file
    uint32_t size = entry.size;
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
    
    // Parse EYN executable header
    if (size < sizeof(struct eyn_exe_header)) {
        // File too small to be a valid EYN executable
        free(buf);
        return EXEC_ERROR_INVALID_FORMAT;
    }
    
    struct eyn_exe_header* hdr = (struct eyn_exe_header*)buf;
    if (hdr->magic[0] != 'E' || hdr->magic[1] != 'Y' || hdr->magic[2] != 'N' || hdr->magic[3] != '\0') {
        // Invalid EYN executable magic
        free(buf);
        return EXEC_ERROR_INVALID_FORMAT;
    }
    
    // EYN executable loaded
    printf("[native_exec] Loaded '%s' (code=%u, data=%u, entry_off=%u)\n", filename, hdr->code_size, hdr->data_size, hdr->entry_point);
    
    // Initialize process structure
    memset(process, 0, sizeof(native_process_t));
    process->pid = g_next_pid++;
    
    // Allocate memory for code and data sections
    process->code_start = (uint32_t)malloc(hdr->code_size);
    if (!process->code_start) {
        // Failed to allocate memory for code section
        free(buf);
        return EXEC_ERROR_MEMORY_ALLOC;
    }
    process->code_size = hdr->code_size;
    
    process->data_start = (uint32_t)malloc(hdr->data_size + 0x1000); // Extra space for safety
    if (!process->data_start) {
        // Failed to allocate memory for data section
        free((void*)process->code_start);
        free(buf);
        return EXEC_ERROR_MEMORY_ALLOC;
    }
    process->data_size = hdr->data_size;
    
    process->stack_start = (uint32_t)malloc(0x10000); // 64KB stack
    if (!process->stack_start) {
        // Failed to allocate memory for stack
        free((void*)process->code_start);
        free((void*)process->data_start);
        free(buf);
        return EXEC_ERROR_MEMORY_ALLOC;
    }
    process->stack_size = 0x10000;
    
    process->entry_point = process->code_start + hdr->entry_point;
    
    // Memory allocated and entry point calculated
    process->esp = process->stack_start + process->stack_size - 4; // Start at top of stack
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
        for (uint32_t i = 0; i < preview; i++) { printf(" %02X", code[i]); }
        printf("\n");
    }
    
    // Copy data section
    if (hdr->data_size > 0) {
        uint8_t* data = (uint8_t*)(buf + sizeof(struct eyn_exe_header) + hdr->code_size);
        memcpy((void*)process->data_start, data, hdr->data_size);
        
        // Fix up data references in code section
        // The assembler generates addresses like 0x02001001, 0x02001002, etc.
        // We need to patch these references to point to the actual data_start
        uint32_t actual_data_addr = process->data_start;
        
        // Search through code section for data references and patch them
        uint8_t* code_ptr = (uint8_t*)process->code_start;
        for (uint32_t i = 0; i < process->code_size - 4; i++) {
            uint32_t* addr_ptr = (uint32_t*)(code_ptr + i);
            uint32_t addr = *addr_ptr;
            
            // Check if this looks like a data address (0x02001000 + offset)
            if (addr >= 0x02001000 && addr < 0x02002000) {
                // Calculate the offset from the base data address
                uint32_t offset = addr - 0x02001000;
                // Patch to point to actual data_start + offset
                *addr_ptr = actual_data_addr + offset;
            }
        }
    }
    
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
    if (process->entry_point == 0 || process->entry_point < process->code_start) {
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
    uint32_t regs[8] = {0}; // eax, ecx, edx, ebx, esp, ebp, esi, edi
    
    // Simple instruction simulation for basic instructions
    uint32_t __max_steps = process->code_size * 16 + 1024;
    if (__max_steps < 2048) __max_steps = 2048;
    uint32_t __steps = 0;
    while (pc < process->code_size && __steps++ < __max_steps) { // Limit to prevent infinite loops
        uint8_t opcode = code_ptr[pc];
        
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
                    // Handle syscall
                    if (regs[0] == 1) { // WRITE
                        if (regs[3] == 1 && regs[2] > 0 && regs[2] <= 512) {
                            // stdout, reasonable length
                            uint32_t buffer_addr = regs[1];
                            if (buffer_addr >= process->data_start && 
                                buffer_addr < process->data_start + process->data_size) {
                                // Address is in data section - print the string
                                char* buffer = (char*)buffer_addr;
                                char output_buffer[101];
                                uint32_t output_pos = 0;
                                
                                for (uint32_t i = 0; i < regs[2] && output_pos < 100; i++) {
                                    if (buffer[i] >= 32 && buffer[i] <= 126) {
                                        output_buffer[output_pos++] = buffer[i];
                                    } else if (buffer[i] == '\n') {
                                        output_buffer[output_pos++] = '\n';
                                    }
                                }
                                output_buffer[output_pos] = '\0';
                                
                                // Print the complete string
                                printf("%s", output_buffer);
                                regs[0] = regs[2]; // Return bytes written
                            } else {
                                regs[0] = -1;
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
                                // Only allow writes into data section for safety
                                uint32_t buffer_addr = regs[1];
                                if (buffer_addr >= process->data_start && 
                                    buffer_addr < process->data_start + process->data_size && n >= 0) {
                                    memcpy((void*)buffer_addr, s, n);
                                    ((char*)buffer_addr)[n] = '\0';
                                    regs[0] = n;
                                } else {
                                    regs[0] = -1;
                                }
                                free(s);
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
        } else if (opcode == 0xC3) {
            // ret
            break;
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
        } else {
            // Unknown opcode - skip
            pc++;
        }
    }
    
    // Process execution completed
    
    // Clean up allocated memory
    if (process->code_start) {
        free((void*)process->code_start);
    }
    if (process->data_start) {
        free((void*)process->data_start);
    }
    if (process->stack_start) {
        free((void*)process->stack_start);
    }
    
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
    
    // Run the process
    result = native_run_process(&process);
    
    // Clean up
    native_cleanup_process(&process);
    
    return result;
}

// Clean up process resources
void native_cleanup_process(native_process_t* process) {
    if (!process) return;
    
    // Cleaning up process
    
    // In a full implementation, this would:
    // 1. Free allocated memory
    // 2. Clean up file handles
    // 3. Remove from process table
    // 4. Notify parent process
    
    process->active = 0;
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
    if (out_pid) *out_pid = g_processes[slot].pid;
    if (g_current_index == -1) g_current_index = slot;
    return EXEC_SUCCESS;
}

void native_exit(int code) {
    (void)code;
    if (!g_current_process) return;
    // free resources
    native_cleanup_process(g_current_process);
    g_current_process->pid = 0;
}

// scheduler hook called on timeslice end
void sched_on_timeslice_end(void) {
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
    // Simple implementation - no actual freeing for now
    // In a full implementation, this would maintain a free list
    (void)ptr; // Suppress unused parameter warning
}

int native_validate_user_memory(uint32 addr, uint32 size) {
    // Check if address is in user space
    if (addr < USER_CODE_BASE || addr + size > USER_HEAP_BASE + 0x100000) {
        return 0;
    }
    return 1;
}