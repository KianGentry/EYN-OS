#ifndef ASSEMBLE_H
#define ASSEMBLE_H

#include <stddef.h>
#include <stdint.h>

// Instruction categories
typedef enum {
    INST_CAT_DATA_MOVEMENT,
    INST_CAT_ARITHMETIC,
    INST_CAT_LOGICAL,
    INST_CAT_CONTROL_FLOW,
    INST_CAT_STRING,
    INST_CAT_SYSTEM,
    INST_CAT_FPU,
    INST_CAT_MMX,
    INST_CAT_SSE
} InstructionCategory;

// Instruction encoding structure
typedef struct {
    const char* mnemonic;
    uint8_t opcode;
    uint8_t modrm_required;
    uint8_t immediate_required;
    uint8_t displacement_required;
    InstructionCategory category;
    const char* description;
} InstructionInfo;

// Section types
typedef enum {
    SECTION_NONE = 0,
    SECTION_TEXT,
    SECTION_DATA
} SectionType;

// Operand types
typedef enum {
    OPERAND_NONE = 0,
    OPERAND_REGISTER,
    OPERAND_IMMEDIATE,
    OPERAND_LABEL,
    OPERAND_MEMORY
} OperandType;

// Operand
typedef struct {
    OperandType type;
    char value[64];
    // Optional size hint from tokens like 'byte', 'word', 'dword'. 0 if unspecified.
    // Valid values: 8, 16, 32
    int size_hint;
} Operand;

// Instruction
typedef struct Instruction {
    char mnemonic[16];
    Operand operands[2];
    SectionType section;
    int line_num;
    struct Instruction* next;
} Instruction;

// Label
typedef struct Label {
    char name[64];
    SectionType section;
    int address;
    int line_num;
    int instr_index; // For SECTION_TEXT: instruction index at definition; for SECTION_DATA: data-def index
    struct Label* next;
} Label;

// Data definition
typedef struct DataDef {
    char directive[8];
    char value[128];
    int line_num;
    struct DataDef* next;
} DataDef;

// AST
typedef struct AST {
    Instruction* instructions;
    Label* labels;
    DataDef* data_defs;
    // If 1, nodes are allocated from an arena and freed in bulk
    int arena_backed;
} AST;

// Symbol table entry
typedef struct SymbolTableEntry {
    char name[64];
    SectionType section;
    int address;
    struct SymbolTableEntry* next;
} SymbolTableEntry;

// Symbol table
typedef struct {
    SymbolTableEntry* head;
} SymbolTable;

// Lexer token types and structures
typedef enum {
    TOKEN_LABEL,
    TOKEN_MNEMONIC,
    TOKEN_REGISTER,
    TOKEN_IMMEDIATE,
    // New: token representing a full memory operand like [eax+4] or [label]
    TOKEN_MEMORY,
    // New: size override keywords like byte/word/dword
    TOKEN_SIZE,
    TOKEN_SECTION,
    TOKEN_DIRECTIVE,
    TOKEN_COMMA,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char text[64];
} Token;

typedef struct {
    const char *src;
    size_t pos;
} Lexer;

void lexer_init(Lexer *lexer, const char *src);
Token lexer_next_token(Lexer *lexer);

// Function prototypes
void add_instruction(AST* ast, Instruction* inst);
void add_label(AST* ast, Label* label);
void add_data_def(AST* ast, DataDef* def);
AST* parse(const char *src);
void free_ast(AST* ast);  // Add function to free AST
void build_symbol_table(AST* ast, SymbolTable* table);
int lookup_label(SymbolTable* table, const char* name, SectionType section);
int generate_code(AST *ast, SymbolTable *table, uint8_t **code, size_t *code_size, uint8_t **data, size_t *data_size, const char* input_path);
int assemble(const char *input_path, const char *output_path);
int assemble_main(int argc, char *argv[]);
void symbol_table_init(SymbolTable* table);

// Instruction set functions
const InstructionInfo* find_instruction_info(const char* mnemonic);
int get_register_encoding(const char* reg_name);
int get_register8_encoding(const char* reg_name);
int get_seg_reg_encoding(const char* reg_name);
int is_valid_register(const char* name);
int is_valid_instruction(const char* name);
const char* get_category_description(InstructionCategory cat);
void print_instruction_help(const char* mnemonic);
void list_all_instructions(void);

// Global assembler verbosity (0=quiet, 1=verbose)
extern int g_asm_verbose;

#endif // ASSEMBLE_H 