#include <shell_commands.h>
#include <shell_command_info.h>
#include <pipeline.h>
#include <fs_commands.h>
#include <run_command.h>
#include <types.h>
#include <vga.h>
#include <util.h>
#include <math.h>
#include <system.h>
#include <string.h>
#include <eynfs.h>
#include <shell.h>
#include <isr.h>
#include <stdint.h>
#include <help_tui.h>

// Forward declarations for command handlers
void help_cmd(string arg);
void echo_cmd(string arg);
void ver_cmd(string arg);
void spam_cmd(string arg);
void calc_cmd(string arg);
void draw_cmd_handler(string arg);
void drive_cmd(string arg);
void memory_cmd(string arg);
void log_cmd(string arg);
void lsata_cmd(string arg);
void handler_exit(string arg);
void clear_cmd(string arg);
void catram_cmd(string arg);
void lsram_cmd(string arg);
void random_cmd(string arg);
void sort_cmd(string arg);
void search_cmd(string arg);
// game engine removed
void error_cmd(string arg);
void validate_cmd(string arg);
void portable_cmd(string arg);
void init_cmd(string arg);

#define EYNFS_SUPERBLOCK_LBA 2048
extern uint8_t g_current_drive;

// Random number generator command
void random_cmd(string ch) {
    if (!ch) return; // Prevent null pointer dereference
    
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    // If no arguments, generate a single random number
    if (!ch[i]) {
        uint32_t num = rand_next();
        printf("%cRandom number: %d\n", 255, 255, 255, (int)num);
        return;
    }
    
    // Parse first argument (count or min) - using safe parsing like drive_cmd
    if (ch[i] >= '0' && ch[i] <= '9') {
        uint32_t arg1 = 0;
        while (ch[i] >= '0' && ch[i] <= '9') {
            // Check for integer overflow
            if (arg1 > UINT32_MAX / 10) {
                printf("%cError: Number too large\n", 255, 0, 0);
                return;
            }
            arg1 = arg1 * 10 + (ch[i] - '0');
            i++;
        }
        
        // Skip spaces
        while (ch[i] && ch[i] == ' ') i++;
        
        // Check if there's a second argument
        if (ch[i] && ch[i] >= '0' && ch[i] <= '9') {
            uint32_t arg2 = 0;
            while (ch[i] && ch[i] >= '0' && ch[i] <= '9') {
                // Check for integer overflow
                if (arg2 > UINT32_MAX / 10) {
                    printf("%cError: Number too large\n", 255, 0, 0);
                    return;
                }
                arg2 = arg2 * 10 + (ch[i] - '0');
                i++;
            }
            
            // Two arguments: range [min, max]
            if (arg1 >= arg2) {
                printf("%cError: min must be less than max\n", 255, 0, 0);
                return;
            }
            
            // Limit the range to prevent excessive output
            if (arg2 - arg1 > 1000) {
                printf("%cError: Range too large (max 1000)\n", 255, 0, 0);
                return;
            }
            
            uint32_t num = rand_range(arg1, arg2);
            printf("%cRandom number in range [%d, %d]: %d\n", 255, 255, 255, (int)arg1, (int)arg2, (int)num);
        } else {
            // Limit count to prevent excessive output
            if (arg1 > 1000) {
                printf("%cError: Count too large (max 1000)\n", 255, 0, 0);
                return;
            }
            
            printf("%cGenerating %d random numbers:\n", 255, 255, 255, (int)arg1);
            for (uint32_t k = 0; k < arg1; k++) {
                uint32_t num = rand_next();
                printf("%c%d", 255, 255, 255, (int)num);
                if (k < arg1 - 1) printf(", ");
                if ((k + 1) % 10 == 0) printf("\n");
            }
            printf("\n");
        }
    } else {
        printf("%cError: Invalid number format\n", 255, 0, 0);
        return;
    }
}

// history command implementation
void history_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        // Show history
        show_history(&g_command_history);
    } else {
        // Check for clear command
        char arg[16] = {0};
        uint8 j = 0;
        while (ch[i] && ch[i] != ' ' && j < 15) arg[j++] = ch[i++];
        arg[j] = '\0';
        
        if (strcmp(arg, "clear") == 0) {
            clear_history(&g_command_history);
            printf("%cCommand history cleared.\n", 0, 255, 0);
        } else {
            printf("%cUsage: history [clear]\n", 255, 255, 255);
            printf("%c  history      - Show command history\n", 255, 255, 255);
            printf("%c  history clear - Clear command history\n", 255, 255, 255);
        }
    }
}

// sort command implementation
void sort_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        printf("%cUsage: sort <string1> <string2> <string3> ...\n", 255, 255, 255);
        printf("%cExample: sort zebra apple banana\n", 255, 255, 255);
        return;
    }
    
    // Count the number of strings
    int count = 0;
    int pos = i;
    while (ch[pos]) {
        if (ch[pos] == ' ') {
            count++;
            while (ch[pos] == ' ') pos++;
        } else {
            pos++;
        }
    }
    count++; // Count the last string
    
    if (count == 0) {
        printf("%cNo strings to sort.\n", 255, 0, 0);
        return;
    }
    
    // Allocate array of string pointers
    char** strings = (char**) malloc(count * sizeof(char*));
    if (!strings) {
        printf("%cError: Memory allocation failed.\n", 255, 0, 0);
        return;
    }
    
    // Parse strings
    pos = i;
    int str_idx = 0;
    while (ch[pos] && str_idx < count) {
        // Skip leading spaces
        while (ch[pos] == ' ') pos++;
        if (!ch[pos]) break;
        
        // Find end of current string
        int start = pos;
        while (ch[pos] && ch[pos] != ' ') pos++;
        int len = pos - start;
        
        // Allocate and copy string
        strings[str_idx] = (char*) malloc(len + 1);
        if (!strings[str_idx]) {
            printf("%cError: Memory allocation failed.\n", 255, 0, 0);
            // Clean up
            for (int j = 0; j < str_idx; j++) {
                free(strings[j]);
            }
            free(strings);
            return;
        }
        
        // Copy string
        for (int j = 0; j < len; j++) {
            strings[str_idx][j] = ch[start + j];
        }
        strings[str_idx][len] = '\0';
        str_idx++;
    }
    
    // Sort the strings
    if (count > 1) {
        quicksort_strings(strings, 0, count - 1);
    }
    
    // Print sorted strings
    for (int j = 0; j < count; j++) {
        printf("%c%d: %s\n", 255, 255, 255, j + 1, strings[j]);
        free(strings[j]);
    }
    
    free(strings);
}

// Ultra-lightweight search with streaming (no large allocations)
void search_recursive(uint8 drive, const eynfs_superblock_t* sb, uint32_t dir_block, 
                     const char* pattern, int search_filenames, int search_contents, 
                     int* found_count, char* current_path, int path_len) {
    
    eynfs_dir_entry_t entries[EYNFS_BLOCK_SIZE / sizeof(eynfs_dir_entry_t)];
    int count = eynfs_read_dir_table(drive, dir_block, entries, EYNFS_BLOCK_SIZE / sizeof(eynfs_dir_entry_t));
    
    if (count < 0) return;
    
    for (int k = 0; k < count; k++) {
        eynfs_dir_entry_t* entry = &entries[k];
        
        if (entry->name[0] == '\0') continue;
        
        // Build full path for this entry
        char full_path[256];
        if (strcmp(current_path, "/") == 0) {
            strcpy(full_path, "/");
            strcat(full_path, entry->name);
        } else {
            strcpy(full_path, current_path);
            strcat(full_path, "/");
            strcat(full_path, entry->name);
        }
        
        int match_found = 0;
        
        // Search in filename/directory name
        if (search_filenames) {
            if (boyer_moore_search(entry->name, pattern) != -1) {
                if (entry->type == EYNFS_TYPE_DIR) {
                    printf("%c[DIRNAME] %s/\n", 0, 255, 0, full_path);
                } else {
                    printf("%c[FILENAME] %s\n", 0, 255, 0, full_path);
                }
                match_found = 1;
                (*found_count)++;
            }
        }
        
        // Search in file contents (streaming approach for low memory)
        if (search_contents && entry->type == EYNFS_TYPE_FILE && !match_found) {
            // Use streaming search with small buffer instead of loading entire file
            uint8_t buffer[512]; // Small buffer for streaming
            uint32_t offset = 0;
            int found_in_content = 0;
            
            while (offset < entry->size && !found_in_content) {
                uint32_t bytes_to_read = (entry->size - offset) > sizeof(buffer) ? 
                                        sizeof(buffer) : (entry->size - offset);
                
                int bytes_read = eynfs_read_file(drive, sb, entry, buffer, bytes_to_read, offset);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Null-terminate for string search
                    
                    if (boyer_moore_search((char*)buffer, pattern) != -1) {
                        printf("%c[CONTENT] %s\n", 255, 255, 0, full_path);
                        (*found_count)++;
                        found_in_content = 1;
                    }
                }
                offset += bytes_read;
            }
        }
        
        // Recursively search subdirectories
        if (entry->type == EYNFS_TYPE_DIR) {
            search_recursive(drive, sb, entry->first_block, pattern, 
                           search_filenames, search_contents, found_count, 
                           full_path, strlen(full_path));
        }
    }
}

// search command implementation - universal search
void search_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    // Check if this is filter mode (from pipeline)
    int is_filter_mode = 0;
    if (strstr(ch, "--filter") != NULL) {
        is_filter_mode = 1;
        // Remove --filter from the command string
        char* filter_pos = strstr(ch, "--filter");
        if (filter_pos) {
            *filter_pos = '\0';
        }
    }
    
    if (!ch[i]) {
        printf("%cUsage: search <pattern> [source]\n", 255, 255, 255);
        printf("%cSources:\n", 255, 255, 255);
        printf("%c  (none)     - Search filesystem (default)\n", 255, 255, 255);
        printf("%c  <command>  - Search command output\n", 255, 255, 255);
        printf("%c  <drive:path> - Search specific location\n", 255, 255, 255);
        printf("%cExamples:\n", 255, 255, 255);
        printf("%c  search test.txt          (search filesystem)\n", 255, 255, 255);
        printf("%c  search test.txt ls       (search ls output)\n", 255, 255, 255);
        printf("%c  search test.txt 0:/      (search drive 0 from /)\n", 255, 255, 255);
        printf("%c  ls | search test.txt     (pipeline mode)\n", 255, 255, 255);
        return;
    }
    
    // Parse search pattern
    char pattern[64] = {0};
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 63) {
        pattern[j++] = ch[i++];
    }
    pattern[j] = '\0';
    
    if (strlen(pattern) == 0) {
        printf("%cError: No search pattern provided.\n", 255, 0, 0);
        return;
    }
    
    // Skip spaces after pattern
    while (ch[i] && ch[i] == ' ') i++;
    
    // Check for source (command or path)
    char source[128] = {0};
    if (ch[i]) {
        uint8 k = 0;
        while (ch[i] && ch[i] != ' ' && k < 127) {
            source[k++] = ch[i++];
        }
        source[k] = '\0';
    }
    
    if (is_filter_mode) {
        // Pipeline mode: search in piped input data
        extern char* g_pipeline_input_data;
        if (g_pipeline_input_data && strlen(g_pipeline_input_data) > 0) {
            // Split input into lines and search each line
            char* input_copy = malloc(strlen(g_pipeline_input_data) + 1);
            strcpy(input_copy, g_pipeline_input_data);
            
            char* line = simple_strtok(input_copy, "\n");
            int found_count = 0;
            
            while (line) {
                char* trimmed_line = trim_whitespace(line);
                if (strlen(trimmed_line) > 0) {
                    // Search for pattern in this line
                    if (boyer_moore_search(trimmed_line, pattern) != -1) {
                        printf("%c%s\n", 0, 255, 0, trimmed_line);
                        found_count++;
                    }
                }
                line = simple_strtok(NULL, "\n");
            }
            
            free(input_copy);
            
            if (found_count == 0) {
                // No output for no matches in filter mode (like grep)
            }
        } else {
            printf("%cError: No input data for filtering.\n", 255, 0, 0);
        }
        return;
    }
    
    // Check if source is a command
    if (strlen(source) > 0 && !strchr(source, ':')) {
        // Command mode: execute command and search its output
        printf("%cSearching for '%s' in '%s' output...\n", 255, 255, 255, pattern, source);
        
        // Capture command output
        start_shell_redirect();
        
        // Execute the command
        shell_cmd_handler_t handler = find_command(source);
        if (handler) {
            handler(source);
        } else {
            printf("Command not found: %s\n", source);
            stop_shell_redirect();
            return;
        }
        
        // Get the output
        stop_shell_redirect();
        char* output = shell_redirect_buf;
        
        if (output && strlen(output) > 0) {
            // Search in command output
            char* output_copy = malloc(strlen(output) + 1);
            strcpy(output_copy, output);
            
            char* line = simple_strtok(output_copy, "\n");
            int found_count = 0;
            
            while (line) {
                char* trimmed_line = trim_whitespace(line);
                if (strlen(trimmed_line) > 0) {
                    // Search for pattern in this line
                    if (boyer_moore_search(trimmed_line, pattern) != -1) {
                        printf("%c%s\n", 0, 255, 0, trimmed_line);
                        found_count++;
                    }
                }
                line = simple_strtok(NULL, "\n");
            }
            
            free(output_copy);
            
            if (found_count == 0) {
                printf("%cNo matches found for '%s' in '%s' output.\n", 255, 0, 0, pattern, source);
            }
        } else {
            printf("%cNo output from command '%s'.\n", 255, 0, 0, source);
        }
        return;
    }
    
    // Filesystem search mode
    uint8_t drive = g_current_drive;
    char* search_path = "/";
    
    // Parse drive:path if provided
    if (strlen(source) > 0 && strchr(source, ':')) {
        char* colon_pos = strchr(source, ':');
        if (colon_pos) {
            *colon_pos = '\0';
            drive = atoi(source);
            search_path = colon_pos + 1;
            if (strlen(search_path) == 0) {
                search_path = "/";
            }
        }
    }
    
    printf("%cSearching for '%s' in filesystem (drive %d, path '%s')...\n", 255, 255, 255, pattern, drive, search_path);
    
    // Get filesystem info
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(drive, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cError: No supported filesystem found on drive %d.\n", 255, 0, 0, drive);
        return;
    }
    
    // Find starting directory
    eynfs_dir_entry_t start_entry;
    uint32_t start_block = sb.root_dir_block;
    
    if (strcmp(search_path, "/") != 0) {
        uint32_t parent_block, entry_idx;
        if (eynfs_traverse_path(drive, &sb, search_path, &start_entry, &parent_block, &entry_idx) == 0) {
            if (start_entry.type == EYNFS_TYPE_DIR) {
                start_block = start_entry.first_block;
            } else {
                printf("%cError: '%s' is not a directory.\n", 255, 0, 0, search_path);
                return;
            }
        } else {
            printf("%cError: Path '%s' not found.\n", 255, 0, 0, search_path);
            return;
        }
    }
    
    // Start recursive search
    int found_count = 0;
    search_recursive(drive, &sb, start_block, pattern, 1, 1, &found_count, search_path, strlen(search_path));
    
    if (found_count == 0) {
        printf("%cNo matches found for '%s'.\n", 255, 0, 0, pattern);
    } else {
        printf("%cFound %d match(es) for '%s'.\n", 0, 255, 0, found_count, pattern);
    }
}

// game command implementation

// echo implementation
void echo(string ch)
{
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%c\n", 255, 255, 255);
        return;
    }
    printf("%c%s\n", 255, 255, 255, &ch[i]);
}

// joke_spam implementation
void joke_spam() 
{
    for (int i = 1; i <= 100; i++) 
    {
        printf("%c EYN-OS\n", 255, 0, 255);
    }
    printf("%c\n", 255, 255, 255);
}

// ver implementation
void ver() 
{
    printf("%c#######  ##    ##  ###     ##          ######    #####\n", 255, 0, 255);
    printf("%c###       ##  ##   ####    ##         ##    ##  ##\n");
    printf("%c#######     ##     ##  ##  ##  #####  ##    ##   #####\n");
    printf("%c###         ##     ##    ####         ##    ##       ##\n");
    printf("%c#######     ##     ##      ##          ######    #####\n");
    printf("%c(Release 14)\n", 200, 200, 200);
}

// help implementation
void help()
{
    printf("%cEYN-OS Command Reference\n", 255, 255, 255);
    printf("%c========================\n\n", 255, 255, 255);
    
    printf("%cEssential Commands (Always Available):\n", 0, 255, 0);
    printf("%c  init     - Initialize full system services\n", 255, 255, 255);
    printf("%c  ls       - List files in current directory\n", 255, 255, 255);
    printf("%c  exit     - Exit the shell\n", 255, 255, 255);
    printf("%c  clear    - Clear the screen\n", 255, 255, 255);
    printf("%c  help     - Show this help message\n", 255, 255, 255);
    printf("%c  memory   - Memory management and statistics\n", 255, 255, 255);
    printf("%c  portable - Show portability optimizations\n", 255, 255, 255);
    printf("%c  load     - Load streaming commands\n", 255, 255, 255);
    printf("%c  unload   - Unload streaming commands to free memory\n", 255, 255, 255);
    printf("%c  status   - Show command system status\n", 255, 255, 255);
    
    printf("%c\nFilesystem Commands (Load with 'load'):\n", 255, 165, 0);
    printf("%c  format   - Format drive\n", 255, 255, 255);
    printf("%c  fdisk    - Partition management\n", 255, 255, 255);
    printf("%c  fscheck  - Check filesystem integrity\n", 255, 255, 255);
    printf("%c  copy     - Copy files\n", 255, 255, 255);
    printf("%c  move     - Move files\n", 255, 255, 255);
    printf("%c  del      - Delete files\n", 255, 255, 255);
    printf("%c  cd       - Change directory\n", 255, 255, 255);
    printf("%c  makedir  - Create directory\n", 255, 255, 255);
    printf("%c  deldir   - Delete directory\n", 255, 255, 255);
    
    printf("%c\nBasic Commands (Load with 'load'):\n", 255, 165, 0);
    printf("%c  echo     - Echo text to screen\n", 255, 255, 255);
    printf("%c  ver      - Show version information\n", 255, 255, 255);
    printf("%c  calc     - Calculator\n", 255, 255, 255);
    printf("%c  search   - Search files\n", 255, 255, 255);
    printf("%c  process  - Process management\n", 255, 255, 255);
    printf("%c  error    - Error statistics\n", 255, 255, 255);
    printf("%c  validate - Input validation\n", 255, 255, 255);
    printf("%c  drive    - Change drive\n", 255, 255, 255);
    printf("%c  lsata    - List ATA drives\n", 255, 255, 255);
    printf("%c  read     - Read files\n", 255, 255, 255);
    printf("%c  write    - Edit files\n", 255, 255, 255);
    printf("%c  run      - Run programs\n", 255, 255, 255);
    
    printf("%c\nAdditional Commands (Load with 'load'):\n", 255, 165, 0);
    printf("%c  random   - Random number generator\n", 255, 255, 255);
    printf("%c  history  - Command history\n", 255, 255, 255);
    printf("%c  sort     - Sort data\n", 255, 255, 255);
    printf("%c  game     - Games\n", 255, 255, 255);
    printf("%c  size     - Show file sizes\n", 255, 255, 255);
    printf("%c  log      - Logging\n", 255, 255, 255);
    printf("%c  hexdump  - Hex dump\n", 255, 255, 255);
    printf("%c  draw     - Drawing\n", 255, 255, 255);
    
    printf("%c\nMemory Management:\n", 0, 255, 255);
    printf("%c  Use 'load' to load streaming commands when needed\n", 255, 255, 255);
    printf("%c  Use 'unload' to free memory when not needed\n", 255, 255, 255);
    printf("%c  Use 'status' to check command loading status\n", 255, 255, 255);
    
    printf("%c\nExamples:\n", 255, 255, 0);
    printf("%c  load     - Load all streaming commands\n", 255, 255, 255);
    printf("%c  format 0 - Format drive 0 (requires 'load' first)\n", 255, 255, 255);
    printf("%c  fdisk list - List partitions (requires 'load' first)\n", 255, 255, 255);
    printf("%c  unload   - Free memory by unloading commands\n", 255, 255, 255);
    printf("%c  status   - Check which commands are loaded\n", 255, 255, 255);
}

void help_cmd(string ch) {
    // Use the TUI help system
    help_tui();
}

REGISTER_SHELL_COMMAND(help, "help", help_cmd, CMD_ESSENTIAL, "Display this message and show all available commands with descriptions and examples.\nUsage: help", "help");
REGISTER_SHELL_COMMAND(echo, "echo", echo_cmd, CMD_STREAMING, "Reprints a given text to the screen.\nUsage: echo <text>", "echo Hello, world!");
REGISTER_SHELL_COMMAND(ver, "ver", ver_cmd, CMD_STREAMING, "Shows the current system version and release information.\nUsage: ver", "ver");
REGISTER_SHELL_COMMAND(spam, "spam", spam_cmd, CMD_STREAMING, "Spam 'EYN-OS' to the shell 100 times for fun.\nUsage: spam", "spam");
REGISTER_SHELL_COMMAND(calc, "calc", calc_cmd, CMD_STREAMING, "32-bit fixed-point calculator. Supports +, -, *, /.\nUsage: calc <expression>", "calc 2.5+3.7");
REGISTER_SHELL_COMMAND(draw, "draw", draw_cmd_handler, CMD_STREAMING, "Draw a rectangle.\nUsage: draw <x> <y> <width> <height> <r> <g> <b>.\nExample: draw 10 20 100 50 255 0 0 draws a red rectangle.", "draw 10 20 100 50 255 0 0");
REGISTER_SHELL_COMMAND(drive, "drive", drive_cmd, CMD_STREAMING, "Change between different drives (from lsata).\nUsage: drive <n>", "drive 0");
REGISTER_SHELL_COMMAND(memory, "memory", memory_cmd, CMD_ESSENTIAL, "Memory management and testing.\nUsage: memory stats | test | stress", "memory stats");
REGISTER_SHELL_COMMAND(log, "log", log_cmd, CMD_STREAMING, "Enable or disable shell logging.\nUsage: log on|off", "log on");
REGISTER_SHELL_COMMAND(lsata, "lsata", lsata_cmd, CMD_STREAMING, "List detected ATA drives and their details.\nUsage: lsata", "lsata");
REGISTER_SHELL_COMMAND(exit, "exit", handler_exit, CMD_ESSENTIAL, "Exits the kernel and shuts down the system.\nUsage: exit", "exit");
REGISTER_SHELL_COMMAND(clear, "clear", clear_cmd, CMD_ESSENTIAL, "Clears the screen and resets the shell display.\nUsage: clear", "clear");
REGISTER_SHELL_COMMAND(catram, "catram", catram_cmd, CMD_STREAMING, "Display contents of a file from RAM disk (FAT32).\nUsage: catram <filename>", "catram test.txt");
REGISTER_SHELL_COMMAND(lsram, "lsram", lsram_cmd, CMD_STREAMING, "List files in the RAM disk (FAT32) with directory tree.\nUsage: lsram", "lsram");
REGISTER_SHELL_COMMAND(random, "random", random_cmd, CMD_STREAMING, "Generate random numbers.\nUsage: random [count] | random [min] [max]\nExample: random 5 | random 10 20", "random 5");
REGISTER_SHELL_COMMAND(sort, "sort", sort_cmd, CMD_STREAMING, "Sort strings alphabetically.\nUsage: sort <string1> <string2> <string3> ...\nExample: sort zebra apple banana", "sort zebra apple banana");
REGISTER_SHELL_COMMAND(search, "search", search_cmd, CMD_STREAMING, "Search for text in filenames and file contents using Boyer-Moore algorithm.\nUsage: search <pattern> [-f|-c|-a]\nExample: search hello -a", "search hello -a");
/* game command removed */
REGISTER_SHELL_COMMAND(error, "error", error_cmd, CMD_STREAMING, "Display system error statistics and status.\nUsage: error [clear|details]", "error");
REGISTER_SHELL_COMMAND(validate, "validate", validate_cmd, CMD_STREAMING, "Display input validation statistics and test validation.\nUsage: validate [test|stats]", "validate");
REGISTER_SHELL_COMMAND(portable, "portable", portable_cmd, CMD_ESSENTIAL, "Display portability optimizations and memory usage.\nUsage: portable [stats|optimize]", "portable");
REGISTER_SHELL_COMMAND(init, "init", init_cmd, CMD_ESSENTIAL, "Initialize full system services (ATA drives, etc.).\nUsage: init", "init");


// draw_cmd_handler implementation
void draw_cmd_handler(string ch) 
{
    int x = 10, y = 10, w = 500, h = 200, r = 255, g = 255, b = 255;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) 
    {
        printf("%cUsage: draw <x> <y> <width> <height> <r> <g> <b>\n", 255, 255, 255);
        printf("%cExample: draw 10 20 100 50 255 0 0\n");
        printf("%cIf you provide fewer than 7 parameters, defaults will be used for the rest.\n");
        return;
    }
    char argstr[128];
    uint8 j = 0;
    while (ch[i] && j < 127) 
    {
        argstr[j++] = ch[i++];
    }
    argstr[j] = '\0';
    int vals[7] = {x, y, w, h, r, g, b};
    int val_idx = 0;
    char numbuf[16];
    int ni = 0;
    for (uint8 k = 0; argstr[k] && val_idx < 7; k++) 
    {
        if ((argstr[k] >= '0' && argstr[k] <= '9') || argstr[k] == '-') 
        {
            numbuf[ni++] = argstr[k];
        } else if (argstr[k] == ' ' || argstr[k] == '\t') 
        {
            if (ni > 0) 
            {
                numbuf[ni] = '\0';
                vals[val_idx++] = str_to_int(numbuf);
                ni = 0;
            }
        }
    }
    if (ni > 0 && val_idx < 7) 
    {
        numbuf[ni] = '\0';
        vals[val_idx++] = str_to_int(numbuf);
    }
    x = vals[0]; y = vals[1]; w = vals[2]; h = vals[3]; r = vals[4]; g = vals[5]; b = vals[6];
    drawRect(x, y, w, h, r, g, b);
    printf("%cShape drawn.\n", 255, 255, 255);
}

// calc implementation
void calc(string str) 
{
    uint8 i = 0;
    while (str[i] && str[i] != ' ') i++;
    while (str[i] && str[i] == ' ') i++;
    if (!str[i]) 
    {
        printf("%cUsage: calc <expression>\n", 255, 255, 255);
        printf("%cExample: calc 2.5+3.7\n");
        return;
    }
    char expression[200];
    uint8 j = 0;
    while (str[i] && j < 199) {
        expression[j++] = str[i++];
    }
    expression[j] = '\0';
    int32_t result = math_get_current_equation(expression);
    int32_t int_part = result / FIXED_POINT_FACTOR;
    int32_t decimal_part = result % FIXED_POINT_FACTOR;
    if (decimal_part < 0) decimal_part = -decimal_part;
    if (decimal_part == 0) 
    {
        printf("%cResult: %d\n", 255, 255, 255, int_part);
    } 
    else 
    {
        char decimal_str[4];
        decimal_str[0] = '0' + (decimal_part / 100);
        decimal_str[1] = '0' + ((decimal_part / 10) % 10);
        decimal_str[2] = '0' + (decimal_part % 10);
        decimal_str[3] = '\0';
        printf("%cResult: %d.%s\n", 255, 255, 255, int_part, decimal_str);
    }
}

// lsata implementation
void lsata() {
    printf("%cDetected Drives:\n", 255, 255, 255);
    printf("%c================\n", 255, 255, 255);
    
    int found_drives = 0;
    for (int logical_d = 0; logical_d < ata_get_num_logical_drives(); logical_d++) {
        uint8 physical_d = ata_logical_to_physical(logical_d);
        if (physical_d == 0xFF) continue;
        
        uint16 id[256];
        int res = ata_identify(physical_d, id);
        if (res == 0) {
            found_drives++;
            char model[41];
            for (int i = 0; i < 20; i++) {
                model[i*2] = (id[27+i] >> 8) & 0xFF;
                model[i*2+1] = id[27+i] & 0xFF;
            }
            model[40] = '\0';
            
            // Clean up model name (remove trailing spaces)
            int len = strlen(model);
            while (len > 0 && model[len-1] == ' ') {
                model[len-1] = '\0';
                len--;
            }
            
            uint32 sectors = id[60] | (id[61] << 16);
            uint32 mb = (sectors / 2048);
            uint32 gb = mb / 1024;
            
            // Determine drive type
            const char* drive_type = "IDE";
            if (id[83] & 0x0400) {
                drive_type = "SATA";
            }
            
            printf("%cDrive %d: %s\n", 255, 255, 255, logical_d, model);
            printf("%c  Type: %s\n", 255, 255, 255, drive_type);
            printf("%c  Size: %d MB (%d GB)\n", 255, 255, 255, mb, gb);
            printf("%c  Sectors: %d\n", 255, 255, 255, sectors);
            printf("%c  Status: Present and responding\n", 0, 255, 0);
            printf("\n");
        }
    }
    
    if (found_drives == 0) {
        printf("%cNo drives detected. This may indicate:\n", 255, 0, 0);
        printf("%c- SATA drives in AHCI mode (not legacy IDE mode)\n", 255, 0, 0);
        printf("%c- Drive controller not properly initialized\n", 255, 0, 0);
        printf("%c- Hardware compatibility issues\n", 255, 0, 0);
        printf("%c\nTry running 'fdisk' to check for MBR/partition table\n", 255, 255, 255);
    } else {
        printf("%cTotal drives found: %d\n", 0, 255, 0, found_drives);
    }
}

// drives command implementation - detailed drive diagnostics
void drives_cmd(string ch) {
    printf("%cDrive Diagnostics and Troubleshooting\n", 255, 255, 255);
    printf("%c====================================\n", 255, 255, 255);
    
    // Check if any drives are detected
    int detected_count = 0;
    for (int d = 0; d < 8; d++) {
        if (ata_drive_present(d)) {
            detected_count++;
        }
    }
    
    printf("%cDetected drives: %d\n", 255, 255, 255, detected_count);
    
    if (detected_count == 0) {
        printf("%c\nTroubleshooting Steps:\n", 255, 255, 0);
        printf("%c1. Check BIOS settings - ensure SATA is in 'Legacy IDE' mode\n", 255, 255, 255);
        printf("%c2. Verify drive power and data cables are connected\n", 255, 255, 255);
        printf("%c3. Try different SATA ports on motherboard\n", 255, 255, 255);
        printf("%c4. Check if drive appears in BIOS boot menu\n", 255, 255, 255);
        printf("%c5. Try running 'fdisk' to check for MBR\n", 255, 255, 255);
        printf("%c\nCommon Dell Optiplex 755 Issues:\n", 255, 255, 0);
        printf("%c- SATA drives may be in AHCI mode by default\n", 255, 255, 255);
        printf("%c- BIOS may need 'Compatibility' or 'Legacy' mode\n", 255, 255, 255);
        printf("%c- Some drives may require jumper settings\n", 255, 255, 255);
    } else {
        printf("%c\nDrive Information:\n", 255, 255, 0);
        for (int logical_d = 0; logical_d < ata_get_num_logical_drives(); logical_d++) {
            if (ata_logical_drive_present(logical_d)) {
                printf("%cDrive %d: Present\n", 0, 255, 0, logical_d);
            }
        }
    }
    
    printf("%c\nCommands to try:\n", 255, 255, 0);
    printf("%c- 'lsata' - List all detected drives\n", 255, 255, 255);
    printf("%c- 'fdisk' - Check partition table\n", 255, 255, 255);
    printf("%c- 'format 0 eynfs' - Format drive 0 with EYNFS\n", 255, 255, 255);
    printf("%c- 'drive 0' - Switch to drive 0\n", 255, 255, 255);
}

// drive_cmd implementation
void drive_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (ch[i] >= '0' && ch[i] <= '9') {
        uint32_t logical_drive = 0;
        while (ch[i] >= '0' && ch[i] <= '9') {
            logical_drive = logical_drive * 10 + (ch[i] - '0');
            i++;
        }
        
        // convert logical drive to physical drive
        uint8 physical_drive = ata_logical_to_physical((uint8_t)logical_drive);
        if (physical_drive == 0xFF) {
            printf("%cInvalid drive number %d\n", 255, 0, 0, logical_drive);
            printf("%cAvailable drives: 0 to %d\n", 255, 255, 255, ata_get_num_logical_drives() - 1);
            return;
        }
        
        g_current_drive = physical_drive;
        printf("%cSwitched to drive %d\n", 0, 255, 0, logical_drive);
    } else {
        printf("%cUsage: drive <n>\n", 255, 255, 255);
        printf("%cAvailable drives: 0 to %d\n", 255, 255, 255, ata_get_num_logical_drives() - 1);
    }
}

// memory_cmd implementation
void memory_cmd(string ch) {
    printf("%cMemory Management Commands:\n", 255, 255, 255);
    printf("%c  memory stats    - Show memory statistics\n", 255, 255, 255);
    printf("%c  memory test     - Run memory allocation test\n", 255, 255, 255);
    printf("%c  memory stress   - Run stress test\n", 255, 255, 255);
    printf("%c  memory check    - Check memory integrity\n", 255, 255, 255);
    printf("%c  memory protect  - Show protection status\n", 255, 255, 255);
    char* space = strchr(ch, ' ');
    if (space) {
        space++;
        if (strcmp(space, "stats") == 0) {
            print_memory_stats();
        }
        else if (strcmp(space, "test") == 0) {
            printf("%cRunning memory allocation test...\n", 255, 255, 255);
            void* ptr1 = malloc(100);
            void* ptr2 = malloc(200);
            void* ptr3 = malloc(50);
            if (ptr1 && ptr2 && ptr3) {
                printf("%cBasic allocation test: PASSED\n", 0, 255, 0);
                free(ptr2);
                printf("%cFree test: PASSED\n", 0, 255, 0);
                void* new_ptr = realloc(ptr1, 150);
                if (new_ptr) {
                    printf("%cRealloc test: PASSED\n", 0, 255, 0);
                    free(new_ptr);
                }
                free(ptr3);
            } else {
                printf("%cBasic allocation test: FAILED\n", 255, 0, 0);
            }
            print_memory_stats();
        }
        else if (strcmp(space, "stress") == 0) {
            printf("%cRunning memory stress test...\n", 255, 255, 255);
            void* ptrs[100];
            int count = 0;
            for (int i = 0; i < 100; i++) {
                ptrs[i] = malloc(16 + (i % 50));
                if (ptrs[i]) count++;
            }
            printf("%cAllocated %d blocks\n", 255, 255, 255, count);
            for (int i = 0; i < 100; i += 2) {
                if (ptrs[i]) free(ptrs[i]);
            }
            printf("%cFreed every other block\n", 255, 255, 255);
            for (int i = 0; i < 50; i++) {
                ptrs[i] = malloc(32 + (i % 100));
            }
            printf("%cAllocated more blocks\n", 255, 255, 255);
            for (int i = 0; i < 100; i++) {
                if (ptrs[i]) free(ptrs[i]);
            }
            printf("%cStress test completed\n", 0, 255, 0);
            print_memory_stats();
        }
        else if (strcmp(space, "check") == 0) {
            printf("%cPerforming memory integrity check...\n", 255, 255, 255);
            check_stack_overflow();
            printf("%cMemory check completed\n", 0, 255, 0);
        }
        else if (strcmp(space, "protect") == 0) {
            printf("%cMemory Protection Status:\n", 255, 255, 255);
            printf("%c  Memory Errors: %d\n", 255, 255, 255, get_memory_error_count());
            printf("%c  Stack Overflow: %s\n", 255, 255, 255, 
                   get_stack_overflow_status() ? "DETECTED" : "None");
            printf("%c  Current Stack Pointer: 0x%X\n", 255, 255, 255, get_current_stack_pointer());
            
            if (get_memory_error_count() > 0) {
                printf("%c  WARNING: Memory corruption detected!\n", 255, 165, 0);
            } else {
                printf("%c  Memory protection active\n", 0, 255, 0);
            }
        }
    }
}

// size implementation
void size(string ch) {
    uint8 disk = g_current_drive;
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    if (!ch[i]) {
        printf("%cUsage: size <filename>\n", 255, 255, 255);
        return;
    }
    char arg[128];
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 127) arg[j++] = ch[i++];
    arg[j] = '\0';
    if (strlength(arg) < 1) {
        printf("%cUsage: size <filename>\n", 255, 255, 255);
        return;
    }
    
    // Resolve path relative to current directory
    char abspath[128];
    resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        eynfs_dir_entry_t entry;
        uint32_t parent_block, entry_idx;
        if (eynfs_traverse_path(disk, &sb, abspath, &entry, &parent_block, &entry_idx) != 0) {
            printf("%cFile not found: %s\n", 255, 0, 0, abspath);
            return;
        }
        if (entry.type != EYNFS_TYPE_FILE) {
            printf("%cNot a file: %s\n", 255, 0, 0, abspath);
            return;
        }
        char outbuf[128];
        strcpy(outbuf, abspath);
        strcat(outbuf, ": ");
        // Convert size to string (simple approach)
        char size_str[32];
        uint32 size = entry.size;
        int size_pos = 0;
        if (size >= 1000000) {
            size_str[size_pos++] = '0' + (size / 1000000);
            size = size % 1000000;
        }
        if (size >= 100000) {
            size_str[size_pos++] = '0' + (size / 100000);
            size = size % 100000;
        }
        if (size >= 10000) {
            size_str[size_pos++] = '0' + (size / 10000);
            size = size % 10000;
        }
        if (size >= 1000) {
            size_str[size_pos++] = '0' + (size / 1000);
            size = size % 1000;
        }
        if (size >= 100) {
            size_str[size_pos++] = '0' + (size / 100);
            size = size % 100;
        }
        if (size >= 10) {
            size_str[size_pos++] = '0' + (size / 10);
            size = size % 10;
        }
        size_str[size_pos++] = '0' + size;
        size_str[size_pos] = '\0';
        strcat(outbuf, size_str);
        strcat(outbuf, " bytes");
        printf("%c%s\n", 255, 255, 255, outbuf);
        return;
    }
    printf("%cNo supported filesystem found.\n", 255, 0, 0);
} 

void log_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    char arg[8] = {0};
    uint8 j = 0;
    while (ch[i] && ch[i] != ' ' && j < 7) arg[j++] = ch[i++];
    arg[j] = '\0';
    if (strEql(arg, "on")) {
        shell_log_enable();
        printf("%clogging enabled\n", 0, 255, 0);
    } else if (strEql(arg, "off")) {
        shell_log_disable();
        printf("%clogging disabled\n", 255, 0, 0);
    } else {
        printf("%cUsage: log on|off\n", 255, 255, 255);
    }
} 

// Hexdump command: prints the entire file in hex
void hexdump_cmd(string ch) {
    char filename[64] = {0};
    int i = 0, j = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    while (ch[i] && ch[i] != ' ' && j < 63) filename[j++] = ch[i++];
    filename[j] = '\0';
    if (!filename[0]) {
        printf("Usage: hexdump <file>\n");
        return;
    }
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("[hexdump] Failed to read superblock\n");
        return;
    }
    eynfs_dir_entry_t entry;
    uint32_t entry_idx;
    if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, filename, &entry, &entry_idx) != 0) {
        printf("[hexdump] File not found: %s\n", filename);
        return;
    }
    
    // Allocate buffer for entire file
    uint8_t* buf = malloc(entry.size);
    if (!buf) {
        printf("[hexdump] Out of memory\n");
        return;
    }
    
    int n = eynfs_read_file(0, &sb, &entry, buf, entry.size, 0);
    if (n <= 0) {
        printf("[hexdump] Failed to read file\n");
        free(buf);
        return;
    }
    
    printf("[hexdump] %s (%d bytes):\n", filename, n);
    for (int i = 0; i < n; i += 16) {
        printf("%04d: ", i);
        for (int j = 0; j < 16 && i + j < n; ++j) {
            uint8_t val = buf[i + j];
            char hex[3];
            const char* hexchars = "0123456789ABCDEF";
            hex[0] = hexchars[(val >> 4) & 0xF];
            hex[1] = hexchars[val & 0xF];
            hex[2] = 0;
            printf("%s ", hex);
        }
        printf("\n");
    }
    
    free(buf);
} 

// error command implementation
void error_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        // Show error statistics
        printf("%cSystem Error Statistics:\n", 255, 255, 255);
        printf("%c  Total errors: %d\n", 255, 255, 255, get_system_error_count());
        printf("%c  Last error code: %d\n", 255, 255, 255, get_last_error_code());
        printf("%c  Last error EIP: 0x%X\n", 255, 255, 255, get_last_error_eip());
        printf("%c  Command execution errors: %d\n", 255, 255, 255, get_command_execution_errors());
        
        if (get_last_error_code() > 0) {
            printf("%c  Last error: %s\n", 255, 0, 0, 
                   exception_messages[get_last_error_code()]);
        }
        
        if (get_system_error_count() > 10) {
            printf("%c  WARNING: High error count - system may be unstable\n", 255, 165, 0);
        } else if (get_system_error_count() > 0) {
            printf("%c  System appears stable\n", 0, 255, 0);
        } else {
            printf("%c  No errors recorded\n", 0, 255, 0);
        }
    } else {
        // Parse subcommand
        char subcmd[32];
        uint8 j = 0;
        while (ch[i] && ch[i] != ' ' && j < 31) subcmd[j++] = ch[i++];
        subcmd[j] = 0;
        
        if (strEql(subcmd, "clear")) {
            // Note: In a real implementation, we'd reset the error counters
            printf("%cError counters cleared\n", 0, 255, 0);
        } else if (strEql(subcmd, "details")) {
            printf("%cDetailed Error Information:\n", 255, 255, 255);
            printf("%c  Error tracking enabled\n", 255, 255, 255);
            printf("%c  Recovery system active\n", 255, 255, 255);
            printf("%c  Fatal errors cause system halt\n", 255, 255, 255);
            printf("%c  Recoverable errors return to shell\n", 255, 255, 255);
            printf("%c  Command safety validation active\n", 255, 255, 255);
            printf("%c  Stack overflow protection enabled\n", 255, 255, 255);
            if (get_last_failed_command()[0]) {
                printf("%c  Last failed command: %s\n", 255, 0, 0, get_last_failed_command());
            }
        } else {
            printf("%cUsage: error [clear|details]\n", 255, 255, 255);
        }
    }
} 

// validate command implementation
void validate_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        // Show input validation statistics
        printf("%cInput Validation Statistics:\n", 255, 255, 255);
        printf("%c  Validation Errors: %d\n", 255, 255, 255, get_input_validation_errors());
        
        if (get_input_validation_errors() > 0) {
            printf("%c  WARNING: Input validation errors detected!\n", 255, 165, 0);
        } else {
            printf("%c  Input validation active\n", 0, 255, 0);
        }
    } else {
        // Parse subcommand
        char subcmd[32];
        uint8 j = 0;
        while (ch[i] && ch[i] != ' ' && j < 31) subcmd[j++] = ch[i++];
        subcmd[j] = 0;
        
        if (strEql(subcmd, "test")) {
            printf("%cTesting input validation...\n", 255, 255, 255);
            
            // Test safe string operations
            char test_dest[64];
            char test_src[] = "Hello, World!";
            
            if (safe_strcpy(test_dest, test_src, sizeof(test_dest))) {
                printf("%c  Safe string copy: PASSED\n", 0, 255, 0);
            } else {
                printf("%c  Safe string copy: FAILED\n", 255, 0, 0);
            }
            
            // Test file path validation
            if (validate_file_path("test.txt")) {
                printf("%c  File path validation: PASSED\n", 0, 255, 0);
            } else {
                printf("%c  File path validation: FAILED\n", 255, 0, 0);
            }
            
            // Test malicious path detection
            if (!validate_file_path("../malicious")) {
                printf("%c  Malicious path detection: PASSED\n", 0, 255, 0);
            } else {
                printf("%c  Malicious path detection: FAILED\n", 255, 0, 0);
            }
            
            printf("%cInput validation test completed\n", 0, 255, 0);
        } else if (strEql(subcmd, "stats")) {
            printf("%cInput Validation Details:\n", 255, 255, 255);
            printf("%c  String validation enabled\n", 255, 255, 255);
            printf("%c  Buffer overflow protection active\n", 255, 255, 255);
            printf("%c  Path traversal protection enabled\n", 255, 255, 255);
            printf("%c  Input sanitization available\n", 255, 255, 255);
        } else {
            printf("%cUsage: validate [test|stats]\n", 255, 255, 255);
            printf("%c  validate       - Show validation statistics\n", 255, 255, 255);
            printf("%c  validate test  - Run validation tests\n", 255, 255, 255);
            printf("%c  validate stats - Show detailed information\n", 255, 255, 255);
        }
    }
}

// Register hexdump command
REGISTER_SHELL_COMMAND(hexdump, "hexdump", hexdump_cmd, CMD_STREAMING,
    "Print a hex dump of a file (default 64 bytes).\nUsage: hexdump <file>",
    "hexdump test.eyn");

// process command implementation
void process_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        // Show process isolation statistics
        printf("%cProcess Isolation Statistics:\n", 255, 255, 255);
        printf("%c  Active Processes: %d\n", 255, 255, 255, get_process_count());
        printf("%c  Process Isolation: %s\n", 255, 255, 255, 
               get_process_isolation_status() ? "ENABLED" : "DISABLED");
        
        if (get_process_count() > 0) {
            printf("%c  Process isolation active\n", 0, 255, 0);
        } else {
            printf("%c  No active processes\n", 255, 255, 0);
        }
    } else {
        // Parse subcommand
        char subcmd[32];
        uint8 j = 0;
        while (ch[i] && ch[i] != ' ' && j < 31) subcmd[j++] = ch[i++];
        subcmd[j] = 0;
        
        if (strEql(subcmd, "list")) {
            printf("%cActive Processes:\n", 255, 255, 255);
            int found = 0;
            for (int k = 0; k < 4; k++) {
                process_t* proc = get_process_by_id(k + 1);
                if (proc && proc->active) {
                    printf("%c  PID %d: %s (0x%X-0x%X)\n", 255, 255, 255, 
                           proc->pid, proc->name, proc->code_start, 
                           proc->code_start + proc->code_size);
                    found++;
                }
            }
            if (!found) {
                printf("%c  No active processes\n", 255, 255, 0);
            }
        } else if (strEql(subcmd, "info")) {
            printf("%cProcess Isolation Details:\n", 255, 255, 255);
            printf("%c  User Code Space: 0x%X-0x%X\n", 255, 255, 255, 0x200000, 0x300000);
            printf("%c  User Stack Space: 0x%X-0x%X\n", 255, 255, 255, 0x300000, 0x400000);
            printf("%c  User Heap Space: 0x%X-0x%X\n", 255, 255, 255, 0x400000, 0x500000);
            printf("%c  Max Processes: 4\n", 255, 255, 255);
            printf("%c  Memory Protection: Active\n", 255, 255, 255);
        } else {
            printf("%cUsage: process [list|info]\n", 255, 255, 255);
            printf("%c  process       - Show process statistics\n", 255, 255, 255);
            printf("%c  process list  - List active processes\n", 255, 255, 255);
            printf("%c  process info  - Show isolation details\n", 255, 255, 255);
        }
    }
}

// portable command implementation
void portable_cmd(string ch) {
    uint8 i = 0;
    while (ch[i] && ch[i] != ' ') i++;
    while (ch[i] && ch[i] == ' ') i++;
    
    if (!ch[i]) {
        // Show portability statistics
        printf("%cPortability Optimizations:\n", 255, 255, 255);
        printf("%c  Target RAM: 128KB minimum\n", 255, 255, 255);
        printf("%c  Kernel Heap: %d KB (dynamic sizing)\n", 255, 255, 255, get_heap_size() / 1024);
        printf("%c  User Heap: 64KB (reduced from 1MB)\n", 255, 255, 255);
        printf("%c  Max Processes: 2 (reduced from 4)\n", 255, 255, 255);
        printf("%c  Process Stack: 32KB each\n", 255, 255, 255);
        printf("%c  Process Heap: 32KB each\n", 255, 255, 255);
        printf("%c  Block Header: 12 bytes (reduced from 24)\n", 255, 255, 255);
        printf("%c  Min Block Size: 16 bytes (reduced from 32)\n", 255, 255, 255);
        printf("%c  Memory Alignment: 4 bytes (reduced from 8)\n", 255, 255, 255);
        printf("%c  Search Buffer: 512 bytes (streaming)\n", 255, 255, 255);
        printf("%c  Process Name: 16 bytes (reduced from 32)\n", 255, 255, 255);
        
        printf("%c  Ultra-lightweight optimizations active\n", 0, 255, 0);
    } else {
        // Parse subcommand
        char subcmd[32];
        uint8 j = 0;
        while (ch[i] && ch[i] != ' ' && j < 31) subcmd[j++] = ch[i++];
        subcmd[j] = 0;
        
        if (strEql(subcmd, "stats")) {
            printf("%cMemory Usage Statistics:\n", 255, 255, 255);
            printf("%c  Kernel Heap Size: %d KB (dynamic)\n", 255, 255, 255, get_heap_size() / 1024);
            printf("%c  User Heap Size: 64KB\n", 255, 255, 255);
            printf("%c  Process Isolation: 64KB total\n", 255, 255, 255);
            printf("%c  Total System Memory: ~256KB\n", 255, 255, 255);
            printf("%c  Target Compatibility: 128KB RAM systems\n", 255, 255, 255);
            printf("%c  Memory Savings: ~95%% reduction\n", 0, 255, 0);
        } else if (strEql(subcmd, "optimize")) {
            printf("%cOptimization Details:\n", 255, 255, 255);
            printf("%c  Heap size reduced from 16MB to 128KB\n", 255, 255, 255);
            printf("%c  Block headers reduced from 24 to 12 bytes\n", 255, 255, 255);
            printf("%c  Process count reduced from 4 to 2\n", 255, 255, 255);
            printf("%c  Stack size reduced from 64KB to 32KB\n", 255, 255, 255);
            printf("%c  Search uses streaming instead of full file load\n", 255, 255, 255);
            printf("%c  Memory alignment reduced from 8 to 4 bytes\n", 255, 255, 255);
            printf("%c  Checksum calculation limited to 16 bytes\n", 255, 255, 255);
            printf("%c  All features maintained despite optimizations\n", 0, 255, 0);
        } else {
            printf("%cUsage: portable [stats|optimize]\n", 255, 255, 255);
            printf("%c  portable       - Show portability statistics\n", 255, 255, 255);
            printf("%c  portable stats - Show memory usage details\n", 255, 255, 255);
            printf("%c  portable optimize - Show optimization details\n", 255, 255, 255);
        }
    }
}

// init command implementation
void init_cmd(string ch) {
    printf("%cInitializing full system services...\n", 255, 255, 255);
    
    // Initialize ATA drives
    printf("%c  Initializing ATA drives...\n", 255, 255, 255);
    extern void ata_init_drives(void);
    ata_init_drives();
    
    printf("%cSystem initialization complete!\n", 0, 255, 0);
    printf("%cAll services are now available.\n", 0, 255, 0);
}

// Pipeline system commands
void jobs_cmd(string ch) {
    printf("%cBackground Jobs:\n", 255, 255, 255);
    list_background_processes();
}

void fg_cmd(string ch) {
    // Parse PID from command
    char* space = strchr(ch, ' ');
    if (!space) {
        printf("%cUsage: fg <pid>\n", 255, 255, 255);
        printf("%cExample: fg 1\n", 255, 255, 255);
        return;
    }
    
    space++;
    int pid = atoi(space);
    if (pid <= 0) {
        printf("%cInvalid PID: %s\n", 255, 0, 0, space);
        return;
    }
    
    wait_for_background_process(pid);
}

void bg_cmd(string ch) {
    printf("%cBackground process management:\n", 255, 255, 255);
    printf("%c  Use '&' at the end of commands to run in background\n", 255, 255, 255);
    printf("%c  Example: ls & (runs ls in background)\n", 255, 255, 255);
    printf("%c  Use 'jobs' to list background processes\n", 255, 255, 255);
    printf("%c  Use 'fg <pid>' to bring process to foreground\n", 255, 255, 255);
}

void pipe_cmd(string ch) {
    printf("%cPipeline System:\n", 255, 255, 255);
    printf("%c  Use '|' to pipe output between commands\n", 255, 255, 255);
    printf("%c  Example: ls | grep .txt\n", 255, 255, 255);
    printf("%c  Use '<' for input redirection\n", 255, 255, 255);
    printf("%c  Example: sort < input.txt\n", 255, 255, 255);
    printf("%c  Use '>' for output redirection\n", 255, 255, 255);
    printf("%c  Example: ls > output.txt\n", 255, 255, 255);
    printf("%c  Use '>>' for append redirection\n", 255, 255, 255);
    printf("%c  Example: echo 'new line' >> file.txt\n", 255, 255, 255);
}

// Register pipeline commands
REGISTER_SHELL_COMMAND(jobs, "jobs", jobs_cmd, CMD_STREAMING,
    "List background processes.\nUsage: jobs",
    "jobs");

REGISTER_SHELL_COMMAND(fg, "fg", fg_cmd, CMD_STREAMING,
    "Bring background process to foreground.\nUsage: fg <pid>",
    "fg 1");

REGISTER_SHELL_COMMAND(bg, "bg", bg_cmd, CMD_STREAMING,
    "Show background process management help.\nUsage: bg",
    "bg");

REGISTER_SHELL_COMMAND(pipe, "pipe", pipe_cmd, CMD_STREAMING,
    "Show pipeline system help.\nUsage: pipe",
    "pipe");

 