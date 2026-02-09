#include <string.h>
#include <stdlib.h>
#include <util.h>
#include <vga.h>
#include <fs_commands.h>
#include <eyn_exe_format.h>
#include <types.h>
#include <eynfs.h>
#include <assemble.h>
#include <utilities/linker.h>
#include <context.h>
#include <misc/sched.h>
int g_asm_verbose = 0;
#include <shell_command_info.h>

// NOTE: The kernel shell environment may not have a usable heap (malloc/free).
// Keep the assembler heapless by using fixed static work buffers.
// Keep sizes small to avoid .bss bloat that can conflict with heap placement.
#define ASM_MAX_SRC_SIZE   4096u
#define ASM_ARENA_MAX      16384u
#define ASM_MAX_CODE_SIZE  4096u
#define ASM_MAX_DATA_SIZE  4096u
#define ASM_MAX_SYMBOLS    64u

static char g_asm_src_buf[ASM_MAX_SRC_SIZE + 1];
static uint8_t g_asm_ast_arena[ASM_ARENA_MAX];
static AST g_asm_static_ast;
static uint8_t g_asm_code_buf[ASM_MAX_CODE_SIZE];
static uint8_t g_asm_data_buf[ASM_MAX_DATA_SIZE];
static SymbolTableEntry g_asm_sym_pool[ASM_MAX_SYMBOLS];
static size_t g_asm_sym_pool_used = 0;

// Simple arena allocator to reduce fragmentation during assembly parsing
typedef struct {
    uint8_t* buf;
    size_t size;
    size_t used;
} asm_arena_t;

static void arena_init(asm_arena_t* a, uint8_t* buf, size_t size) {
    a->buf = buf; a->size = size; a->used = 0;
}
static void* arena_alloc(asm_arena_t* a, size_t n) {
    if (!a || !a->buf) return NULL;
    size_t aligned = (n + 7) & ~((size_t)7);
    if (aligned == 0) aligned = 8;
    if (a->used + aligned > a->size) return NULL;
    void* p = a->buf + a->used;
    a->used += aligned;
    return p;
}

// Helper to count bytes for a db directive value string, handling quotes, escapes, and numbers
static int count_db_bytes(const char* s) {
    int count = 0;
    const char* p = s;
    while (*p) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            // String literal: count characters until closing quote, handling escapes
            p++; // skip opening quote
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    // Escape sequence counts as 1 byte
                    p += 2;
                } else {
                    p++;
                }
                count++;
            }
            if (*p == '"') p++; // skip closing quote
        } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            // Hex literal -> 1 byte
            count += 1;
            // advance to next comma or end
            while (*p && *p != ',') p++;
        } else {
            // Decimal or token -> 1 byte
            count += 1;
            while (*p && *p != ',') p++;
        }
        if (*p == ',') p++;
    }
    return count;
}

// Custom hex parser to avoid strtol dependency issues
static int parse_hex(const char* str) {
    int result = 0;
    if (strncmp(str, "0x", 2) == 0) {
        str += 2; // Skip "0x"
    }
    
    while (*str) {
        char c = *str++;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            break; // Invalid hex character
        }
        result = (result << 4) | digit;
    }
    return result;
}

void handler_assemble(string arg);
#define EYNFS_SUPERBLOCK_LBA 2048
extern uint8_t g_current_drive;

static int g_asm_error_count = 0;
static int g_asm_warning_count = 0;

static int asm_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void asm_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Colored error/warning printing ---
void print_error(const char* file, int line, const char* msg, const char* line_text) {
    if (!asm_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) return;
    g_asm_error_count++;
    // Bright red: 255,0,0
    printf("\n");
    printf("[error] %s:%d: ", file, line);
    vga_set_color(255,0,0);
    printf("%s\n", msg);
    vga_set_color(255,255,255);
    if (line_text) {
        printf("    %s\n", line_text);
    }
}

void print_warning(const char* file, int line, const char* msg, const char* line_text) {
    if (!asm_ctx_allow(CAP_WRITE_CONSOLE, SCHED_COST_CONSOLE)) return;
    g_asm_warning_count++;
    // Pink: 255,105,180
    printf("\n");
    printf("[warning] %s:%d: ", file, line);
    vga_set_color(255,105,180);
    printf("%s\n", msg);
    vga_set_color(255,255,255);
    if (line_text) {
        printf("    %s\n", line_text);
    }
}

void lexer_init(Lexer *lexer, const char *src) {
    lexer->src = src;
    lexer->pos = 0;
    lexer->has_pushback = 0;
}

static void lexer_unget_token(Lexer* lexer, Token token) {
    if (!lexer) return;
    lexer->has_pushback = 1;
    lexer->pushback = token;
}

Token lexer_next_token(Lexer *lexer) {
    if (lexer->has_pushback) {
        lexer->has_pushback = 0;
        return lexer->pushback;
    }
    Token token = {TOKEN_EOF, ""};
    const char* src = lexer->src;
    size_t len = strlen(src);
    size_t pos = lexer->pos;

    // Skip whitespace
    while (pos < len && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r')) pos++;

    // Skip comments
    if (pos < len && (src[pos] == ';' || src[pos] == '#')) {
        while (pos < len && src[pos] != '\n') pos++;
    }

    // Newline
    if (pos < len && src[pos] == '\n') {
        token.type = TOKEN_NEWLINE;
        token.text[0] = '\n';
        token.text[1] = 0;
        lexer->pos = pos + 1;
        return token;
    }

    // End of input
    if (pos >= len || src[pos] == 0) {
        token.type = TOKEN_EOF;
        token.text[0] = 0;
        lexer->pos = pos;
        return token;
    }

    // Comma
    if (src[pos] == ',') {
        token.type = TOKEN_COMMA;
        token.text[0] = ',';
        token.text[1] = 0;
        lexer->pos = pos + 1;
        return token;
    }

    // String literal in double quotes: "..."
    if (src[pos] == '"') {
        size_t start = pos;
        pos++; // skip opening quote
        while (pos < len && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < len) {
                pos += 2; // skip escape sequence
            } else {
                pos++;
            }
        }
        if (pos < len && src[pos] == '"') pos++; // include closing quote
        size_t slen = pos - start;
        if (slen >= sizeof(token.text)) slen = sizeof(token.text) - 1;
        memcpy(token.text, (char*)src + start, slen);
        token.text[slen] = 0;
        token.type = TOKEN_IMMEDIATE; // Treat string as an immediate value for db
        lexer->pos = pos;
        return token;
    }

    // Memory operand in square brackets: [ ... ]
    if (src[pos] == '[') {
        size_t start = pos;
        pos++; // skip '['
        while (pos < len && src[pos] != ']') pos++;
        if (pos < len && src[pos] == ']') pos++; // include ']'
        size_t mlen = pos - start;
        if (mlen >= sizeof(token.text)) mlen = sizeof(token.text) - 1;
        memcpy(token.text, (char*)src + start, mlen);
        token.text[mlen] = 0;
        token.type = TOKEN_MEMORY;
        lexer->pos = pos;
        return token;
    }

    // Identifiers, labels, mnemonics, registers, directives, section, size keywords
    if ((src[pos] >= 'A' && src[pos] <= 'Z') || (src[pos] >= 'a' && src[pos] <= 'z') || src[pos] == '_' || src[pos] == '.') {
        size_t start = pos;
        while (pos < len && ((src[pos] >= 'A' && src[pos] <= 'Z') || (src[pos] >= 'a' && src[pos] <= 'z') || (src[pos] >= '0' && src[pos] <= '9') || src[pos] == '.' || src[pos] == '_')) pos++;
        size_t id_len = pos - start;
        if (id_len >= sizeof(token.text)) id_len = sizeof(token.text) - 1;
        memcpy(token.text, (char*)src + start, id_len);
        token.text[id_len] = 0;
        // Size keywords
        if (!strcmp(token.text, "byte") || !strcmp(token.text, "word") || !strcmp(token.text, "dword")) {
            token.type = TOKEN_SIZE;
            lexer->pos = pos;
            return token;
        }
        // Label (if followed by ':')
        if (src[pos] == ':') {
            token.type = TOKEN_LABEL;
            lexer->pos = pos + 1;
            return token;
        }
        // Section (also accept GAS-style .section)
        if (strcmp(token.text, "section") == 0 || strcmp(token.text, ".section") == 0) {
            token.type = TOKEN_SECTION;
            strcpy(token.text, "section");
            lexer->pos = pos;
            return token;
        }
        // Register - check against full register set
        if (is_valid_register(token.text)) {
            token.type = TOKEN_REGISTER;
            lexer->pos = pos;
            return token;
        }
        // Mnemonic - check against full instruction set
        if (is_valid_instruction(token.text)) {
            token.type = TOKEN_MNEMONIC;
            lexer->pos = pos;
            return token;
        }
        // Directive (with minimal GAS compat)
        if (strcmp(token.text, ".global") == 0) {
            token.type = TOKEN_DIRECTIVE;
            strcpy(token.text, "global");
            lexer->pos = pos;
            return token;
        }
        if (strcmp(token.text, ".ascii") == 0) {
            token.type = TOKEN_DIRECTIVE;
            strcpy(token.text, "db");
            lexer->pos = pos;
            return token;
        }
        if (strcmp(token.text, ".intel_syntax") == 0) {
            token.type = TOKEN_DIRECTIVE;
            strcpy(token.text, "intel_syntax");
            lexer->pos = pos;
            return token;
        }

        const char* dirs[] = {"db","dw","dd","resb","resw","resd","align","global"};
        for (int i = 0; i < 8; i++) {
            if (strcmp(token.text, dirs[i]) == 0) {
                token.type = TOKEN_DIRECTIVE;
                lexer->pos = pos;
                return token;
            }
        }
        // Otherwise, treat as unknown identifier
        token.type = TOKEN_UNKNOWN;
        lexer->pos = pos;
        return token;
    }

    // Numbers (immediates)
    if (src[pos] == '-' && (pos + 1) < len && ((src[pos+1] >= '0' && src[pos+1] <= '9'))) {
        size_t start = pos;
        pos++; // consume '-'
        if (pos < len && src[pos] == '0' && (pos + 1) < len && (src[pos+1] == 'x' || src[pos+1] == 'X')) {
            pos += 2;
            while (pos < len && ((src[pos] >= '0' && src[pos] <= '9') || (src[pos] >= 'a' && src[pos] <= 'f') || (src[pos] >= 'A' && src[pos] <= 'F'))) pos++;
        } else {
            while (pos < len && (src[pos] >= '0' && src[pos] <= '9')) pos++;
        }
        size_t num_len = pos - start;
        if (num_len >= sizeof(token.text)) num_len = sizeof(token.text) - 1;
        memcpy(token.text, (char*)src + start, num_len);
        token.text[num_len] = 0;
        token.type = TOKEN_IMMEDIATE;
        lexer->pos = pos;
        return token;
    }
    if (src[pos] >= '0' && src[pos] <= '9') {
        size_t start = pos;
        if (src[pos] == '0' && (src[pos+1] == 'x' || src[pos+1] == 'X')) {
            pos += 2;
            while (pos < len && ((src[pos] >= '0' && src[pos] <= '9') || (src[pos] >= 'a' && src[pos] <= 'f') || (src[pos] >= 'A' && src[pos] <= 'F'))) pos++;
        } else {
            while (pos < len && (src[pos] >= '0' && src[pos] <= '9')) pos++;
        }
        size_t num_len = pos - start;
        if (num_len >= sizeof(token.text)) num_len = sizeof(token.text) - 1;
        memcpy(token.text, (char*)src + start, num_len);
        token.text[num_len] = 0;
        token.type = TOKEN_IMMEDIATE;
        lexer->pos = pos;
        return token;
    }

    // Unknown/invalid
    token.type = TOKEN_UNKNOWN;
    token.text[0] = src[pos];
    token.text[1] = 0;
    lexer->pos = pos + 1;
    return token;
}

// AST appenders ---
void add_instruction(AST* ast, Instruction* inst) {
    inst->next = 0;
    if (!ast->instructions) {
        ast->instructions = inst;
    } else {
        Instruction* cur = ast->instructions;
        while (cur->next) cur = cur->next;
        cur->next = inst;
    }
}

void add_label(AST* ast, Label* label) {
    label->next = 0;
    if (!ast->labels) {
        ast->labels = label;
    } else {
        Label* cur = ast->labels;
        while (cur->next) cur = cur->next;
        cur->next = label;
    }
}

void add_data_def(AST* ast, DataDef* def) {
    def->next = 0;
    if (!ast->data_defs) {
        ast->data_defs = def;
    } else {
        DataDef* cur = ast->data_defs;
        while (cur->next) cur = cur->next;
        cur->next = def;
    }
}

// Code Generator ---
// Runner mapping assumptions (must match run_command.c):
// - Code is loaded at USER_CODE_ADDR
// - Data (if any) is placed at USER_CODE_ADDR + 0x1000
// We emit absolute addresses for labels by assuming code base = 0, data base = 0x1000 relative to code base.
// Must match run_command.c (USER_CODE_ADDR and data at code+0x1000)
static uint32 g_asm_code_base = 0x02000000u;      // runtime code base (.eyn default)
static uint32 g_asm_data_base = 0x02001000u;      // runtime data base (.eyn default)

static inline uint32 asm_align_up_u32(uint32 v, uint32 a) {
    if (a == 0) return v;
    return (v + a - 1u) & ~(a - 1u);
}
// Helper: get register encoding (for mov/add)
static int reg_encoding(const char* reg) {
    return get_register_encoding(reg);
}
// Helper: 8-bit reg encoding
static int reg8_encoding(const char* reg) {
    return get_register8_encoding(reg);
}

// Helper: parse immediate (decimal or hex)
static int parse_imm(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        int val = 0;
        for (int i = 2; s[i]; i++) {
            char c = s[i];
            val *= 16;
            if (c >= '0' && c <= '9') val += c - '0';
            else if (c >= 'a' && c <= 'f') val += 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') val += 10 + (c - 'A');
        }
        return val;
    } else {
        return str_to_int((char*)s);
    }
}

static int count_csv_values(const char* s) {
    if (!s) return 0;
    int count = 0;
    const char* p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        count++;
        while (*p && *p != ',') p++;
    }
    return count;
}

// Compute ModR/M (+ optional SIB + displacement) bytes for a memory operand.
// Mirrors emit_ea() decision-making.
static int ea_encoded_bytes(int base, int index, int scale, int has_disp, int disp, int is_abs) {
    (void)scale;
    if (is_abs) {
        // ModR/M + disp32
        return 1 + 4;
    }
    int need_sib = (index >= 0) || (base == 4);
    int disp_size = 0;
    if (!has_disp) {
        if (base == 5) {
            // special case: [ebp] with mod=00 encodes as disp32; we use mod=01 disp8=0
            disp_size = 1;
        }
    } else {
        if (disp >= -128 && disp <= 127) disp_size = 1;
        else disp_size = 4;
    }
    return 1 + (need_sib ? 1 : 0) + disp_size;
}

// Sizing-only parser for a memory token. Any unknown identifier is treated as absolute disp32.
static void parse_memory_operand_syntax(const char* mem_token,
                                        int* base_reg, int* index_reg, int* scale,
                                        int* has_disp, int* disp,
                                        int* is_abs) {
    *base_reg = -1; *index_reg = -1; *scale = 1; *has_disp = 0; *disp = 0; *is_abs = 0;
    if (!mem_token || mem_token[0] != '[') return;

    char expr[80]; int elen = 0;
    for (int i = 1; mem_token[i] && mem_token[i] != ']'; i++) {
        if (elen < (int)sizeof(expr) - 1) expr[elen++] = mem_token[i];
    }
    expr[elen] = 0;

    int sign = 1;
    char token[40];
    int tpos = 0;
    for (int i = 0; ; i++) {
        char c = expr[i];
        if (c == ' ' || c == '\t') continue;
        if (c == '+' || c == '-' || c == 0) {
            if (tpos > 0) {
                token[tpos] = 0;
                const char* star = strchr(token, '*');
                if (star) {
                    char l[16]={0}, rhs[16]={0};
                    int ln = (int)(star - token); if (ln > 15) ln = 15;
                    memcpy(l, token, ln); l[ln] = 0;
                    strncpy(rhs, star+1, sizeof(rhs)-1);
                    int rcode = get_register_encoding(l);
                    int sc = parse_imm(rhs);
                    if (rcode >= 0) {
                        if (sc==1||sc==2||sc==4||sc==8) { *index_reg = rcode; *scale = sc; }
                    }
                } else {
                    int rcode = get_register_encoding(token);
                    if (rcode >= 0) {
                        if (*base_reg < 0) *base_reg = rcode;
                        else if (*index_reg < 0 && rcode != 4) *index_reg = rcode;
                    } else {
                        // number or identifier; identifiers are treated as absolute disp32
                        int is_number = 0;
                        if ((token[0] >= '0' && token[0] <= '9') ||
                            (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) ||
                            (token[0] == '-' && token[1] >= '0' && token[1] <= '9') ||
                            (token[0] == '-' && token[1] == '0' && (token[2] == 'x' || token[2] == 'X'))) {
                            is_number = 1;
                        }
                        if (!is_number) {
                            *is_abs = 1;
                        } else {
                            int val = parse_imm(token);
                            *disp += sign * val; *has_disp = 1;
                        }
                    }
                }
                tpos = 0;
            }
            if (c == 0) break;
            sign = (c == '+') ? 1 : -1;
        } else {
            if (tpos < (int)sizeof(token)-1) token[tpos++] = c;
        }
    }

    if (*is_abs && (*base_reg >= 0 || *index_reg >= 0)) {
        *is_abs = 0;
    }
}

// Parse memory operand token like "[eax]", "[ebx+4]", "[0x2000]", or "[label]"
// Outputs:
//  - base_reg: register code 0-7 or -1 if absolute
//  - has_disp/disp: displacement if present for [reg+disp]
//  - is_abs/abs_addr: for [imm32] or [label]
// Parse [base + index*scale + disp] and [label] or [imm32].
static void parse_memory_operand(const char* mem_token, SymbolTable* table,
                                 int* base_reg, int* index_reg, int* scale,
                                 int* has_disp, int* disp,
                                 int* is_abs, int* abs_addr) {
    *base_reg = -1; *index_reg = -1; *scale = 1; *has_disp = 0; *disp = 0; *is_abs = 0; *abs_addr = 0;
    if (!mem_token || mem_token[0] != '[') return;
    // Extract inside brackets
    char expr[80]; int elen = 0;
    for (int i = 1; mem_token[i] && mem_token[i] != ']'; i++) {
        if (elen < (int)sizeof(expr) - 1) expr[elen++] = mem_token[i];
    }
    expr[elen] = 0;

    // Tokenize by + and -
    int sign = 1; char token[40]; int tpos = 0;
    for (int i = 0; ; i++) {
        char c = expr[i];
        if (c == ' ' || c == '\t') continue;
        if (c == '+' || c == '-' || c == 0) {
            if (tpos > 0) {
                token[tpos] = 0;
                // reg*scale or reg or number/label
                const char* star = strchr(token, '*');
                if (star) {
                    char l[16]={0}, rhs[16]={0};
                    int ln=(int)(star-token); if (ln>15) ln=15; memcpy(l, token, ln); l[ln]=0;
                    strncpy(rhs, star+1, sizeof(rhs)-1);
                    int rcode = get_register_encoding(l);
                    int sc = parse_imm(rhs);
                    if (rcode >= 0) {
                        if (sc==1||sc==2||sc==4||sc==8) { *index_reg = rcode; *scale = sc; }
                    }
                } else {
                    int rcode = get_register_encoding(token);
                    if (rcode >= 0) {
                        if (*base_reg < 0) *base_reg = rcode; else if (*index_reg < 0 && rcode != 4) *index_reg = rcode; // avoid ESP as index
                    } else {
                        // numeric or label
                        int addr = lookup_label(table, token, SECTION_DATA);
                        if (addr < 0) addr = lookup_label(table, token, SECTION_TEXT);
                        if (addr >= 0) {
                            printf("[parse_mem] label '%s' -> addr 0x%X\n", token, addr);
                            *is_abs = 1; *abs_addr = addr;
                        } else {
                            int val = parse_imm(token);
                            *disp += sign * val; *has_disp = 1;
                        }
                    }
                }
                tpos = 0;
            }
            if (c == 0) break;
            sign = (c == '+') ? 1 : -1;
        } else {
            if (tpos < (int)sizeof(token)-1) token[tpos++] = c;
        }
    }

    // If absolute detected with any reg/index, drop absolute (unsupported combo)
    if (*is_abs && (*base_reg >= 0 || *index_reg >= 0)) {
        *is_abs = 0; // fallback: treat as disp only with base if provided
    }
}

// Emit ModR/M (+SIB/disp) for r/m32 addressing.
static void emit_ea(uint8_t **code, int *code_pos, int text_bytes,
                    int reg_field, int base, int index, int scale,
                    int has_disp, int disp, int is_abs, int abs_addr) {
    if (is_abs) {
        (*code)[(*code_pos)++] = 0x00 | (reg_field<<3) | 0x05; // [disp32]
        (*code)[(*code_pos)++] = abs_addr & 0xFF;
        (*code)[(*code_pos)++] = (abs_addr>>8) & 0xFF;
        (*code)[(*code_pos)++] = (abs_addr>>16) & 0xFF;
        (*code)[(*code_pos)++] = (abs_addr>>24) & 0xFF;
        return;
    }
    int need_sib = (index >= 0) || (base == 4);
    int mod = 0; int disp_size = 0;
    if (!has_disp) {
        if (base == 5) { mod = 1; disp_size = 1; disp = 0; }
        else mod = 0;
    } else {
        if (disp >= -128 && disp <= 127) { mod = 1; disp_size = 1; }
        else { mod = 2; disp_size = 4; }
    }
    if (need_sib) {
        (*code)[(*code_pos)++] = (mod<<6) | (reg_field<<3) | 0x04; // r/m = 100
        int ss = (scale==1?0: scale==2?1: scale==4?2: 3);
        int idx = (index >=0 && index != 4) ? index : 4; // 4 indicates none
        int bas = (base >= 0) ? base : 5;
        (*code)[(*code_pos)++] = (ss<<6) | (idx<<3) | bas;
    } else {
        (*code)[(*code_pos)++] = (mod<<6) | (reg_field<<3) | (base<0?5:base);
    }
    if (disp_size == 1) {
        (*code)[(*code_pos)++] = (uint8_t)disp;
    } else if (disp_size == 4) {
        (*code)[(*code_pos)++] = disp & 0xFF;
        (*code)[(*code_pos)++] = (disp>>8) & 0xFF;
        (*code)[(*code_pos)++] = (disp>>16) & 0xFF;
        (*code)[(*code_pos)++] = (disp>>24) & 0xFF;
    }
}

// Emit EA for 8-bit forms uses same ModR/M/SIB as 32-bit; kept separate for clarity if future differs
static void emit_ea8(uint8_t **code, int *code_pos, int text_bytes,
                    int reg_field, int base, int index, int scale,
                    int has_disp, int disp, int is_abs, int abs_addr) {
    emit_ea(code, code_pos, text_bytes, reg_field, base, index, scale, has_disp, disp, is_abs, abs_addr);
}

// Map ALU opcodes for r/m32,r32 and r32,r/m32 and imm group ext.
static void get_alu_op(const char* m, uint8_t* rm_r, uint8_t* r_rm, int* ext) {
    *rm_r = 0; *r_rm = 0; *ext = -1;
    if (!strcmp(m, "add")) { *rm_r=0x01; *r_rm=0x03; *ext=0; }
    else if (!strcmp(m, "or")) { *rm_r=0x09; *r_rm=0x0B; *ext=1; }
    else if (!strcmp(m, "and")) { *rm_r=0x21; *r_rm=0x23; *ext=4; }
    else if (!strcmp(m, "sub")) { *rm_r=0x29; *r_rm=0x2B; *ext=5; }
    else if (!strcmp(m, "xor")) { *rm_r=0x31; *r_rm=0x33; *ext=6; }
    else if (!strcmp(m, "cmp")) { *rm_r=0x39; *r_rm=0x3B; *ext=7; }
}

// Map Jcc to short and near opcodes.
static int get_jcc_codes(const char* m, uint8_t* short_opc, uint8_t* near_opc) {
    struct { const char* n; uint8_t s; uint8_t n2; } t[] = {
        {"je",  0x74, 0x84}, {"jz",  0x74, 0x84},
        {"jne", 0x75, 0x85}, {"jnz", 0x75, 0x85},
        {"ja",  0x77, 0x87}, {"jnbe",0x77, 0x87},
        {"jae", 0x73, 0x83}, {"jnb", 0x73, 0x83},
        {"jb",  0x72, 0x82}, {"jnae",0x72, 0x82},
        {"jbe", 0x76, 0x86}, {"jna", 0x76, 0x86},
        {"jg",  0x7F, 0x8F}, {"jnle",0x7F, 0x8F},
        {"jge", 0x7D, 0x8D}, {"jnl", 0x7D, 0x8D},
        {"jl",  0x7C, 0x8C}, {"jnge",0x7C, 0x8C},
        {"jle", 0x7E, 0x8E}, {"jng", 0x7E, 0x8E},
        {"js",  0x78, 0x88}, {"jns", 0x79, 0x89},
        {"jo",  0x70, 0x80}, {"jno", 0x71, 0x81},
        {"jp",  0x7A, 0x8A}, {"jpe", 0x7A, 0x8A},
        {"jnp", 0x7B, 0x8B}, {"jpo", 0x7B, 0x8B},
    };
    for (unsigned i=0;i<sizeof(t)/sizeof(t[0]);i++) {
        if (!strcmp(m, t[i].n)) { *short_opc = t[i].s; *near_opc = t[i].n2; return 1; }
    }
    return 0;
}

// Helper function to check for buffer overflow
static int check_code_overflow(int code_pos, int buffer_size) {
    if (code_pos >= buffer_size) {
        printf("[codegen] Code buffer overflow! Generated %d bytes, but estimated limit was %d bytes.\n", code_pos, buffer_size);
        return 1;
    }
    return 0;
}

// Helper function to check for data buffer overflow
static int check_data_overflow(int data_pos, int buffer_size) {
    if (data_pos >= buffer_size) {
        printf("[codegen] Data buffer overflow! Generated %d bytes, but estimated limit was %d bytes.\n", data_pos, buffer_size);
        return 1;
    }
    return 0;
}

// Helper: build a simple label table with byte offsets
static int estimate_instr_size(const Instruction* inst) {
    if (!inst) return 1;

    // We intentionally force fixed-size encodings for branches (always near rel32)
    // so label address computation stays stable.

    if (!strcmp(inst->mnemonic, "mov")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) return 5;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_LABEL) return 5;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_REGISTER) return 2;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[1].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        if (inst->operands[0].type == OPERAND_MEMORY && inst->operands[1].type == OPERAND_REGISTER) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "movzx") || !strcmp(inst->mnemonic, "movsx")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_REGISTER) return 3;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[1].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 2 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "lea")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[1].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "imul")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_REGISTER) return 3;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[1].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 2 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "mul") || !strcmp(inst->mnemonic, "div") || !strcmp(inst->mnemonic, "idiv") ||
        !strcmp(inst->mnemonic, "neg") || !strcmp(inst->mnemonic, "not")) {
        if (inst->operands[0].type == OPERAND_REGISTER) return 2;
        if (inst->operands[0].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "add") || !strcmp(inst->mnemonic, "sub") || !strcmp(inst->mnemonic, "and") ||
        !strcmp(inst->mnemonic, "or")  || !strcmp(inst->mnemonic, "xor") || !strcmp(inst->mnemonic, "cmp")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_REGISTER) return 2;
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[1].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        if (inst->operands[0].type == OPERAND_MEMORY && inst->operands[1].type == OPERAND_REGISTER) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        if ((inst->operands[0].type == OPERAND_REGISTER || inst->operands[0].type == OPERAND_MEMORY) &&
            inst->operands[1].type == OPERAND_IMMEDIATE) {
            int imm = parse_imm(inst->operands[1].value);
            int use_imm8 = (imm >= -128 && imm <= 127) ? 1 : 0;
            int ea = 0;
            if (inst->operands[0].type == OPERAND_REGISTER) ea = 1; // ModR/M
            else {
                int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
                parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
                ea = ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
            }
            return 1 + ea + (use_imm8 ? 1 : 4);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "shl") || !strcmp(inst->mnemonic, "shr")) {
        if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) return 3;
        return 1;
    }

    if (!strcmp(inst->mnemonic, "jmp")) {
        if (inst->operands[0].type == OPERAND_LABEL) return 5;
        if (inst->operands[0].type == OPERAND_REGISTER) return 2;
        if (inst->operands[0].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "call")) {
        if (inst->operands[0].type == OPERAND_LABEL) return 5;
        if (inst->operands[0].type == OPERAND_REGISTER) return 2;
        if (inst->operands[0].type == OPERAND_MEMORY) {
            int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0;
            parse_memory_operand_syntax(inst->operands[0].value, &base,&index,&scale,&has_disp,&disp,&is_abs);
            return 1 + ea_encoded_bytes(base,index,scale,has_disp,disp,is_abs);
        }
        return 1;
    }

    if (!strcmp(inst->mnemonic, "je") || !strcmp(inst->mnemonic, "jz") ||
        !strcmp(inst->mnemonic, "jne") || !strcmp(inst->mnemonic, "jnz") ||
        !strcmp(inst->mnemonic, "ja") || !strcmp(inst->mnemonic, "jnbe") ||
        !strcmp(inst->mnemonic, "jae") || !strcmp(inst->mnemonic, "jnb") ||
        !strcmp(inst->mnemonic, "jb") || !strcmp(inst->mnemonic, "jnae") ||
        !strcmp(inst->mnemonic, "jbe") || !strcmp(inst->mnemonic, "jna") ||
        !strcmp(inst->mnemonic, "jg") || !strcmp(inst->mnemonic, "jnle") ||
        !strcmp(inst->mnemonic, "jge") || !strcmp(inst->mnemonic, "jnl") ||
        !strcmp(inst->mnemonic, "jl") || !strcmp(inst->mnemonic, "jnge") ||
        !strcmp(inst->mnemonic, "jle") || !strcmp(inst->mnemonic, "jng") ||
        !strcmp(inst->mnemonic, "js") || !strcmp(inst->mnemonic, "jns") ||
        !strcmp(inst->mnemonic, "jo") || !strcmp(inst->mnemonic, "jno") ||
        !strcmp(inst->mnemonic, "jp") || !strcmp(inst->mnemonic, "jpe") ||
        !strcmp(inst->mnemonic, "jnp") || !strcmp(inst->mnemonic, "jpo")) {
        return 6; // 0F 8x rel32
    }

    if (!strcmp(inst->mnemonic, "ret")) return 1;
    if (!strcmp(inst->mnemonic, "int")) return 2;
    if (!strcmp(inst->mnemonic, "push")) {
        if (inst->operands[0].type == OPERAND_REGISTER) return 1;
        if (inst->operands[0].type == OPERAND_IMMEDIATE) return 5;
        return 1;
    }
    if (!strcmp(inst->mnemonic, "pop")) return 1;
    if (!strcmp(inst->mnemonic, "inc") || !strcmp(inst->mnemonic, "dec") ||
        !strcmp(inst->mnemonic, "nop") || !strcmp(inst->mnemonic, "hlt") ||
        !strcmp(inst->mnemonic, "cli") || !strcmp(inst->mnemonic, "sti")) return 1;

    return 1;
}

static void add_symbol(SymbolTable* table, const char* name, SectionType section, int address) {
    if (!table) return;
    if (g_asm_sym_pool_used >= ASM_MAX_SYMBOLS) {
        // No heap available; fail silently to avoid corrupting memory.
        // Codegen will likely error later due to missing labels.
        return;
    }
    SymbolTableEntry* e = &g_asm_sym_pool[g_asm_sym_pool_used++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name)-1);
    e->name[sizeof(e->name)-1] = 0;
    e->section = section;
    e->address = address;
    e->next = table->head;
    table->head = e;
}

static void build_label_addresses(AST* ast, SymbolTable* table, int* out_text_size, int* out_data_size) {
    int text_bytes = 0;
    int data_bytes = 0;
    // First, estimate text size and assign label addresses tied to instruction indices
    Instruction* inst = ast->instructions;
    int inst_index = 0;
    while (inst) {
        if (inst->section == SECTION_TEXT) {
            // Assign any labels that point to this instruction index
            Label* lbl_it = ast->labels;
            while (lbl_it) {
                if (lbl_it->section == SECTION_TEXT && lbl_it->instr_index == inst_index) {
                    add_symbol(table, lbl_it->name, SECTION_TEXT, (int)g_asm_code_base + text_bytes);
                }
                lbl_it = lbl_it->next;
            }
            text_bytes += estimate_instr_size(inst);
            inst_index++;
        }
        inst = inst->next;
    }
    // Also assign labels that point exactly at end of text (if any)
    Label* lbl_tail = ast->labels;
    while (lbl_tail) {
        if (lbl_tail->section == SECTION_TEXT && lbl_tail->instr_index == inst_index) {
            add_symbol(table, lbl_tail->name, SECTION_TEXT, (int)g_asm_code_base + text_bytes);
        }
        lbl_tail = lbl_tail->next;
    }

    DataDef* def = ast->data_defs;
    int data_index = 0;
    while (def) {
        int added = 0;
        if (!strcmp(def->directive, "db")) added = count_db_bytes(def->value);
        else if (!strcmp(def->directive, "dw")) { int n = count_csv_values(def->value); if (n <= 0) n = 1; added = 2 * n; }
        else if (!strcmp(def->directive, "dd")) { int n = count_csv_values(def->value); if (n <= 0) n = 1; added = 4 * n; }
        else if (!strcmp(def->directive, "resb")) { added = parse_imm(def->value); }
        else if (!strcmp(def->directive, "resw")) { added = parse_imm(def->value) * 2; }
        else if (!strcmp(def->directive, "resd")) { added = parse_imm(def->value) * 4; }
        else if (!strcmp(def->directive, "align")) {
            int a = parse_imm(def->value); if (a<=0) a=1; int rem = data_bytes % a; added = (rem==0)?0:(a-rem);
        }
        // Assign any labels that point to this data index
        Label* lbl2 = ast->labels;
        while (lbl2) {
            if (lbl2->section == SECTION_DATA && lbl2->instr_index == data_index) {
                add_symbol(table, lbl2->name, SECTION_DATA, (int)g_asm_data_base + data_bytes);
            }
            lbl2 = lbl2->next;
        }
        data_bytes += added;
        data_index++;
        def = def->next;
    }
    if (out_text_size) *out_text_size = text_bytes;
    if (out_data_size) *out_data_size = data_bytes;
}

// Main code generator
// For now, only .text section is emitted as code, .data as data
// Returns 0 on success, sets *code, *code_size, *data, *data_size
int generate_code(AST *ast, SymbolTable *table, uint8_t **code, size_t *code_size, uint8_t **data, size_t *data_size, const char* input_path) {
    // Compute label addresses and size estimates (already includes instruction/data sizes)
    int text_bytes = 0;
    int data_bytes = 0;
    build_label_addresses(ast, table, &text_bytes, &data_bytes);
    Instruction* inst = 0;
    DataDef* def = 0;
    
    // Add small padding for safety (RET insertion, etc.)
    text_bytes += 64;
    data_bytes += 32;
    
    // Cap at reasonable limits (increased to 64KB each)
    if (text_bytes > 65536) text_bytes = 65536;
    if (data_bytes > 65536) data_bytes = 65536;
    
    // Note: removed previous 8KB total cap; assembler can handle larger sources now
    
    // Allocate buffers (static; no heap)
    if (text_bytes < 0 || data_bytes < 0 ||
        (uint32)text_bytes > ASM_MAX_CODE_SIZE || (uint32)data_bytes > ASM_MAX_DATA_SIZE) {
        printf("[codegen] Output too large: text=%d data=%d (max %u/%u)\n",
               text_bytes, data_bytes, (unsigned)ASM_MAX_CODE_SIZE, (unsigned)ASM_MAX_DATA_SIZE);
        *code = NULL;
        *data = NULL;
        return 1;
    }
    *code = g_asm_code_buf;
    *data = g_asm_data_buf;
    // Initialize with zeros so any unused tail is predictable (will be trimmed later)
    memset(*code, 0, text_bytes);
    memset(*data, 0, data_bytes);
    *code_size = text_bytes;
    *data_size = data_bytes;
    
    // Emit code
    int code_pos = 0;
    int actual_code_size = 0;
    int last_was_ret = 0;
    inst = ast->instructions;
    while (inst) {
        if (inst->section != SECTION_TEXT) { inst = inst->next; continue; }
        
        
        
        const InstructionInfo* info = find_instruction_info(inst->mnemonic);
        if (!info) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown instruction: %s", inst->mnemonic);
            print_error(input_path, inst->line_num, msg, NULL);
            (*code)[code_pos++] = 0x90; // NOP as fallback
            inst = inst->next;
            continue;
        }
        
        // Emit opcode
        (*code)[code_pos++] = info->opcode;
        
            // Check for buffer overflow after opcode
    if (check_code_overflow(code_pos, text_bytes)) {
        return 1;
    }
        
        // Handle different instruction types
    if (strcmp(inst->mnemonic, "mov") == 0) {
            if (inst->operands[0].type == OPERAND_REGISTER && 
                inst->operands[1].type == OPERAND_IMMEDIATE) {
                // mov reg, imm32
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0xB8 + reg; // Override opcode
                    (*code)[code_pos++] = imm & 0xFF;
                    (*code)[code_pos++] = (imm >> 8) & 0xFF;
                    (*code)[code_pos++] = (imm >> 16) & 0xFF;
                    (*code)[code_pos++] = (imm >> 24) & 0xFF;
                    
                    // Check for buffer overflow after mov reg, imm32
                    if (check_code_overflow(code_pos, text_bytes)) {
                        return 1;
                    }
                    
                }
            } else if (inst->operands[0].type == OPERAND_REGISTER && 
                       inst->operands[1].type == OPERAND_REGISTER) {
                // mov reg, reg
                int is8 = inst->operands[0].size_hint==8 || inst->operands[1].size_hint==8;
                int reg1 = is8 ? reg8_encoding(inst->operands[0].value) : reg_encoding(inst->operands[0].value);
                int reg2 = is8 ? reg8_encoding(inst->operands[1].value) : reg_encoding(inst->operands[1].value);
                if (reg1 >= 0 && reg2 >= 0) {
                    if (is8) {
                        (*code)[code_pos-1] = 0x88; // mov r/m8, r8
                        (*code)[code_pos++] = 0xC0 | (reg2 << 3) | reg1; // dst in r/m, src in reg
                    } else {
                        // Use 32-bit register-to-register move opcode
                        (*code)[code_pos-1] = 0x89; // mov r/m32, r32
                        (*code)[code_pos++] = 0xC0 | (reg2 << 3) | reg1; // ModR/M (mod=11, reg=src, r/m=dst)
                    }
                    
                    // Check for buffer overflow after mov reg, reg
                    if (check_code_overflow(code_pos, text_bytes)) {
                        return 1;
                    }
                    
                }
            } else if (inst->operands[0].type == OPERAND_REGISTER &&
                       inst->operands[1].type == OPERAND_LABEL) {
                // mov reg, label -> absolute address
                int reg = reg_encoding(inst->operands[0].value);
                int is_text = 0;
                int is_data = 0;
                int addr = lookup_label(table, inst->operands[1].value, SECTION_TEXT);
                if (addr >= 0) {
                    is_text = 1;
                } else {
                    addr = lookup_label(table, inst->operands[1].value, SECTION_DATA);
                    if (addr >= 0) is_data = 1;
                }
                if (addr < 0) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Undefined label: %s", inst->operands[1].value);
                    print_error(input_path, inst->line_num, msg, NULL);
                    addr = 0;
                }
                // Safety: some builds may have stored label addresses as offsets (0 for .text, 0x1000 for .data).
                // Normalize to absolute runtime addresses based on CODE_BASE/DATA_BASE.
                if (is_text && addr < (int)g_asm_code_base) {
                    addr += (int)g_asm_code_base;
                }
                if (is_data && addr < (int)g_asm_data_base) {
                    /* If stored as a data-section-relative offset, preserve the offset
                     * while mapping to the runtime DATA_BASE. For example, if addr was
                     * 0x1000 (meaning start of data), and DATA_BASE is 0x20001000,
                     * the resulting address should be DATA_BASE + (addr - DATA_SECTION_BASE)
                     * where addr is offset within the data section. Here we assume
                     * that previously-stored data addresses are offsets relative to 0
                     * (i.e., an offset), so just add DATA_BASE.
                     */
                    addr = (int)g_asm_data_base + addr;
                }
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0xB8 + reg;
                    (*code)[code_pos++] = addr & 0xFF;
                    (*code)[code_pos++] = (addr >> 8) & 0xFF;
                    (*code)[code_pos++] = (addr >> 16) & 0xFF;
                    (*code)[code_pos++] = (addr >> 24) & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            } else if (inst->operands[0].type == OPERAND_REGISTER &&
                       inst->operands[1].type == OPERAND_MEMORY) {
                // mov r32, [mem]
                int want8 = inst->operands[1].size_hint == 8 || reg8_encoding(inst->operands[0].value) >= 0;
                int dst = want8 ? reg8_encoding(inst->operands[0].value) : reg_encoding(inst->operands[0].value);
                int base=-1, index=-1, scale=1, has_disp=0, disp=0, is_abs=0, abs=0;
                parse_memory_operand(inst->operands[1].value, table, &base, &index, &scale, &has_disp, &disp, &is_abs, &abs);
                if (want8) {
                    (*code)[code_pos-1] = 0x8A; // mov r8, r/m8
                    emit_ea8(code, &code_pos, text_bytes, dst, base, index, scale, has_disp, disp, is_abs, abs);
                } else {
                    (*code)[code_pos-1] = 0x8B; // mov r32, r/m32
                    emit_ea(code, &code_pos, text_bytes, dst, base, index, scale, has_disp, disp, is_abs, abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            } else if (inst->operands[0].type == OPERAND_MEMORY &&
                       inst->operands[1].type == OPERAND_REGISTER) {
                // mov [mem], r32
                int want8 = inst->operands[0].size_hint == 8 || reg8_encoding(inst->operands[1].value) >= 0;
                int src = want8 ? reg8_encoding(inst->operands[1].value) : reg_encoding(inst->operands[1].value);
                int base=-1, index=-1, scale=1, has_disp=0, disp=0, is_abs=0, abs=0;
                parse_memory_operand(inst->operands[0].value, table, &base, &index, &scale, &has_disp, &disp, &is_abs, &abs);
                if (want8) {
                    (*code)[code_pos-1] = 0x88; // mov r/m8, r8
                    emit_ea8(code, &code_pos, text_bytes, src, base, index, scale, has_disp, disp, is_abs, abs);
                } else {
                    (*code)[code_pos-1] = 0x89; // mov r/m32, r32
                    emit_ea(code, &code_pos, text_bytes, src, base, index, scale, has_disp, disp, is_abs, abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (!strcmp(inst->mnemonic, "movzx") || !strcmp(inst->mnemonic, "movsx")) {
            // movzx/movsx r32, r/m8 or r/m16 (implement r/m8 here)
            int is_zx = !strcmp(inst->mnemonic, "movzx");
            if (inst->operands[0].type == OPERAND_REGISTER &&
                (inst->operands[1].type == OPERAND_REGISTER || inst->operands[1].type == OPERAND_MEMORY)) {
                // Emit prefix 0F and opcode B6 (r/m8) for movzx, BE for movsx
                (*code)[code_pos-1] = 0x0F;
                (*code)[code_pos++] = is_zx ? 0xB6 : 0xBE; // byte source
                if (inst->operands[1].type == OPERAND_REGISTER) {
                    int dst = reg_encoding(inst->operands[0].value);
                    int src8 = reg8_encoding(inst->operands[1].value);
                    if (dst < 0 || src8 < 0) {
                        // Fallback to NOP on invalid regs
                        (*code)[code_pos-2] = 0x90; // replace 0F with NOP
                        code_pos -= 1; // discard second byte
                    } else {
                        (*code)[code_pos++] = 0xC0 | (dst<<3) | src8;
                    }
                } else {
                    int dst = reg_encoding(inst->operands[0].value);
                    int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                    parse_memory_operand(inst->operands[1].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                    emit_ea8(code, &code_pos, text_bytes, dst, base,index,scale,has_disp,disp,is_abs,abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (!strcmp(inst->mnemonic, "add") || !strcmp(inst->mnemonic, "sub") || !strcmp(inst->mnemonic, "and") || !strcmp(inst->mnemonic, "or") || !strcmp(inst->mnemonic, "xor") || !strcmp(inst->mnemonic, "cmp")) {
            // ALU: r/m32, r32  | r32, r/m32 | r/m32, imm32
            uint8_t rm_r=0, r_rm=0; int ext=-1;
            get_alu_op(inst->mnemonic, &rm_r, &r_rm, &ext);
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_REGISTER) {
                int dst = reg_encoding(inst->operands[0].value);
                int src = reg_encoding(inst->operands[1].value);
                if (dst>=0 && src>=0) {
                    (*code)[code_pos-1] = r_rm; // r32, r/m32
                    (*code)[code_pos++] = 0xC0 | (dst<<3) | src;
                }
            } else if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
                int reg = reg_encoding(inst->operands[0].value);
                int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                parse_memory_operand(inst->operands[1].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                (*code)[code_pos-1] = r_rm; // r32, r/m32
                emit_ea(code, &code_pos, text_bytes, reg, base,index,scale,has_disp,disp,is_abs,abs);
            } else if (inst->operands[0].type == OPERAND_MEMORY && inst->operands[1].type == OPERAND_REGISTER) {
                int reg = reg_encoding(inst->operands[1].value);
                int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                parse_memory_operand(inst->operands[0].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                (*code)[code_pos-1] = rm_r; // r/m32, r32
                emit_ea(code, &code_pos, text_bytes, reg, base,index,scale,has_disp,disp,is_abs,abs);
            } else if ((inst->operands[0].type == OPERAND_REGISTER || inst->operands[0].type == OPERAND_MEMORY) && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int imm = parse_imm(inst->operands[1].value);
                int use_imm8 = (imm >= -128 && imm <= 127) ? 1 : 0;
                (*code)[code_pos-1] = use_imm8 ? 0x83 : 0x81; // 83 => sign-extended imm8
                if (inst->operands[0].type == OPERAND_REGISTER) {
                    int rm = reg_encoding(inst->operands[0].value);
                    (*code)[code_pos++] = 0xC0 | (ext<<3) | rm;
                } else {
                    int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                    parse_memory_operand(inst->operands[0].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                    emit_ea(code, &code_pos, text_bytes, ext, base,index,scale,has_disp,disp,is_abs,abs);
                }
                if (use_imm8) { (*code)[code_pos++] = (uint8_t)imm; }
                else { (*code)[code_pos++] = imm & 0xFF; (*code)[code_pos++] = (imm>>8)&0xFF; (*code)[code_pos++] = (imm>>16)&0xFF; (*code)[code_pos++] = (imm>>24)&0xFF; }
            }
            if (check_code_overflow(code_pos, text_bytes)) return 1;
        } else if (!strcmp(inst->mnemonic, "lea")) {
            // lea r32, [mem]
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_MEMORY) {
                int dst = reg_encoding(inst->operands[0].value);
                int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                parse_memory_operand(inst->operands[1].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                (*code)[code_pos-1] = 0x8D; // lea r32, r/m32
                emit_ea(code, &code_pos, text_bytes, dst, base,index,scale,has_disp,disp,is_abs,abs);
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (!strcmp(inst->mnemonic, "imul")) {
            // imul r32, r/m32   -> 0x0F 0xAF /r
            if (inst->operands[0].type == OPERAND_REGISTER && (inst->operands[1].type == OPERAND_REGISTER || inst->operands[1].type == OPERAND_MEMORY)) {
                (*code)[code_pos-1] = 0x0F; (*code)[code_pos++] = 0xAF;
                if (inst->operands[1].type == OPERAND_REGISTER) {
                    int dst = reg_encoding(inst->operands[0].value);
                    int src = reg_encoding(inst->operands[1].value);
                    (*code)[code_pos++] = 0xC0 | (dst<<3) | src;
                } else {
                    int dst = reg_encoding(inst->operands[0].value);
                    int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                    parse_memory_operand(inst->operands[1].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                    emit_ea(code, &code_pos, text_bytes, dst, base,index,scale,has_disp,disp,is_abs,abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (!strcmp(inst->mnemonic, "mul") || !strcmp(inst->mnemonic, "div") || !strcmp(inst->mnemonic, "idiv") || !strcmp(inst->mnemonic, "neg") || !strcmp(inst->mnemonic, "not")) {
            // One-operand group encodings on r/m32
            int ext = -1; int needs_f7 = 1;
            if (!strcmp(inst->mnemonic, "mul")) ext = 4; else if (!strcmp(inst->mnemonic, "div")) ext = 6; else if (!strcmp(inst->mnemonic, "idiv")) ext = 7; else if (!strcmp(inst->mnemonic, "neg")) { ext = 3; } else if (!strcmp(inst->mnemonic, "not")) { ext = 2; }
            (*code)[code_pos-1] = needs_f7 ? 0xF7 : 0xF6; // Always 32-bit here
            if (inst->operands[0].type == OPERAND_REGISTER) {
                int rm = reg_encoding(inst->operands[0].value);
                (*code)[code_pos++] = 0xC0 | (ext<<3) | rm;
            } else if (inst->operands[0].type == OPERAND_MEMORY) {
                int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                parse_memory_operand(inst->operands[0].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                emit_ea(code, &code_pos, text_bytes, ext, base,index,scale,has_disp,disp,is_abs,abs);
            }
            if (check_code_overflow(code_pos, text_bytes)) return 1;
        } else if (strcmp(inst->mnemonic, "jmp") == 0) {
            if (inst->operands[0].type == OPERAND_LABEL) {
                int target = lookup_label(table, inst->operands[0].value, SECTION_TEXT);
                int rel32 = (target >= 0) ? (target - ((int)g_asm_code_base + code_pos + 5)) : 0;
                // Always emit near JMP rel32 so sizing is stable.
                (*code)[code_pos-1] = 0xE9; // near
                (*code)[code_pos++] = rel32 & 0xFF; (*code)[code_pos++] = (rel32>>8)&0xFF; (*code)[code_pos++] = (rel32>>16)&0xFF; (*code)[code_pos++] = (rel32>>24)&0xFF;
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            } else if (inst->operands[0].type == OPERAND_REGISTER || inst->operands[0].type == OPERAND_MEMORY) {
                // jmp r/m32
                (*code)[code_pos-1] = 0xFF;
                if (inst->operands[0].type == OPERAND_REGISTER) {
                    int rm = reg_encoding(inst->operands[0].value);
                    (*code)[code_pos++] = 0xE0 | rm; // /4
                } else {
                    int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                    parse_memory_operand(inst->operands[0].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                    emit_ea(code, &code_pos, text_bytes, 4, base,index,scale,has_disp,disp,is_abs,abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (strcmp(inst->mnemonic, "call") == 0) {
            if (inst->operands[0].type == OPERAND_LABEL) {
                int target = lookup_label(table, inst->operands[0].value, SECTION_TEXT);
                int rel32 = (target >= 0) ? (target - ((int)g_asm_code_base + code_pos + 5)) : 0;
                (*code)[code_pos-1] = 0xE8; // call rel32
                (*code)[code_pos++] = rel32 & 0xFF; (*code)[code_pos++] = (rel32>>8)&0xFF; (*code)[code_pos++] = (rel32>>16)&0xFF; (*code)[code_pos++] = (rel32>>24)&0xFF;
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            } else if (inst->operands[0].type == OPERAND_REGISTER || inst->operands[0].type == OPERAND_MEMORY) {
                (*code)[code_pos-1] = 0xFF;
                if (inst->operands[0].type == OPERAND_REGISTER) {
                    int rm = reg_encoding(inst->operands[0].value);
                    (*code)[code_pos++] = 0xD0 | rm; // /2
                } else {
                    int base=-1,index=-1,scale=1,has_disp=0,disp=0,is_abs=0,abs=0;
                    parse_memory_operand(inst->operands[0].value, table, &base,&index,&scale,&has_disp,&disp,&is_abs,&abs);
                    emit_ea(code, &code_pos, text_bytes, 2, base,index,scale,has_disp,disp,is_abs,abs);
                }
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else if (strcmp(inst->mnemonic, "ret") == 0) {
            (*code)[code_pos-1] = 0xC3; // ret
            last_was_ret = 1;
        } else if (strcmp(inst->mnemonic, "int") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_IMMEDIATE) {
                int imm = parse_imm(inst->operands[0].value);
                (*code)[code_pos-1] = 0xCD; // int imm8
                (*code)[code_pos++] = imm & 0xFF;
                
                if (check_code_overflow(code_pos, text_bytes)) {
                    return 1;
                }
                
            }
        } else if (strcmp(inst->mnemonic, "push") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER) {
                int reg = reg_encoding(inst->operands[0].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x50 + reg; // push reg
                }
            } else if (inst->operands[0].type == OPERAND_IMMEDIATE) {
                int imm = parse_imm(inst->operands[0].value);
                (*code)[code_pos-1] = 0x68; // push imm32
                (*code)[code_pos++] = imm & 0xFF;
                (*code)[code_pos++] = (imm >> 8) & 0xFF;
                (*code)[code_pos++] = (imm >> 16) & 0xFF;
                (*code)[code_pos++] = (imm >> 24) & 0xFF;
                
                if (check_code_overflow(code_pos, text_bytes)) {
                    return 1;
                }
                
            }
        } else if (strcmp(inst->mnemonic, "pop") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER) {
                int reg = reg_encoding(inst->operands[0].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x58 + reg; // pop reg
                }
            }
        } else if (strcmp(inst->mnemonic, "inc") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER) {
                int reg = reg_encoding(inst->operands[0].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x40 + reg; // inc reg
                }
            }
        } else if (strcmp(inst->mnemonic, "dec") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER) {
                int reg = reg_encoding(inst->operands[0].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x48 + reg; // dec reg
                }
            }
        } else if (strcmp(inst->mnemonic, "nop") == 0) {
            last_was_ret = 0;
            (*code)[code_pos-1] = 0x90; // nop
        } else if (strcmp(inst->mnemonic, "hlt") == 0) {
            last_was_ret = 0;
            (*code)[code_pos-1] = 0xF4; // hlt
        } else if (strcmp(inst->mnemonic, "cli") == 0) {
            last_was_ret = 0;
            (*code)[code_pos-1] = 0xFA; // cli
        } else if (strcmp(inst->mnemonic, "sti") == 0) {
            last_was_ret = 0;
            (*code)[code_pos-1] = 0xFB; // sti
        } else if (strcmp(inst->mnemonic, "and") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x81; // and r/m, imm32
                    (*code)[code_pos++] = 0xE0 | reg; // ModR/M for AND
                    (*code)[code_pos++] = imm & 0xFF;
                    (*code)[code_pos++] = (imm >> 8) & 0xFF;
                    (*code)[code_pos++] = (imm >> 16) & 0xFF;
                    (*code)[code_pos++] = (imm >> 24) & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (strcmp(inst->mnemonic, "or") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x81; // or r/m, imm32
                    (*code)[code_pos++] = 0xC8 | reg; // ModR/M for OR
                    (*code)[code_pos++] = imm & 0xFF;
                    (*code)[code_pos++] = (imm >> 8) & 0xFF;
                    (*code)[code_pos++] = (imm >> 16) & 0xFF;
                    (*code)[code_pos++] = (imm >> 24) & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (strcmp(inst->mnemonic, "xor") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x81; // xor r/m, imm32
                    (*code)[code_pos++] = 0xF0 | reg; // ModR/M for XOR
                    (*code)[code_pos++] = imm & 0xFF;
                    (*code)[code_pos++] = (imm >> 8) & 0xFF;
                    (*code)[code_pos++] = (imm >> 16) & 0xFF;
                    (*code)[code_pos++] = (imm >> 24) & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (strcmp(inst->mnemonic, "shl") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0xC1; // shl r/m, imm8
                    (*code)[code_pos++] = 0xE0 | reg; // ModR/M for SHL
                    (*code)[code_pos++] = imm & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (strcmp(inst->mnemonic, "shr") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0xC1; // shr r/m, imm8
                    (*code)[code_pos++] = 0xE8 | reg; // ModR/M for SHR
                    (*code)[code_pos++] = imm & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (strcmp(inst->mnemonic, "cmp") == 0) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_REGISTER && inst->operands[1].type == OPERAND_IMMEDIATE) {
                int reg = reg_encoding(inst->operands[0].value);
                int imm = parse_imm(inst->operands[1].value);
                if (reg >= 0) {
                    (*code)[code_pos-1] = 0x81; // cmp r/m, imm32
                    (*code)[code_pos++] = 0xF8 | reg; // ModR/M for CMP
                    (*code)[code_pos++] = imm & 0xFF;
                    (*code)[code_pos++] = (imm >> 8) & 0xFF;
                    (*code)[code_pos++] = (imm >> 16) & 0xFF;
                    (*code)[code_pos++] = (imm >> 24) & 0xFF;
                    if (check_code_overflow(code_pos, text_bytes)) return 1;
                }
            }
        } else if (!strcmp(inst->mnemonic, "je") || !strcmp(inst->mnemonic, "jz") ||
                   !strcmp(inst->mnemonic, "jne") || !strcmp(inst->mnemonic, "jnz") ||
                   !strcmp(inst->mnemonic, "ja") || !strcmp(inst->mnemonic, "jnbe") ||
                   !strcmp(inst->mnemonic, "jae") || !strcmp(inst->mnemonic, "jnb") ||
                   !strcmp(inst->mnemonic, "jb") || !strcmp(inst->mnemonic, "jnae") ||
                   !strcmp(inst->mnemonic, "jbe") || !strcmp(inst->mnemonic, "jna") ||
                   !strcmp(inst->mnemonic, "jg") || !strcmp(inst->mnemonic, "jnle") ||
                   !strcmp(inst->mnemonic, "jge") || !strcmp(inst->mnemonic, "jnl") ||
                   !strcmp(inst->mnemonic, "jl") || !strcmp(inst->mnemonic, "jnge") ||
                   !strcmp(inst->mnemonic, "jle") || !strcmp(inst->mnemonic, "jng") ||
                   !strcmp(inst->mnemonic, "js") || !strcmp(inst->mnemonic, "jns") ||
                   !strcmp(inst->mnemonic, "jo") || !strcmp(inst->mnemonic, "jno") ||
                   !strcmp(inst->mnemonic, "jp") || !strcmp(inst->mnemonic, "jpe") ||
                   !strcmp(inst->mnemonic, "jnp") || !strcmp(inst->mnemonic, "jpo")) {
            last_was_ret = 0;
            if (inst->operands[0].type == OPERAND_LABEL) {
                uint8_t s=0,n=0; if (!get_jcc_codes(inst->mnemonic, &s, &n)) { (*code)[code_pos-1]=0x90; }
                int target = lookup_label(table, inst->operands[0].value, SECTION_TEXT);
                int rel32 = (target >= 0) ? (target - ((int)g_asm_code_base + code_pos + 6)) : 0;
                // Always emit near Jcc rel32 so sizing is stable.
                (*code)[code_pos-1] = 0x0F; (*code)[code_pos++] = n;
                (*code)[code_pos++] = rel32 & 0xFF; (*code)[code_pos++] = (rel32>>8)&0xFF; (*code)[code_pos++] = (rel32>>16)&0xFF; (*code)[code_pos++] = (rel32>>24)&0xFF;
                if (check_code_overflow(code_pos, text_bytes)) return 1;
            }
        } else {
            last_was_ret = 0;
            char msg[128];
            snprintf(msg, sizeof(msg), "Unsupported instruction: %s", inst->mnemonic);
            print_error(input_path, inst->line_num, msg, NULL);
            (*code)[code_pos-1] = 0x90; // NOP as fallback
        }
        
        inst = inst->next;
        actual_code_size = code_pos;
    }

    // Do not auto-append RET: callers/user programs must control their own termination.
    
    // Emit data
    int data_pos = 0;
    int actual_data_size = 0;
    def = ast->data_defs;
    while (def) {
        // Check for buffer overflow
        if (check_data_overflow(data_pos, data_bytes)) {
            return 1;
        }
        
        if (strcmp(def->directive, "db") == 0) {
            // Parse comma-separated values for db
            
            // Manual parsing to handle quoted strings properly
            const char* p = def->value;
            while (*p && data_pos < data_bytes) {
                // Skip whitespace
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                
                if (*p == '"') {
                    // String literal with escape sequence support
                    p++; // Skip opening quote
                    while (*p && *p != '"' && data_pos < data_bytes) {
                        if (*p == '\\' && p[1]) {
                            // Handle escape sequences
                            char c = p[1];
                            switch (c) {
                                case 'n': (*data)[data_pos++] = '\n'; break;
                                case 'r': (*data)[data_pos++] = '\r'; break;
                                case 't': (*data)[data_pos++] = '\t'; break;
                                case '0': (*data)[data_pos++] = '\0'; break;
                                case '\\': (*data)[data_pos++] = '\\'; break;
                                case '"': (*data)[data_pos++] = '"'; break;
                                default: (*data)[data_pos++] = c; break;
                            }
                            p += 2;
                        } else {
                            (*data)[data_pos++] = *p++;
                        }
                    }
                    if (*p == '"') p++; // Skip closing quote
                } else if (strncmp(p, "0x", 2) == 0) {
                    // Hex value: 0x0A
                    int val = parse_hex(p);
                    // Hex value processed
                    if (data_pos < data_bytes) {
                        (*data)[data_pos++] = val & 0xFF;
                    }
                    // Skip to next comma or end
                    while (*p && *p != ',') p++;
                } else {
                    // Decimal value or other
                    int val = parse_imm(p);
                    // Decimal value processed
                    if (data_pos < data_bytes) {
                        (*data)[data_pos++] = val & 0xFF;
                    }
                    // Skip to next comma or end
                    while (*p && *p != ',') p++;
                }
                
                // Skip comma and continue
                if (*p == ',') p++;
            }
        } else if (strcmp(def->directive, "dw") == 0) {
            const char* p = def->value;
            while (*p && data_pos + 1 < data_bytes) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                int val = (strncmp(p, "0x", 2) == 0) ? parse_hex(p) : parse_imm(p);
                (*data)[data_pos++] = val & 0xFF;
                (*data)[data_pos++] = (val >> 8) & 0xFF;
                while (*p && *p != ',') p++;
            }
        } else if (strcmp(def->directive, "dd") == 0) {
            const char* p = def->value;
            while (*p && data_pos + 3 < data_bytes) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                int val = (strncmp(p, "0x", 2) == 0) ? parse_hex(p) : parse_imm(p);
                (*data)[data_pos++] = val & 0xFF;
                (*data)[data_pos++] = (val >> 8) & 0xFF;
                (*data)[data_pos++] = (val >> 16) & 0xFF;
                (*data)[data_pos++] = (val >> 24) & 0xFF;
                while (*p && *p != ',') p++;
            }
        } else if (strcmp(def->directive, "resb") == 0) {
            int n = parse_imm(def->value); while (n-- > 0 && data_pos < data_bytes) { (*data)[data_pos++] = 0x00; }
        } else if (strcmp(def->directive, "resw") == 0) {
            int n = parse_imm(def->value) * 2; while (n-- > 0 && data_pos < data_bytes) { (*data)[data_pos++] = 0x00; }
        } else if (strcmp(def->directive, "resd") == 0) {
            int n = parse_imm(def->value) * 4; while (n-- > 0 && data_pos < data_bytes) { (*data)[data_pos++] = 0x00; }
        } else if (strcmp(def->directive, "align") == 0) {
            int a = parse_imm(def->value); if (a<=0) a=1; int rem = data_pos % a; int pad = (rem==0)?0:(a-rem); while (pad-- > 0 && data_pos < data_bytes) { (*data)[data_pos++] = 0x00; }
        }
        
        def = def->next;
        actual_data_size = data_pos;
    }
    // Override sizes to actual emitted sizes
    *code_size = (size_t)actual_code_size;
    *data_size = (size_t)actual_data_size;
    
    return 0;
}

// File I/O helpers ---
// Reads the entire file at 'filename' from the current drive into a buffer allocated with my_malloc.
// Returns pointer and sets out_size, or NULL on error.
char* read_file_to_buffer(const char* filename, uint32_t* out_size) {
    if (!asm_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return 0;
    printf("[assemble] read_file_to_buffer: filename='%s'\n", filename);
    printf("[assemble] g_current_drive = %d\n", g_current_drive);
    eynfs_superblock_t sb;
    printf("[assemble] Reading superblock at LBA %d...\n", EYNFS_SUPERBLOCK_LBA);
    int sb_res = eynfs_read_superblock(g_current_drive, EYNFS_SUPERBLOCK_LBA, &sb);
    printf("[assemble] eynfs_read_superblock returned %d\n", sb_res);
    if (sb_res != 0) {
        printf("[assemble] No supported filesystem found (read error).\n");
        return 0;
    }
    printf("[assemble] Superblock magic: 0x%X (expected 0x%X)\n", sb.magic, EYNFS_MAGIC);
    if (sb.magic != EYNFS_MAGIC) {
        printf("[assemble] No supported filesystem found (bad magic).\n");
        return 0;
    }
    eynfs_dir_entry_t entry;
    if (eynfs_find_in_dir(g_current_drive, &sb, sb.root_dir_block, filename, &entry, 0) != 0) {
        printf("[assemble] File not found: %s\n", filename);
        return 0;
    }
    uint32_t size = entry.size;
    
    // Memory safety: limit file size to prevent excessive buffering
    if (size > ASM_MAX_SRC_SIZE) {
        printf("[assemble] Warning: Source file too large (%d bytes), limiting to 32KB\n", size);
        size = ASM_MAX_SRC_SIZE;
    }

    int n = eynfs_read_file(g_current_drive, &sb, &entry, g_asm_src_buf, size, 0);
    if (n < 0) {
        printf("[assemble] Failed to read file: %s\n", filename);
        return 0;
    }
    if (n < 0) n = 0;
    if ((uint32_t)n > size) n = (int)size;
    g_asm_src_buf[n] = '\0';
    if (out_size) *out_size = (uint32_t)n;
    return g_asm_src_buf;
}

static void symbol_table_free(SymbolTable* table) {
    if (!table) return;
    table->head = 0;
    g_asm_sym_pool_used = 0;
}

static int estimate_text_size_bytes(AST* ast) {
    int sz = 0;
    if (!ast) return 0;
    int ctr = 0;
    for (Instruction* inst = ast->instructions; inst; inst = inst->next) {
        if ((++ctr & 0xF) == 0) {
            asm_ctx_account(SCHED_COST_ALLOC);
        }
        if (inst->section != SECTION_TEXT) continue;
        sz += estimate_instr_size(inst);
    }
    return sz;
}

int assemble(const char *input_path, const char *output_path) {
    g_asm_error_count = 0;
    g_asm_warning_count = 0;

    uint32_t src_size = 0;
    char* src = read_file_to_buffer(input_path, &src_size);
    if (!src) {
        print_error(input_path, 0, "Failed to read input file or out of memory.", NULL);
        return 1;
    }

    AST *ast = parse(src);
    // Source buffer is static; no free.

    if (!ast) {
        print_error(input_path, 0, "Parse failed.", NULL);
        return 1;
    }

    // Output mode selection
    const char* out_ext = strrchr(output_path, '.');
    int want_uelf = (out_ext && strcmp(out_ext, ".uelf") == 0) ? 1 : 0;
    if (want_uelf && !asm_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return 1;

    if (want_uelf) {
        // Ring3 loader expects segments in the user range starting at 0x00400000.
        g_asm_code_base = 0x00400000u;
        int text_est = estimate_text_size_bytes(ast);
        g_asm_data_base = g_asm_code_base + asm_align_up_u32((uint32)text_est, 0x1000u);
    } else {
        g_asm_code_base = 0x02000000u;
        g_asm_data_base = 0x02001000u;
    }
    SymbolTable symtab;
    symbol_table_init(&symtab);
    uint8_t *code = 0;
    size_t code_size = 0;
    uint8_t *data = 0;
    size_t data_size = 0;
    int gen_result = generate_code(ast, &symtab, &code, &code_size, &data, &data_size, input_path);
    if (gen_result != 0) {
        print_error(input_path, 0, "Code generation failed.", NULL);
        free_ast(ast);
        return 1;
    }

    if (g_asm_error_count > 0) {
        // Avoid producing an output file when errors were reported.
        symbol_table_free(&symtab);
        free_ast(ast);
        return 1;
    }

    // We no longer need the AST; free it before allocating large buffers
    free_ast(ast);
    ast = NULL;

    // Determine entry point
    int start_addr = lookup_label(&symtab, "_start", SECTION_TEXT);
    uint32 entry_va = (start_addr >= 0) ? (uint32)start_addr : g_asm_code_base;

    if (want_uelf) {
        if (start_addr < 0) {
            print_error(input_path, 0, "Missing required entry label '_start' for .uelf output.", NULL);
            symbol_table_free(&symtab);
            return 1;
        }
        if (code_size == 0) {
            print_error(input_path, 0, "No .text bytes generated (is your code in 'section .text'?).", NULL);
            symbol_table_free(&symtab);
            return 1;
        }
    }

    if (want_uelf) {
        // Use the new linker for .uelf output to produce GNU ld compatible ELF32
        LinkConfig link_cfg;
        link_config_init(&link_cfg);
        
        // Set input file name for FILE symbol
        const char* base_name = strrchr(input_path, '/');
        if (!base_name) base_name = input_path; else base_name++;
        link_cfg.input_name = base_name;
        
        // Set addresses
        link_cfg.text_vaddr = g_asm_code_base;
        link_cfg.rodata_vaddr = g_asm_data_base;
        link_cfg.entry_vaddr = entry_va;
        
        // Set sections
        link_cfg.text.data = code;
        link_cfg.text.size = code_size;
        link_cfg.text.vaddr = g_asm_code_base;
        link_cfg.text.align = 0x1000;
        link_cfg.text.flags = SHF_ALLOC | SHF_EXECINSTR;
        
        link_cfg.rodata.data = data;
        link_cfg.rodata.size = data_size;
        link_cfg.rodata.vaddr = g_asm_data_base;
        link_cfg.rodata.align = 0x1000;
        link_cfg.rodata.flags = SHF_ALLOC;
        
        // Add FILE symbol (local, ABS section)
        // Generate .o name from input path
        char obj_name[80];
        int olen = strlen(base_name);
        if (olen > (int)sizeof(obj_name) - 3) olen = sizeof(obj_name) - 3;
        memcpy(obj_name, base_name, olen);
        // Change extension to .o
        char* dot = strrchr(obj_name, '.');
        if (dot && dot - obj_name < olen) {
            strcpy(dot, ".o");
        } else {
            obj_name[olen] = '\0';
            strcat(obj_name, ".o");
        }
        link_add_symbol(&link_cfg, obj_name, 0, 0, STB_LOCAL, STT_FILE, SHN_ABS);
        
        // Add data labels (local, .rodata section = 2)
        for (SymbolTableEntry* e = symtab.head; e; e = e->next) {
            if (e->section == SECTION_DATA) {
                link_add_symbol(&link_cfg, e->name, e->address, 0, STB_LOCAL, STT_NOTYPE, 2);
            }
        }
        
        // Add _start as global (section 1 = .text)
        if (start_addr >= 0) {
            link_add_symbol(&link_cfg, "_start", start_addr, 0, STB_GLOBAL, STT_NOTYPE, 1);
        }
        
        int w = link_write_uelf(&link_cfg, output_path);
        symbol_table_free(&symtab);
        if (w != 0) {
            print_error(output_path, 0, "Failed to write .uelf output.", NULL);
            return 1;
        }
        printf("Assembly successful: %s -> %s\n", input_path, output_path);
        return 0;
    }

    // Prepare EYN-OS header
    struct eyn_exe_header hdr;
            memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, EYN_MAGIC, 4);
    hdr.version = EYN_EXE_VERSION;
    hdr.flags = 0;  // No special flags for now
    hdr.reserved = 0;  // Reserved field
    // Look up the _start label to get the entry point
    if (start_addr >= 0) {
        hdr.entry_point = start_addr - (int)g_asm_code_base;  // Convert absolute address to offset
        // Found _start label
    } else {
        hdr.entry_point = 0;  // Default to start of code if no _start label
        printf("[assemble] _start label not found, using entry point 0 (start of code)\n");
    }
    hdr.code_size = code_size;
    hdr.data_size = data_size;
    hdr.bss_size = 0;  // No BSS for now
    hdr.dyn_table_off = 0;  // No dynamic linking for now
    hdr.dyn_table_size = 0;  // No dynamic linking for now
    // Debug: print header and small dumps
    printf("[assemble] hdr: code=%d data=%d entry_off=%d hdr_size=%d\n", (int)hdr.code_size, (int)hdr.data_size, (int)hdr.entry_point, (int)sizeof(hdr));
    if (code && code_size > 0) {
        uint32_t prev = code_size < 16 ? (uint32_t)code_size : 16;
        printf("[assemble] code[0..%d):", (int)prev);
        for (uint32_t i = 0; i < prev; i++) {
            if ((i & 0xF) == 0) asm_ctx_account(SCHED_COST_CONSOLE);
            printf(" %02X", code[i]);
        }
        printf("\n");
    }
    if (data && data_size > 0) {
        uint32_t prev = data_size < 16 ? (uint32_t)data_size : 16;
        printf("[assemble] data[0..%d):", (int)prev);
        for (uint32_t i = 0; i < prev; i++) {
            if ((i & 0xF) == 0) asm_ctx_account(SCHED_COST_CONSOLE);
            printf(" %02X", data[i]);
        }
        printf("\n");
    }
    
    // Get filesystem info for output
    eynfs_superblock_t sb;
    eynfs_dir_entry_t entry;
    if (eynfs_read_superblock(g_current_drive, EYNFS_SUPERBLOCK_LBA, &sb) != 0) {
        printf("[assemble] Failed to read superblock for output\n");
        return 1;
    }
    
    // Stream the output to disk to reduce peak memory
    eynfs_stream_t stream;
    if (!asm_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return 1;
    if (eynfs_stream_begin(g_current_drive, output_path, &stream) != 0) {
        printf("[assemble] Failed to open stream for output file\n");
        return 1;
    }
    int w1 = eynfs_stream_write(&stream, &hdr, sizeof(hdr));
    if (w1 < 0) {
        printf("[assemble] Failed to write header\n");
        return 1;
    }
    printf("[assemble] wrote header bytes: %d\n", w1);
    if (code && code_size > 0) {
        int w2 = eynfs_stream_write(&stream, code, code_size);
        if (w2 < 0) {
            printf("[assemble] Failed to write code segment\n");
            return 1;
        }
        printf("[assemble] wrote code bytes: %d\n", w2);
    }
    if (data && data_size > 0) {
        int w3 = eynfs_stream_write(&stream, data, data_size);
        if (w3 < 0) {
            printf("[assemble] Failed to write data segment\n");
            return 1;
        }
        printf("[assemble] wrote data bytes: %d\n", w3);
    }
    if (eynfs_stream_end(&stream) != 0) {
        printf("[assemble] Failed to finalize output file\n");
        return 1;
    }

    symbol_table_free(&symtab);

    size_t total_size = sizeof(hdr) + code_size + data_size;
    printf("Successfully wrote %d bytes to %s\n", (int)total_size, output_path);
    printf("Assembly successful: %s -> %s\n", input_path, output_path);
    return 0;
}

int assemble_main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: assemble <input_asm_file> <output_bin_file>\n");
        return 1;
    }
    return assemble(argv[1], argv[2]);
} 

// Ensure these functions are not static and are implemented:

void symbol_table_init(SymbolTable* table) {
    table->head = 0;
    g_asm_sym_pool_used = 0;
}

int lookup_label(SymbolTable* table, const char* name, SectionType section) {
    SymbolTableEntry* cur = table->head;
    while (cur) {
        if (cur->section == section && strcmp(cur->name, name) == 0) {
            return cur->address;
        }
        cur = cur->next;
    }
    return -1;
}

// Helper to check if a label is used (stub: always returns 0 for now)
int label_is_used(const char* name, SymbolTable* table) {
    // TODO: Implement real usage tracking
    return 0;
}

AST* parse(const char *src) {
    Lexer lexer;
    lexer_init(&lexer, src);
    Token token;
    SectionType current_section = SECTION_TEXT; // default to text so labels/instructions without an explicit .section land in .text
    int line_num = 1;
    // Static arena so assembly works even when the heap is unavailable.
    size_t src_len = src ? strlen(src) : 0;
    size_t arena_size = src_len * 4 + 4096;
    if (arena_size > ASM_ARENA_MAX) {
        printf("[parse] Source too complex: need arena %d bytes (max %d)\n",
               (int)arena_size, (int)ASM_ARENA_MAX);
        return 0;
    }

    AST* ast = &g_asm_static_ast;
    memset(ast, 0, sizeof(*ast));
    ast->arena_buf = g_asm_ast_arena;
    ast->arena_size = arena_size;
    ast->arena_backed = 1;

    asm_arena_t arena;
    arena_init(&arena, (uint8_t*)ast->arena_buf, ast->arena_size);
    int text_instr_index = 0;
    int data_def_index = 0;

    while (1) {
        token = lexer_next_token(&lexer);
        if (token.type == TOKEN_EOF) break;
        if (token.type == TOKEN_NEWLINE) {
            line_num++;
            continue;
        }
        if (token.type == TOKEN_SECTION) {
            // Expect .text / .data next (treat .rodata as .data)
            Token next = lexer_next_token(&lexer);
            if (strcmp(next.text, ".text") == 0) {
                current_section = SECTION_TEXT;
                if (g_asm_verbose) printf("[parse] Section: .text\n");
            } else if (strcmp(next.text, ".data") == 0) {
                current_section = SECTION_DATA;
                if (g_asm_verbose) printf("[parse] Section: .data\n");
            } else if (strcmp(next.text, ".rodata") == 0) {
                current_section = SECTION_DATA;
                if (g_asm_verbose) printf("[parse] Section: .rodata (treated as .data)\n");
            } else {
                if (g_asm_verbose) printf("[parse] Unknown section: %s\n", next.text);
                current_section = SECTION_NONE;
            }
            continue;
        }
        if (token.type == TOKEN_LABEL) {
            // Add label to AST
            Label* label = (Label*)arena_alloc(&arena, sizeof(Label));
            if (!label) { printf("[parse] Arena exhausted at line %d\n", line_num); free_ast(ast); return 0; }
            strncpy(label->name, token.text, sizeof(label->name)-1);
            label->name[sizeof(label->name)-1] = 0;
            label->section = current_section;
            label->address = 0; // To be filled in codegen
            label->line_num = line_num;
            // Record position within section for later address calc
            if (current_section == SECTION_TEXT) {
                label->instr_index = text_instr_index;
            } else if (current_section == SECTION_DATA) {
                label->instr_index = data_def_index;
            } else {
                label->instr_index = 0;
            }
            add_label(ast, label);
            if (g_asm_verbose) printf("[parse] Label: %s (section %d)\n", label->name, label->section);
            continue;
        }
        if (token.type == TOKEN_MNEMONIC) {
            // Parse instruction and operands
            Instruction* inst = (Instruction*)arena_alloc(&arena, sizeof(Instruction));
            if (!inst) { printf("[parse] Arena exhausted at line %d\n", line_num); free_ast(ast); return 0; }
            memset(inst, 0, sizeof(Instruction));
            strncpy(inst->mnemonic, token.text, sizeof(inst->mnemonic)-1);
            inst->mnemonic[sizeof(inst->mnemonic)-1] = 0;
            inst->section = current_section;
            inst->line_num = line_num;
            // Parse up to 2 operands with comma handling
            int op_index = 0;
            int pending_size_hint = 0; // 0=unspecified, else 8/16/32
            while (op_index < 2) {
                Token next = lexer_next_token(&lexer);
                if (next.type == TOKEN_COMMA) {
                    // stray comma; skip
                    continue;
                } else if (next.type == TOKEN_SIZE) {
                    // size override before an operand
                    if (!strcmp(next.text, "byte")) pending_size_hint = 8;
                    else if (!strcmp(next.text, "word")) pending_size_hint = 16;
                    else if (!strcmp(next.text, "dword")) pending_size_hint = 32;
                    // continue to fetch the actual operand token
                    continue;
                } else if (next.type == TOKEN_MEMORY) {
                    inst->operands[op_index].type = OPERAND_MEMORY;
                    strncpy(inst->operands[op_index].value, next.text, sizeof(inst->operands[op_index].value)-1);
                    inst->operands[op_index].value[sizeof(inst->operands[op_index].value)-1] = 0;
                    inst->operands[op_index].size_hint = pending_size_hint;
                    pending_size_hint = 0;
                    op_index++;
                    Token sep = lexer_next_token(&lexer);
                    if (sep.type == TOKEN_COMMA) {
                        continue;
                    }
                    if (sep.type == TOKEN_NEWLINE) {
                        line_num++;
                        break;
                    }
                    lexer_unget_token(&lexer, sep);
                    break;
                } else if (next.type == TOKEN_REGISTER) {
                    inst->operands[op_index].type = OPERAND_REGISTER;
                    strncpy(inst->operands[op_index].value, next.text, sizeof(inst->operands[op_index].value)-1);
                    inst->operands[op_index].value[sizeof(inst->operands[op_index].value)-1] = 0;
                    inst->operands[op_index].size_hint = pending_size_hint;
                    pending_size_hint = 0;
                    op_index++;
                    Token sep = lexer_next_token(&lexer);
                    if (sep.type == TOKEN_COMMA) {
                        continue;
                    }
                    if (sep.type == TOKEN_NEWLINE) {
                        line_num++;
                        break;
                    }
                    lexer_unget_token(&lexer, sep);
                    break;
                } else if (next.type == TOKEN_IMMEDIATE) {
                    inst->operands[op_index].type = OPERAND_IMMEDIATE;
                    strncpy(inst->operands[op_index].value, next.text, sizeof(inst->operands[op_index].value)-1);
                    inst->operands[op_index].value[sizeof(inst->operands[op_index].value)-1] = 0;
                    inst->operands[op_index].size_hint = pending_size_hint;
                    pending_size_hint = 0;
                    op_index++;
                    Token sep = lexer_next_token(&lexer);
                    if (sep.type == TOKEN_COMMA) {
                        continue;
                    }
                    if (sep.type == TOKEN_NEWLINE) {
                        line_num++;
                        break;
                    }
                    lexer_unget_token(&lexer, sep);
                    break;
                } else if (next.type == TOKEN_LABEL || next.type == TOKEN_UNKNOWN) {
                    inst->operands[op_index].type = OPERAND_LABEL;
                    strncpy(inst->operands[op_index].value, next.text, sizeof(inst->operands[op_index].value)-1);
                    inst->operands[op_index].value[sizeof(inst->operands[op_index].value)-1] = 0;
                    inst->operands[op_index].size_hint = pending_size_hint;
                    pending_size_hint = 0;
                    op_index++;
                    Token sep = lexer_next_token(&lexer);
                    if (sep.type == TOKEN_COMMA) {
                        continue;
                    }
                    if (sep.type == TOKEN_NEWLINE) {
                        line_num++;
                        break;
                    }
                    lexer_unget_token(&lexer, sep);
                    break;
                } else {
                    break;
                }
            }
            add_instruction(ast, inst);
            if (current_section == SECTION_TEXT) {
                text_instr_index++;
            }
            if (g_asm_verbose) printf("[parse] Instruction: %s (section %d, line %d)\n", inst->mnemonic, inst->section, inst->line_num);
            continue;
        }
        if (token.type == TOKEN_DIRECTIVE) {
            // Data definition or global
            if (strcmp(token.text, "global") == 0) {
                // Skip for now (could add to export table)
                Token next = lexer_next_token(&lexer);
                if (g_asm_verbose) printf("[parse] Global: %s\n", next.text);
                continue;
            } else if (strcmp(token.text, "intel_syntax") == 0) {
                // GAS: .intel_syntax noprefix -- ignore remainder of line
                while (1) {
                    Token skip = lexer_next_token(&lexer);
                    if (skip.type == TOKEN_EOF) break;
                    if (skip.type == TOKEN_NEWLINE) { line_num++; break; }
                }
                continue;
            } else {
                // db, dw, dd
                DataDef* def = (DataDef*)arena_alloc(&arena, sizeof(DataDef));
                if (!def) { printf("[parse] Arena exhausted at line %d\n", line_num); free_ast(ast); return 0; }
                strncpy(def->directive, token.text, sizeof(def->directive)-1);
                def->directive[sizeof(def->directive)-1] = 0;
                
                // Parse comma-separated values for db/dw/dd
                char full_value[256] = {0};
                int value_len = 0;
                
                while (1) {
                    Token next = lexer_next_token(&lexer);
                    if (next.type == TOKEN_COMMA) {
                        if (value_len < sizeof(full_value) - 1) {
                            full_value[value_len++] = ',';
                            full_value[value_len++] = ' ';
                        }
                        continue;
                    } else if (next.type == TOKEN_EOF || next.type == TOKEN_NEWLINE) {
                        break;
                    } else {
                        // Add the value
                        if (value_len < sizeof(full_value) - strlen(next.text) - 1) {
                            if (value_len > 0) {
                                full_value[value_len++] = ' ';
                            }
                            strcpy(full_value + value_len, next.text);
                            value_len += strlen(next.text);
                        }
                    }
                }
                
                strncpy(def->value, full_value, sizeof(def->value)-1);
                def->value[sizeof(def->value)-1] = 0;
                def->line_num = line_num;
                add_data_def(ast, def);
                if (current_section == SECTION_DATA) {
                    data_def_index++;
                }
                if (g_asm_verbose) printf("[parse] Data: %s %s (line %d)\n", def->directive, def->value, def->line_num);
                continue;
            }
        }
        // Unknown or unsupported token: skip
    }
    return ast;
}

REGISTER_SHELL_COMMAND(assemble, "assemble", handler_assemble, CMD_STREAMING, "Converts assembly code into machine code.\nSupports NASM syntax.\nUsage: assemble <input file> <output file>", "assemble example.asm example.eyn");

// Free AST and all its components
void free_ast(AST* ast) {
    if (!ast) return;
    // Static arena: nothing to free.
    // Clear pointers so accidental reuse is obvious.
    memset(ast, 0, sizeof(*ast));
}