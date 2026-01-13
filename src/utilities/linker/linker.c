/**
 * @file linker.c
 * @brief EYN-OS ELF32 linker implementation
 * 
 * Produces ELF32 executables that match the output of:
 *   as --32 -o obj.o src.s
 *   ld -m elf_i386 -nostdlib -e _start -T user_elf32.ld -o out.uelf obj.o
 * 
 * Layout (matching GNU ld with user_elf32.ld):
 *   0x0000: ELF header (52 bytes)
 *   0x0034: Program headers (2 x 32 bytes = 64 bytes)
 *   0x0074: Padding to 0x1000
 *   0x1000: .text section (code)
 *   0x1000 + align(text_size, 0x1000): Padding
 *   0x2000: .rodata section (data)
 *   After .rodata: .symtab (symbol table)
 *   After .symtab: .strtab (string table)
 *   After .strtab: .shstrtab (section header string table)
 *   Aligned: Section headers (6 sections)
 */

#include <string.h>
#include <stdlib.h>
#include <utilities/linker.h>
#include <vga.h>
#include <eynfs.h>
#include <shell_command_info.h>

extern uint8_t g_current_drive;

// Section header string table contents (must match exact layout)
// Byte layout: "\0.symtab\0.strtab\0.shstrtab\0.text\0.rodata\0"
// Offsets:      0  1       9        17         27     33
static const char shstrtab_data[] = 
    "\0.symtab\0.strtab\0.shstrtab\0.text\0.rodata";
#define SHSTRTAB_SIZE 41  // Including final null terminator

// Offsets in shstrtab (matching GNU ld output)
#define SHSTRTAB_OFF_NULL      0x00
#define SHSTRTAB_OFF_SYMTAB    0x01  // ".symtab"
#define SHSTRTAB_OFF_STRTAB    0x09  // ".strtab"
#define SHSTRTAB_OFF_SHSTRTAB  0x11  // ".shstrtab"
#define SHSTRTAB_OFF_TEXT      0x1b  // ".text"
#define SHSTRTAB_OFF_RODATA    0x21  // ".rodata"

// Page alignment
#define PAGE_SIZE 0x1000

// Align value up to alignment boundary
static inline uint32 align_up(uint32 v, uint32 align) {
    if (align == 0) return v;
    return (v + align - 1) & ~(align - 1);
}

void link_config_init(LinkConfig *cfg) {
    memset(cfg, 0, sizeof(LinkConfig));
    cfg->text_vaddr = 0x00400000;
    cfg->rodata_vaddr = 0x00401000;
    cfg->entry_vaddr = 0x00400000;
    cfg->text.align = PAGE_SIZE;
    cfg->rodata.align = PAGE_SIZE;
    cfg->text.flags = SHF_ALLOC | SHF_EXECINSTR;
    cfg->rodata.flags = SHF_ALLOC;
}

int link_add_symbol(LinkConfig *cfg, const char *name, uint32 value,
                    uint32 size, uint8 binding, uint8 type, uint16 section) {
    if (cfg->symbol_count >= LINK_MAX_SYMBOLS) return -1;
    
    LinkSymbol *sym = &cfg->symbols[cfg->symbol_count++];
    strncpy(sym->name, name, sizeof(sym->name) - 1);
    sym->name[sizeof(sym->name) - 1] = '\0';
    sym->value = value;
    sym->size = size;
    sym->binding = binding;
    sym->type = type;
    sym->section = section;
    
    return 0;
}

// Build string table from symbols, returns size
static int build_strtab(const LinkConfig *cfg, char *strtab, int max_size) {
    int pos = 0;
    strtab[pos++] = '\0';  // First byte is always null
    
    for (int i = 0; i < cfg->symbol_count; i++) {
        int len = strlen(cfg->symbols[i].name);
        if (pos + len + 1 > max_size) return -1;
        memcpy(strtab + pos, cfg->symbols[i].name, len + 1);
        pos += len + 1;
    }
    
    return pos;
}

// Find symbol name offset in strtab
static int find_strtab_offset(const char *strtab, int strtab_size, const char *name) {
    int pos = 1;  // Skip initial null
    while (pos < strtab_size) {
        if (strcmp(strtab + pos, name) == 0) {
            return pos;
        }
        pos += strlen(strtab + pos) + 1;
    }
    return 0;  // Not found, return null string
}

uint32 link_calc_file_size(const LinkConfig *cfg) {
    // Calculate all component sizes
    uint32 ehdr_size = sizeof(Elf32_Ehdr);
    uint32 phdr_size = 2 * sizeof(Elf32_Phdr);  // Two LOAD segments
    
    uint32 text_offset = PAGE_SIZE;  // .text at 0x1000
    uint32 text_size = cfg->text.size;

    // Place .rodata immediately after .text, aligned to a page boundary.
    // (The previous fixed 0x2000 offset breaks when .text exceeds 0x1000 bytes.)
    uint32 rodata_offset = align_up(text_offset + text_size, PAGE_SIZE);
    uint32 rodata_size = cfg->rodata.size;
    
    // Calculate sizes for metadata sections
    char strtab[LINK_MAX_STRTAB];
    int strtab_size = build_strtab(cfg, strtab, sizeof(strtab));
    if (strtab_size < 0) strtab_size = 1;
    
    uint32 symtab_size = (cfg->symbol_count + 1) * sizeof(Elf32_Sym);  // +1 for null entry
    uint32 shstrtab_size = SHSTRTAB_SIZE;
    
    // Layout after rodata:
    // symtab starts right after rodata
    uint32 symtab_offset = rodata_offset + rodata_size;
    // Align symtab_offset to 4 bytes for symtab
    symtab_offset = align_up(symtab_offset, 4);
    
    uint32 strtab_offset = symtab_offset + symtab_size;
    uint32 shstrtab_offset = strtab_offset + strtab_size;
    
    // Section headers come after shstrtab, aligned to 8 bytes (matching GNU ld)
    uint32 shdr_offset = align_up(shstrtab_offset + shstrtab_size, 8);
    uint32 shdr_size = 6 * sizeof(Elf32_Shdr);  // 6 sections
    
    return shdr_offset + shdr_size;
}

int link_write_uelf(const LinkConfig *cfg, const char *output_path) {
    if (!cfg || !output_path || !output_path[0]) return -1;
    if (!cfg->text.data || cfg->text.size == 0) return -1;
    
    // Build string table
    char strtab[LINK_MAX_STRTAB];
    int strtab_size = build_strtab(cfg, strtab, sizeof(strtab));
    if (strtab_size < 0) {
        printf("[linker] String table overflow\n");
        return -1;
    }
    
    // Calculate layout
    uint32 text_offset = PAGE_SIZE;
    uint32 text_size = cfg->text.size;

    // Place .rodata immediately after .text, aligned to a page boundary.
    // (The previous fixed 0x2000 offset breaks when .text exceeds 0x1000 bytes.)
    uint32 rodata_offset = align_up(text_offset + text_size, PAGE_SIZE);
    uint32 rodata_size = cfg->rodata.size;
    
    uint32 symtab_size = (cfg->symbol_count + 1) * sizeof(Elf32_Sym);
    
    // Offsets after rodata
    uint32 symtab_offset = rodata_offset + rodata_size;
    symtab_offset = align_up(symtab_offset, 4);
    
    uint32 strtab_offset = symtab_offset + symtab_size;
    uint32 shstrtab_offset = strtab_offset + strtab_size;
    uint32 shdr_offset = align_up(shstrtab_offset + SHSTRTAB_SIZE, 8);  // GNU ld uses 8-byte alignment
    
    // Build ELF header
    Elf32_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS32;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_386;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = cfg->entry_vaddr;
    ehdr.e_phoff = sizeof(Elf32_Ehdr);
    ehdr.e_shoff = shdr_offset;
    ehdr.e_flags = 0;
    ehdr.e_ehsize = sizeof(Elf32_Ehdr);
    ehdr.e_phentsize = sizeof(Elf32_Phdr);
    ehdr.e_phnum = 2;
    ehdr.e_shentsize = sizeof(Elf32_Shdr);
    ehdr.e_shnum = 6;
    ehdr.e_shstrndx = 5;  // .shstrtab is section 5
    
    // Build program headers
    Elf32_Phdr phdr[2];
    memset(phdr, 0, sizeof(phdr));
    
    // .text segment
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_offset = text_offset;
    phdr[0].p_vaddr = cfg->text_vaddr;
    phdr[0].p_paddr = cfg->text_vaddr;
    phdr[0].p_filesz = text_size;
    phdr[0].p_memsz = text_size;
    phdr[0].p_flags = PF_R | PF_X;
    phdr[0].p_align = PAGE_SIZE;
    
    // .rodata segment
    phdr[1].p_type = PT_LOAD;
    phdr[1].p_offset = rodata_offset;
    phdr[1].p_vaddr = cfg->rodata_vaddr;
    phdr[1].p_paddr = cfg->rodata_vaddr;
    phdr[1].p_filesz = rodata_size;
    phdr[1].p_memsz = rodata_size;
    phdr[1].p_flags = PF_R;
    phdr[1].p_align = PAGE_SIZE;
    
    // Build section headers
    Elf32_Shdr shdr[6];
    memset(shdr, 0, sizeof(shdr));
    
    // [0] NULL section
    // (already zeroed)
    
    // [1] .text
    shdr[1].sh_name = SHSTRTAB_OFF_TEXT;
    shdr[1].sh_type = SHT_PROGBITS;
    shdr[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdr[1].sh_addr = cfg->text_vaddr;
    shdr[1].sh_offset = text_offset;
    shdr[1].sh_size = text_size;
    shdr[1].sh_link = 0;
    shdr[1].sh_info = 0;
    shdr[1].sh_addralign = PAGE_SIZE;
    shdr[1].sh_entsize = 0;
    
    // [2] .rodata
    shdr[2].sh_name = SHSTRTAB_OFF_RODATA;
    shdr[2].sh_type = SHT_PROGBITS;
    shdr[2].sh_flags = SHF_ALLOC;
    shdr[2].sh_addr = cfg->rodata_vaddr;
    shdr[2].sh_offset = rodata_offset;
    shdr[2].sh_size = rodata_size;
    shdr[2].sh_link = 0;
    shdr[2].sh_info = 0;
    shdr[2].sh_addralign = PAGE_SIZE;
    shdr[2].sh_entsize = 0;
    
    // [3] .symtab
    shdr[3].sh_name = SHSTRTAB_OFF_SYMTAB;
    shdr[3].sh_type = SHT_SYMTAB;
    shdr[3].sh_flags = 0;
    shdr[3].sh_addr = 0;
    shdr[3].sh_offset = symtab_offset;
    shdr[3].sh_size = symtab_size;
    shdr[3].sh_link = 4;  // .strtab section index
    shdr[3].sh_info = cfg->symbol_count + 1;  // Index of first global symbol (after locals+1)
    shdr[3].sh_addralign = 4;
    shdr[3].sh_entsize = sizeof(Elf32_Sym);
    
    // Count local symbols to set sh_info properly
    int local_count = 1;  // Start with 1 for null symbol
    for (int i = 0; i < cfg->symbol_count; i++) {
        if (cfg->symbols[i].binding == STB_LOCAL) local_count++;
    }
    shdr[3].sh_info = local_count;
    
    // [4] .strtab
    shdr[4].sh_name = SHSTRTAB_OFF_STRTAB;
    shdr[4].sh_type = SHT_STRTAB;
    shdr[4].sh_flags = 0;
    shdr[4].sh_addr = 0;
    shdr[4].sh_offset = strtab_offset;
    shdr[4].sh_size = strtab_size;
    shdr[4].sh_link = 0;
    shdr[4].sh_info = 0;
    shdr[4].sh_addralign = 1;
    shdr[4].sh_entsize = 0;
    
    // [5] .shstrtab
    shdr[5].sh_name = SHSTRTAB_OFF_SHSTRTAB;
    shdr[5].sh_type = SHT_STRTAB;
    shdr[5].sh_flags = 0;
    shdr[5].sh_addr = 0;
    shdr[5].sh_offset = shstrtab_offset;
    shdr[5].sh_size = SHSTRTAB_SIZE;
    shdr[5].sh_link = 0;
    shdr[5].sh_info = 0;
    shdr[5].sh_addralign = 1;
    shdr[5].sh_entsize = 0;
    
    // Build symbol table using stack buffer (no malloc needed)
    // Max entries = LINK_MAX_SYMBOLS + 1 (for null entry) = 33 entries * 16 bytes = 528 bytes
    Elf32_Sym syms[LINK_MAX_SYMBOLS + 1];
    int sym_entries = cfg->symbol_count + 1;
    memset(syms, 0, sizeof(syms));
    
    // Entry 0: null symbol (already zeroed)
    
    // Add symbols (local symbols first, then globals)
    int sym_idx = 1;
    
    // First pass: add local symbols
    for (int i = 0; i < cfg->symbol_count; i++) {
        if (cfg->symbols[i].binding == STB_LOCAL) {
            syms[sym_idx].st_name = find_strtab_offset(strtab, strtab_size, cfg->symbols[i].name);
            syms[sym_idx].st_value = cfg->symbols[i].value;
            syms[sym_idx].st_size = cfg->symbols[i].size;
            syms[sym_idx].st_info = ELF32_ST_INFO(cfg->symbols[i].binding, cfg->symbols[i].type);
            syms[sym_idx].st_other = 0;
            syms[sym_idx].st_shndx = cfg->symbols[i].section;
            sym_idx++;
        }
    }
    
    // Second pass: add global symbols
    for (int i = 0; i < cfg->symbol_count; i++) {
        if (cfg->symbols[i].binding != STB_LOCAL) {
            syms[sym_idx].st_name = find_strtab_offset(strtab, strtab_size, cfg->symbols[i].name);
            syms[sym_idx].st_value = cfg->symbols[i].value;
            syms[sym_idx].st_size = cfg->symbols[i].size;
            syms[sym_idx].st_info = ELF32_ST_INFO(cfg->symbols[i].binding, cfg->symbols[i].type);
            syms[sym_idx].st_other = 0;
            syms[sym_idx].st_shndx = cfg->symbols[i].section;
            sym_idx++;
        }
    }
    
    // Open output stream
    eynfs_stream_t stream;
    if (eynfs_stream_begin(g_current_drive, output_path, &stream) != 0) {
        printf("[linker] Failed to open output file: %s\n", output_path);
        return -1;
    }
    
    // Write ELF header
    if (eynfs_stream_write(&stream, &ehdr, sizeof(ehdr)) < 0) {
        printf("[linker] Failed to write ELF header\n");
        return -1;
    }
    
    // Write program headers
    if (eynfs_stream_write(&stream, phdr, sizeof(phdr)) < 0) {
        printf("[linker] Failed to write program headers\n");
        return -1;
    }
    
    // Pad to text_offset
    uint32 current_offset = sizeof(ehdr) + sizeof(phdr);
    uint8 zero[64];
    memset(zero, 0, sizeof(zero));
    while (current_offset < text_offset) {
        uint32 pad = text_offset - current_offset;
        if (pad > sizeof(zero)) pad = sizeof(zero);
        if (eynfs_stream_write(&stream, zero, pad) < 0) {
            printf("[linker] Failed to pad to .text\n");
            return -1;
        }
        current_offset += pad;
    }
    
    // Write .text
    if (eynfs_stream_write(&stream, (void *)cfg->text.data, text_size) < 0) {
        printf("[linker] Failed to write .text\n");
        return -1;
    }
    current_offset += text_size;
    
    // Pad to rodata_offset
    while (current_offset < rodata_offset) {
        uint32 pad = rodata_offset - current_offset;
        if (pad > sizeof(zero)) pad = sizeof(zero);
        if (eynfs_stream_write(&stream, zero, pad) < 0) {
            printf("[linker] Failed to pad to .rodata\n");
            return -1;
        }
        current_offset += pad;
    }
    
    // Write .rodata
    if (rodata_size > 0 && cfg->rodata.data) {
        if (eynfs_stream_write(&stream, (void *)cfg->rodata.data, rodata_size) < 0) {
            printf("[linker] Failed to write .rodata\n");
            return -1;
        }
        current_offset += rodata_size;
    }
    
    // Pad to symtab_offset
    while (current_offset < symtab_offset) {
        uint32 pad = symtab_offset - current_offset;
        if (pad > sizeof(zero)) pad = sizeof(zero);
        if (eynfs_stream_write(&stream, zero, pad) < 0) {
            printf("[linker] Failed to pad to .symtab\n");
            return -1;
        }
        current_offset += pad;
    }
    
    // Write .symtab
    if (eynfs_stream_write(&stream, syms, symtab_size) < 0) {
        printf("[linker] Failed to write .symtab\n");
        return -1;
    }
    current_offset += symtab_size;
    
    // Write .strtab
    if (eynfs_stream_write(&stream, strtab, strtab_size) < 0) {
        printf("[linker] Failed to write .strtab\n");
        return -1;
    }
    current_offset += strtab_size;
    
    // Write .shstrtab
    if (eynfs_stream_write(&stream, (void *)shstrtab_data, SHSTRTAB_SIZE) < 0) {
        printf("[linker] Failed to write .shstrtab\n");
        return -1;
    }
    current_offset += SHSTRTAB_SIZE;
    
    // Pad to section headers
    while (current_offset < shdr_offset) {
        uint32 pad = shdr_offset - current_offset;
        if (pad > sizeof(zero)) pad = sizeof(zero);
        if (eynfs_stream_write(&stream, zero, pad) < 0) {
            printf("[linker] Failed to pad to section headers\n");
            return -1;
        }
        current_offset += pad;
    }
    
    // Write section headers
    if (eynfs_stream_write(&stream, shdr, sizeof(shdr)) < 0) {
        printf("[linker] Failed to write section headers\n");
        return -1;
    }
    
    // Finalize
    if (eynfs_stream_end(&stream) != 0) {
        printf("[linker] Failed to finalize output file\n");
        return -1;
    }
    
    return 0;
}

// Shell command handler for link command
void handler_link(string arg);

void handler_link(string arg) {
    (void)arg;
    printf("EYN-OS Linker\n");
    printf("Usage: link <input.o> <output.uelf>\n");
    printf("Note: Currently designed to be called from the assembler.\n");
    printf("      Use 'assemble file.s file.uelf' to assemble and link.\n");
}

REGISTER_SHELL_COMMAND(link, "link", handler_link, CMD_STREAMING, 
    "Links object files into ELF32 executables.\n"
    "Usage: link <input.o> <output.uelf>",
    "link program.o program.uelf");
