# EYN-OS Header Files and API

This document provides a comprehensive overview of all header files in EYN-OS and their associated APIs.

## Core System Headers

### `types.h`
Defines basic data types and constants used throughout EYN-OS.

#### Data Types
```c
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;
```

#### Constants
```c
#define NULL ((void*)0)
#define TRUE 1
#define FALSE 0
```

### `system.h`
Provides system-level functions and hardware access.

#### I/O Functions
```c
uint8_t inportb(uint16_t port);
void outportb(uint16_t port, uint8_t value);
void io_wait();
```

#### Interrupt Functions
```c
void cli();  // Clear interrupts
void sti();  // Set interrupts
void hlt();  // Halt processor
```

### `string.h`
String manipulation and memory functions.

#### Memory Functions
```c
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* dest, int c, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
```

#### String Functions
```c
size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int strcmp(const char* s1, const char* s2);
char* strtok(char* str, const char* delim);
```

## Hardware Driver Headers

### `vga.h`
Framebuffer drawing, text rendering, and bitmap font management.

#### Drawing and text
```c
void drawRect(int x, int y, int w, int h, int r, int g, int b);
void drawTextAt(int x, int y, const char* text, int r, int g, int b);
void drawCharAt(int x, int y, int charnum, int r, int g, int b);
void drawCharAt_font(int font_handle, int x, int y, int charnum, int r, int g, int b);
void clearScreen(void);
```

#### Font acquisition and lifetime
Fonts are loaded from `.hex` files via the VFS and stored as a 256-glyph bitmap table in RAM.

```c
int vga_font_acquire_hex(uint8 drive, const char* path);
void vga_font_release(int font_handle);

int vga_system_font_acquire(void);
int vga_system_font_set(uint8 drive, const char* path);
```

Notes:
- `vga_system_font_set()` accepts `path = "builtin"` (or NULL/empty) to revert to the built-in fallback font.
- The text pipeline currently treats the character value as a **glyph index 0–255**.

#### Font metrics
Use these helpers for layout; the active system font can be 8×8 or 8×16.

```c
int vga_text_cell_w(void);
int vga_text_cell_h(void);
int vga_font_glyph_h(int font_handle);
```

### `kb.h`
Keyboard input handling.

#### Input Functions
```c
char readChar();
char* readStr(char* buffer, int maxLen);
char* readStr_with_history(char* buffer, int maxLen);
int keyboard_available();
int get_key();
```

#### Key Constants
```c
#define KEY_UP 0x1001
#define KEY_DOWN 0x1002
#define KEY_LEFT 0x1003
#define KEY_RIGHT 0x1004
#define KEY_ENTER '\n'
#define KEY_ESCAPE 27
```

### `ata.h`
ATA/IDE disk controller interface.

#### Disk Functions
```c
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t* buffer, uint8_t count);
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t* buffer, uint8_t count);
int ata_identify(uint8_t drive);
```

## Filesystem Headers

### `eynfs.h`
EYNFS native filesystem definitions.

#### Data Structures
```c
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t root_dir_block;
    uint32_t block_size;
    char volume_name[32];
    uint8_t reserved[480];
} eynfs_superblock_t;

typedef struct {
    char name[32];
    uint32_t size;
    uint32_t first_block;
    uint32_t parent_block;
    uint32_t entry_index;
    uint8_t type;
    uint8_t attributes;
    uint16_t reserved;
    uint8_t padding[28];
} eynfs_dir_entry_t;
```

#### Filesystem Functions
```c
int eynfs_read_superblock(uint8_t drive, uint32_t lba, eynfs_superblock_t* sb);
int eynfs_find_in_dir(uint8_t drive, eynfs_superblock_t* sb, uint32_t dir_block, 
                     const char* name, eynfs_dir_entry_t* entry);
int eynfs_read_file(uint8_t drive, eynfs_superblock_t* sb, eynfs_dir_entry_t* entry,
                   void* buffer, uint32_t size, uint32_t offset);
int eynfs_write_file(uint8_t drive, eynfs_superblock_t* sb, eynfs_dir_entry_t* entry,
                    const void* data, uint32_t size, uint32_t offset);
```

### `fat32.h`
FAT32 filesystem support.

#### FAT32 Structures

### `rei.h`
REI (Raw EYN Image) format support.

#### Data Structures
```c
typedef struct {
    uint32_t magic;        // Magic number to identify REI
    uint16_t width;        // Image width in pixels
    uint16_t height;       // Image height in pixels
    uint8_t depth;         // Color depth (1, 3, or 4 bytes per pixel)
    uint8_t reserved1;     // Reserved for future use
    uint16_t reserved2;    // Reserved for future use
} rei_header_t;

typedef struct {
    rei_header_t header;
    uint8_t* data;         // Raw pixel data
    size_t data_size;      // Size of pixel data
} rei_image_t;
```

#### Core Functions
```c
int rei_parse_image(const uint8_t* data, size_t size, rei_image_t* image);
int rei_display_image(const rei_image_t* image, int x, int y);
void rei_free_image(rei_image_t* image);
```

#### Constants
```c
#define REI_MAGIC 0x52454900        // Magic number ('REI\0')
#define REI_MAX_WIDTH 320           // Maximum image width
#define REI_MAX_HEIGHT 200          // Maximum image height
#define REI_DEPTH_MONO 1            // Monochrome (1 byte per pixel)
#define REI_DEPTH_RGB 3             // RGB (3 bytes per pixel)
#define REI_DEPTH_RGBA 4            // RGBA (4 bytes per pixel)
```
```c
typedef struct {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t large_sectors;
} fat32_boot_sector_t;
```

## User Interface Headers

### `tui.h`
Text User Interface framework.

#### TUI Functions
```c
void tui_init();
void tui_draw_text(int x, int y, const char* text, uint8_t color);
int tui_read_key();
void tui_clear_window();
void tui_draw_border(int x, int y, int width, int height);
```

#### TUI Structures
```c
typedef struct {
    int x, y;
    int width, height;
    char* title;
} tui_window_t;
```

### `help_tui.h`
Help system TUI interface.

#### Help Functions
```c
void help_tui();
void display_command_help(const char* command);
void sort_commands_alphabetically();
```

### `write_editor.h`
Text editor interface.

#### Editor Functions
```c
void write_editor(const char* filename);
void save_write_editor_buffer(const char* filename, const char* buffer, int len);
char* load_write_editor_buffer(const char* filename);
```

## Shell System Headers

### `shell.h`
Shell core functionality.

#### Shell Structures
```c
typedef struct {
    char commands[MAX_HISTORY_SIZE][MAX_COMMAND_LENGTH];
    int count;
    int current;
} command_history_t;
```

#### Shell Functions
```c
void launch_shell();
void add_to_history(const char* command);
void clear_history();
void show_history();
```

### `shell_commands.h`
Shell command declarations.

#### Command Functions
```c
void ls_cmd(string args);
void cd_cmd(string args);
void del_cmd(string args);
void write_cmd(string args);
void help_cmd(string args);
void random_cmd(string args);
void sort_cmd(string args);
void search_cmd(string args);
void game_cmd(string args);
```

### `shell_command_info.h`
Command registration system.

#### Registration Macro
```c
#define REGISTER_SHELL_COMMAND(name, description, function) \
    { name, description, function }
```

#### Command Structure
```c
typedef struct {
    const char* name;
    const char* description;
    void (*function)(string args);
} shell_command_t;
```

## Development Tools Headers

### `assemble.h`
Assembler type definitions and functions.

#### Assembler Structures
```c
typedef struct {
    char* name;
    int type;
    int value;
} token_t;

typedef struct {
    char* name;
    uint32_t address;
} symbol_t;
```

#### Assembler Functions
```c
int assemble_file(const char* input, const char* output);
token_t* lexer(const char* source);
ast_node_t* parser(token_t* tokens);
void generate_code(ast_node_t* ast, const char* output);
```

### `eyn_exe_format.h`
EYN executable format definitions.

#### Executable Header
```c
typedef struct {
    char magic[4];      // "EYN\0"
    uint32_t version;   // Format version
    uint32_t entry;     // Entry point
    uint32_t code_size; // Code section size
    uint32_t data_size; // Data section size
    uint32_t bss_size;  // BSS section size
} eyn_header_t;
```

## Math and Utilities Headers

### `math.h`
Mathematical functions and algorithms.

#### Random Number Functions
```c
void rand_seed(uint32_t seed);
uint32_t rand_next();
uint32_t rand_range(uint32_t min, uint32_t max);
```

#### Sorting Functions
```c
void quicksort_strings(char** arr, int low, int high);
int partition_strings(char** arr, int low, int high);
void swap_strings(char** a, char** b);
```

#### Search Functions
```c
int boyer_moore_search(const char* text, const char* pattern);
void build_bad_char_table(const char* pattern, int* bad_char);
void build_good_suffix_table(const char* pattern, int* good_suffix);
```

### `util.h`
General utility functions.

#### Memory Management
```c
void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t count, size_t size);
```

#### Utility Functions
```c
int str_to_int(const char* str);
char* int_to_string(int value);
void sleep(uint8_t times);
uint32_t detect_available_memory(void);
void print_memory_stats(void);
uint32_t get_heap_size(void);
int get_memory_error_count(void);
int get_stack_overflow_status(void);
uint32_t get_current_stack_pointer(void);
void check_stack_overflow(void);
void putchar(char c);
```

## Game Engine Headers

### `game_engine.h`
Game engine framework.

#### Game Structures
```c
typedef struct {
    char title[32];
    char controls[32];
    uint32_t board_width;
    uint32_t board_height;
} game_data_t;

typedef struct {
    game_data_t* data;
    uint8_t* board;
    int score;
    int paused;
    int game_over;
    uint32_t last_move_time;
} game_state_t;
```

#### Game Functions
```c
int game_engine_init();
int game_load_from_file(const char* filename);
int game_run();
void game_engine_cleanup();
void game_draw_board(game_state_t* state);
int game_handle_input(game_state_t* state);
```

## Filesystem Command Headers

### `fs_commands.h`
Filesystem command utilities.

#### Path Functions
```c
void resolve_path(const char* input, const char* cwd, char* out, size_t outsz);
int eynfs_ls_depth(uint8_t drive, eynfs_superblock_t* sb, uint32_t dir_block, int depth);
int write_output_to_file(const char* filename, const char* data, int len);
```

#### Global Variables
```c
extern char shell_current_path[128];
extern uint8_t g_current_drive;
```

## Interrupt and CPU Headers

### `idt.h`
Interrupt Descriptor Table definitions.

#### IDT Structures
```c
typedef struct {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t base_hi;
} idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;
```

#### IDT Functions
```c
void idt_install();
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
```

### `isr.h`
Interrupt Service Routines.

## Diagnostics and Reliability Headers

### `panic.h`
Panic and assertion diagnostics with a graphical report and serial backtrace.

#### Functions
```c
void panic(const char* msg, const char* file, int line);
void panicf(const char* file, int line, const char* fmt, ...);
void assert_fail(const char* expr, const char* file, int line);
void backtrace(void);
int  panic_is_in_progress(void);
```

#### Macros
```c
#define PANIC(msg) panic((msg), __FILE__, __LINE__)
#define PANICF(fmt, ...) panicf(__FILE__, __LINE__, (fmt), __VA_ARGS__)
#define ASSERT(x) do { if (!(x)) assert_fail(#x, __FILE__, __LINE__); } while (0)
```

### `watchdog.h`
System watchdog to detect stalls and trigger a panic with last progress source.

#### Functions
```c
void watchdog_init(uint32 timeout_ticks);
void watchdog_kick(const char* source);
void watchdog_on_tick(void);
void watchdog_set_timeout(uint32 timeout_ticks);
uint32 watchdog_get_timeout(void);
uint32 watchdog_get_ticks_since_kick(void);
const char* watchdog_get_last_source(void);
```

#### ISR Functions
```c
void isr_install();
void fault_handler(struct regs* r);
void irq_handler(struct regs* r);
```

#### ISR Structures
```c
typedef struct {
    uint32_t edi, esi, ebp, esp;
    uint32_t ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;
```

## API Usage Guidelines

### Header Inclusion
Always include headers between greater/lesser than signs:
```c
#include <vga.h>
#include <string.h>
```

### Function Naming
- **System functions**: Lowercase with underscores
- **Constants**: Uppercase with underscores
- **Types**: Lowercase with underscores, ending in `_t`

### Error Handling
Most functions return:
- `0` for success
- `-1` for general error
- Positive values for specific error codes

### Memory Management
- Use `malloc()` and `free()` for dynamic allocation
- Always check return values for NULL
- Free memory when no longer needed

---