
#include <utilities/shell/fs_commands.h>
#include <utilities/shell/shell_redirect.h>

#include <fs/vfs.h>

#include <string.h>

/*
 * Shared helpers used by shell redirection/pipelines.
 *
 * Keep memory bounded: AArch64 QEMU virt defaults to ~8MB RAM, and i386 is
 * commonly run with small -m values too.
 */

int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
    if (!filename) return -1;
    if (!buf) buf = "";
    if (len < 0) len = 0;

    char abspath[128];
    resolve_path(filename, shell_current_path, abspath, sizeof(abspath));

    int wr = vfs_write_file(disk, abspath, buf, (uint32)len);
    return (wr >= 0) ? 0 : -1;
}

int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
    if (!filename) return -1;
    if (!buf) buf = "";
    if (len < 0) len = 0;

    char abspath[128];
    resolve_path(filename, shell_current_path, abspath, sizeof(abspath));

    vfs_stat_t st;
    uint32 old_size = 0;

    int has_old = (vfs_stat(disk, abspath, &st) == 0 && st.type == VFS_NODE_FILE);
    if (has_old) {
        if (vfs_get_file_size(disk, abspath, &old_size) != 0) return -1;
        if (old_size > 4096u) return -1;
    }

    static char tmp[4096 + SHELL_REDIRECT_BUF_SIZE];
    uint32 pos = 0;

    if (has_old && old_size > 0) {
        int br = vfs_read_file(disk, abspath, tmp, (int)old_size);
        if (br < 0) return -1;
        pos = (uint32)br;
    }

    if ((uint32)len > (uint32)sizeof(tmp) - pos) return -1;
    memcpy(tmp + pos, buf, (size_t)len);
    pos += (uint32)len;

    int wr = vfs_write_file(disk, abspath, tmp, pos);
    return (wr >= 0) ? 0 : -1;
}
