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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cNo supported filesystem found.\n", 255, 0, 0);
        return;
    }
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
    eynfs_dir_entry_t entry;
    if (strcmp(abspath, "/") == 0) {
        strncpy(shell_current_path, "/", sizeof(shell_current_path)-1);
        shell_current_path[sizeof(shell_current_path)-1] = '\0';
        return;
    }
    if (eynfs_traverse_path(disk, &sb, abspath, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_DIR) {
        strncpy(shell_current_path, abspath, sizeof(shell_current_path)-1);
        shell_current_path[sizeof(shell_current_path)-1] = '\0';
    } else {
        printf("%cDirectory not found: %s\n", 255, 0, 0, abspath);
    }
}

// Helper: recursive ls with depth, indentation, and color
void eynfs_ls_depth(uint8 disk, uint32_t dir_block, int depth, int max_depth, int indent) {
    eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(sizeof(eynfs_dir_entry_t) * MAX_ENTRIES);
    if (!entries) {
        printf("%cOut of memory for directory listing\n", 255, 0, 0);
        return;
    }
    int count = eynfs_read_dir_table(disk, dir_block, entries, MAX_ENTRIES);
    int valid_count = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].name[0] == '\0') {
            continue; // Skip empty entries
        }
        valid_count++;
        // Print indentation in blue
        for (int d = 0; d < indent; ++d);
        if (indent > 0) printf("%c || ", 120, 120, 255);
        if (entries[i].type == EYNFS_TYPE_DIR) {
            printf("%c%s/\n", 120, 120, 255, entries[i].name);
            if (depth < max_depth) {
                eynfs_ls_depth(disk, entries[i].first_block, depth+1, max_depth, indent+1);
            }
        } else {
            printf("%c%s\n", 255, 255, 255, entries[i].name);
        }
    }
    free(entries);
}

// Refactor ls to detect filesystem and dispatch
void ls(string input) {
    uint8 disk = g_current_drive;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        // EYNFS implementation (already present)
        int max_depth = 0;
        uint8 i = 0;
        while (input[i] && input[i] != ' ') i++;
        while (input[i] && input[i] == ' ') i++;
        if (input[i]) {
            max_depth = str_to_uint(&input[i]);
            if (max_depth > 10) max_depth = 10;
        }
        char abspath[128];
        resolve_path("", shell_current_path, abspath, sizeof(abspath));
        eynfs_dir_entry_t entry;
        uint32_t dir_block = sb.root_dir_block;
        if (strcmp(abspath, "/") != 0) {
            if (eynfs_traverse_path(disk, &sb, abspath, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_DIR) {
                dir_block = entry.first_block;
            } else {
                printf("%cDirectory not found: %s\n", 255, 0, 0, abspath);
                return;
            }
        }
        eynfs_ls_depth(disk, dir_block, 0, max_depth, 0);
        return;
    }
    // FAT32 fallback
    uint32 partition_lba_start = fat32_get_partition_lba_start(disk);
    struct fat32_bpb bpb;
    if (fat32_read_bpb_sector(disk, partition_lba_start, &bpb) == 0) {
        int max_depth = 1;
        uint8 i = 0;
        while (input[i] && input[i] != ' ') i++;
        while (input[i] && input[i] == ' ') i++;
        if (input[i]) {
            int val = 0;
            while (input[i] >= '0' && input[i] <= '9') {
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
        extern volatile int g_user_interrupt;
        g_user_interrupt = 0;
        int depth = 0;
        // Only support depth 1 for now (can be extended)
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
                    if (entries[i].Attr & 0x10) {
                        printf("%c%s/\n", 120, 120, 255, name);
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
        return;
    }
    printf("%cNo supported filesystem found on drive %d.\n", 255, 0, 0, disk);
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t entry;
        uint32_t parent_block, entry_idx;
        if (eynfs_traverse_path(disk, &sb, abspath, &entry, &parent_block, &entry_idx) == 0 && entry.type == EYNFS_TYPE_FILE) {
            if (eynfs_delete_entry(disk, &sb, parent_block, entry.name) == 0) {
                printf("%cFile '%s' deleted successfully.\n", 0, 255, 0, abspath);
            } else {
                printf("%cFailed to delete file '%s'.\n", 255, 0, 0, abspath);
            }
        } else {
            printf("%cFile not found: %s\n", 255, 0, 0, abspath);
        }
        return;
    }
    printf("%cNo supported filesystem found.\n", 255, 0, 0);
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("No supported filesystem found.\n");
        return -1;
    }
    
    // Simple approach: just create a new file directly
    // Delete existing file if it exists
    eynfs_dir_entry_t entry;
    uint32_t entry_idx;
    if (eynfs_find_in_dir(disk, &sb, sb.root_dir_block, filename, &entry, &entry_idx) == 0) {
        if (eynfs_delete_entry(disk, &sb, sb.root_dir_block, filename) != 0) {
            printf("Warning: Failed to delete existing file: %s\n", filename);
        }
    }
    
    // Create new file
    if (eynfs_create_entry(disk, &sb, sb.root_dir_block, filename, EYNFS_TYPE_FILE) != 0) {
        printf("Failed to create file: %s\n", filename);
        return -1;
    }
    
    // Find the newly created file
    if (eynfs_find_in_dir(disk, &sb, sb.root_dir_block, filename, &entry, &entry_idx) != 0) {
        printf("Failed to find file after creation: %s\n", filename);
        return -1;
    }
    
    // Write the file data
    int written = eynfs_write_file(disk, &sb, &entry, buf, len, sb.root_dir_block, entry_idx);
    if (written != len) {
        printf("Failed to write file data: expected %d, got %d\n", len, written);
        return -1;
    }
    
    // Clear the filesystem cache to ensure new files are visible
    eynfs_cache_clear();
    
    printf("Successfully wrote %d bytes to %s\n", len, filename);
    return 0;
}

// Append output to file using EYNFS append mode
int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("No supported filesystem found.\n");
        return -1;
    }
    
    // Check if file exists
    eynfs_dir_entry_t entry;
    uint32_t entry_idx;
    if (eynfs_find_in_dir(disk, &sb, sb.root_dir_block, filename, &entry, &entry_idx) == 0) {
        // File exists, read existing content and append
        char* existing_content = malloc(entry.size + 1);
        if (existing_content) {
            int bytes_read = eynfs_read_file(disk, &sb, &entry, existing_content, entry.size, 0);
            if (bytes_read > 0) {
                existing_content[bytes_read] = '\0';
                
                // Combine existing content with new content
                char* combined = malloc(bytes_read + len + 1);
                if (combined) {
                    strcpy(combined, existing_content);
                    strcat(combined, buf);
                    
                    // Write combined content back to file
                    int written = eynfs_write_file(disk, &sb, &entry, combined, bytes_read + len, sb.root_dir_block, entry_idx);
                    free(combined);
                    
                    if (written == bytes_read + len) {
                        printf("Successfully appended %d bytes to %s\n", len, filename);
                        free(existing_content);
                        eynfs_cache_clear();
                        return 0;
                    } else {
                        printf("Failed to append to file '%s'.\n", filename);
                    }
                }
            }
            free(existing_content);
        }
    } else {
        // File doesn't exist, create it (same as overwrite)
        int result = write_output_to_file(buf, len, filename, disk);
        if (result == 0) {
            printf("Successfully created and wrote %d bytes to %s\n", len, filename);
            return 0;
        } else {
            printf("Failed to create file '%s'.\n", filename);
        }
    }
    
    return -1;
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cNo supported filesystem found.\n", 255, 0, 0);
        return;
    }
    // Find parent directory and base name
    char parent_path[128];
    if (abspath) {
        strcpy(parent_path, abspath);
    } else {
        strcpy(parent_path, "/");
    }
    char* last_slash = strrchr(parent_path, '/');
    if (!last_slash || last_slash == parent_path) {
        strcpy(parent_path, "/");
    } else {
        *last_slash = '\0';
    }
    eynfs_dir_entry_t parent_entry;
    if (eynfs_traverse_path(disk, &sb, parent_path, &parent_entry, NULL, NULL) != 0 || parent_entry.type != EYNFS_TYPE_DIR) {
        printf("%cParent directory not found: %s\n", 255, 0, 0, parent_path);
        return;
    }
    const char* dirname = get_basename(abspath);
    if (eynfs_create_entry(disk, &sb, parent_entry.first_block, dirname, EYNFS_TYPE_DIR) == 0) {
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
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cNo supported filesystem found.\n", 255, 0, 0);
        return;
    }
    eynfs_dir_entry_t entry;
    uint32_t parent_block, entry_idx;
    if (eynfs_traverse_path(disk, &sb, abspath, &entry, &parent_block, &entry_idx) != 0 || entry.type != EYNFS_TYPE_DIR) {
        printf("%cDirectory not found: %s\n", 255, 0, 0, abspath);
        return;
    }
    // Check if directory is empty
    eynfs_dir_entry_t* entries = (eynfs_dir_entry_t*)malloc(sizeof(eynfs_dir_entry_t) * MAX_ENTRIES);
    if (!entries) {
        printf("%cOut of memory for directory check\n", 255, 0, 0);
        return;
    }
    int count = eynfs_read_dir_table(disk, entry.first_block, entries, MAX_ENTRIES);
    int empty = 1;
    for (int k = 0; k < count; ++k) {
        if (entries[k].name[0] != '\0') { empty = 0; break; }
    }
    if (!empty) {
        printf("%cDirectory not empty: %s\n", 255, 0, 0, abspath);
        free(entries);
        return;
    }
    if (eynfs_delete_entry(disk, &sb, parent_block, entry.name) == 0) {
        printf("%cDirectory '%s' deleted successfully.\n", 0, 255, 0, abspath);
    } else {
        printf("%cFailed to delete directory '%s'.\n", 255, 0, 0, abspath);
    }
    free(entries);
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
    
    if (!ch[i]) {
        printf("%cUsage: copy <source> <destination>\n", 255, 255, 255);
        printf("%cExample: copy file1.txt file2.txt\n", 255, 255, 255);
        return;
    }
    
    // Parse source filename
    char source[128] = {0};
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) {
        source[j++] = ch[i++];
    }
    source[j] = '\0';
    
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        printf("%cError: Destination filename required.\n", 255, 0, 0);
        return;
    }
    
    // Parse destination filename
    char dest[128] = {0};
    j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) {
        dest[j++] = ch[i++];
    }
    dest[j] = '\0';
    
    // Resolve paths
    char source_path[256], dest_path[256];
    resolve_path(source, shell_current_path, source_path, sizeof(source_path));
    resolve_path(dest, shell_current_path, dest_path, sizeof(dest_path));
    
    // Check if source and destination are the same
    if (strcmp(source_path, dest_path) == 0) {
        printf("%cError: Source and destination are the same.\n", 255, 0, 0);
        return;
    }
    
    // Read source file
    eynfs_superblock_t sb;
    uint8_t disk = g_current_drive;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cError: No supported filesystem found.\n", 255, 0, 0);
        return;
    }
    
    eynfs_dir_entry_t source_entry;
    uint32_t source_parent_block, source_entry_idx;
    if (eynfs_traverse_path(disk, &sb, source_path, &source_entry, &source_parent_block, &source_entry_idx) != 0) {
        printf("%cError: Source file not found: %s\n", 255, 0, 0, source_path);
        return;
    }
    
    if (source_entry.type != EYNFS_TYPE_FILE) {
        printf("%cError: Source is not a file: %s\n", 255, 0, 0, source_path);
        return;
    }
    
    // Read file content
    uint8_t* file_content = (uint8_t*)malloc(source_entry.size + 1);
    if (!file_content) {
        printf("%cError: Memory allocation failed.\n", 255, 0, 0);
        return;
    }
    
    int bytes_read = eynfs_read_file(disk, &sb, &source_entry, file_content, source_entry.size, 0);
    if (bytes_read <= 0) {
        printf("%cError: Failed to read source file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Find parent directory for destination
    char dest_dir[256];
    const char* dest_name = get_basename(dest_path);
    char* last_slash = strrchr(dest_path, '/');
    if (last_slash && last_slash != dest_path) {
        strncpy(dest_dir, dest_path, last_slash - dest_path);
        dest_dir[last_slash - dest_path] = '\0';
    } else {
        strcpy(dest_dir, "/");
    }
    
    // Get destination parent directory
    eynfs_dir_entry_t dest_parent;
    uint32_t dest_parent_block;
    if (eynfs_traverse_path(disk, &sb, dest_dir, &dest_parent, &dest_parent_block, NULL) != 0) {
        printf("%cError: Destination directory not found: %s\n", 255, 0, 0, dest_dir);
        free(file_content);
        return;
    }
    
    if (dest_parent.type != EYNFS_TYPE_DIR) {
        printf("%cError: Destination is not a directory: %s\n", 255, 0, 0, dest_dir);
        free(file_content);
        return;
    }
    
    // Create the destination file
    if (eynfs_create_entry(disk, &sb, dest_parent.first_block, dest_name, EYNFS_TYPE_FILE) != 0) {
        printf("%cError: Failed to create destination file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Find the created entry
    eynfs_dir_entry_t dest_entry;
    uint32_t dest_entry_idx;
    if (eynfs_find_in_dir(disk, &sb, dest_parent.first_block, dest_name, &dest_entry, &dest_entry_idx) != 0) {
        printf("%cError: Failed to locate created file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Use the improved eynfs_write_file function
    if (eynfs_write_file(disk, &sb, &dest_entry, file_content, bytes_read, dest_parent.first_block, dest_entry_idx) != bytes_read) {
        printf("%cError: Failed to write destination file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    printf("%cFile copied: %s -> %s (%d bytes)\n", 0, 255, 0, source_path, dest_path, bytes_read);
    free(file_content);
}

// Move command implementation - rewritten from scratch
void move_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        printf("%cUsage: move <source> <destination>\n", 255, 255, 255);
        printf("%cExample: move file1.txt /backup/file1.txt\n", 255, 255, 255);
        return;
    }
    
    // Parse source filename
    char source[128] = {0};
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) {
        source[j++] = ch[i++];
    }
    source[j] = '\0';
    
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        printf("%cError: Destination filename required.\n", 255, 0, 0);
        return;
    }
    
    // Parse destination filename
    char dest[128] = {0};
    j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) {
        dest[j++] = ch[i++];
    }
    dest[j] = '\0';
    
    // Resolve paths
    char source_path[256], dest_path[256];
    resolve_path(source, shell_current_path, source_path, sizeof(source_path));
    resolve_path(dest, shell_current_path, dest_path, sizeof(dest_path));
    
    // Check if source and destination are the same
    if (strcmp(source_path, dest_path) == 0) {
        printf("%cError: Source and destination are the same.\n", 255, 0, 0);
        return;
    }
    
    // Read source file
    eynfs_superblock_t sb;
    uint8_t disk = g_current_drive;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cError: No supported filesystem found.\n", 255, 0, 0);
        return;
    }
    
    eynfs_dir_entry_t source_entry;
    uint32_t source_parent_block, source_entry_idx;
    if (eynfs_traverse_path(disk, &sb, source_path, &source_entry, &source_parent_block, &source_entry_idx) != 0) {
        printf("%cError: Source file not found: %s\n", 255, 0, 0, source_path);
        return;
    }
    
    if (source_entry.type != EYNFS_TYPE_FILE) {
        printf("%cError: Source is not a file: %s\n", 255, 0, 0, source_path);
        return;
    }
    
    // Read file content
    uint8_t* file_content = (uint8_t*)malloc(source_entry.size + 1);
    if (!file_content) {
        printf("%cError: Memory allocation failed.\n", 255, 0, 0);
        return;
    }
    
    int bytes_read = eynfs_read_file(disk, &sb, &source_entry, file_content, source_entry.size, 0);
    if (bytes_read <= 0) {
        printf("%cError: Failed to read source file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Find parent directory for destination
    char dest_dir[256];
    const char* dest_name = get_basename(dest_path);
    char* last_slash = strrchr(dest_path, '/');
    if (last_slash && last_slash != dest_path) {
        strncpy(dest_dir, dest_path, last_slash - dest_path);
        dest_dir[last_slash - dest_path] = '\0';
    } else {
        strcpy(dest_dir, "/");
    }
    
    // Get destination parent directory
    eynfs_dir_entry_t dest_parent;
    uint32_t dest_parent_block;
    if (eynfs_traverse_path(disk, &sb, dest_dir, &dest_parent, &dest_parent_block, NULL) != 0) {
        printf("%cError: Destination directory not found: %s\n", 255, 0, 0, dest_dir);
        free(file_content);
        return;
    }
    
    if (dest_parent.type != EYNFS_TYPE_DIR) {
        printf("%cError: Destination is not a directory: %s\n", 255, 0, 0, dest_dir);
        free(file_content);
        return;
    }
    
    // Create the destination file
    if (eynfs_create_entry(disk, &sb, dest_parent.first_block, dest_name, EYNFS_TYPE_FILE) != 0) {
        printf("%cError: Failed to create destination file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Find the created entry
    eynfs_dir_entry_t dest_entry;
    uint32_t dest_entry_idx;
    if (eynfs_find_in_dir(disk, &sb, dest_parent.first_block, dest_name, &dest_entry, &dest_entry_idx) != 0) {
        printf("%cError: Failed to locate created file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Use the improved eynfs_write_file function
    if (eynfs_write_file(disk, &sb, &dest_entry, file_content, bytes_read, dest_parent.first_block, dest_entry_idx) != bytes_read) {
        printf("%cError: Failed to write destination file.\n", 255, 0, 0);
        free(file_content);
        return;
    }
    
    // Now delete the source file
    if (eynfs_delete_entry(disk, &sb, source_parent_block, source_entry.name) != 0) {
        printf("%cWarning: File copied but failed to delete source: %s\n", 255, 255, 0, source_path);
    } else {
        printf("%cFile moved: %s -> %s (%d bytes)\n", 0, 255, 0, source_path, dest_path, bytes_read);
    }
    
    free(file_content);
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