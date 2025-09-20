#include <assemble.h>
#include <string.h>
#include <util.h>
#include <vga.h>

// i386 Instruction Set Implementation
// This file contains the complete i386 instruction set support for the EYN-OS assembler

// Register encodings for ModR/M byte
static const char* reg_names[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
};

static const char* reg8_names[] = {
    "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"
};

static const char* seg_reg_names[] = {
    "es", "cs", "ss", "ds", "fs", "gs"
};

// Data Movement Instructions
static const InstructionInfo data_movement_instructions[] = {
    {"mov", 0x88, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Move data between registers and memory"},
    {"mov", 0xB8, 0, 1, 0, INST_CAT_DATA_MOVEMENT, "Move immediate to register"},
    {"mov", 0xA0, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Move from memory to accumulator"},
    {"mov", 0xA2, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Move from accumulator to memory"},
    {"xchg", 0x86, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Exchange register with register/memory"},
    {"xchg", 0x90, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Exchange register with EAX"},
    {"lea", 0x8D, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load effective address"},
    {"lds", 0xC5, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load DS:register from memory"},
    {"les", 0xC4, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load ES:register from memory"},
    {"lfs", 0x0F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load FS:register from memory"},
    {"lgs", 0x0F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load GS:register from memory"},
    {"lss", 0x0F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Load SS:register from memory"},
    {"push", 0x50, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Push register onto stack"},
    {"push", 0x68, 0, 1, 0, INST_CAT_DATA_MOVEMENT, "Push immediate onto stack"},
    {"push", 0xFF, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Push memory onto stack"},
    {"pop", 0x58, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Pop register from stack"},
    {"pop", 0x8F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Pop memory from stack"},
    {"pusha", 0x60, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Push all general registers"},
    {"popa", 0x61, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Pop all general registers"},
    {"pushf", 0x9C, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Push flags register"},
    {"popf", 0x9D, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Pop flags register"},
    {"pushfd", 0x9C, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Push extended flags register"},
    {"popfd", 0x9D, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Pop extended flags register"},
    {"cbw", 0x98, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Convert byte to word"},
    {"cwd", 0x99, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Convert word to doubleword"},
    {"cwde", 0x98, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Convert word to extended doubleword"},
    {"cdq", 0x99, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Convert doubleword to quadword"},
    {"movsx", 0x0F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Move with sign extension"},
    {"movzx", 0x0F, 1, 0, 0, INST_CAT_DATA_MOVEMENT, "Move with zero extension"},
    {"bswap", 0x0F, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Byte swap register"},
    {"xlat", 0xD7, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Translate byte using lookup table"},
    {"xlatb", 0xD7, 0, 0, 0, INST_CAT_DATA_MOVEMENT, "Translate byte using lookup table"},
    {NULL, 0, 0, 0, 0, INST_CAT_DATA_MOVEMENT, NULL}
};

// Arithmetic Instructions
static const InstructionInfo arithmetic_instructions[] = {
    {"add", 0x00, 1, 0, 0, INST_CAT_ARITHMETIC, "Add register/memory to register"},
    {"add", 0x80, 1, 1, 0, INST_CAT_ARITHMETIC, "Add immediate to register/memory"},
    {"add", 0x04, 0, 1, 0, INST_CAT_ARITHMETIC, "Add immediate to AL"},
    {"add", 0x05, 0, 1, 0, INST_CAT_ARITHMETIC, "Add immediate to EAX"},
    {"adc", 0x10, 1, 0, 0, INST_CAT_ARITHMETIC, "Add with carry"},
    {"adc", 0x80, 1, 1, 0, INST_CAT_ARITHMETIC, "Add with carry immediate"},
    {"sub", 0x28, 1, 0, 0, INST_CAT_ARITHMETIC, "Subtract register/memory from register"},
    {"sub", 0x80, 1, 1, 0, INST_CAT_ARITHMETIC, "Subtract immediate from register/memory"},
    {"sub", 0x2C, 0, 1, 0, INST_CAT_ARITHMETIC, "Subtract immediate from AL"},
    {"sub", 0x2D, 0, 1, 0, INST_CAT_ARITHMETIC, "Subtract immediate from EAX"},
    {"sbb", 0x18, 1, 0, 0, INST_CAT_ARITHMETIC, "Subtract with borrow"},
    {"sbb", 0x80, 1, 1, 0, INST_CAT_ARITHMETIC, "Subtract with borrow immediate"},
    {"inc", 0x40, 0, 0, 0, INST_CAT_ARITHMETIC, "Increment register"},
    {"inc", 0xFE, 1, 0, 0, INST_CAT_ARITHMETIC, "Increment memory"},
    {"dec", 0x48, 0, 0, 0, INST_CAT_ARITHMETIC, "Decrement register"},
    {"dec", 0xFE, 1, 0, 0, INST_CAT_ARITHMETIC, "Decrement memory"},
    {"mul", 0xF6, 1, 0, 0, INST_CAT_ARITHMETIC, "Unsigned multiply"},
    {"imul", 0xF6, 1, 0, 0, INST_CAT_ARITHMETIC, "Signed multiply"},
    {"imul", 0x69, 1, 1, 0, INST_CAT_ARITHMETIC, "Signed multiply immediate"},
    {"div", 0xF6, 1, 0, 0, INST_CAT_ARITHMETIC, "Unsigned divide"},
    {"idiv", 0xF6, 1, 0, 0, INST_CAT_ARITHMETIC, "Signed divide"},
    {"neg", 0xF6, 1, 0, 0, INST_CAT_ARITHMETIC, "Negate"},
    {"cmp", 0x38, 1, 0, 0, INST_CAT_ARITHMETIC, "Compare register/memory with register"},
    {"cmp", 0x80, 1, 1, 0, INST_CAT_ARITHMETIC, "Compare immediate with register/memory"},
    {"cmp", 0x3C, 0, 1, 0, INST_CAT_ARITHMETIC, "Compare immediate with AL"},
    {"cmp", 0x3D, 0, 1, 0, INST_CAT_ARITHMETIC, "Compare immediate with EAX"},
    {"daa", 0x27, 0, 0, 0, INST_CAT_ARITHMETIC, "Decimal adjust after addition"},
    {"das", 0x2F, 0, 0, 0, INST_CAT_ARITHMETIC, "Decimal adjust after subtraction"},
    {"aaa", 0x37, 0, 0, 0, INST_CAT_ARITHMETIC, "ASCII adjust after addition"},
    {"aas", 0x3F, 0, 0, 0, INST_CAT_ARITHMETIC, "ASCII adjust after subtraction"},
    {"aam", 0xD4, 0, 1, 0, INST_CAT_ARITHMETIC, "ASCII adjust after multiplication"},
    {"aad", 0xD5, 0, 1, 0, INST_CAT_ARITHMETIC, "ASCII adjust before division"},
    {NULL, 0, 0, 0, 0, INST_CAT_ARITHMETIC, NULL}
};
    
    // Logical Instructions
static const InstructionInfo logical_instructions[] = {
    {"and", 0x20, 1, 0, 0, INST_CAT_LOGICAL, "Logical AND"},
    {"and", 0x80, 1, 1, 0, INST_CAT_LOGICAL, "Logical AND immediate"},
    {"and", 0x24, 0, 1, 0, INST_CAT_LOGICAL, "Logical AND immediate with AL"},
    {"and", 0x25, 0, 1, 0, INST_CAT_LOGICAL, "Logical AND immediate with EAX"},
    {"or", 0x08, 1, 0, 0, INST_CAT_LOGICAL, "Logical OR"},
    {"or", 0x80, 1, 1, 0, INST_CAT_LOGICAL, "Logical OR immediate"},
    {"or", 0x0C, 0, 1, 0, INST_CAT_LOGICAL, "Logical OR immediate with AL"},
    {"or", 0x0D, 0, 1, 0, INST_CAT_LOGICAL, "Logical OR immediate with EAX"},
    {"xor", 0x30, 1, 0, 0, INST_CAT_LOGICAL, "Logical XOR"},
    {"xor", 0x80, 1, 1, 0, INST_CAT_LOGICAL, "Logical XOR immediate"},
    {"xor", 0x34, 0, 1, 0, INST_CAT_LOGICAL, "Logical XOR immediate with AL"},
    {"xor", 0x35, 0, 1, 0, INST_CAT_LOGICAL, "Logical XOR immediate with EAX"},
    {"not", 0xF6, 1, 0, 0, INST_CAT_LOGICAL, "Logical NOT"},
    {"test", 0x84, 1, 0, 0, INST_CAT_LOGICAL, "Test register/memory with register"},
    {"test", 0xF6, 1, 1, 0, INST_CAT_LOGICAL, "Test immediate with register/memory"},
    {"test", 0xA8, 0, 1, 0, INST_CAT_LOGICAL, "Test immediate with AL"},
    {"test", 0xA9, 0, 1, 0, INST_CAT_LOGICAL, "Test immediate with EAX"},
    {"shl", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Shift left by 1"},
    {"shl", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Shift left by immediate"},
    {"shr", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Shift right by 1"},
    {"shr", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Shift right by immediate"},
    {"sal", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Shift arithmetic left by 1"},
    {"sal", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Shift arithmetic left by immediate"},
    {"sar", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Shift arithmetic right by 1"},
    {"sar", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Shift arithmetic right by immediate"},
    {"rol", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Rotate left by 1"},
    {"rol", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Rotate left by immediate"},
    {"ror", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Rotate right by 1"},
    {"ror", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Rotate right by immediate"},
    {"rcl", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Rotate left through carry by 1"},
    {"rcl", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Rotate left through carry by immediate"},
    {"rcr", 0xD0, 1, 0, 0, INST_CAT_LOGICAL, "Rotate right through carry by 1"},
    {"rcr", 0xC0, 1, 1, 0, INST_CAT_LOGICAL, "Rotate right through carry by immediate"},
    {"bt", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit test"},
    {"bts", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit test and set"},
    {"btr", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit test and reset"},
    {"btc", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit test and complement"},
    {"bsf", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit scan forward"},
    {"bsr", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Bit scan reverse"},
    {"sete", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if equal"},
    {"setne", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if not equal"},
    {"seta", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if above"},
    {"setae", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if above or equal"},
    {"setb", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if below"},
    {"setbe", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if below or equal"},
    {"setg", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if greater"},
    {"setge", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if greater or equal"},
    {"setl", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if less"},
    {"setle", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if less or equal"},
    {"sets", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if sign"},
    {"setns", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if not sign"},
    {"seto", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if overflow"},
    {"setno", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if not overflow"},
    {"setp", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if parity"},
    {"setnp", 0x0F, 1, 0, 0, INST_CAT_LOGICAL, "Set if not parity"},
    {NULL, 0, 0, 0, 0, INST_CAT_LOGICAL, NULL}
};
    
    // Control Flow Instructions
static const InstructionInfo control_flow_instructions[] = {
    {"jmp", 0xE9, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Unconditional jump"},
    {"jmp", 0xEB, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Short unconditional jump"},
    {"jmp", 0xFF, 1, 0, 0, INST_CAT_CONTROL_FLOW, "Indirect jump"},
    {"call", 0xE8, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Call procedure"},
    {"call", 0xFF, 1, 0, 0, INST_CAT_CONTROL_FLOW, "Indirect call"},
    {"ret", 0xC3, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Return from procedure"},
    {"ret", 0xC2, 0, 1, 0, INST_CAT_CONTROL_FLOW, "Return from procedure with immediate"},
    {"je", 0x74, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if equal"},
    {"jz", 0x74, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if zero"},
    {"jne", 0x75, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not equal"},
    {"jnz", 0x75, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not zero"},
    {"ja", 0x77, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if above"},
    {"jnbe", 0x77, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not below or equal"},
    {"jae", 0x73, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if above or equal"},
    {"jnb", 0x73, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not below"},
    {"jb", 0x72, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if below"},
    {"jnae", 0x72, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not above or equal"},
    {"jbe", 0x76, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if below or equal"},
    {"jna", 0x76, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not above"},
    {"jg", 0x7F, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if greater"},
    {"jnle", 0x7F, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not less or equal"},
    {"jge", 0x7D, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if greater or equal"},
    {"jnl", 0x7D, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not less"},
    {"jl", 0x7C, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if less"},
    {"jnge", 0x7C, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not greater or equal"},
    {"jle", 0x7E, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if less or equal"},
    {"jng", 0x7E, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not greater"},
    {"js", 0x78, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if sign"},
    {"jns", 0x79, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not sign"},
    {"jo", 0x70, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if overflow"},
    {"jno", 0x71, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not overflow"},
    {"jp", 0x7A, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if parity"},
    {"jpe", 0x7A, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if parity even"},
    {"jnp", 0x7B, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if not parity"},
    {"jpo", 0x7B, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if parity odd"},
    {"jcxz", 0xE3, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if CX is zero"},
    {"jecxz", 0xE3, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Jump if ECX is zero"},
    {"loop", 0xE2, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Loop while CX is not zero"},
    {"loope", 0xE1, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Loop while equal"},
    {"loopz", 0xE1, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Loop while zero"},
    {"loopne", 0xE0, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Loop while not equal"},
    {"loopnz", 0xE0, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Loop while not zero"},
    {"int", 0xCD, 0, 1, 0, INST_CAT_CONTROL_FLOW, "Software interrupt"},
    {"int3", 0xCC, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Breakpoint interrupt"},
    {"into", 0xCE, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Interrupt on overflow"},
    {"iret", 0xCF, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Interrupt return"},
    {"iretd", 0xCF, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Interrupt return doubleword"},
    {"bound", 0x62, 1, 0, 0, INST_CAT_CONTROL_FLOW, "Check array bounds"},
    {"enter", 0xC8, 0, 1, 0, INST_CAT_CONTROL_FLOW, "Enter procedure"},
    {"leave", 0xC9, 0, 0, 0, INST_CAT_CONTROL_FLOW, "Leave procedure"},
    {NULL, 0, 0, 0, 0, INST_CAT_CONTROL_FLOW, NULL}
};
    
    // String Instructions
static const InstructionInfo string_instructions[] = {
    {"movs", 0xA4, 0, 0, 0, INST_CAT_STRING, "Move string byte"},
    {"movsb", 0xA4, 0, 0, 0, INST_CAT_STRING, "Move string byte"},
    {"movs", 0xA5, 0, 0, 0, INST_CAT_STRING, "Move string word/doubleword"},
    {"movsw", 0xA5, 0, 0, 0, INST_CAT_STRING, "Move string word"},
    {"movsd", 0xA5, 0, 0, 0, INST_CAT_STRING, "Move string doubleword"},
    {"cmps", 0xA6, 0, 0, 0, INST_CAT_STRING, "Compare string byte"},
    {"cmpsb", 0xA6, 0, 0, 0, INST_CAT_STRING, "Compare string byte"},
    {"cmps", 0xA7, 0, 0, 0, INST_CAT_STRING, "Compare string word/doubleword"},
    {"cmpsw", 0xA7, 0, 0, 0, INST_CAT_STRING, "Compare string word"},
    {"cmpsd", 0xA7, 0, 0, 0, INST_CAT_STRING, "Compare string doubleword"},
    {"scas", 0xAE, 0, 0, 0, INST_CAT_STRING, "Scan string byte"},
    {"scasb", 0xAE, 0, 0, 0, INST_CAT_STRING, "Scan string byte"},
    {"scas", 0xAF, 0, 0, 0, INST_CAT_STRING, "Scan string word/doubleword"},
    {"scasw", 0xAF, 0, 0, 0, INST_CAT_STRING, "Scan string word"},
    {"scasd", 0xAF, 0, 0, 0, INST_CAT_STRING, "Scan string doubleword"},
    {"lods", 0xAC, 0, 0, 0, INST_CAT_STRING, "Load string byte"},
    {"lodsb", 0xAC, 0, 0, 0, INST_CAT_STRING, "Load string byte"},
    {"lods", 0xAD, 0, 0, 0, INST_CAT_STRING, "Load string word/doubleword"},
    {"lodsw", 0xAD, 0, 0, 0, INST_CAT_STRING, "Load string word"},
    {"lodsd", 0xAD, 0, 0, 0, INST_CAT_STRING, "Load string doubleword"},
    {"stos", 0xAA, 0, 0, 0, INST_CAT_STRING, "Store string byte"},
    {"stosb", 0xAA, 0, 0, 0, INST_CAT_STRING, "Store string byte"},
    {"stos", 0xAB, 0, 0, 0, INST_CAT_STRING, "Store string word/doubleword"},
    {"stosw", 0xAB, 0, 0, 0, INST_CAT_STRING, "Store string word"},
    {"stosd", 0xAB, 0, 0, 0, INST_CAT_STRING, "Store string doubleword"},
    {"ins", 0x6C, 0, 0, 0, INST_CAT_STRING, "Input string byte"},
    {"insb", 0x6C, 0, 0, 0, INST_CAT_STRING, "Input string byte"},
    {"ins", 0x6D, 0, 0, 0, INST_CAT_STRING, "Input string word/doubleword"},
    {"insw", 0x6D, 0, 0, 0, INST_CAT_STRING, "Input string word"},
    {"insd", 0x6D, 0, 0, 0, INST_CAT_STRING, "Input string doubleword"},
    {"outs", 0x6E, 0, 0, 0, INST_CAT_STRING, "Output string byte"},
    {"outsb", 0x6E, 0, 0, 0, INST_CAT_STRING, "Output string byte"},
    {"outs", 0x6F, 0, 0, 0, INST_CAT_STRING, "Output string word/doubleword"},
    {"outsw", 0x6F, 0, 0, 0, INST_CAT_STRING, "Output string word"},
    {"outsd", 0x6F, 0, 0, 0, INST_CAT_STRING, "Output string doubleword"},
    {"rep", 0xF3, 0, 0, 0, INST_CAT_STRING, "Repeat prefix"},
    {"repe", 0xF3, 0, 0, 0, INST_CAT_STRING, "Repeat while equal"},
    {"repz", 0xF3, 0, 0, 0, INST_CAT_STRING, "Repeat while zero"},
    {"repne", 0xF2, 0, 0, 0, INST_CAT_STRING, "Repeat while not equal"},
    {"repnz", 0xF2, 0, 0, 0, INST_CAT_STRING, "Repeat while not zero"},
    {NULL, 0, 0, 0, 0, INST_CAT_STRING, NULL}
};
    
    // System Instructions
static const InstructionInfo system_instructions[] = {
    {"cli", 0xFA, 0, 0, 0, INST_CAT_SYSTEM, "Clear interrupt flag"},
    {"sti", 0xFB, 0, 0, 0, INST_CAT_SYSTEM, "Set interrupt flag"},
    {"cld", 0xFC, 0, 0, 0, INST_CAT_SYSTEM, "Clear direction flag"},
    {"std", 0xFD, 0, 0, 0, INST_CAT_SYSTEM, "Set direction flag"},
    {"clc", 0xF8, 0, 0, 0, INST_CAT_SYSTEM, "Clear carry flag"},
    {"stc", 0xF9, 0, 0, 0, INST_CAT_SYSTEM, "Set carry flag"},
    {"cmc", 0xF5, 0, 0, 0, INST_CAT_SYSTEM, "Complement carry flag"},
    {"clts", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Clear task-switched flag"},
    {"invd", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Invalidate cache"},
    {"wbinvd", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Write back and invalidate cache"},
    {"hlt", 0xF4, 0, 0, 0, INST_CAT_SYSTEM, "Halt processor"},
    {"nop", 0x90, 0, 0, 0, INST_CAT_SYSTEM, "No operation"},
    {"wait", 0x9B, 0, 0, 0, INST_CAT_SYSTEM, "Wait for coprocessor"},
    {"lock", 0xF0, 0, 0, 0, INST_CAT_SYSTEM, "Lock bus"},
    {"esc", 0xD8, 1, 0, 0, INST_CAT_SYSTEM, "Escape to coprocessor"},
    {"lgdt", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Load global descriptor table"},
    {"lidt", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Load interrupt descriptor table"},
    {"sgdt", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Store global descriptor table"},
    {"sidt", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Store interrupt descriptor table"},
    {"lldt", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Load local descriptor table"},
    {"sldt", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Store local descriptor table"},
    {"ltr", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Load task register"},
    {"str", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Store task register"},
    {"lmsw", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Load machine status word"},
    {"smsw", 0x0F, 1, 0, 0, INST_CAT_SYSTEM, "Store machine status word"},
    {"clts", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Clear task-switched flag"},
    {"invd", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Invalidate cache"},
    {"wbinvd", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Write back and invalidate cache"},
    {"rdtsc", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Read time stamp counter"},
    {"rdmsr", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Read model specific register"},
    {"wrmsr", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "Write model specific register"},
    {"cpuid", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "CPU identification"},
    {"syscall", 0x0F, 0, 0, 0, INST_CAT_SYSTEM, "System call"},
    {"int", 0xCD, 0, 1, 0, INST_CAT_SYSTEM, "Software interrupt"},
    {NULL, 0, 0, 0, 0, INST_CAT_SYSTEM, NULL}
};

// Function to find instruction info
const InstructionInfo* find_instruction_info(const char* mnemonic) {
    // Search through all instruction arrays
    const InstructionInfo* arrays[] = {
        data_movement_instructions,
        arithmetic_instructions,
        logical_instructions,
        control_flow_instructions,
        string_instructions,
        system_instructions
    };
    
    for (int i = 0; i < 6; i++) {
        const InstructionInfo* info = arrays[i];
        while (info->mnemonic != NULL) {
            if (strcmp(info->mnemonic, mnemonic) == 0) {
                return info;
            }
            info++;
        }
    }
    return NULL;
}

// Function to get register encoding
int get_register_encoding(const char* reg_name) {
    for (int i = 0; i < 8; i++) {
        if (strcmp(reg_names[i], reg_name) == 0) {
            return i;
        }
    }
    return -1;
}

// Function to get 8-bit register encoding
int get_register8_encoding(const char* reg_name) {
    for (int i = 0; i < 8; i++) {
        if (strcmp(reg8_names[i], reg_name) == 0) {
            return i;
        }
    }
            return -1;
    }
    
// Function to get segment register encoding
int get_seg_reg_encoding(const char* reg_name) {
    for (int i = 0; i < 6; i++) {
        if (strcmp(seg_reg_names[i], reg_name) == 0) {
            return i;
        }
    }
    return -1;
}

// Function to check if string is a valid register
int is_valid_register(const char* name) {
    return get_register_encoding(name) >= 0 || 
           get_register8_encoding(name) >= 0 || 
           get_seg_reg_encoding(name) >= 0;
}

// Function to check if string is a valid instruction
int is_valid_instruction(const char* name) {
    return find_instruction_info(name) != NULL;
}

// Function to get instruction category description
const char* get_category_description(InstructionCategory cat) {
    switch (cat) {
        case INST_CAT_DATA_MOVEMENT: return "Data Movement";
        case INST_CAT_ARITHMETIC: return "Arithmetic";
        case INST_CAT_LOGICAL: return "Logical";
        case INST_CAT_CONTROL_FLOW: return "Control Flow";
        case INST_CAT_STRING: return "String";
        case INST_CAT_SYSTEM: return "System";
        case INST_CAT_FPU: return "FPU";
        case INST_CAT_MMX: return "MMX";
        case INST_CAT_SSE: return "SSE";
        default: return "Unknown";
    }
}

// Function to print instruction help
void print_instruction_help(const char* mnemonic) {
    const InstructionInfo* info = find_instruction_info(mnemonic);
    if (info) {
        printf("Instruction: %s\n", info->mnemonic);
        printf("Category: %s\n", get_category_description(info->category));
        printf("Description: %s\n", info->description);
        printf("Opcode: 0x%02X\n", info->opcode);
        printf("ModR/M required: %s\n", info->modrm_required ? "Yes" : "No");
        printf("Immediate required: %s\n", info->immediate_required ? "Yes" : "No");
    } else {
        printf("Unknown instruction: %s\n", mnemonic);
    }
}

// Function to list all supported instructions
void list_all_instructions() {
    const InstructionInfo* arrays[] = {
        data_movement_instructions,
        arithmetic_instructions,
        logical_instructions,
        control_flow_instructions,
        string_instructions,
        system_instructions
    };
    
    const char* category_names[] = {
        "Data Movement",
        "Arithmetic", 
        "Logical",
        "Control Flow",
        "String",
        "System"
    };
    
    for (int i = 0; i < 6; i++) {
        printf("\n%s Instructions:\n", category_names[i]);
        printf("================\n");
        const InstructionInfo* info = arrays[i];
        while (info->mnemonic != NULL) {
            printf("  %s - %s\n", info->mnemonic, info->description);
            info++;
        }
    }
} 