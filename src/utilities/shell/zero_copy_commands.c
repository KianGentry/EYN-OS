#include <zero_copy.h>
#include <shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <context.h>
#include <misc/sched.h>

// Command handlers for zero-copy operations
void zopen_cmd(const shell_args_t* args);
void zclose_cmd(const shell_args_t* args);
void zread_cmd(const shell_args_t* args);
void zwrite_cmd(const shell_args_t* args);
void zseek_cmd(const shell_args_t* args);
void zsplice_cmd(const shell_args_t* args);
void ztee_cmd(const shell_args_t* args);
void zstats_cmd(const shell_args_t* args);

static int zc_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Register commands with the shell system
REGISTER_SHELL_COMMAND(zopen_cmd_info, "zopen", zopen_cmd, CMD_STREAMING,
                       "Open file with zero-copy operations", "zopen <filename> [r|w|rw]");

REGISTER_SHELL_COMMAND(zclose_cmd_info, "zclose", zclose_cmd, CMD_STREAMING,
                       "Close zero-copy file", "zclose <fd>");

REGISTER_SHELL_COMMAND(zread_cmd_info, "zread", zread_cmd, CMD_STREAMING,
                       "Read from zero-copy file", "zread <fd> <count>");

REGISTER_SHELL_COMMAND(zwrite_cmd_info, "zwrite", zwrite_cmd, CMD_STREAMING,
                       "Write to zero-copy file", "zwrite <fd> <data>");

REGISTER_SHELL_COMMAND(zseek_cmd_info, "zseek", zseek_cmd, CMD_STREAMING,
                       "Seek in zero-copy file", "zseek <fd> <offset> [set|cur|end]");


REGISTER_SHELL_COMMAND(zsplice_cmd_info, "zsplice", zsplice_cmd, CMD_STREAMING,
                       "Splice data between files", "zsplice <fd_in> <fd_out> <count>");

REGISTER_SHELL_COMMAND(ztee_cmd_info, "ztee", ztee_cmd, CMD_STREAMING,
                       "Tee data between files", "ztee <fd_in> <fd_out> <count>");

REGISTER_SHELL_COMMAND(zstats_cmd_info, "zstats", zstats_cmd, CMD_STREAMING,
                       "Show zero-copy statistics", "zstats");

// Zero-copy open command
void zopen_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2) {
        printf("%cUsage: zopen <filename> [r|w|rw]\n", 255, 255, 255);
        printf("%cExample: zopen test.txt r\n", 255, 255, 255);
        printf("%cExample: zopen data.bin rw\n", 255, 255, 255);
        return;
    }

    const char* filename = args->argv[1];
    
    // Parse flags
    uint8_t flags = ZERO_COPY_READ_ONLY;  // Default to read-only
    if (args->argc >= 3) {
        const char* mode = args->argv[2];
        if (mode && strncmp(mode, "rw", 2) == 0) {
            flags = ZERO_COPY_READ_WRITE;
        } else if (mode && strncmp(mode, "w", 1) == 0) {
            flags = ZERO_COPY_WRITE_ONLY;
        }
    }

    uint32 caps = CAP_READ_FS;
    if (flags == ZERO_COPY_WRITE_ONLY) caps = CAP_WRITE_FS;
    if (flags == ZERO_COPY_READ_WRITE) caps = CAP_READ_FS | CAP_WRITE_FS;
    if (!zc_ctx_allow(caps, SCHED_COST_FS)) return;
    
    int fd = zero_copy_open(filename, flags);
    if (fd >= 0) {
        printf("%cZero-copy file opened with fd: %d\n", 0, 255, 0, fd);
    } else {
        printf("%cFailed to open zero-copy file: %s\n", 255, 0, 0, filename);
    }
}

// Zero-copy close command
void zclose_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2) {
        printf("%cUsage: zclose <fd>\n", 255, 255, 255);
        printf("%cExample: zclose 0\n", 255, 255, 255);
        return;
    }

    int fd = atoi(args->argv[1]);
    
    if (zero_copy_close(fd) == 0) {
        printf("%cZero-copy file closed: fd %d\n", 0, 255, 0, fd);
    } else {
        printf("%cFailed to close zero-copy file: fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy read command
void zread_cmd(const shell_args_t* args) {
    if (!args || args->argc < 3) {
        printf("%cUsage: zread <fd> <count>\n", 255, 255, 255);
        printf("%cExample: zread 0 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return;
    
    int fd = atoi(args->argv[1]);
    size_t count = (size_t)atoi(args->argv[2]);
    
    // Allocate buffer and read
    char* buffer = (char*)malloc(count + 1);
    if (!buffer) {
        printf("%cError: Out of memory for read buffer\n", 255, 0, 0);
        return;
    }
    
    size_t bytes_read = zero_copy_read(fd, buffer, count);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("%cRead %d bytes from fd %d:\n", 0, 255, 0, (int)bytes_read, fd);
        printf("%c%s\n", 255, 255, 255, buffer);
    } else {
        printf("%cFailed to read from fd %d\n", 255, 0, 0, fd);
    }
    
    free(buffer);
}

// Zero-copy write command
void zwrite_cmd(const shell_args_t* args) {
    if (!args || args->argc < 3) {
        printf("%cUsage: zwrite <fd> <data>\n", 255, 255, 255);
        printf("%cExample: zwrite 0 Hello World\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    int fd = atoi(args->argv[1]);
    const char* data = shell_args_rest_raw(args, 2);
    if (!data || !data[0]) {
        printf("%cError: Missing data parameter\n", 255, 0, 0);
        return;
    }
    size_t bytes_written = zero_copy_write(fd, data, strlen(data));
    if (bytes_written > 0) {
        printf("%cWrote %d bytes to fd %d\n", 0, 255, 0, (int)bytes_written, fd);
    } else {
        printf("%cFailed to write to fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy seek command
void zseek_cmd(const shell_args_t* args) {
    if (!args || args->argc < 3) {
        printf("%cUsage: zseek <fd> <offset> [set|cur|end]\n", 255, 255, 255);
        printf("%cExample: zseek 0 100 set\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    
    int fd = atoi(args->argv[1]);
    int32_t offset = (int32_t)atoi(args->argv[2]);
    int whence = 0;
    if (args->argc >= 4) {
        const char* w = args->argv[3];
        if (w && strcmp(w, "cur") == 0) whence = 1;
        else if (w && strcmp(w, "end") == 0) whence = 2;
    }
    
    if (zero_copy_seek(fd, offset, whence) == 0) {
        printf("%cSeek successful on fd %d\n", 0, 255, 0, fd);
    } else {
        printf("%cSeek failed on fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy splice command
void zsplice_cmd(const shell_args_t* args) {
    if (!args || args->argc < 4) {
        printf("%cUsage: zsplice <fd_in> <fd_out> <count>\n", 255, 255, 255);
        printf("%cExample: zsplice 0 1 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    int fd_in = atoi(args->argv[1]);
    int fd_out = atoi(args->argv[2]);
    size_t count = (size_t)atoi(args->argv[3]);
    
    int bytes_spliced = zero_copy_splice(fd_in, fd_out, count);
    if (bytes_spliced > 0) {
        printf("%cSpliced %d bytes from fd %d to fd %d\n", 0, 255, 0, bytes_spliced, fd_in, fd_out);
    } else {
        printf("%cSplice failed\n", 255, 0, 0);
    }
}

// Zero-copy tee command
void ztee_cmd(const shell_args_t* args) {
    if (!args || args->argc < 4) {
        printf("%cUsage: ztee <fd_in> <fd_out> <count>\n", 255, 255, 255);
        printf("%cExample: ztee 0 1 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    int fd_in = atoi(args->argv[1]);
    int fd_out = atoi(args->argv[2]);
    size_t count = (size_t)atoi(args->argv[3]);
    
    int bytes_teed = zero_copy_tee(fd_in, fd_out, count);
    if (bytes_teed > 0) {
        printf("%cTee'd %d bytes from fd %d to fd %d\n", 0, 255, 0, bytes_teed, fd_in, fd_out);
    } else {
        printf("%cTee failed\n", 255, 0, 0);
    }
}

// Zero-copy statistics command
void zstats_cmd(const shell_args_t* args) {
    (void)args;
    print_zero_copy_stats();
} 