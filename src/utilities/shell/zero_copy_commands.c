#include <zero_copy.h>
#include <shell_command_info.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <context.h>
#include <misc/sched.h>

// Command handlers for zero-copy operations
void zopen_cmd(string arg);
void zclose_cmd(string arg);
void zread_cmd(string arg);
void zwrite_cmd(string arg);
void zseek_cmd(string arg);
void zsplice_cmd(string arg);
void ztee_cmd(string arg);
void zstats_cmd(string arg);

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
void zopen_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zopen <filename> [r|w|rw]\n", 255, 255, 255);
        printf("%cExample: zopen test.txt r\n", 255, 255, 255);
        printf("%cExample: zopen data.bin rw\n", 255, 255, 255);
        return;
    }
    
    char filename[128];
    uint8 j = 0;
    while (arg[i] && arg[i] != ' ' && j < 127) {
        filename[j++] = arg[i++];
    }
    filename[j] = '\0';
    
    // Parse flags
    uint8_t flags = ZERO_COPY_READ_ONLY;  // Default to read-only
    while (arg[i] && arg[i] == ' ') i++;
    if (arg[i]) {
        if (strncmp(&arg[i], "w", 1) == 0) {
            flags = ZERO_COPY_WRITE_ONLY;
        } else if (strncmp(&arg[i], "rw", 2) == 0) {
            flags = ZERO_COPY_READ_WRITE;
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
void zclose_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zclose <fd>\n", 255, 255, 255);
        printf("%cExample: zclose 0\n", 255, 255, 255);
        return;
    }
    
    int fd = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd = fd * 10 + (arg[i] - '0');
        i++;
    }
    
    if (zero_copy_close(fd) == 0) {
        printf("%cZero-copy file closed: fd %d\n", 0, 255, 0, fd);
    } else {
        printf("%cFailed to close zero-copy file: fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy read command
void zread_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zread <fd> <count>\n", 255, 255, 255);
        printf("%cExample: zread 0 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    
    // Parse file descriptor
    int fd = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd = fd * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing count parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse count
    size_t count = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        count = count * 10 + (arg[i] - '0');
        i++;
    }
    
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
void zwrite_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zwrite <fd> <data>\n", 255, 255, 255);
        printf("%cExample: zwrite 0 Hello World\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    // Parse file descriptor
    int fd = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd = fd * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing data parameter\n", 255, 0, 0);
        return;
    }
    
    // Get data to write
    char data[256];
    uint8 j = 0;
    while (arg[i] && j < 255) {
        data[j++] = arg[i++];
    }
    data[j] = '\0';
    
    size_t bytes_written = zero_copy_write(fd, data, strlen(data));
    if (bytes_written > 0) {
        printf("%cWrote %d bytes to fd %d\n", 0, 255, 0, (int)bytes_written, fd);
    } else {
        printf("%cFailed to write to fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy seek command
void zseek_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zseek <fd> <offset> [set|cur|end]\n", 255, 255, 255);
        printf("%cExample: zseek 0 100 set\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS, SCHED_COST_FS)) return;
    
    // Parse file descriptor
    int fd = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd = fd * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing offset parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse offset
    int32_t offset = 0;
    int negative = 0;
    if (arg[i] == '-') {
        negative = 1;
        i++;
    }
    while (arg[i] >= '0' && arg[i] <= '9') {
        offset = offset * 10 + (arg[i] - '0');
        i++;
    }
    if (negative) offset = -offset;
    
    // Parse whence
    int whence = 0;  // Default to SEEK_SET
    while (arg[i] && arg[i] == ' ') i++;
    if (arg[i]) {
        if (strncmp(&arg[i], "cur", 3) == 0) {
            whence = 1;  // SEEK_CUR
        } else if (strncmp(&arg[i], "end", 3) == 0) {
            whence = 2;  // SEEK_END
        }
    }
    
    if (zero_copy_seek(fd, offset, whence) == 0) {
        printf("%cSeek successful on fd %d\n", 0, 255, 0, fd);
    } else {
        printf("%cSeek failed on fd %d\n", 255, 0, 0, fd);
    }
}

// Zero-copy splice command
void zsplice_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: zsplice <fd_in> <fd_out> <count>\n", 255, 255, 255);
        printf("%cExample: zsplice 0 1 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    // Parse input file descriptor
    int fd_in = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd_in = fd_in * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing output fd parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse output file descriptor
    int fd_out = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd_out = fd_out * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing count parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse count
    size_t count = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        count = count * 10 + (arg[i] - '0');
        i++;
    }
    
    int bytes_spliced = zero_copy_splice(fd_in, fd_out, count);
    if (bytes_spliced > 0) {
        printf("%cSpliced %d bytes from fd %d to fd %d\n", 0, 255, 0, bytes_spliced, fd_in, fd_out);
    } else {
        printf("%cSplice failed\n", 255, 0, 0);
    }
}

// Zero-copy tee command
void ztee_cmd(string arg) {
    uint8 i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cUsage: ztee <fd_in> <fd_out> <count>\n", 255, 255, 255);
        printf("%cExample: ztee 0 1 100\n", 255, 255, 255);
        return;
    }

    if (!zc_ctx_allow(CAP_READ_FS | CAP_WRITE_FS, SCHED_COST_FS)) return;
    
    // Parse input file descriptor
    int fd_in = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd_in = fd_in * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing output fd parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse output file descriptor
    int fd_out = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        fd_out = fd_out * 10 + (arg[i] - '0');
        i++;
    }
    
    while (arg[i] && arg[i] == ' ') i++;
    
    if (!arg[i]) {
        printf("%cError: Missing count parameter\n", 255, 0, 0);
        return;
    }
    
    // Parse count
    size_t count = 0;
    while (arg[i] >= '0' && arg[i] <= '9') {
        count = count * 10 + (arg[i] - '0');
        i++;
    }
    
    int bytes_teed = zero_copy_tee(fd_in, fd_out, count);
    if (bytes_teed > 0) {
        printf("%cTee'd %d bytes from fd %d to fd %d\n", 0, 255, 0, bytes_teed, fd_in, fd_out);
    } else {
        printf("%cTee failed\n", 255, 0, 0);
    }
}

// Zero-copy statistics command
void zstats_cmd(string arg) {
    print_zero_copy_stats();
} 