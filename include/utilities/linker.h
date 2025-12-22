/**
 * @file linker.h
 * @brief EYN-OS ELF32 linker for producing .uelf executables
 * 
 * This linker produces ELF32 executables compatible with the user_elf_run loader.
 * It aims to produce output identical to GNU ld when using the user_elf32.ld script.
 */
#ifndef LINKER_H
#define LINKER_H

#include <stddef.h>
#include <stdint.h>
#include <misc/types.h>

// ELF32 Data Types
typedef uint32 Elf32_Addr;
typedef uint16 Elf32_Half;
typedef uint32 Elf32_Off;
typedef int32  Elf32_Sword;
typedef uint32 Elf32_Word;

// ELF Magic
#define EI_NIDENT 16
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

// e_ident indices
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_VERSION 6
#define EI_OSABI   7
#define EI_PAD     8

// ELF class
#define ELFCLASSNONE 0
#define ELFCLASS32   1
#define ELFCLASS64   2

// Data encoding
#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF Version
#define EV_NONE    0
#define EV_CURRENT 1

// Object file type
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4

// Machine type
#define EM_NONE  0
#define EM_386   3
#define EM_X86_64 62

// Section header types
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_SHLIB    10
#define SHT_DYNSYM   11

// Section flags
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

// Program header types
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6

// Program header flags
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

// Symbol binding
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

// Symbol type
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

// Special section indices
#define SHN_UNDEF 0
#define SHN_ABS   0xFFF1

// Macros for symbol info
#define ELF32_ST_BIND(i)    ((i) >> 4)
#define ELF32_ST_TYPE(i)    ((i) & 0xf)
#define ELF32_ST_INFO(b,t)  (((b) << 4) + ((t) & 0xf))

// ELF32 Header
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

// ELF32 Program Header
typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

// ELF32 Section Header
typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

// ELF32 Symbol
typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

// Maximum symbols and string table sizes
#define LINK_MAX_SYMBOLS 64
#define LINK_MAX_STRTAB  512

// Symbol entry for linker
typedef struct {
    char name[64];
    uint32 value;
    uint32 size;
    uint8  binding;   // STB_LOCAL, STB_GLOBAL
    uint8  type;      // STT_NOTYPE, STT_FILE, etc.
    uint16 section;   // Section index (1=.text, 2=.rodata, SHN_ABS, etc.)
} LinkSymbol;

// Input section for linker
typedef struct {
    const uint8 *data;
    uint32 size;
    uint32 vaddr;
    uint32 align;
    uint32 flags;  // SHF_* flags
} LinkSection;

// Linker configuration
typedef struct {
    const char *input_name;     // Source file name (for FILE symbol)
    uint32 text_vaddr;          // Virtual address for .text (default 0x00400000)
    uint32 rodata_vaddr;        // Virtual address for .rodata
    uint32 entry_vaddr;         // Entry point virtual address
    
    // Input sections
    LinkSection text;           // .text section
    LinkSection rodata;         // .rodata section
    
    // Symbols
    LinkSymbol symbols[LINK_MAX_SYMBOLS];
    int symbol_count;
} LinkConfig;

/**
 * Initialize a link configuration with defaults.
 */
void link_config_init(LinkConfig *cfg);

/**
 * Add a symbol to the link configuration.
 * @param cfg Linker configuration
 * @param name Symbol name
 * @param value Symbol value (address)
 * @param size Symbol size (0 for labels)
 * @param binding Symbol binding (STB_LOCAL or STB_GLOBAL)
 * @param type Symbol type (STT_NOTYPE, STT_FILE, etc.)
 * @param section Section index (1=.text, 2=.rodata, SHN_ABS)
 * @return 0 on success, -1 if symbol table full
 */
int link_add_symbol(LinkConfig *cfg, const char *name, uint32 value,
                    uint32 size, uint8 binding, uint8 type, uint16 section);

/**
 * Link and write a .uelf file.
 * @param cfg Linker configuration
 * @param output_path Output file path
 * @return 0 on success, non-zero on failure
 */
int link_write_uelf(const LinkConfig *cfg, const char *output_path);

/**
 * Calculate the total size of the output ELF file.
 * @param cfg Linker configuration
 * @return Total file size in bytes
 */
uint32 link_calc_file_size(const LinkConfig *cfg);

#endif /* LINKER_H */
