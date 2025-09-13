#include <shell_script.h>
#include <eynfs.h>
#include <util.h>
#include <string.h>
#include <vga.h>
#include <shell.h>
#include <shell_command_info.h>
#include <types.h>

// EYNFS constants
#define EYNFS_SUPERBLOCK_LBA 2048

// Shell script execution context
typedef struct {
    char script_path[256];
    char current_line[512];
    int line_number;
    int error_count;
    int success_count;
} shell_script_context_t;

// Forward declarations
static int execute_shell_line(const char* line, shell_script_context_t* ctx);
static int parse_shell_file(const char* filename, shell_script_context_t* ctx);
static void trim_whitespace(char* str);
static int is_comment_line(const char* line);
static int is_empty_line(const char* line);

// Execute a shell script file
exec_result_t execute_shell_script(const char* filename) {
    shell_script_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    // copy filename to context
    safe_strcpy(ctx.script_path, filename, sizeof(ctx.script_path));
    ctx.line_number = 0;
    ctx.error_count = 0;
    ctx.success_count = 0;
    
    printf("%cExecuting shell script: %s\n", 0, 255, 0, filename);
    
    // parse and execute the shell file
    int result = parse_shell_file(filename, &ctx);
    
    // print execution summary
    printf("%cShell script execution completed:\n", 255, 255, 255);
    printf("%c  Lines processed: %d\n", 255, 255, 255, ctx.line_number);
    printf("%c  Commands successful: %d\n", 0, 255, 0, ctx.success_count);
    if (ctx.error_count > 0) {
        printf("%c  Commands failed: %d\n", 255, 0, 0, ctx.error_count);
    }
    
    return (result == 0) ? EXEC_SUCCESS : EXEC_ERROR_EXECUTION_FAILED;
}

// Parse and execute a shell script file
static int parse_shell_file(const char* filename, shell_script_context_t* ctx) {
    // read EYNFS superblock
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, EYNFS_SUPERBLOCK_LBA, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cError: No supported filesystem found\n", 255, 0, 0);
        return -1;
    }
    
    // find the shell script file
    eynfs_dir_entry_t entry;
    if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, filename, &entry, 0) != 0) {
        printf("%cError: Shell script file not found: %s\n", 255, 0, 0, filename);
        return -1;
    }
    
    // check if it's a .shell file
    const char* ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".shell") != 0) {
        printf("%cError: File must have .shell extension: %s\n", 255, 0, 0, filename);
        return -1;
    }
    
    // read the shell script file
    uint32_t size = entry.size;
    char* script_content = (char*)malloc(size + 1);
    if (!script_content) {
        printf("%cError: Memory allocation failed\n", 255, 0, 0);
        return -1;
    }
    
    int bytes_read = eynfs_read_file(0, &sb, &entry, script_content, size, 0);
    if (bytes_read < 0) {
        printf("%cError: Failed to read shell script file\n", 255, 0, 0);
        free(script_content);
        return -1;
    }
    
    script_content[bytes_read] = '\0';
    
    // parse and execute each line
    char* line_start = script_content;
    char* line_end;
    
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        // extract line
        size_t line_len = line_end - line_start;
        if (line_len >= sizeof(ctx->current_line)) {
            line_len = sizeof(ctx->current_line) - 1;
        }
        
        memcpy(ctx->current_line, line_start, line_len);
        ctx->current_line[line_len] = '\0';
        ctx->line_number++;
        
        // trim whitespace
        trim_whitespace(ctx->current_line);
        
        // skip empty lines and comments
        if (!is_empty_line(ctx->current_line) && !is_comment_line(ctx->current_line)) {
            printf("%c[%d] %s\n", 200, 200, 200, ctx->line_number, ctx->current_line);
            
            // execute the line
            if (execute_shell_line(ctx->current_line, ctx) == 0) {
                ctx->success_count++;
            } else {
                ctx->error_count++;
                printf("%cError on line %d: %s\n", 255, 0, 0, ctx->line_number, ctx->current_line);
            }
        }
        
        line_start = line_end + 1;
    }
    
    // handle last line if it doesn't end with newline
    if (*line_start != '\0') {
        size_t line_len = strlen(line_start);
        if (line_len >= sizeof(ctx->current_line)) {
            line_len = sizeof(ctx->current_line) - 1;
        }
        
        memcpy(ctx->current_line, line_start, line_len);
        ctx->current_line[line_len] = '\0';
        ctx->line_number++;
        
        trim_whitespace(ctx->current_line);
        
        if (!is_empty_line(ctx->current_line) && !is_comment_line(ctx->current_line)) {
            printf("%c[%d] %s\n", 200, 200, 200, ctx->line_number, ctx->current_line);
            
            if (execute_shell_line(ctx->current_line, ctx) == 0) {
                ctx->success_count++;
            } else {
                ctx->error_count++;
                printf("%cError on line %d: %s\n", 255, 0, 0, ctx->line_number, ctx->current_line);
            }
        }
    }
    
    free(script_content);
    return 0;
}

// Execute a single shell command line
static int execute_shell_line(const char* line, shell_script_context_t* ctx) {
    if (!line || strlen(line) == 0) {
        return 0; // empty line is success
    }
    
    // parse command and arguments
    char cmd[256];
    char args[512];
    
    // find first space to separate command from arguments
    int i = 0;
    while (line[i] && line[i] != ' ' && i < 255) {
        cmd[i] = line[i];
        i++;
    }
    cmd[i] = '\0';
    
    // skip spaces after command
    while (line[i] && line[i] == ' ') i++;
    
    // copy remaining arguments
    if (line[i]) {
        safe_strcpy(args, &line[i], sizeof(args));
    } else {
        args[0] = '\0';
    }
    
    // find the command handler
    shell_cmd_handler_t handler = find_command(cmd);
    if (!handler) {
        printf("%cUnknown command: %s\n", 255, 0, 0, cmd);
        return -1;
    }
    
    // execute the command
    // we need to reconstruct the full command line for the handler
    char full_cmd[512];
    if (strlen(args) > 0) {
        snprintf(full_cmd, sizeof(full_cmd), "%s %s", cmd, args);
    } else {
        safe_strcpy(full_cmd, cmd, sizeof(full_cmd));
    }
    
    // execute the command handler
    handler(full_cmd);
    
    return 0; // assume success for now
}

// trim whitespace from string
static void trim_whitespace(char* str) {
    if (!str) return;
    
    // trim leading whitespace
    char* start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\r')) {
        start++;
    }
    
    // trim trailing whitespace
    char* end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end--;
    }
    
    // move trimmed string to beginning
    if (start != str) {
        memmove(str, start, end - start + 1);
    }
    str[end - start + 1] = '\0';
}

// check if line is a comment (starts with #)
static int is_comment_line(const char* line) {
    if (!line) return 0;
    
    // skip leading whitespace
    while (*line && (*line == ' ' || *line == '\t')) {
        line++;
    }
    
    return (*line == '#');
}

// check if line is empty (only whitespace)
static int is_empty_line(const char* line) {
    if (!line) return 1;
    
    while (*line) {
        if (*line != ' ' && *line != '\t' && *line != '\r' && *line != '\n') {
            return 0;
        }
        line++;
    }
    
    return 1;
}
