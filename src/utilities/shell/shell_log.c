#include <utilities/shell/shell_log.h>

#include <fs/vfs.h>
#include <misc/printf.h>

#include <string.h>
#include <stdlib.h>

// Public globals (compat)
char* shell_log_buf = NULL;
int shell_log_buf_size = 0;
int shell_log_pos = 0;
int shell_log_line_count = 0;
int shell_log_line_starts[1001];
int shell_log_current_line_start = 0;
int shell_log_active = 0;

static int g_shell_log_in_flush = 0;
static int g_shell_log_write_failed = 0;

void shell_log_enable(void) {
    g_shell_log_write_failed = 0;
    shell_log_active = 1;
}
void shell_log_disable(void) { shell_log_active = 0; }

void init_dynamic_log_buffer(void) {
    if (shell_log_buf != NULL) return;

    // Conservative defaults; keep memory bounded on small-RAM configs.
    int want = 16384;
#if defined(__i386__) || defined(__x86_64__)
    // i386 build often has more RAM; allow a larger buffer if it allocates.
    want = 32768;
#endif

    if (want > LOG_BUF_SIZE) want = LOG_BUF_SIZE;
    if (want < 4096) want = 4096;

    shell_log_buf = (char*)malloc((size_t)want);
    if (!shell_log_buf) {
        shell_log_buf_size = 0;
        return;
    }

    shell_log_buf_size = want;
    shell_log_pos = 0;
    shell_log_line_count = 0;
    shell_log_current_line_start = 0;
    shell_log_buf[0] = '\0';
}

static void shell_log_warn(const char* msg) {
    // Avoid recursion: temporarily disable logging while printing warnings.
    int was_active = shell_log_active;
    shell_log_active = 0;
    printf("%cWarning: %s\n", 255, 165, 0, msg);
    shell_log_active = was_active;
}

void shell_log_flush(void) {
    if (g_shell_log_write_failed) {
        // Persistent failure (e.g. read-only disk). Avoid spamming warnings.
        return;
    }
    if (g_shell_log_in_flush) return;
    g_shell_log_in_flush = 1;

    init_dynamic_log_buffer();
    if (!shell_log_buf || shell_log_pos <= 0) {
        g_shell_log_in_flush = 0;
        return;
    }

    // Write to a fixed path. This matches the historical i386 behavior (root log file).
    const char* path = "/log";

    // Bound I/O and allocations to avoid OOM.
    const uint32 max_total = 8192;
    uint32 old_size = 0;

    vfs_stat_t st;
    int has_old = (vfs_stat(0, path, &st) == 0 && st.type == VFS_NODE_FILE);
    if (has_old) {
        if (vfs_get_file_size(0, path, &old_size) != 0) {
            shell_log_warn("Failed to stat log file");
            g_shell_log_in_flush = 0;
            return;
        }
        if (old_size > max_total) {
            // Too large to safely append; truncate by ignoring old content.
            old_size = 0;
        }
    }

    uint32 new_len = (uint32)shell_log_pos;
    if (new_len > max_total) new_len = max_total;

    // Keep at least half budget for new data.
    if (old_size > max_total / 2) old_size = max_total / 2;
    if (new_len > max_total - old_size) new_len = max_total - old_size;

    uint32 total = old_size + new_len;
    char* tmp = (char*)malloc((size_t)total);
    if (!tmp) {
        shell_log_warn("Out of memory during log flush");
        g_shell_log_in_flush = 0;
        return;
    }

    uint32 pos = 0;
    if (has_old && old_size > 0) {
        int br = vfs_read_file(0, path, tmp, (int)old_size);
        if (br < 0) br = 0;
        pos = (uint32)br;
        if (pos > old_size) pos = old_size;
    }

    if (new_len > 0) {
        memcpy(tmp + pos, shell_log_buf, (size_t)new_len);
        pos += new_len;
    }

    if (vfs_write_file(0, path, tmp, pos) < 0) {
        shell_log_warn("Failed to write /log (disk may be read-only)");
        // Disable logging to prevent repeated warnings on every newline.
        g_shell_log_write_failed = 1;
        shell_log_active = 0;
    }

    free(tmp);

    // Reset in-memory buffer after flush.
    shell_log_pos = 0;
    if (shell_log_buf) shell_log_buf[0] = '\0';
    shell_log_line_count = 0;
    shell_log_current_line_start = 0;

    g_shell_log_in_flush = 0;
}

void shell_log_append(const char* data, int len) {
    if (!shell_log_active) return;
    if (!data || len <= 0) return;
    if (g_shell_log_in_flush) return;

    init_dynamic_log_buffer();
    if (!shell_log_buf || shell_log_buf_size <= 1) return;

    for (int i = 0; i < len; ++i) {
        char c = data[i];

        if (shell_log_pos >= shell_log_buf_size - 1) {
            // If full, force a flush and continue.
            shell_log_buf[shell_log_pos] = '\0';
            shell_log_flush();
            if (!shell_log_buf || shell_log_buf_size <= 1) return;
        }

        if (shell_log_pos == 0 || shell_log_buf[shell_log_pos - 1] == '\n') {
            shell_log_current_line_start = shell_log_pos;
        }

        shell_log_buf[shell_log_pos++] = c;

        if (c == '\n') {
            // Record line start
            shell_log_line_starts[shell_log_line_count] = shell_log_current_line_start;
            shell_log_line_count++;

            // Keep only last 1000 lines (same semantics as the old VGA path).
            if (shell_log_line_count > 1000) {
                int first_line_start = shell_log_line_starts[1];
                int bytes_to_keep = shell_log_pos - first_line_start;
                if (bytes_to_keep > 0 && first_line_start < shell_log_pos) {
                    memmove(shell_log_buf, shell_log_buf + first_line_start, (size_t)bytes_to_keep);
                    shell_log_pos = bytes_to_keep;
                    for (int j = 0; j < 1000; j++) {
                        shell_log_line_starts[j] = shell_log_line_starts[j + 1] - first_line_start;
                    }
                    shell_log_line_count = 1000;
                }
            }

            shell_log_buf[shell_log_pos] = '\0';
            shell_log_flush();
        }
    }

    shell_log_buf[shell_log_pos] = '\0';
}
