#include <shell_script.h>
#include <eynfs.h>
#include <util.h>
#include <string.h>
#include <vga.h>
#include <shell.h>
#include <shell_command_info.h>
#include <types.h>
#include <math.h>
#include <context.h>
#include <misc/sched.h>

// EYNFS constants
#define EYNFS_SUPERBLOCK_LBA 2048
// Global capture mode is defined in vga.c; declare extern here
extern int g_shell_capture_mode;

// Shell script execution context
typedef struct {
    char script_path[256];
    char current_line[512];
    int line_number;
    int error_count;
    int success_count;
    // simple variables store
    struct { char key[32]; char val[128]; } vars[16];
    int var_count;
} shell_script_context_t;

// Forward declarations
static int execute_shell_line(const char* line, shell_script_context_t* ctx);
static int parse_shell_file(const char* filename, shell_script_context_t* ctx);
static void trim_whitespace(char* str);
static int is_comment_line(const char* line);
static int is_empty_line(const char* line);
static const char* get_var(shell_script_context_t* ctx, const char* key);
static void set_var(shell_script_context_t* ctx, const char* key, const char* val);
static void expand_vars(const char* in, char* out, size_t outsz, shell_script_context_t* ctx);
static void substitute_command_outputs(const char* in, char* out, size_t outsz, shell_script_context_t* ctx);

static int script_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* cctx = current_command_context;
    if (cctx && !cap_check(cctx->caps, caps)) return 0;
    if (cctx) {
        scheduler_account(cctx->wo, cost);
        scheduler_yield_if_needed(cctx->wo);
        if (sched_det_is_enabled()) cctx->det_seq++;
    }
    return 1;
}

static void script_ctx_account(uint32 cost) {
    command_context_t* cctx = current_command_context;
    if (!cctx) return;
    scheduler_account(cctx->wo, cost);
    scheduler_yield_if_needed(cctx->wo);
    if (sched_det_is_enabled()) cctx->det_seq++;
}

// Execute a shell script file
exec_result_t execute_shell_script(const char* filename) {
    if (!script_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return EXEC_ERROR_EXECUTION_FAILED;
    shell_script_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    // copy filename to context
    safe_strcpy(ctx.script_path, filename, sizeof(ctx.script_path));
    ctx.line_number = 0;
    ctx.error_count = 0;
    ctx.success_count = 0;
    
    // Silent by default: don't print a prologue; scripts should behave like typing commands directly
    
    // Ensure we begin with a clean redirect/capture state
    extern int shell_redirect_active; if (shell_redirect_active) { stop_shell_redirect(); }
    g_shell_capture_mode = 0;

    // parse and execute the shell file
    int result = parse_shell_file(filename, &ctx);
    
    // Silent by default: no summary footer
    
    return (result == 0) ? EXEC_SUCCESS : EXEC_ERROR_EXECUTION_FAILED;
}

// Parse and execute a shell script file
static int parse_shell_file(const char* filename, shell_script_context_t* ctx) {
    if (!script_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return -1;
    // read EYNFS superblock
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cError: No supported filesystem found\n", 255, 0, 0);
        return -1;
    }
    
    // find the shell script file (support subdirectories)
    eynfs_dir_entry_t entry; uint32_t pb = 0, ei = 0;
    if (eynfs_traverse_path(0, &sb, filename, &entry, &pb, &ei) != 0) {
        // Fallback to root lookup for legacy
        if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, filename, &entry, 0) != 0) {
            printf("%cError: Shell script file not found: %s\n", 255, 0, 0, filename);
            return -1;
        }
    }
    
    // check if it's a .shell file
    const char* ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".shell") != 0) {
        printf("%cError: File must have .shell extension: %s\n", 255, 0, 0, filename);
        return -1;
    }
    
    // read the shell script file
    uint32_t size = entry.size;
    if (!script_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return -1;
    char* script_content = (char*)malloc(size + 1);
    if (!script_content) {
        printf("%cError: Memory allocation failed\n", 255, 0, 0);
        return -1;
    }
    
    int bytes_read = eynfs_read_file(0, &sb, &entry, script_content, size, 0);
    if (bytes_read < 0) {
        printf("%cError: Failed to read shell script file\n", 255, 0, 0);
        free(script_content);
        return -1;
    }
    
    script_content[bytes_read] = '\0';
    
    // parse and execute each line
    char* line_start = script_content;
    char* line_end;

    // control flow stacks: support simple ifeq/ifneq and counted loops
    int if_stack[16]; // 1=executing, 0=skipping
    int if_sp = 0;    // stack pointer
    int skipping = 0; // current skip state
    // loop support: store positions and remaining counts
    int loop_count_stack[8];
    char* loop_pos_stack[8];
    int loop_sp = 0;
    
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        // extract line
        size_t line_len = line_end - line_start;
        if (line_len >= sizeof(ctx->current_line)) {
            line_len = sizeof(ctx->current_line) - 1;
        }
        
        memcpy(ctx->current_line, line_start, line_len);
        ctx->current_line[line_len] = '\0';
        ctx->line_number++;
        if ((ctx->line_number & 0x3F) == 0) script_ctx_account(SCHED_COST_FS);
        
        // trim whitespace
        trim_whitespace(ctx->current_line);
        
        // skip empty lines and comments
        if (is_empty_line(ctx->current_line) || is_comment_line(ctx->current_line)) {
            ;
        } else {
            // handle control directives
            if (strncmp(ctx->current_line, "set ", 4) == 0) {
                // set KEY=VALUE
                const char* kv = ctx->current_line + 4;
                const char* eq = strchr(kv, '=');
                if (eq && !skipping) {
                    char key[32]; char val[160];
                    size_t klen = (size_t)(eq - kv); if (klen > sizeof(key)-1) klen = sizeof(key)-1;
                    memcpy(key, kv, klen); key[klen] = '\0'; trim_whitespace(key);
                    safe_strcpy(val, eq+1, sizeof(val)); trim_whitespace(val);
                    // First expand $VARs outside of command substitution, then apply $(...)
                    char tmp[160]; char finalv[160];
                    expand_vars(val, tmp, sizeof(tmp), ctx);
                    substitute_command_outputs(tmp, finalv, sizeof(finalv), ctx);
                    set_var(ctx, key, finalv);
                }
            } else if (strncmp(ctx->current_line, "ifeq ", 5) == 0) {
                // ifeq A B  -> compare expanded tokens
                char a[160]={0}, b[160]={0};
                const char* p = ctx->current_line + 5;
                while (*p==' ') p++;
                const char* sp = strchr(p, ' ');
                if (sp) {
                    size_t al = (size_t)(sp - p); if (al > sizeof(a)-1) al = sizeof(a)-1; memcpy(a, p, al); a[al]='\0';
                    while (*sp==' ') sp++;
                    safe_strcpy(b, sp, sizeof(b));
                    char ea[160], eb[160]; expand_vars(a, ea, sizeof(ea), ctx); expand_vars(b, eb, sizeof(eb), ctx);
                    int cond = (strcmp(ea, eb) == 0);
                    if_stack[if_sp++] = cond && !skipping; skipping = skipping || !cond;
                } else { if_stack[if_sp++] = 0; skipping = 1; }
            } else if (strncmp(ctx->current_line, "ifneq ", 6) == 0) {
                char a[160]={0}, b[160]={0};
                const char* p = ctx->current_line + 6;
                while (*p==' ') p++;
                const char* sp = strchr(p, ' ');
                if (sp) {
                    size_t al = (size_t)(sp - p); if (al > sizeof(a)-1) al = sizeof(a)-1; memcpy(a, p, al); a[al]='\0';
                    while (*sp==' ') sp++;
                    safe_strcpy(b, sp, sizeof(b));
                    char ea[160], eb[160]; expand_vars(a, ea, sizeof(ea), ctx); expand_vars(b, eb, sizeof(eb), ctx);
                    int cond = (strcmp(ea, eb) != 0);
                    if_stack[if_sp++] = cond && !skipping; skipping = skipping || !cond;
                } else { if_stack[if_sp++] = 0; skipping = 1; }
            } else if (strcmp(ctx->current_line, "else") == 0) {
                if (if_sp > 0) {
                    if_stack[if_sp-1] = !if_stack[if_sp-1];
                    // recompute skipping from stack
                    skipping = 0; for (int k=0;k<if_sp;k++){ if (!if_stack[k]) { skipping=1; break; } }
                }
            } else if (strcmp(ctx->current_line, "endif") == 0) {
                if (if_sp > 0) { if_sp--; }
                // recompute skipping
                skipping = 0; for (int k=0;k<if_sp;k++){ if (!if_stack[k]) { skipping=1; break; } }
            } else if (strncmp(ctx->current_line, "loop ", 5) == 0) {
                // loop N
                int n = 0; const char* p = ctx->current_line + 5; while (*p==' ') p++;
                // expand variables if any
                char exp[64]; expand_vars(p, exp, sizeof(exp), ctx);
                n = atoi(exp);
                if (!skipping && n > 0 && loop_sp < 8) {
                    loop_count_stack[loop_sp] = n;
                    loop_pos_stack[loop_sp] = line_end + 1; // start at the next line after loop directive
                    loop_sp++;
                }
            } else if (strcmp(ctx->current_line, "endloop") == 0) {
                if (loop_sp > 0) {
                    if (--loop_count_stack[loop_sp-1] > 0) {
                        // jump back
                        line_start = loop_pos_stack[loop_sp-1];
                        continue; // restart loop iteration without advancing line_start
                    } else {
                        loop_sp--; // finished
                    }
                }
            } else {
                // Normal command line; expand variables if not skipping
                if (!skipping) {
                    // Safety guard: ensure we're not unintentionally still in redirect mode
                    extern int shell_redirect_active; if (shell_redirect_active) { stop_shell_redirect(); }
                    char expanded[512]; expand_vars(ctx->current_line, expanded, sizeof(expanded), ctx);
                    char substituted[512]; substitute_command_outputs(expanded, substituted, sizeof(substituted), ctx);
                    // silent script execution: don't echo each expanded line
                    if (execute_shell_line(substituted, ctx) == 0) {
                        ctx->success_count++;
                    } else {
                        ctx->error_count++;
                        printf("%cError on line %d: %s\n", 255, 0, 0, ctx->line_number, substituted);
                    }
                }
            }
        }
        
        line_start = line_end + 1;
    }
    
    // handle last line if it doesn't end with newline
    if (*line_start != '\0') {
        size_t line_len = strlen(line_start);
        if (line_len >= sizeof(ctx->current_line)) {
            line_len = sizeof(ctx->current_line) - 1;
        }
        
        memcpy(ctx->current_line, line_start, line_len);
        ctx->current_line[line_len] = '\0';
        ctx->line_number++;
        
        trim_whitespace(ctx->current_line);
        
        if (!is_empty_line(ctx->current_line) && !is_comment_line(ctx->current_line)) {
            if (strncmp(ctx->current_line, "set ", 4) == 0 ||
                strncmp(ctx->current_line, "ifeq ", 5) == 0 ||
                strncmp(ctx->current_line, "ifneq ", 6) == 0 ||
                strcmp(ctx->current_line, "else") == 0 ||
                strcmp(ctx->current_line, "endif") == 0 ||
                strncmp(ctx->current_line, "loop ", 5) == 0 ||
                strcmp(ctx->current_line, "endloop") == 0) {
                // Reuse same control handling by pretending last line loop. Simpler: just ignore here; it was handled above via in-loop path
            } else if (!skipping) {
                extern int shell_redirect_active; if (shell_redirect_active) { stop_shell_redirect(); }
                char expanded[512]; expand_vars(ctx->current_line, expanded, sizeof(expanded), ctx);
                char substituted[512]; substitute_command_outputs(expanded, substituted, sizeof(substituted), ctx);
                // silent script execution: don't echo each expanded line
                if (execute_shell_line(substituted, ctx) == 0) {
                    ctx->success_count++;
                } else {
                    ctx->error_count++;
                    printf("%cError on line %d: %s\n", 255, 0, 0, ctx->line_number, substituted);
                }
            }
        }
    }
    
    free(script_content);
    return 0;
}

// Execute a single shell command line
static int execute_shell_line(const char* line, shell_script_context_t* ctx) {
    if (!line || strlen(line) == 0) {
        return 0; // empty line is success
    }
    // Ensure no redirection/capture flags are active before running a normal command
    extern int shell_redirect_active; if (shell_redirect_active) { stop_shell_redirect(); }
    extern int g_shell_capture_mode; g_shell_capture_mode = 0;
    // As a belt-and-suspenders guard, force flags to known state
    shell_redirect_active = 0;
    g_shell_capture_mode = 0;
    // Execute through the unified shell path so behavior matches interactive use
    handle_shell_command((char*)line);
    // Ensure we didn't leave capture/redirect on due to a misbehaving command
    g_shell_capture_mode = 0;
    if (shell_redirect_active) { stop_shell_redirect(); }
    return 0;
}

// trim whitespace from string
static void trim_whitespace(char* str) {
    if (!str) return;
    
    // trim leading whitespace
    char* start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\r')) {
        start++;
    }
    
    // trim trailing whitespace
    char* end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end--;
    }
    
    // move trimmed string to beginning
    if (start != str) {
        memmove(str, start, end - start + 1);
    }
    str[end - start + 1] = '\0';
}

// check if line is a comment (starts with #)
static int is_comment_line(const char* line) {
    if (!line) return 0;
    
    // skip leading whitespace
    while (*line && (*line == ' ' || *line == '\t')) {
        line++;
    }
    
    return (*line == '#');
}

// check if line is empty (only whitespace)
static int is_empty_line(const char* line) {
    if (!line) return 1;
    
    while (*line) {
        if (*line != ' ' && *line != '\t' && *line != '\r' && *line != '\n') {
            return 0;
        }
        line++;
    }
    
    return 1;
}

// variable helpers: ${KEY} expansion (also $KEY simple)
static const char* get_var(shell_script_context_t* ctx, const char* key) {
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].key, key) == 0) return ctx->vars[i].val;
    }
    return "";
}

static void set_var(shell_script_context_t* ctx, const char* key, const char* val) {
    if (!key || !*key) return;
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].key, key) == 0) { safe_strcpy(ctx->vars[i].val, val, sizeof(ctx->vars[i].val)); return; }
    }
    if (ctx->var_count < 16) {
        safe_strcpy(ctx->vars[ctx->var_count].key, key, sizeof(ctx->vars[ctx->var_count].key));
        safe_strcpy(ctx->vars[ctx->var_count].val, val, sizeof(ctx->vars[ctx->var_count].val));
        ctx->var_count++;
    } else {
        /* Script-visible error: variable store full */
        printf("%cWarning: variable table full, cannot store '%s'\n", 255, 255, 0, key);
    }
}

static void expand_vars(const char* in, char* out, size_t outsz, shell_script_context_t* ctx) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outsz; ) {
        if (in[i] == '$') {
            if (in[i+1] == '(') {
                // Preserve command substitution marker for higher-level handler
                if (o + 2 < outsz) { out[o++] = '$'; out[o++] = '('; }
                i += 2;
                continue;
            } else if (in[i+1] == '{') {
                i += 2; char key[32]; size_t k=0;
                while (in[i] && in[i] != '}' && k < sizeof(key)-1) key[k++] = in[i++];
                key[k]='\0'; if (in[i] == '}') i++;
                const char* v = get_var(ctx, key);
                for (size_t t=0; v[t] && o + 1 < outsz; t++) out[o++] = v[t];
            } else {
                i++; char key[32]; size_t k=0;
                while (in[i] && ((in[i]>='A'&&in[i]<='Z')||(in[i]>='a'&&in[i]<='z')||(in[i]>='0'&&in[i]<='9')||in[i]=='_') && k<sizeof(key)-1) key[k++]=in[i++];
                key[k]='\0'; const char* v = get_var(ctx, key);
                for (size_t t=0; v[t] && o + 1 < outsz; t++) out[o++] = v[t];
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

// Replace $(command ...) with captured output
static void substitute_command_outputs(const char* in, char* out, size_t outsz, shell_script_context_t* ctx) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outsz; ) {
        if (in[i] == '$' && in[i+1] == '(') {
            i += 2; // skip $(
            char cmd[200]; size_t c = 0;
            int depth = 1; // allow simple nesting $( ... $(..) ... )
            while (in[i] && c < sizeof(cmd)-1) {
                if (in[i] == '(') { depth++; }
                if (in[i] == ')') { depth--; if (depth == 0) { i++; break; } }
                cmd[c++] = in[i++];
            }
            cmd[c] = '\0';
            // Trim whitespace
            trim_whitespace(cmd);
            // Expand variables in cmd before executing
            char expanded_cmd[220]; expand_vars(cmd, expanded_cmd, sizeof(expanded_cmd), ctx);

            // Fast path: $(random [args]) -> compute directly without printing
            if (strncmp(expanded_cmd, "random", 6) == 0 && (expanded_cmd[6] == '\0' || expanded_cmd[6] == ' ')) {
                const char* p = expanded_cmd + 6; while (*p == ' ') p++;
                // Parse up to two integer arguments
                int have1 = 0, have2 = 0; int a = 0, b = 0; int sign = 1;
                // read first number
                if (*p == '-' ) { sign = -1; p++; }
                while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; have1 = 1; }
                a *= sign; sign = 1; while (*p == ' ') p++;
                // read optional second
                if (*p == '-' ) { sign = -1; p++; }
                while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; have2 = 1; }
                b *= sign;
                uint32_t val;
                if (!have1) {
                    val = rand_next();
                } else if (have1 && !have2) {
                    // one arg interpreted as count in command; for substitution return a single number
                    val = rand_next();
                } else {
                    int min = a, max = b; if (min > max) { int t = min; min = max; max = t; }
                    val = rand_range((uint32_t)min, (uint32_t)max);
                }
                // Write directly into out buffer
                char numbuf[16];
                snprintf(numbuf, sizeof(numbuf), "%d", (int)val);
                for (size_t t=0; numbuf[t] && o + 1 < outsz; t++) out[o++] = numbuf[t];
                continue;
            }

            // Capture output, preserving any prior redirect state
            if (!script_ctx_allow(CAP_WRITE_CONSOLE | CAP_ALLOC_MEMORY, SCHED_COST_CONSOLE)) {
                continue;
            }
            extern int shell_redirect_active;
            int was_redirecting = shell_redirect_active;
            if (!was_redirecting) start_shell_redirect();
            // Force capture mode to guarantee interception
            extern int g_shell_capture_mode; g_shell_capture_mode = 1;
            handle_shell_command(expanded_cmd);
            g_shell_capture_mode = 0;
            // Read captured buffer BEFORE stopping/clearing redirect
            extern char shell_redirect_buf[];
            char captured[200]; safe_strcpy(captured, shell_redirect_buf, sizeof(captured));
            if (!was_redirecting) {
                stop_shell_redirect();
                // Now clear buffer to avoid any late prints elsewhere
                shell_redirect_buf[0] = '\0';
            }
            // Trim both ends
            trim_whitespace(captured);
            // If output contains letters, try to extract the last integer token
            int has_alpha = 0; for (size_t z=0; captured[z]; z++){ if ((captured[z]>='A'&&captured[z]<='Z')||(captured[z]>='a'&&captured[z]<='z')){ has_alpha=1; break; } }
            if (has_alpha) {
                // Scan from end to find last number
                int len = (int)strlen(captured);
                int end = len - 1;
                // move left to last digit
                while (end >= 0 && (captured[end] < '0' || captured[end] > '9')) end--;
                if (end >= 0) {
                    int start = end;
                    while (start >= 0 && (captured[start] >= '0' && captured[start] <= '9')) start--;
                    start++;
                    // Optional minus sign directly before the number
                    if (start > 0 && captured[start-1] == '-') start--;
                    for (int t = start; t <= end && o + 1 < outsz; t++) out[o++] = captured[t];
                } else {
                    // No digits at all, fall back to original captured
                    for (size_t t = 0; captured[t] && o + 1 < outsz; t++) out[o++] = captured[t];
                }
            } else {
                for (size_t t = 0; captured[t] && o + 1 < outsz; t++) out[o++] = captured[t];
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}
