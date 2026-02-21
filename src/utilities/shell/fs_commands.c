#include <fs_commands.h>
#include <shell_command_info.h>
#include <types.h>
#include <vga.h>
#include <util.h>
#include <fat32.h>
#include <eynfs.h>
#include <string.h>
#include <write_editor.h>
#include <kb.h>
#include <system.h>
#include <subcommands.h>
#include <stdint.h>
#include <help_tui.h>
#include <fs/vfs.h>
#include <terminals.h>
#include <context.h>
#include <misc/sched.h>
#include <utilities/shell/shell_args.h>

// Forward declarations for command handlers
void ls_cmd(const shell_args_t* args);
void read_cmd(const shell_args_t* args);
void del(const shell_args_t* args);
void write_cmd(const shell_args_t* args);
void size(const shell_args_t* args);
void cd(const shell_args_t* args);
void makedir(const shell_args_t* args);
void deldir(const shell_args_t* args);
void fscheck(const shell_args_t* args);
void copy_cmd(const shell_args_t* args);
void move_cmd(const shell_args_t* args);
void fatfix_cmd(const shell_args_t* args);

// EYNFS integration: assume superblock at LBA 2048 on drive 0
#define EYNFS_SUPERBLOCK_LBA 2048
#define EYNFS_DRIVE 0

// Global variable for current drive (default 0)
extern uint8_t g_current_drive;

// Helper function prototypes (if not already declared)
void to_fat32_83(const char* input, char* output);
int parse_redirection(const char* input, char* cmd, char* filename);
uint32 str_to_uint(const char* s);

extern void* fat32_disk_img;
extern void poll_keyboard_for_ctrl_c();

#define MAX_ENTRIES 128  // Increased from 9 to 128 to support larger directories

extern char shell_current_path[128]; // from shell.c

static int fs_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void fs_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Helper: resolve relative/absolute path to absolute
void resolve_path(const char* input, const char* cwd, char* out, size_t outsz) {
    if (!input || !input[0]) {
        if (!out || outsz == 0) return;
        if (!cwd) { out[0] = '\0'; return; }
        size_t i = 0;
        while (i + 1 < outsz && cwd[i]) {
            out[i] = cwd[i];
            i++;
        }
        out[i] = '\0';
        return;
    }
    if (input[0] == '/') {
        strncpy(out, input, outsz-1); out[outsz-1] = '\0';
        return;
    }
    // Join cwd and input
    char tmp[256];
    size_t cwdlen = strlen(cwd);
    if (cwdlen > 1 && cwd[cwdlen-1] == '/') cwdlen--;
    strncpy(tmp, cwd, cwdlen); tmp[cwdlen] = '\0';
    if (cwdlen > 0 && tmp[cwdlen-1] != '/') strncat(tmp, "/", sizeof(tmp)-strlen(tmp)-1);
    strncat(tmp, input, sizeof(tmp)-strlen(tmp)-1);
    // Normalize: handle ., .., //
    char* parts[64]; int nparts = 0;
    char* tok; char* save;
    for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (nparts > 0) nparts--; continue; }
        parts[nparts++] = tok;
    }
    out[0] = '/'; out[1] = '\0';
    for (int i = 0; i < nparts; ++i) {
        strncat(out, parts[i], outsz-strlen(out)-1);
        if (i < nparts-1) strncat(out, "/", outsz-strlen(out)-1);
    }
    // Do NOT add a trailing slash
}

// Helper: get last path component (filename) from a path
static const char* get_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

// cd command
void cd(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        printf("%cUsage: cd <directory>\n", 255, 255, 255);
        return;
    }

    const char* arg = args->argv[1];
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    if (strcmp(abspath, "/") == 0) {
        strncpy(shell_current_path, "/", sizeof(shell_current_path)-1);
        shell_current_path[sizeof(shell_current_path)-1] = '\0';
        return;
    }
    vfs_stat_t st;
    if (vfs_stat(disk, abspath, &st) == 0 && st.type == VFS_NODE_DIR) {
        strncpy(shell_current_path, abspath, sizeof(shell_current_path)-1);
        shell_current_path[sizeof(shell_current_path)-1] = '\0';
        return;
    }
    printf("%cDirectory not found: %s\n", 255, 0, 0, abspath);
}

typedef struct {
    uint8 disk;
    char dir_path[128];
} ls_ctx_t;

typedef struct {
    int icon_pad;
    int max_item_width;
    int count;
} ls_measure_t;

typedef struct {
    uint8 disk;
    char dir_path[128];
    int icon_pad;
    int col_width;
} ls_cols_ctx_t;

typedef struct {
    char name[64];
    int is_dir;
} ls_entry_t;

typedef struct {
    uint8 disk;
    char dir_path[128];
    int icon_pad;
    int max_item_width;
    int count;
    ls_entry_t entries[MAX_ENTRIES];
} ls_collect_t;

static void ls_print_spaces(int n) {
    for (int i = 0; i < n; ++i) putchar(' ');
}

static int ls_icon_pad_cols(void) {
    // Icons are either 8x8 or 16x16 pixels. Reserve enough character cells
    // to cover the icon width based on the current text cell width.
    int cell_w = vga_text_cell_w();
    if (cell_w <= 0) cell_w = 8;

    int icon_px = (vga_text_cell_h() >= 16) ? 16 : 8;
    int pad = (icon_px + cell_w - 1) / cell_w;
    if (pad < 1) pad = 1;
    return pad;
}

static int ls_any_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)name; (void)is_dir; (void)size;
    int* found = (int*)user;
    *found = 1;
    return 1; // stop
}

static int ls_dir_is_empty(uint8 disk, const char* abspath) {
    int found = 0;
    if (vfs_listdir(disk, abspath, ls_any_cb, &found) != 0) {
        // If we can't read the directory, treat as non-empty to avoid lying.
        return 0;
    }
    return found ? 0 : 1;
}

static void ls_build_child_path(const char* parent, const char* name, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!parent || !name) { out[0] = '\0'; return; }
    if (strcmp(parent, "/") == 0) {
        snprintf(out, out_sz, "/%s", name);
    } else {
        snprintf(out, out_sz, "%s/%s", parent, name);
    }
    out[out_sz - 1] = '\0';
}

static void ls_icon_key_for_entry(uint8 disk, const char* parent_dir, const char* name, int is_dir, char out_key[16]) {
    if (!out_key) return;
    out_key[0] = '\0';

    if (is_dir) {
        char child[192];
        ls_build_child_path(parent_dir, name, child, sizeof(child));
        int empty = ls_dir_is_empty(disk, child);
        safe_strcpy(out_key, empty ? "dir_empty" : "dir_full", 16);
        return;
    }

    // File: choose icon by extension -> file_<ext>, else file_none
    const char* dot = NULL;
    for (const char* p = name; *p; ++p) if (*p == '.') dot = p;
    if (!dot || dot[1] == '\0') { safe_strcpy(out_key, "file_none", 16); return; }

    char ext[9];
    int ei = 0;
    for (const char* p = dot + 1; *p && ei < 8; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[ei++] = c;
    }
    ext[ei] = '\0';
    if (ei == 0) { safe_strcpy(out_key, "file_none", 16); return; }

    // Special-case mappings
    // - .s should use the assembler icon
    // - executables should use the EYN icon (native + ring3)
    // - .hex should use the bin icon (fonts, hex blobs)
    if (strcmp(ext, "s") == 0 || strcmp(ext, "asm") == 0) {
        safe_strcpy(out_key, "file_asm", 16);
        return;
    }
    if (strcmp(ext, "hex") == 0) {
        safe_strcpy(out_key, "file_bin", 16);
        return;
    }
    if (strcmp(ext, "uelf") == 0 || strcmp(ext, "elf") == 0 || strcmp(ext, "eyn") == 0 ||
        strcmp(ext, "bin") == 0 || strcmp(ext, "flat") == 0) {
        safe_strcpy(out_key, "file_eyn", 16);
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "file_%s", ext);
    key[sizeof(key) - 1] = '\0';
    safe_strcpy(out_key, key, 16);
}

static int vfs_ls_measure_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    ls_measure_t* m = (ls_measure_t*)user;
    if (!m) return 0;

    int w = m->icon_pad;
    w += (int)strlen(name);
    if (is_dir) w += 1; // trailing '/'

    if (w > m->max_item_width) m->max_item_width = w;
    m->count++;
    return 0;
}

static void ls_print_entry_cols(const ls_cols_ctx_t* ctx, const char* name, int is_dir) {
    if (!ctx || !name) return;

    char icon_key[16];
    ls_icon_key_for_entry(ctx->disk, ctx->dir_path, name, is_dir, icon_key);
    shell_register_redirect_icon(icon_key);

    ls_print_spaces(ctx->icon_pad);

    if (is_dir) {
        printf("%c%s/", 120, 120, 255, name);
    } else {
        printf("%c%s", 255, 255, 255, name);
    }
}

static int vfs_ls_collect_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    ls_collect_t* c = (ls_collect_t*)user;
    if (!c || !name) return 0;
    if (c->count >= MAX_ENTRIES) return 1; // stop

    ls_entry_t* e = &c->entries[c->count++];
    if ((c->count & 0xF) == 0) fs_ctx_account(SCHED_COST_FS);
    safe_strcpy(e->name, name, sizeof(e->name));
    e->is_dir = is_dir;

    int w = c->icon_pad + (int)strlen(e->name) + (is_dir ? 1 : 0);
    if (w > c->max_item_width) c->max_item_width = w;
    return 0;
}

static int ls_tolower_ascii(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int ls_name_cmp(const char* a, const char* b) {
    // Case-insensitive ASCII compare; ties broken by original bytes.
    if (!a) a = "";
    if (!b) b = "";
    for (int i = 0; a[i] || b[i]; ++i) {
        int ca = ls_tolower_ascii((unsigned char)a[i]);
        int cb = ls_tolower_ascii((unsigned char)b[i]);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        // identical lower-case; use original for stable-ish ordering
        if ((unsigned char)a[i] != (unsigned char)b[i]) {
            return ((unsigned char)a[i] < (unsigned char)b[i]) ? -1 : 1;
        }
        if (!a[i] || !b[i]) break;
    }
    return 0;
}

static void ls_sort_entries(ls_entry_t* entries, int count) {
    // Simple insertion sort (MAX_ENTRIES is small). Directories first, then alphabetical.
    for (int i = 1; i < count; ++i) {
        ls_entry_t key = entries[i];
        int j = i - 1;
        while (j >= 0) {
            int ad = entries[j].is_dir;
            int bd = key.is_dir;
            int cmp = 0;
            if (ad != bd) {
                // directories first
                cmp = (ad > bd) ? -1 : 1;
            } else {
                cmp = ls_name_cmp(entries[j].name, key.name);
            }
            if (cmp <= 0) break;
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = key;
    }
}

// VFS-based ls: prints name and registers per-line icon marker for GUI rendering
__attribute__((unused))
static int vfs_ls_print_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    ls_ctx_t* ctx = (ls_ctx_t*)user;
    char icon_key[16];
    ls_icon_key_for_entry(ctx ? ctx->disk : 0, ctx ? ctx->dir_path : "/", name, is_dir, icon_key);
    // Tell the shell redirect pipeline which icon to draw for this output line.
    shell_register_redirect_icon(icon_key);

    // Reserve icon width in the text stream instead of shifting the whole line in pixels.
    int icon_pad = ls_icon_pad_cols();

    if (is_dir) {
        printf("%c", 120, 120, 255);
        ls_print_spaces(icon_pad);
        printf("%s/\n", name);
    } else {
        printf("%c", 255, 255, 255);
        ls_print_spaces(icon_pad);
        printf("%s\n", name);
    }
    return 0;
}

// Simple ls command using VFS (works for EYNFS and FAT32)
void ls(string input) {
    uint8 disk = g_current_drive;
    
    // Skip command name to get to argument
    uint8 i = 0;
    while (input[i] && input[i] != ' ') i++;
    while (input[i] && input[i] == ' ') i++;
    
    // Resolve path (use argument if provided, otherwise current directory)
    char abspath[128];
    if (input[i]) {
        resolve_path(&input[i], shell_current_path, abspath, sizeof(abspath));
    } else {
        resolve_path("", shell_current_path, abspath, sizeof(abspath));
    }

    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;

    // Check if it's a valid directory
    vfs_stat_t st;
    if (vfs_stat(disk, abspath, &st) != 0) {
        printf("%cPath not found: %s\n", 255, 0, 0, abspath);
        return;
    }
    if (st.type != VFS_NODE_DIR) {
        printf("%cNot a directory: %s\n", 255, 0, 0, abspath);
        return;
    }
    
    // List directory contents
    const int icon_pad = ls_icon_pad_cols();
    const int col_sep = 2;

    // Collect entries first so we can print in a taller (column-major) layout.
    // NOTE: This buffer is intentionally static to avoid blowing the kernel stack.
    // `ls_collect_t` is ~10KB due to the MAX_ENTRIES array.
    static ls_collect_t col;
    col.disk = disk;
    safe_strcpy(col.dir_path, abspath, sizeof(col.dir_path));
    col.icon_pad = icon_pad;
    col.max_item_width = 0;
    col.count = 0;

    if (vfs_listdir(disk, abspath, vfs_ls_collect_cb, &col) != 0) {
        printf("%cFailed to list directory: %s\n", 255, 0, 0, abspath);
        return;
    }
    if (col.count == 0) return;

    ls_sort_entries(col.entries, col.count);

    // Prefer taller columns (fill down the screen) instead of producing lots of short columns.
    int col_width = col.max_item_width + col_sep;
    if (col_width < 1) col_width = 1;
    int max_cols = TERM_COLS / col_width;
    if (max_cols < 1) max_cols = 1;

    // Reserve one line for the prompt so `ls` doesn't push it off-screen.
    int max_rows = TERM_ROWS;
    if (max_rows > 1) max_rows -= 1;
    if (max_rows < 1) max_rows = 1;

    // Choose enough columns so we fill down the screen first.
    int cols = (col.count + max_rows - 1) / max_rows;
    if (cols < 1) cols = 1;
    if (cols > max_cols) cols = max_cols;

    // Print only the rows we actually need (prevents huge blank gaps).
    int rows = (col.count + cols - 1) / cols;
    if (rows < 1) rows = 1;

    ls_cols_ctx_t ctx;
    ctx.disk = disk;
    safe_strcpy(ctx.dir_path, abspath, sizeof(ctx.dir_path));
    ctx.icon_pad = icon_pad;
    ctx.col_width = col_width;

    for (int r = 0; r < rows; ++r) {
        for (int cidx = 0; cidx < cols; ++cidx) {
            int idx = cidx * rows + r;
            if (idx >= col.count) {
                // Keep alignment for intermediate columns.
                if (cidx < cols - 1) ls_print_spaces(col_width);
                continue;
            }

            const ls_entry_t* e = &col.entries[idx];
            ls_print_entry_cols(&ctx, e->name, e->is_dir);

            if (cidx < cols - 1) {
                int item_w = ctx.icon_pad + (int)strlen(e->name) + (e->is_dir ? 1 : 0);
                int pad = ctx.col_width - item_w;
                if (pad < 1) pad = 1;
                ls_print_spaces(pad);
            }
        }
        putchar('\n');
    }
}

// Main read command implementation with smart detection
void read_cmd(const shell_args_t* args) {
    const char* filename = (args && args->argc >= 2) ? args->argv[1] : NULL;
    if (!filename || !filename[0]) {
        printf("%cUsage: read <filename>\n", 255, 255, 255);
        printf("%cDisplay text files (.txt) or render markdown (.md).\n", 255, 255, 255);
        printf("%cFor images, use: view <file.rei> or vieww <file.rei>\n", 255, 255, 255);
        return;
    }

    string ch = (string)(args ? args->raw : "");

    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    
    // Check file extension for smart detection
    int name_len = strlen(filename);
    int is_md = (name_len >= 3 && strcmp(filename + name_len - 3, ".md") == 0);
    int is_rei = (name_len >= 4 && strcmp(filename + name_len - 4, ".rei") == 0);
    
    // Smart detection based on file extension
    if (is_md) {
        printf("%cRendering markdown file...\n", 120, 120, 255);
        read_md_cmd(ch);
    } else if (is_rei) {
        // Guide users toward the GUI viewer commands instead of using read
        printf("%cImage files are not supported by 'read'. Use: view <file.rei> or vieww <file.rei>\n", 255, 165, 0);
        return;
    } else {
        // Default to raw display for .txt and other files
        printf("%cDisplaying as raw text...\n", 120, 120, 255);
        read_raw_cmd(ch);
    }
}

// del implementation
void del(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    const char* arg = (args && args->argc >= 2) ? args->argv[1] : NULL;
    if (!arg || !arg[0]) {
        printf("%cUsage: del <filename>\n", 255, 255, 255);
        printf("%cDeletes the specified file from the filesystem.\n", 255, 255, 255);
        return;
    }
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (!fs_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    vfs_stat_t st;
    if (vfs_stat(disk, abspath, &st) != 0 || st.type != VFS_NODE_FILE) {
        printf("%cFile not found: %s\n", 255, 0, 0, abspath);
        return;
    }
    if (vfs_unlink(disk, abspath) == 0) {
        printf("%cFile '%s' deleted successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to delete file '%s'.\n", 255, 0, 0, abspath);
    }
}

// write implementation
void write_cmd(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    const char* arg = (args && args->argc >= 2) ? args->argv[1] : NULL;
    if (!arg || !arg[0]) {
        printf("%cUsage: write <filename>\n", 255, 255, 255);
        printf("%cOpens a text editor for the specified file.\n", 255, 255, 255);
        return;
    }
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (!fs_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return;
    // Pass the full path to write_editor for subdirectory support
    write_editor(abspath, disk);
}

void to_fat32_83(const char* input, char* output)
{
    // convert input like "test.txt" to "TEST    TXT" (cancerous)
    int i = 0, j = 0;
    // copy name part (up to dot or 8 chars)
    while (input[i] && input[i] != '.' && j < 8) 
	{
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32; // to upper
        output[j++] = c;
        i++;
    }

    // pad with spaces
    while (j < 8) output[j++] = ' ';
    // if dot, skip it
    if (input[i] == '.') i++;
    int ext = 0;
    // copy extension (up to 3 chars)
    while (input[i] && ext < 3) 
	{
        char c = input[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        output[j++] = c;
        i++; ext++;
    }

    // pad extension with spaces
    while (ext < 3) 
	{
		output[j++] = ' '; ext++; 
	}
    output[j] = '\0';
}

// writefat implementation
void writefat(string ch)
{
    if (!fs_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;
    uint32 partition_lba_start = fat32_get_partition_lba_start(0);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(0, partition_lba_start, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB from drive 0\n", 255, 0, 0);
        return;
    }
    shell_args_t args;
    if (shell_args_parse(&args, ch) != 0 || args.argc < 3 || !args.argv[1] || !args.argv[2]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }

    const char* filename = args.argv[1];
    char fatname[12];
    to_fat32_83(filename, fatname);

    const char* data_str = shell_args_rest_raw(&args, 2);
    if (!data_str || !data_str[0]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }

    char data[512];
    uint32 data_len = (uint32)strlen(data_str);
    if (data_len > 511) data_len = 511;
    memcpy(data, data_str, data_len);
    data[data_len] = '\0';

    int res = fat32_write_file_sector(0, partition_lba_start, &bpb, fatname, data, (int)data_len);
    if (res < 0) {
        printf("%cFailed to write file to disk. Error %d\n", 255, 0, 0, res);
    } else {
        printf("%cFile written successfully to disk.\n", 0, 255, 0);
    }
}

// FAT32 repair: fix entries marked as directory that aren't real directories
static int fatfix_dir(uint8 drive, const char* dirpath) {
    uint32 part_lba = fat32_get_partition_lba_start(drive);
    struct fat32_bpb bpb; if (fat32_read_bpb_sector(drive, part_lba, &bpb) != 0) return -1;
    // Resolve directory cluster
    struct fat32_dir_entry dent;
    uint32 dclus = bpb.RootClus;
    memset(&dent, 0, sizeof(dent));
    dent.Attr = 0x10;
    if (strcmp(dirpath, "/") == 0) { dclus = bpb.RootClus; memset(&dent,0,sizeof(dent)); dent.Attr = 0x10; }
    else {
        // Local FAT32 absolute path traversal (8.3)
        char tmp[256]; strncpy(tmp, dirpath, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        char* p = tmp; if (*p == '/') p++;
        uint32 cur = bpb.RootClus;
        if (*p == '\0') { dclus = cur; dent.Attr = 0x10; }
        else {
            char comp[13]; struct fat32_dir_entry ent;
            while (*p) {
                size_t k=0; while (*p && *p != '/' && k < sizeof(comp)-1) comp[k++]=*p++;
                comp[k]='\0'; if (*p == '/') p++;
                char fatname[12]; to_fat32_83(comp, fatname);
                int found = fat32_find_entry_sector(drive, &bpb, cur, fatname, &ent);
                if (found < 0) return -2;
                cur = (uint32)found;
                if (*p == '\0') { dclus = cur; dent = ent; break; }
                if (!(ent.Attr & 0x10)) return -2; // not a dir
            }
        }
        if (!(dent.Attr & 0x10)) return -2;
    }
    uint32 first_data_sec = bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    uint8 sector[512]; uint32 cluster = dclus; int fixes = 0;
    while (cluster < 0x0FFFFFF8) {
        fs_ctx_account(SCHED_COST_FS);
        uint32 csec = first_data_sec + ((cluster - 2) * bpb.SecPerClus);
        for (uint32 s=0; s<bpb.SecPerClus; ++s) {
            if (ata_read_sector(drive, part_lba + csec + s, sector) != 0) return -3;
            struct fat32_dir_entry* ents = (struct fat32_dir_entry*)sector; int per = bpb.BytsPerSec / sizeof(struct fat32_dir_entry);
            int dirty = 0;
            for (int i=0;i<per;++i) {
                if (ents[i].Name[0] == 0x00) break; // end
                if ((ents[i].Attr & 0x0F) == 0x0F) continue; // LFN
                if (ents[i].Name[0] == 0xE5) continue; // deleted
                if (ents[i].Attr & 0x08) continue; // volume label
                // Skip dots
                if (ents[i].Name[0] == '.') continue;
                if (ents[i].Attr & 0x10) {
                    // Validate directory by checking first two entries of its cluster
                    uint32 fclus = ((uint32)ents[i].FstClusHI << 16) | ents[i].FstClusLO;
                    int is_real_dir = 0;
                    if (fclus >= 2) {
                        uint8 dsec[512];
                        if (ata_read_sector(drive, part_lba + first_data_sec + ((fclus - 2) * bpb.SecPerClus), dsec) == 0) {
                            struct fat32_dir_entry* dents = (struct fat32_dir_entry*)dsec;
                            // Check dot entries pattern
                            if (dents[0].Attr & 0x10) {
                                char n0[12]; for (int j=0;j<11;j++) n0[j]=dents[0].Name[j]; n0[11]='\0';
                                char n1[12]; for (int j=0;j<11;j++) n1[j]=dents[1].Name[j]; n1[11]='\0';
                                if (n0[0]=='.' && n1[0]=='.' && n1[1]=='.') is_real_dir = 1;
                            }
                        }
                    }
                    if (!is_real_dir) {
                        ents[i].Attr = 0x20; // flip to file
                        dirty = 1; fixes++;
                    }
                }
            }
            if (dirty) {
                if (ata_write_sector(drive, part_lba + csec + s, sector) != 0) return -4;
            }
        }
        cluster = fat32_next_cluster_sector(drive, part_lba, &bpb, cluster);
    }
    return fixes;
}

void fatfix_cmd(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    if (!fs_ctx_allow(CAP_DEV_DISK, SCHED_COST_FS)) return;

    const char* arg = (args && args->argc >= 2) ? args->argv[1] : "";
    char abspath[256];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    // Ensure working on FAT32
    eynfs_superblock_t sb; if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        printf("%cThis command only applies to FAT32 drives.\n", 255, 165, 0);
        return;
    }
    int res = fatfix_dir(disk, abspath);
    if (res < 0) printf("%cfatfix failed on %s (err %d)\n", 255, 0, 0, abspath, res);
    else printf("%cfatfix: repaired %d entries under %s\n", 0, 255, 0, res, abspath);
}

// catram implementation
void catram(string ch) {
    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    if (!fat32_disk_img) {
        printf("%cFAT32 disk image not loaded!\n", 255, 0, 0);
        return;
    }
    
    struct fat32_bpb bpb;
    if (fat32_read_bpb(fat32_disk_img, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB\n", 255, 0, 0);
        return;
    }
    
    shell_args_t args;
    if (shell_args_parse(&args, ch) != 0 || args.argc < 2 || !args.argv[1] || !args.argv[1][0]) {
        printf("%cUsage: catram <filename>\n", 255, 255, 255);
        return;
    }

    const char* filename = args.argv[1];
    
    // Convert to FAT32 8.3 format
    char fatname[12];
    to_fat32_83(filename, fatname);
    
    // Find and read the file
    uint32 cluster = bpb.RootClus;
    uint32 byts_per_sec = bpb.BytsPerSec;
    uint32 sec_per_clus = bpb.SecPerClus;
    uint32 rsvd_sec_cnt = bpb.RsvdSecCnt;
    uint32 num_fats = bpb.NumFATs;
    uint32 fatsz = bpb.FATSz32;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    
    // Search for the file
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        for (uint32 sec = 0; sec < sec_per_clus; sec++) {
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)fat32_disk_img + (cluster_first_sec + sec) * byts_per_sec);
            int entry_count = byts_per_sec / sizeof(struct fat32_dir_entry);
            
            for (int i = 0; i < entry_count; i++) {
                if (entries[i].Name[0] == 0x00) break;
                if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                if (entries[i].Name[0] == 0xE5) continue;
                
                char name[12];
                for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
                name[11] = '\0';
                
                if (strcmp(name, fatname) == 0) {
                    // Found the file, read its contents
                    uint32 file_cluster = entries[i].FstClusLO | (entries[i].FstClusHI << 16);
                    uint32 file_size = entries[i].FileSize;
                    
                    printf("%cFile: %s (size: %d bytes)\n", 255, 255, 255, filename, file_size);
                    printf("%c---\n", 200, 200, 200);
                    
                    // Read file contents
                    uint32 bytes_read = 0;
                    uint32 current_cluster = file_cluster;
                    
                    while (current_cluster < 0x0FFFFFF8 && bytes_read < file_size) {
                        uint32 cluster_first_sec = first_data_sec + ((current_cluster - 2) * sec_per_clus);
                        for (uint32 sec = 0; sec < sec_per_clus && bytes_read < file_size; sec++) {
                            char* sector_data = (char*)fat32_disk_img + (cluster_first_sec + sec) * byts_per_sec;
                            uint32 bytes_to_read = byts_per_sec;
                            if (bytes_read + bytes_to_read > file_size) {
                                bytes_to_read = file_size - bytes_read;
                            }
                            
                            // Print file contents
                            for (uint32 k = 0; k < bytes_to_read; k++) {
                                printf("%c", sector_data[k]);
                            }
                            
                            bytes_read += bytes_to_read;
                        }
                        current_cluster = fat32_next_cluster(fat32_disk_img, &bpb, current_cluster);
                    }
                    
                    printf("%c\n---\n", 200, 200, 200);
                    return;
                }
            }
        }
        cluster = fat32_next_cluster(fat32_disk_img, &bpb, cluster);
    }
    
    printf("%cFile not found: %s\n", 255, 0, 0, filename);
}

// lsram implementation
void lsram(string input) 
{
    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    if (!fat32_disk_img) 
    {
        printf("%cFAT32 disk image not loaded!\n", 255, 0, 0);
        return;
    }
    struct fat32_bpb bpb;
    if (fat32_read_bpb(fat32_disk_img, &bpb) != 0) 
    {
        printf("%cFailed to read FAT32 BPB\n", 255, 0, 0);
        return;
    }
    int max_depth = 1;
    uint8 i = 0;
    while (input[i] && input[i] != ' ') i++;
    while (input[i] && input[i] == ' ') i++;
    if (input[i]) {
        int val = 0;
        while (input[i] >= '0' && input[i] <= '9') 
        {
            val = val * 10 + (input[i] - '0');
            i++;
        }
        if (val > 0) max_depth = val;
    }
    printf("%cFAT32 directory tree (depth: %d):\n\n", 255, 255, 255, max_depth);
    uint32 byts_per_sec = bpb.BytsPerSec;
    uint32 sec_per_clus = bpb.SecPerClus;
    uint32 rsvd_sec_cnt = bpb.RsvdSecCnt;
    uint32 num_fats = bpb.NumFATs;
    uint32 fatsz = bpb.FATSz32;
    uint32 root_clus = bpb.RootClus;
    uint32 first_data_sec = rsvd_sec_cnt + (num_fats * fatsz);
    uint32 cluster = root_clus;
    uint8 sector[512];
    extern volatile int g_user_interrupt;
    g_user_interrupt = 0;
    while (cluster < 0x0FFFFFF8) {
        uint32 cluster_first_sec = first_data_sec + ((cluster - 2) * sec_per_clus);
        for (uint32 sec = 0; sec < sec_per_clus; sec++) {
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)((char*)fat32_disk_img + (cluster_first_sec + sec) * byts_per_sec);
            int entry_count = byts_per_sec / sizeof(struct fat32_dir_entry);
            for (int i = 0; i < entry_count; i++) {
                if (entries[i].Name[0] == 0x00) return;
                if ((entries[i].Attr & 0x0F) == 0x0F) continue;
                if (entries[i].Name[0] == 0xE5) continue;
                char name[12];
                for (int j = 0; j < 11; j++) name[j] = entries[i].Name[j];
                name[11] = '\0';
                if (entries[i].Attr & 0x10) {
                    printf("%c%s <DIR>\n", 255, 255, 255, name);
                } else {
                    printf("%c%s\n", 255, 255, 255, name);
                }
                poll_keyboard_for_ctrl_c();
                sleep(30);
                if (g_user_interrupt) {
                    printf("\n^C [Interrupted by user]\n", 255, 0, 0);
                    g_user_interrupt = 0;
                    return;
                }
            }
        }
        cluster = fat32_next_cluster(fat32_disk_img, &bpb, cluster);
    }
}

// writeram implementation
void writeram(string ch)
{
    if (!fs_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    if (!fat32_disk_img) {
        printf("%cFAT32 disk image not loaded!\n", 255, 0, 0);
        return;
    }
    struct fat32_bpb bpb;
    if (fat32_read_bpb(fat32_disk_img, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB\n", 255, 0, 0);
        return;
    }
    shell_args_t args;
    if (shell_args_parse(&args, ch) != 0 || args.argc < 3 || !args.argv[1] || !args.argv[2]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }

    const char* filename = args.argv[1];
    char fatname[12];
    to_fat32_83(filename, fatname);

    const char* data_str = shell_args_rest_raw(&args, 2);
    if (!data_str || !data_str[0]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }

    char data[512];
    uint32 data_len = (uint32)strlen(data_str);
    if (data_len > 511) data_len = 511;
    memcpy(data, data_str, data_len);
    data[data_len] = '\0';

    int res = fat32_write_file(fat32_disk_img, &bpb, fatname, data, (int)data_len);
    if (res < 0) {
        printf("%cFailed to write file.\n", 255, 0, 0);
    } else {
        printf("%cFile written successfully.\n", 0, 255, 0);
    }
}

int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
    if (!fs_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return -1;
    int written = vfs_write_file(disk, filename, buf, (uint32)len);
    if (written != len) {
        printf("Failed to write file data: expected %d, got %d\n", len, written);
        return -1;
    }
    printf("Successfully wrote %d bytes to %s\n", len, filename);
    return 0;
}

// Append output to file using EYNFS append mode
int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
    if (!fs_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
    // Read existing contents if any
    vfs_stat_t st;
    int existing_len = 0;
    char* existing = NULL;
    if (vfs_stat(disk, filename, &st) == 0 && st.type == VFS_NODE_FILE && st.size > 0) {
        existing = (char*)malloc(st.size);
        if (!existing) return -1;
        int n = vfs_read_file(disk, filename, existing, (int)st.size);
        if (n < 0) { free(existing); return -1; }
        existing_len = n;
    }
    // Concatenate existing + new
    char* combined = (char*)malloc(existing_len + len);
    if (!combined) { if (existing) free(existing); return -1; }
    if (existing_len) memcpy(combined, existing, existing_len);
    memcpy(combined + existing_len, buf, len);
    int written = vfs_write_file(disk, filename, combined, (uint32)(existing_len + len));
    free(combined);
    if (existing) free(existing);
    if (written != existing_len + len) {
        printf("Failed to append to %s\n", filename);
        return -1;
    }
    printf("Successfully appended %d bytes to %s\n", len, filename);
    return 0;
}

// Filesystem integrity check
int check_filesystem_integrity(uint8_t disk) {
    if (!fs_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0) {
        printf("Cannot read superblock - filesystem may be corrupted.\n");
        return -1;
    }
    
    if (sb.magic != EYNFS_MAGIC) {
        printf("Invalid filesystem magic - filesystem is corrupted.\n");
        return -1;
    }
    
    // Try to read the root directory - count entries first
    int entry_count = eynfs_count_dir_entries(disk, sb.root_dir_block);
    if (entry_count < 0) {
        printf("Cannot count root directory entries - filesystem is corrupted.\n");
        return -1;
    }
    
    size_t allocation_size = sizeof(eynfs_dir_entry_t) * entry_count;
    
    // Safety check: limit allocation to prevent memory exhaustion
    if (allocation_size > 16384) { // 16KB limit for directory operations
        printf("%cWarning: Directory allocation too large (%d bytes), limiting to 16KB\n", 255, 165, 0, allocation_size);
        entry_count = 16384 / sizeof(eynfs_dir_entry_t);
        allocation_size = 16384;
    }
    
    eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(allocation_size);
    if (!entries) {
        printf("Out of memory for filesystem integrity check.\n");
        return -1;
    }
    int count = eynfs_read_dir_table(disk, sb.root_dir_block, entries, entry_count);
    if (count < 0) {
        printf("Cannot read root directory - filesystem is corrupted.\n");
        free(entries);
        return -1;
    }
    
    free(entries);
    printf("Filesystem integrity check passed.\n");
    return 0;
}

// makedir implementation
void makedir(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    const char* arg = (args && args->argc >= 2) ? args->argv[1] : NULL;
    if (!arg || !arg[0]) {
        printf("%cUsage: makedir <directory>\n", 255, 255, 255);
        printf("%cCreates a new directory at the specified path.\n", 255, 255, 255);
        return;
    }
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (!fs_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    if (vfs_mkdir(disk, abspath) == 0) {
        printf("%cDirectory '%s' created successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to create directory '%s'.\n", 255, 0, 0, abspath);
    }
}

// deldir implementation
void deldir(const shell_args_t* args) {
    uint8 disk = g_current_drive;
    const char* arg = (args && args->argc >= 2) ? args->argv[1] : NULL;
    if (!arg || !arg[0]) {
        printf("%cUsage: deldir <directory>\n", 255, 255, 255);
        printf("%cRemoves the specified directory (must be empty).\n", 255, 255, 255);
        return;
    }
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (!fs_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    if (vfs_rmdir(disk, abspath) == 0) {
        printf("%cDirectory '%s' deleted successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to delete directory '%s'.\n", 255, 0, 0, abspath);
    }
}

// fscheck command implementation
void fscheck(const shell_args_t* args) {
    string ch = (string)(args ? args->raw : "");
    uint8 disk = g_current_drive;
    if (!fs_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    printf("Checking filesystem integrity on drive %d...\n", disk);
    int result = check_filesystem_integrity(disk);
    if (result == 0) {
        printf("%cFilesystem is healthy.\n", 0, 255, 0);
    } else {
        printf("%cFilesystem corruption detected!\n", 255, 0, 0);
        printf("%cRecommendation: Reboot and reformat if problems persist.\n", 255, 255, 0);
    }
}

// Copy command implementation - rewritten from scratch
void copy_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        printf("%cUsage: copy <source> <destination>\n", 255, 255, 255);
        printf("%cExample: copy file1.txt file2.txt\n", 255, 255, 255);
        return;
    }
    if (args->argc < 3 || !args->argv[2] || !args->argv[2][0]) {
        printf("%cError: Destination filename required.\n", 255, 0, 0);
        return;
    }
    const char* source = args->argv[1];
    const char* dest = args->argv[2];
    if (!fs_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return;
    // Resolve to absolute
    char src_path[256], dst_path[256]; resolve_path(source, shell_current_path, src_path, sizeof(src_path)); resolve_path(dest, shell_current_path, dst_path, sizeof(dst_path));
    if (strcmp(src_path, dst_path) == 0) { printf("%cError: Source and destination are the same.\n", 255, 0, 0); return; }
    uint8 disk = g_current_drive; vfs_stat_t st;
    if (vfs_stat(disk, src_path, &st) != 0 || st.type != VFS_NODE_FILE) { printf("%cError: Source file not found: %s\n", 255, 0, 0, src_path); return; }
    // If destination is a directory, append basename
    vfs_stat_t dstst; if (vfs_stat(disk, dst_path, &dstst) == 0 && dstst.type == VFS_NODE_DIR) {
        char base[128]; const char* b = get_basename(src_path); strncpy(base, b, sizeof(base)-1); base[sizeof(base)-1] = '\0';
        size_t len = strlen(dst_path); char newdst[256]; strncpy(newdst, dst_path, sizeof(newdst)-1); newdst[sizeof(newdst)-1]='\0';
        if (len > 1 && newdst[len-1] != '/') strncat(newdst, "/", sizeof(newdst)-strlen(newdst)-1);
        strncat(newdst, base, sizeof(newdst)-strlen(newdst)-1); strncpy(dst_path, newdst, sizeof(dst_path)-1); dst_path[sizeof(dst_path)-1] = '\0';
    }
    // Read whole source into memory
    if (st.size == 0) {
        int w = vfs_write_file(disk, dst_path, "", 0); if (w < 0) { printf("%cError: Failed to create destination file.\n", 255, 0, 0); } else { printf("%cFile copied: %s -> %s (0 bytes)\n", 0, 255, 0, src_path, dst_path); }
        return;
    }
    uint8* buf = (uint8*)malloc(st.size); if (!buf) { printf("%cError: Memory allocation failed.\n", 255, 0, 0); return; }
    int n = vfs_read_file(disk, src_path, buf, (int)st.size); if (n < 0) { printf("%cError: Failed to read source file.\n", 255, 0, 0); free(buf); return; }
    int w = vfs_write_file(disk, dst_path, buf, (uint32)n); free(buf);
    if (w != n) { printf("%cError: Failed to write destination file.\n", 255, 0, 0); return; }
    printf("%cFile copied: %s -> %s (%d bytes)\n", 0, 255, 0, src_path, dst_path, n);
}

// Move command implementation - rewritten from scratch
void move_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2 || !args->argv[1] || !args->argv[1][0]) {
        printf("%cUsage: move <source> <destination>\n", 255, 255, 255);
        printf("%cExample: move file1.txt /backup/file1.txt\n", 255, 255, 255);
        return;
    }
    if (args->argc < 3 || !args->argv[2] || !args->argv[2][0]) {
        printf("%cError: Destination filename required.\n", 255, 0, 0);
        return;
    }
    const char* source = args->argv[1];
    const char* dest = args->argv[2];
    if (!fs_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return;
    char src_path[256], dst_path[256]; resolve_path(source, shell_current_path, src_path, sizeof(src_path)); resolve_path(dest, shell_current_path, dst_path, sizeof(dst_path));
    if (strcmp(src_path, dst_path) == 0) { printf("%cError: Source and destination are the same.\n", 255, 0, 0); return; }
    uint8 disk = g_current_drive; vfs_stat_t st;
    if (vfs_stat(disk, src_path, &st) != 0 || st.type != VFS_NODE_FILE) { printf("%cError: Source file not found: %s\n", 255, 0, 0, src_path); return; }
    // Allow destination dir
    vfs_stat_t dstst; if (vfs_stat(disk, dst_path, &dstst) == 0 && dstst.type == VFS_NODE_DIR) {
        char base[128]; const char* b = get_basename(src_path); strncpy(base, b, sizeof(base)-1); base[sizeof(base)-1] = '\0';
        size_t len = strlen(dst_path); char newdst[256]; strncpy(newdst, dst_path, sizeof(newdst)-1); newdst[sizeof(newdst)-1]='\0';
        if (len > 1 && newdst[len-1] != '/') strncat(newdst, "/", sizeof(newdst)-strlen(newdst)-1);
        strncat(newdst, base, sizeof(newdst)-strlen(newdst)-1); strncpy(dst_path, newdst, sizeof(dst_path)-1); dst_path[sizeof(dst_path)-1] = '\0';
    }
    // Copy then unlink source (until vfs_rename exists)
    int bytes_written = 0;
    if (st.size == 0) {
        int w = vfs_write_file(disk, dst_path, "", 0);
        if (w < 0) { printf("%cError: Failed to write destination file.\n", 255, 0, 0); return; }
        bytes_written = 0;
    } else {
        uint8* buf = (uint8*)malloc(st.size);
        if (!buf) { printf("%cError: Memory allocation failed.\n", 255, 0, 0); return; }
        int n = vfs_read_file(disk, src_path, buf, (int)st.size);
        if (n < 0) { printf("%cError: Failed to read source file.\n", 255, 0, 0); free(buf); return; }
        int w = vfs_write_file(disk, dst_path, buf, (uint32)n);
        free(buf);
        if (w != n) { printf("%cError: Failed to write destination file.\n", 255, 0, 0); return; }
        bytes_written = w;
    }
    if (vfs_unlink(disk, src_path) != 0) { printf("%cWarning: File copied but failed to delete source: %s\n", 255, 255, 0, src_path); return; }
    printf("%cFile moved: %s -> %s (%d bytes)\n", 0, 255, 0, src_path, dst_path, bytes_written);
}

REGISTER_SHELL_COMMAND(ls, "ls", ls_cmd, CMD_STREAMING, "List files in the root directory of the selected drive.\nUsage: ls", "ls");
REGISTER_SHELL_COMMAND(read, "read", read_cmd, CMD_STREAMING, "Display text files (.txt) or render markdown (.md). For images, use 'view' or 'vieww'.\nUsage: read <filename>", "read myfile.txt");
REGISTER_SHELL_COMMAND(del, "del", del, CMD_STREAMING, "Delete a file from the filesystem.\nUsage: del <filename>", "del myfile.txt");
REGISTER_SHELL_COMMAND(write, "write", write_cmd, CMD_STREAMING, "Open nano-like text editor for a file.\nUsage: write <filename>", "write myfile.txt");
REGISTER_SHELL_COMMAND(size, "size", size, CMD_STREAMING, "Show the size of a file in bytes.\nUsage: size <filename>", "size myfile.txt");
REGISTER_SHELL_COMMAND(cd, "cd", cd, CMD_STREAMING, "Change the current directory.\nUsage: cd <directory>", "cd myfolder");
REGISTER_SHELL_COMMAND(makedir, "makedir", makedir, CMD_STREAMING, "Create a new directory.\nUsage: makedir <directory>", "makedir myfolder");
REGISTER_SHELL_COMMAND(deldir, "deldir", deldir, CMD_STREAMING, "Delete an empty directory.\nUsage: deldir <directory>", "deldir myfolder");
REGISTER_SHELL_COMMAND(fscheck, "fscheck", fscheck, CMD_STREAMING, "Check filesystem integrity.\nUsage: fscheck", "fscheck");
REGISTER_SHELL_COMMAND(copy_cmd, "copy", copy_cmd, CMD_STREAMING, "Copy a file from source to destination.\nUsage: copy <source> <destination>", "copy file1.txt file2.txt");
REGISTER_SHELL_COMMAND(move_cmd, "move", move_cmd, CMD_STREAMING, "Move a file from source to destination.\nUsage: move <source> <destination>", "move file1.txt /backup/file1.txt");
REGISTER_SHELL_COMMAND(fatfix_cmd, "fatfix", fatfix_cmd, CMD_STREAMING, "Scan and repair FAT32 entries incorrectly marked as directories.\nUsage: fatfix [path]", "fatfix /" );