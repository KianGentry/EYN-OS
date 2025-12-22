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

// Forward declarations for command handlers
void ls_cmd(string arg);
void read_cmd(string arg);
void del(string arg);
void write_cmd(string arg);
void size(string arg);
void cd(string arg);
void makedir(string arg);
void deldir(string arg);
void fscheck(string arg);
void copy_cmd(string arg);
void move_cmd(string arg);
void fatfix_cmd(string arg);

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

// Helper: resolve relative/absolute path to absolute
void resolve_path(const char* input, const char* cwd, char* out, size_t outsz) {
    if (!input || !input[0]) {
        strncpy(out, cwd, outsz-1); out[outsz-1] = '\0';
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
void cd(string input) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (input[i] && input[i] != ' ') i++;
    while (input[i] && input[i] == ' ') i++;
    if (!input[i]) {
        printf("%cUsage: cd <directory>\n", 255, 255, 255);
        return;
    }
    char arg[128]; uint8 j = 0;
    while (input[i] && input[i] != ' ' && j < 127) arg[j++] = input[i++];
    arg[j] = '\0';
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
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

    char key[16];
    snprintf(key, sizeof(key), "file_%s", ext);
    key[sizeof(key) - 1] = '\0';
    safe_strcpy(out_key, key, 16);
}

// VFS-based ls: prints name and registers per-line icon marker for GUI rendering
static int vfs_ls_print_cb(const char* name, int is_dir, uint32 size, void* user) {
    (void)size;
    ls_ctx_t* ctx = (ls_ctx_t*)user;
    char icon_key[16];
    ls_icon_key_for_entry(ctx ? ctx->disk : 0, ctx ? ctx->dir_path : "/", name, is_dir, icon_key);
    // Tell the shell redirect pipeline which icon to draw for this output line.
    shell_register_redirect_icon(icon_key);

    if (is_dir) {
        printf("%c%s/\n", 120, 120, 255, name);
    } else {
        printf("%c%s\n", 255, 255, 255, name);
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
    ls_ctx_t ctx;
    ctx.disk = disk;
    safe_strcpy(ctx.dir_path, abspath, sizeof(ctx.dir_path));
    if (vfs_listdir(disk, abspath, vfs_ls_print_cb, &ctx) != 0) {
        printf("%cFailed to list directory: %s\n", 255, 0, 0, abspath);
    }
}

// Main read command implementation with smart detection
void read_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: read <filename>\n", 255, 255, 255);
        printf("%cDisplay text files (.txt) or render markdown (.md).\n", 255, 255, 255);
        printf("%cFor images, use: view <file.rei> or vieww <file.rei>\n", 255, 255, 255);
        return;
    }
    
    // Parse filename
    char filename[128] = {0};
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) {
        filename[j++] = ch[i++];
    }
    filename[j] = '\0';
    
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
void del(string ch) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: del <filename>\n", 255, 255, 255);
        printf("%cDeletes the specified file from the filesystem.\n", 255, 255, 255);
        return;
    }
    char arg[128]; uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++];
    arg[j] = '\0';
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
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
void write_cmd(string ch) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: write <filename>\n", 255, 255, 255);
        printf("%cOpens a text editor for the specified file.\n", 255, 255, 255);
        return;
    }
    char arg[128]; uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++];
    arg[j] = '\0';
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
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
    uint32 partition_lba_start = fat32_get_partition_lba_start(0);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(0, partition_lba_start, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB from drive 0\n", 255, 0, 0);
        return;
    }
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char arg[64];
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 63) {
        arg[j++] = ch[i++];
    }
    arg[j] = '\0';
    if (strlength(arg) < 1) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char fatname[12];
    to_fat32_83(arg, fatname);
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char data[512];
    j = 0;
    while (ch[i] && j < 511) {
        data[j++] = ch[i++];
    }
    data[j] = '\0';
    int res = fat32_write_file_sector(0, partition_lba_start, &bpb, fatname, data, j);
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
    struct fat32_dir_entry dent; uint32 dclus;
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

void fatfix_cmd(string ch) {
    uint8 disk = g_current_drive;
    // Parse optional path arg
    uint8 i = 0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i] == ' ') i++;
    char arg[128] = {0}; uint8 j = 0; if (ch[i]) { while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++]; arg[j]='\0'; }
    char abspath[256];
    if (arg[0]) resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    else resolve_path("", shell_current_path, abspath, sizeof(abspath));
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
    if (!fat32_disk_img) {
        printf("%cFAT32 disk image not loaded!\n", 255, 0, 0);
        return;
    }
    
    struct fat32_bpb bpb;
    if (fat32_read_bpb(fat32_disk_img, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB\n", 255, 0, 0);
        return;
    }
    
    // Parse filename
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        printf("%cUsage: catram <filename>\n", 255, 255, 255);
        return;
    }
    
    char filename[64];
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 63) {
        filename[j++] = ch[i++];
    }
    filename[j] = '\0';
    
    if (strlength(filename) < 1) {
        printf("%cUsage: catram <filename>\n", 255, 255, 255);
        return;
    }
    
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
    if (!fat32_disk_img) {
        printf("%cFAT32 disk image not loaded!\n", 255, 0, 0);
        return;
    }
    struct fat32_bpb bpb;
    if (fat32_read_bpb(fat32_disk_img, &bpb) != 0) {
        printf("%cFailed to read FAT32 BPB\n", 255, 0, 0);
        return;
    }
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char arg[64];
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 63) {
        arg[j++] = ch[i++];
    }
    arg[j] = '\0';
    if (strlength(arg) < 1) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char fatname[12];
    to_fat32_83(arg, fatname);
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: writefat <filename> <data>\n", 255, 255, 255);
        return;
    }
    char data[512];
    j = 0;
    while (ch[i] && j < 511) {
        data[j++] = ch[i++];
    }
    data[j] = '\0';
    int res = fat32_write_file(fat32_disk_img, &bpb, fatname, data, j);
    if (res < 0) {
        printf("%cFailed to write file.\n", 255, 0, 0);
    } else {
        printf("%cFile written successfully.\n", 0, 255, 0);
    }
}

int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
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
void makedir(string ch) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: makedir <directory>\n", 255, 255, 255);
        printf("%cCreates a new directory at the specified path.\n", 255, 255, 255);
        return;
    }
    char arg[128]; uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++];
    arg[j] = '\0';
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (vfs_mkdir(disk, abspath) == 0) {
        printf("%cDirectory '%s' created successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to create directory '%s'.\n", 255, 0, 0, abspath);
    }
}

// deldir implementation
void deldir(string ch) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: deldir <directory>\n", 255, 255, 255);
        printf("%cRemoves the specified directory (must be empty).\n", 255, 255, 255);
        return;
    }
    char arg[128]; uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++];
    arg[j] = '\0';
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    if (vfs_rmdir(disk, abspath) == 0) {
        printf("%cDirectory '%s' deleted successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to delete directory '%s'.\n", 255, 0, 0, abspath);
    }
}

// fscheck command implementation
void fscheck(string ch) {
    uint8 disk = g_current_drive;
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
void copy_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) { printf("%cUsage: copy <source> <destination>\n", 255, 255, 255); printf("%cExample: copy file1.txt file2.txt\n", 255, 255, 255); return; }
    // Parse source and dest
    char source[128] = {0}; uint8 j = 0; while (ch[i] && ch[i] != ' ' && j < 127) { source[j++] = ch[i++]; } source[j] = '\0';
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) { printf("%cError: Destination filename required.\n", 255, 0, 0); return; }
    char dest[128] = {0}; j = 0; while (ch[i] && ch[i] != ' ' && j < 127) { dest[j++] = ch[i++]; } dest[j] = '\0';
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
void move_cmd(string ch) {
    uint8 i = 0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) { printf("%cUsage: move <source> <destination>\n", 255, 255, 255); printf("%cExample: move file1.txt /backup/file1.txt\n", 255, 255, 255); return; }
    char source[128] = {0}; uint8 j = 0; while (ch[i] && ch[i] != ' ' && j < 127) { source[j++] = ch[i++]; } source[j] = '\0';
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) { printf("%cError: Destination filename required.\n", 255, 0, 0); return; }
    char dest[128] = {0}; j = 0; while (ch[i] && ch[i] != ' ' && j < 127) { dest[j++] = ch[i++]; } dest[j] = '\0';
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