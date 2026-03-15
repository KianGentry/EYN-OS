#include <fcntl.h>
#include <dirent.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("EYN-OS graphical installer.", "installer");

#define MAX_DRIVES 8
/*
 * LOW-MEM INVARIANT: Keep installer copy reads small.
 *
 * Why: RAM:/ media is read through VFS demand paths under tight memory.
 * Smaller reads reduce transient kernel buffer pressure and avoid wide
 * multi-block reads that are more failure-prone in installer mode.
 */
#define COPY_BUF_SZ 256
#define MAX_PATH_LEN 256
#define MAX_STATUS 128

#define PAYLOAD_MAGIC "EYNPKG1\0"
#define PAYLOAD_MAGIC_LEGACY "EYNPKG1"
#define PAYLOAD_TYPE_FILE 1u
#define PAYLOAD_TYPE_DIR 2u
#define PAYLOAD_FLAG_RLE 1u
#define PAYLOAD_DIR_PROGRESS_UNITS 1024u

/*
 * ABI-INVARIANT: Raw on-disk LBA used for installer-written kernel payload.
 *
 * Why: GRUB core image embedded by installer boots via blocklist
 * `multiboot (hd0)+KERNEL_RAW_LBA,<sectors>` to avoid filesystem module
 * dependencies during early boot.
 * Invariant: Must stay below first partition start (currently 2048).
 */
#define KERNEL_RAW_LBA 1024u

typedef enum {
    STEP_WELCOME = 0,
    STEP_SELECT_DRIVE = 1,
    STEP_FORMAT = 2,
    STEP_COPY = 3,
    STEP_BOOTLOADER = 4,
    STEP_DONE = 5,
    STEP_ERROR = 6,
} installer_step_t;

typedef struct {
    int drive_count;
    int drives[MAX_DRIVES];
    int selected_idx;
    int selected_drive;

    installer_step_t step;
    int running;

    int copied_files;
    int copied_dirs;
    int progress_permille;
    int progress_total;
    int progress_done;
    int progress_uses_work_units;
    uint32_t progress_work_total;
    uint32_t progress_work_done;
    char status[MAX_STATUS];
    char current_item[160];
    char warning[MAX_STATUS];
    char error[MAX_STATUS];
} installer_t;

static int g_installer_gui_handle = -1;
static int g_gui_draw_failures = 0;
static void draw_ui(int h, installer_t* s);
static int read_exact_fd(int fd, void* buf, int len);

static int gui_call_ok(int rc, const char* what, installer_t* s) {
    if (rc >= 0) return 1;
    g_gui_draw_failures++;
    if (g_gui_draw_failures <= 16 || (g_gui_draw_failures % 64) == 0) {
        int step = s ? (int)s->step : -1;
        printf("[installer] gui call failed: %s rc=%d step=%d failures=%d\n",
               what ? what : "?", rc, step, g_gui_draw_failures);
    }
    return 0;
}

static void installer_ui_pulse(installer_t* s) {
    if (!s) return;
    if (g_installer_gui_handle >= 0) {
        draw_ui(g_installer_gui_handle, s);
    }
}

static int clamp_permille(int v) {
    if (v < 0) return 0;
    if (v > 1000) return 1000;
    return v;
}

static void progress_reset(installer_t* s, int total) {
    if (!s) return;
    s->progress_total = (total > 0) ? total : 0;
    s->progress_done = 0;
    s->progress_uses_work_units = 0;
    s->progress_work_total = 0;
    s->progress_work_done = 0;
    s->progress_permille = 0;
    s->current_item[0] = '\0';
    installer_ui_pulse(s);
}

static void progress_work_reset(installer_t* s, uint32_t total_work) {
    if (!s) return;
    s->progress_total = 0;
    s->progress_done = 0;
    s->progress_uses_work_units = 1;
    s->progress_work_total = (total_work > 0u) ? total_work : 1u;
    s->progress_work_done = 0;
    s->progress_permille = 0;
    s->current_item[0] = '\0';
    installer_ui_pulse(s);
}

static void progress_step(installer_t* s, const char* item) {
    if (!s) return;
    if (item) {
        strncpy(s->current_item, item, sizeof(s->current_item) - 1);
        s->current_item[sizeof(s->current_item) - 1] = '\0';
    }
    if (s->progress_total > 0) {
        s->progress_done++;
        if (s->progress_done > s->progress_total) s->progress_done = s->progress_total;
        s->progress_permille = clamp_permille((s->progress_done * 1000) / s->progress_total);
    }
    installer_ui_pulse(s);
}

static void progress_note_item(installer_t* s, const char* item) {
    if (!s) return;
    if (!item) item = "";
    strncpy(s->current_item, item, sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
}

static void progress_set_fraction(installer_t* s, uint32_t entry_done, uint32_t entry_total) {
    if (!s) return;
    if (s->progress_uses_work_units) {
        if (s->progress_work_total == 0u || entry_total == 0u) return;
        if (entry_done > entry_total) entry_done = entry_total;

        uint32_t in_flight = s->progress_work_done + entry_done;
        if (in_flight > s->progress_work_total) in_flight = s->progress_work_total;
        s->progress_permille = clamp_permille((int)((in_flight * 1000u) / s->progress_work_total));
        installer_ui_pulse(s);
        return;
    }

    if (s->progress_total <= 0) return;
    if (entry_total == 0u) return;

    if (entry_done > entry_total) entry_done = entry_total;

    /* Keep in-flight entry progress below the next completed-entry boundary. */
    uint32_t entry_permille = (entry_done * 1000u) / entry_total;
    if (entry_permille >= 1000u) entry_permille = 999u;

    int combined = (int)(((uint32_t)s->progress_done * 1000u + entry_permille) /
                         (uint32_t)s->progress_total);
    combined = clamp_permille(combined);
    if (combined > s->progress_permille) {
        s->progress_permille = combined;
    }
    installer_ui_pulse(s);
}

static void progress_work_advance(installer_t* s, uint32_t work_done) {
    if (!s || !s->progress_uses_work_units) return;
    s->progress_work_done += work_done;
    if (s->progress_work_done > s->progress_work_total) {
        s->progress_work_done = s->progress_work_total;
    }
    s->progress_permille = clamp_permille((int)((s->progress_work_done * 1000u) / s->progress_work_total));
    installer_ui_pulse(s);
}

static void status_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "";
    strncpy(s->status, msg, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = '\0';
    installer_ui_pulse(s);
}

static void error_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "Unknown error";
    strncpy(s->error, msg, sizeof(s->error) - 1);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static void error_set_path(installer_t* s, const char* prefix, const char* path) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s", prefix, path);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static void error_set_path_code(installer_t* s, const char* prefix, const char* path, int code) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s (rc=%d)", prefix, path, code);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
    installer_ui_pulse(s);
}

static int path_join(const char* base, const char* name, char* out, int out_cap) {
    if (!base || !name || !out || out_cap <= 0) return -1;
    if (strcmp(base, "/") == 0) {
        int n = snprintf(out, (size_t)out_cap, "/%s", name);
        return (n > 0 && n < out_cap) ? 0 : -1;
    }
    int n = snprintf(out, (size_t)out_cap, "%s/%s", base, name);
    return (n > 0 && n < out_cap) ? 0 : -1;
}

static int source_join(const char* rel, char* out, int out_cap) {
    if (!rel || !out || out_cap <= 0) return -1;
    if (strcmp(rel, "/") == 0) {
        int n = snprintf(out, (size_t)out_cap, "RAM:/");
        return (n > 0 && n < out_cap) ? 0 : -1;
    }
    int n = snprintf(out, (size_t)out_cap, "RAM:%s", rel);
    return (n > 0 && n < out_cap) ? 0 : -1;
}

static int count_tree_from_ram(const char* rel_src, int* out_entries) {
    if (!rel_src || !out_entries) return -1;

    char src_path[MAX_PATH_LEN];
    if (source_join(rel_src, src_path, sizeof(src_path)) != 0) return -1;

    int dfd = open(src_path, O_RDONLY, 0);
    if (dfd < 0) return -1;

    int total = 0;
    eyn_dirent_t entries[12];
    for (;;) {
        int bytes = getdents(dfd, entries, sizeof(entries));
        if (bytes < 0) {
            close(dfd);
            return -1;
        }
        if (bytes == 0) break;

        int count = bytes / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            const char* name = entries[i].name;
            if (!name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            total++;
            if (entries[i].is_dir) {
                char child_rel[MAX_PATH_LEN];
                if (path_join(rel_src, name, child_rel, sizeof(child_rel)) != 0) {
                    close(dfd);
                    return -1;
                }
                int sub = 0;
                if (count_tree_from_ram(child_rel, &sub) != 0) {
                    close(dfd);
                    return -1;
                }
                total += sub;
            }
        }
    }

    close(dfd);
    *out_entries = total;
    return 0;
}

static int payload_count_entries(const char* payload_path, int* out_entries, uint32_t* out_work_units) {
    if (!payload_path || !out_entries || !out_work_units) return -1;
    int fd = open(payload_path, O_RDONLY, 0);
    if (fd < 0) return -1;

    char first16[16];
    if (read_exact_fd(fd, first16, (int)sizeof(first16)) != 0) {
        close(fd);
        return -1;
    }

    int pos = -1;
    if (memcmp(first16, PAYLOAD_MAGIC, 8) == 0) pos = 8;
    else if (memcmp(first16, PAYLOAD_MAGIC_LEGACY, 7) == 0) pos = 7;
    else {
        for (int i = 0; i <= 9; ++i) {
            if (memcmp(first16 + i, PAYLOAD_MAGIC_LEGACY, 7) == 0) {
                pos = i + 7;
                break;
            }
        }
    }
    if (pos < 0) {
        close(fd);
        return -1;
    }

    uint8_t hdr[12];
    int count = 0;
    uint32_t total_work = 0;

    while (1) {
        int have = (int)sizeof(first16) - pos;
        if (have < 0) have = 0;
        if (have > (int)sizeof(hdr)) have = (int)sizeof(hdr);
        if (have > 0) memcpy(hdr, first16 + pos, (size_t)have);
        if (have < (int)sizeof(hdr)) {
            if (read_exact_fd(fd, hdr + have, (int)sizeof(hdr) - have) != 0) {
                close(fd);
                return -1;
            }
        }

        pos = (int)sizeof(first16);

        uint16_t path_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
        uint8_t etype = hdr[2];
        uint32_t orig_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                     ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        uint32_t stored_size = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                               ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

        if (path_len == 0 && etype == 0) break;
        count++;

        if (etype == PAYLOAD_TYPE_FILE) {
            uint32_t file_work = (stored_size > 0u) ? stored_size : ((orig_size > 0u) ? orig_size : 1u);
            total_work += file_work;
        } else if (etype == PAYLOAD_TYPE_DIR) {
            total_work += PAYLOAD_DIR_PROGRESS_UNITS;
        }

        if (lseek(fd, (long)path_len + (long)stored_size, SEEK_CUR) < 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    *out_entries = count;
    *out_work_units = (total_work > 0u) ? total_work : (uint32_t)count;
    return 0;
}

static int copy_file_stream_with_ui(installer_t* s, const char* src_ram_path, const char* dst_abs_path) {
    int fd = open(src_ram_path, O_RDONLY, 0);
    if (fd < 0) return -10;

    uint32_t src_size = 0;
    {
        int sz = lseek(fd, 0, SEEK_END);
        if (sz > 0 && lseek(fd, 0, SEEK_SET) >= 0) {
            src_size = (uint32_t)sz;
        }
    }

    int sh = eynfs_stream_begin(dst_abs_path);
    if (sh < 0) {
        close(fd);
        return sh;
    }

    char buf[COPY_BUF_SZ];
    int chunks_since_pulse = 0;
    uint32_t copied_bytes = 0;
    for (;;) {
        int n = (int)read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            (void)eynfs_stream_end(sh);
            return -30;
        }
        if (n == 0) break;
        int w = (int)eynfs_stream_write(sh, buf, (size_t)n);
        if (w != n) {
            close(fd);
            (void)eynfs_stream_end(sh);
            return -40;
        }
        copied_bytes += (uint32_t)n;
        chunks_since_pulse++;
        if (s && chunks_since_pulse >= 64) {
            if (src_size > 0u) progress_set_fraction(s, copied_bytes, src_size);
            else installer_ui_pulse(s);
            chunks_since_pulse = 0;
        }
    }

    if (s && src_size > 0u) {
        progress_set_fraction(s, copied_bytes, src_size);
    }

    if (close(fd) != 0) return -50;

    {
        int end_rc = eynfs_stream_end(sh);
        if (end_rc != 0) return -60;
    }
    return 0;
}

static int installer_target_preflight(installer_t* s) {
    const char* probe = "/.__install_probe";
    const char marker = 'X';

    int wr = writefile(probe, &marker, 1);
    if (wr < 0) {
        error_set(s, "Target write probe failed");
        return -1;
    }
    (void)unlink(probe);
    return 0;
}

static int installer_ram_preflight(installer_t* s) {
    int fd = open("RAM:/", O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "Could not open RAM:/ root");
        return -1;
    }

    eyn_dirent_t entries[16];
    int bytes = getdents(fd, entries, sizeof(entries));
    close(fd);

    if (bytes < 0) {
        error_set(s, "Could not list RAM:/ root");
        return -1;
    }
    if (bytes == 0) {
        error_set(s, "RAM:/ is empty (no files)");
        return -1;
    }

    fd = open("RAM:/binaries/installer", O_RDONLY, 0);
    if (fd < 0) {
        error_set(s, "RAM:/binaries/installer missing");
        return -1;
    }
    close(fd);
    return 0;
}

static int copy_tree_from_ram(installer_t* s, const char* rel_src, const char* dst_abs) {
    char src_path[MAX_PATH_LEN];
    if (source_join(rel_src, src_path, sizeof(src_path)) != 0) {
        error_set_path(s, "Path too long", rel_src);
        return -1;
    }

    int dfd = open(src_path, O_RDONLY, 0);
    if (dfd < 0) {
        error_set_path(s, "Failed opening RAM source", src_path);
        return -1;
    }

    eyn_dirent_t entries[12];
    for (;;) {
        int bytes = getdents(dfd, entries, sizeof(entries));
        if (bytes < 0) {
            close(dfd);
            error_set_path(s, "Failed listing directory", src_path);
            return -1;
        }
        if (bytes == 0) break;

        int count = bytes / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            const char* name = entries[i].name;
            if (!name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            char child_rel[MAX_PATH_LEN];
            char child_dst[MAX_PATH_LEN];
            char child_src_ram[MAX_PATH_LEN];

            if (path_join(rel_src, name, child_rel, sizeof(child_rel)) != 0) {
                close(dfd);
                error_set_path(s, "Path too long", name);
                return -1;
            }
            if (path_join(dst_abs, name, child_dst, sizeof(child_dst)) != 0) {
                close(dfd);
                error_set_path(s, "Target path too long", name);
                return -1;
            }
            if (source_join(child_rel, child_src_ram, sizeof(child_src_ram)) != 0) {
                close(dfd);
                error_set_path(s, "Source path too long", child_rel);
                return -1;
            }

            if (entries[i].is_dir) {
                (void)mkdir(child_dst, 0);
                s->copied_dirs++;
                {
                    char item[160];
                    snprintf(item, sizeof(item), "Creating dir: %s", child_dst);
                    progress_step(s, item);
                }
                if (copy_tree_from_ram(s, child_rel, child_dst) != 0) {
                    close(dfd);
                    return -1;
                }
            } else {
                {
                    char item[160];
                    snprintf(item, sizeof(item), "Copying file: %s", child_dst);
                    progress_note_item(s, item);
                }
                int cfr = copy_file_stream_with_ui(s, child_src_ram, child_dst);
                if (cfr != 0) {
                    close(dfd);
                    if (cfr == -10) error_set_path(s, "Copy source open failed", child_src_ram);
                    else if (cfr == -30) error_set_path(s, "Copy source read failed", child_src_ram);
                    else if (cfr == -40) error_set_path(s, "Copy target write failed", child_dst);
                    else if (cfr == -50) error_set_path(s, "Copy source close failed", child_src_ram);
                    else if (cfr == -60) error_set_path(s, "Copy finalize failed", child_dst);
                    else if (cfr < 0) error_set_path_code(s, "Copy target create failed", child_dst, cfr);
                    else error_set_path(s, "Copy failed", child_src_ram);
                    return -1;
                }
                s->copied_files++;
                progress_step(s, NULL);
            }
        }
    }

    close(dfd);
    return 0;
}

static int read_exact_fd(int fd, void* buf, int len) {
    uint8_t* p = (uint8_t*)buf;
    int got = 0;
    while (got < len) {
        int n = (int)read(fd, p + got, (size_t)(len - got));
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

typedef struct {
    int fd;
    uint8_t stash[32];
    int stash_len;
    int stash_pos;
} payload_reader_t;

static int payload_read_exact(payload_reader_t* pr, void* buf, int len) {
    uint8_t* out = (uint8_t*)buf;
    int got = 0;
    while (got < len) {
        if (pr->stash_pos < pr->stash_len) {
            out[got++] = pr->stash[pr->stash_pos++];
            continue;
        }
        int n = (int)read(pr->fd, out + got, (size_t)(len - got));
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

static int payload_find_magic(payload_reader_t* pr, installer_t* s) {
    if (read_exact_fd(pr->fd, pr->stash, 16) != 0) {
        error_set(s, "Invalid installer payload archive (short header)");
        return -1;
    }
    pr->stash_len = 16;
    pr->stash_pos = 0;

    if (memcmp(pr->stash, PAYLOAD_MAGIC, 8) == 0) {
        pr->stash_pos = 8;
        return 0;
    }
    if (memcmp(pr->stash, PAYLOAD_MAGIC_LEGACY, 7) == 0) {
        pr->stash_pos = 7;
        return 0;
    }

    for (int i = 0; i <= 9; ++i) {
        if (memcmp(pr->stash + i, PAYLOAD_MAGIC_LEGACY, 7) == 0) {
            pr->stash_pos = i + 7;
            return 0;
        }
    }

    char detail[96];
    snprintf(detail, sizeof(detail),
             "Invalid installer payload archive (magic %02X %02X %02X %02X %02X %02X %02X %02X)",
             (unsigned)pr->stash[0], (unsigned)pr->stash[1],
             (unsigned)pr->stash[2], (unsigned)pr->stash[3],
             (unsigned)pr->stash[4], (unsigned)pr->stash[5],
             (unsigned)pr->stash[6], (unsigned)pr->stash[7]);
    error_set(s, detail);
    return -1;
}

static int stream_write_exact(int sh, const void* buf, int len) {
    int w = (int)eynfs_stream_write(sh, buf, (size_t)len);
    return (w == len) ? 0 : -1;
}

static int install_from_payload_archive(installer_t* s, const char* payload_path) {
    int fd = open(payload_path, O_RDONLY, 0);
    if (fd < 0) {
        return 1; /* archive not present -> caller may fallback */
    }

    payload_reader_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.fd = fd;
    if (payload_find_magic(&pr, s) != 0) {
        close(fd);
        return -1;
    }

    for (;;) {
        uint8_t hdr[12];
        if (payload_read_exact(&pr, hdr, (int)sizeof(hdr)) != 0) {
            close(fd);
            error_set(s, "Corrupt payload header");
            return -1;
        }

        uint16_t path_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
        uint8_t etype = hdr[2];
        uint8_t flags = hdr[3];
        uint32_t orig_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        uint32_t stored_size = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

        if (path_len == 0 && etype == 0) {
            close(fd);
            return 0;
        }
        if (path_len == 0 || path_len >= MAX_PATH_LEN) {
            close(fd);
            error_set(s, "Payload path too long");
            return -1;
        }

        char path[MAX_PATH_LEN];
        if (payload_read_exact(&pr, path, (int)path_len) != 0) {
            close(fd);
            error_set(s, "Corrupt payload path");
            return -1;
        }
        path[path_len] = '\0';

        if (path[0] != '/') {
            close(fd);
            error_set(s, "Payload path must be absolute");
            return -1;
        }

        if (etype == PAYLOAD_TYPE_DIR) {
            (void)mkdir(path, 0);
            s->copied_dirs++;
            {
                char item[160];
                snprintf(item, sizeof(item), "Creating dir: %s", path);
                progress_step(s, item);
            }
            if (s->progress_uses_work_units) {
                progress_work_advance(s, PAYLOAD_DIR_PROGRESS_UNITS);
            }
            continue;
        }

        if (etype != PAYLOAD_TYPE_FILE) {
            close(fd);
            error_set(s, "Unknown payload entry type");
            return -1;
        }

        int sh = eynfs_stream_begin(path);
        if (sh < 0) {
            close(fd);
            error_set_path_code(s, "Payload target create failed", path, sh);
            return -1;
        }

        {
            char item[160];
            snprintf(item, sizeof(item), "Copying file: %s", path);
            progress_note_item(s, item);
        }

        uint32_t out_written = 0;
        uint32_t in_read = 0;
        int chunks_since_pulse = 0;

        if (flags & PAYLOAD_FLAG_RLE) {
            while (in_read < stored_size) {
                uint8_t ctrl;
                if (payload_read_exact(&pr, &ctrl, 1) != 0) {
                    (void)eynfs_stream_end(sh);
                    close(fd);
                    error_set_path(s, "Corrupt compressed payload", path);
                    return -1;
                }
                in_read++;

                if (ctrl < 128u) {
                    uint32_t lit = (uint32_t)ctrl + 1u;
                    uint8_t tmp[128];
                    if (lit > stored_size - in_read) {
                        (void)eynfs_stream_end(sh);
                        close(fd);
                        error_set_path(s, "Payload literal overflow", path);
                        return -1;
                    }
                    if (payload_read_exact(&pr, tmp, (int)lit) != 0) {
                        (void)eynfs_stream_end(sh);
                        close(fd);
                        error_set_path(s, "Corrupt payload literal", path);
                        return -1;
                    }
                    in_read += lit;
                    if (stream_write_exact(sh, tmp, (int)lit) != 0) {
                        (void)eynfs_stream_end(sh);
                        close(fd);
                        error_set_path(s, "Payload write failed", path);
                        return -1;
                    }
                    out_written += lit;
                    chunks_since_pulse++;
                    if (chunks_since_pulse >= 64) {
                        if (stored_size > 0u) progress_set_fraction(s, in_read, stored_size);
                        else installer_ui_pulse(s);
                        chunks_since_pulse = 0;
                    }
                } else {
                    uint32_t run = (uint32_t)ctrl - 127u;
                    uint8_t val;
                    if (payload_read_exact(&pr, &val, 1) != 0) {
                        (void)eynfs_stream_end(sh);
                        close(fd);
                        error_set_path(s, "Corrupt payload run", path);
                        return -1;
                    }
                    in_read++;
                    uint8_t tmp[128];
                    memset(tmp, val, sizeof(tmp));
                    if (stream_write_exact(sh, tmp, (int)run) != 0) {
                        (void)eynfs_stream_end(sh);
                        close(fd);
                        error_set_path(s, "Payload write failed", path);
                        return -1;
                    }
                    out_written += run;
                    chunks_since_pulse++;
                    if (chunks_since_pulse >= 64) {
                        if (stored_size > 0u) progress_set_fraction(s, in_read, stored_size);
                        else installer_ui_pulse(s);
                        chunks_since_pulse = 0;
                    }
                }
            }
        } else {
            uint8_t tmp[COPY_BUF_SZ];
            while (in_read < stored_size) {
                uint32_t rem = stored_size - in_read;
                uint32_t take = rem > (uint32_t)sizeof(tmp) ? (uint32_t)sizeof(tmp) : rem;
                if (payload_read_exact(&pr, tmp, (int)take) != 0) {
                    (void)eynfs_stream_end(sh);
                    close(fd);
                    error_set_path(s, "Corrupt payload data", path);
                    return -1;
                }
                in_read += take;
                if (stream_write_exact(sh, tmp, (int)take) != 0) {
                    (void)eynfs_stream_end(sh);
                    close(fd);
                    error_set_path(s, "Payload write failed", path);
                    return -1;
                }
                out_written += take;
                chunks_since_pulse++;
                if (chunks_since_pulse >= 64) {
                    if (stored_size > 0u) progress_set_fraction(s, in_read, stored_size);
                    else installer_ui_pulse(s);
                    chunks_since_pulse = 0;
                }
            }
        }

        if (stored_size > 0u) {
            progress_set_fraction(s, in_read, stored_size);
        }

        if (out_written != orig_size) {
            (void)eynfs_stream_end(sh);
            close(fd);
            error_set_path(s, "Payload size mismatch", path);
            return -1;
        }

        if (eynfs_stream_end(sh) != 0) {
            close(fd);
            error_set_path(s, "Payload finalize failed", path);
            return -1;
        }

        s->copied_files++;
        if (s->progress_uses_work_units) {
            uint32_t file_work = (stored_size > 0u) ? stored_size : ((orig_size > 0u) ? orig_size : 1u);
            progress_work_advance(s, file_work);
        }
        progress_step(s, path);
    }
}

static int install_mbr_boot_code(int logical_drive) {
    int fd_boot = open("RAM:/installer/grub/boot.img", O_RDONLY, 0);
    if (fd_boot < 0) return -1;

    unsigned char boot[512];
    int boot_bytes = (int)read(fd_boot, boot, sizeof(boot));
    close(fd_boot);
    if (boot_bytes != 512) return -2;

    int fd_core = open("RAM:/installer/grub/core.img", O_RDONLY, 0);
    if (fd_core < 0) return -3;

    int core_size = lseek(fd_core, 0, SEEK_END);
    if (core_size <= 0) {
        close(fd_core);
        return -4;
    }
    if (lseek(fd_core, 0, SEEK_SET) < 0) {
        close(fd_core);
        return -5;
    }

    unsigned char* core = (unsigned char*)malloc((size_t)core_size);
    if (!core) {
        close(fd_core);
        return -6;
    }

    int got = 0;
    while (got < core_size) {
        int n = (int)read(fd_core, core + got, (size_t)(core_size - got));
        if (n <= 0) {
            free(core);
            close(fd_core);
            return -7;
        }
        got += n;
    }
    close(fd_core);

    int core_sectors = (core_size + 511) / 512;
    if (core_sectors <= 0 || core_sectors >= 2048) {
        free(core);
        return -8;
    }

    int fd_kernel = open("/boot/kernel.bin", O_RDONLY, 0);
    if (fd_kernel < 0) {
        free(core);
        return -12;
    }
    int kernel_size = lseek(fd_kernel, 0, SEEK_END);
    if (kernel_size <= 0 || lseek(fd_kernel, 0, SEEK_SET) < 0) {
        close(fd_kernel);
        free(core);
        return -13;
    }
    int kernel_sectors = (kernel_size + 511) / 512;

    /* Rebuild MBR with current partition entries while replacing only boot code. */
    eyn_installer_partitions_t parts;
    memset(&parts, 0, sizeof(parts));
    if (eyn_sys_installer_get_partitions((uint32_t)logical_drive, &parts) != 0) {
        free(core);
        return -9;
    }

    unsigned char mbr[512];
    memset(mbr, 0, sizeof(mbr));
    memcpy(mbr, boot, 446);

    int has_bootable = 0;
    for (int i = 0; i < 4; ++i) {
        if (!parts.partitions[i].present) continue;
        if (parts.partitions[i].bootable) has_bootable = 1;
    }

    for (int i = 0; i < 4; ++i) {
        if (!parts.partitions[i].present) continue;

        unsigned char* e = mbr + 0x1BE + i * 16;
        int mark_boot = parts.partitions[i].bootable ? 1 : 0;
        if (!has_bootable && i == 0) mark_boot = 1;

        e[0] = (unsigned char)(mark_boot ? 0x80 : 0x00); /* status */
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;           /* CHS start (LBA) */
        e[4] = parts.partitions[i].type;                 /* type */
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;           /* CHS end (LBA) */

        uint32_t lba = parts.partitions[i].lba_start;
        uint32_t cnt = parts.partitions[i].sector_count;
        e[8]  = (unsigned char)(lba & 0xFFu);
        e[9]  = (unsigned char)((lba >> 8) & 0xFFu);
        e[10] = (unsigned char)((lba >> 16) & 0xFFu);
        e[11] = (unsigned char)((lba >> 24) & 0xFFu);
        e[12] = (unsigned char)(cnt & 0xFFu);
        e[13] = (unsigned char)((cnt >> 8) & 0xFFu);
        e[14] = (unsigned char)((cnt >> 16) & 0xFFu);
        e[15] = (unsigned char)((cnt >> 24) & 0xFFu);

        if (i == 0) {
            uint32_t raw_end = KERNEL_RAW_LBA + (uint32_t)kernel_sectors;
            if (raw_end >= parts.partitions[i].lba_start) {
                close(fd_kernel);
                free(core);
                return -14;
            }
        }
    }

    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (eyn_sys_installer_write_sector((uint32_t)logical_drive, 0, mbr) != 0) {
        free(core);
        return -10;
    }

    /* Embed GRUB core image directly after MBR (LBA 1..N). */
    for (int s = 0; s < core_sectors; ++s) {
        unsigned char sec[512];
        memset(sec, 0, sizeof(sec));

        int off = s * 512;
        int rem = core_size - off;
        int take = rem > 512 ? 512 : rem;
        if (take > 0) memcpy(sec, core + off, (size_t)take);

        if (eyn_sys_installer_write_sector((uint32_t)logical_drive, (uint32_t)(1 + s), sec) != 0) {
            close(fd_kernel);
            free(core);
            return -11;
        }
    }

    /* Write raw kernel blob at fixed LBA expected by embedded GRUB config. */
    for (int s = 0; s < kernel_sectors; ++s) {
        unsigned char sec[512];
        memset(sec, 0, sizeof(sec));
        int n = (int)read(fd_kernel, sec, sizeof(sec));
        if (n < 0) {
            close(fd_kernel);
            free(core);
            return -15;
        }

        if (eyn_sys_installer_write_sector((uint32_t)logical_drive,
                                           (uint32_t)(KERNEL_RAW_LBA + (uint32_t)s),
                                           sec) != 0) {
            close(fd_kernel);
            free(core);
            return -16;
        }

        if (n == 0) break;
    }

    close(fd_kernel);

    free(core);
    return 0;
}

static int write_grub_cfg(void) {
    const char* cfg =
        "set default=0\n"
        "set timeout=0\n"
        "menuentry \"EYN-OS\" {\n"
        "    multiboot /boot/kernel.bin\n"
        "    boot\n"
        "}\n";

    (void)mkdir("/boot", 0);
    (void)mkdir("/boot/grub", 0);

    /*
     * Use the EYNFS stream writer here as well, to match the installer's
     * copy path and avoid backend differences in writefile() behavior.
     */
    int sh = eynfs_stream_begin("/boot/grub/grub.cfg");
    if (sh < 0) return sh;

    int len = (int)strlen(cfg);
    int w = (int)eynfs_stream_write(sh, cfg, (size_t)len);
    if (w != len) {
        (void)eynfs_stream_end(sh);
        return -200;
    }

    int end_rc = eynfs_stream_end(sh);
    if (end_rc != 0) return -201;

    return 0;
}

static int run_install(installer_t* s) {
    if (!s) return -1;

    status_set(s, "Checking RAM:/ installer media ...");
    progress_reset(s, 100);
    s->progress_permille = 30;
    installer_ui_pulse(s);
    if (installer_ram_preflight(s) != 0) return -101;

    if (eyn_sys_installer_prepare_drive((uint32_t)s->selected_drive) != 0) {
        error_set(s, "Drive partition/format failed");
        return -102;
    }
    s->progress_permille = 80;
    installer_ui_pulse(s);

    if (eyn_sys_drive_set_logical((uint32_t)s->selected_drive) < 0) {
        error_set(s, "Could not switch to target drive");
        return -103;
    }

    status_set(s, "Verifying target filesystem ...");
    if (installer_target_preflight(s) != 0) return -104;
    s->progress_permille = 120;
    installer_ui_pulse(s);

    s->step = STEP_COPY;
    status_set(s, "Applying payload archive ...");
    {
        int total_entries = 0;
        uint32_t total_work = 0;
        if (payload_count_entries("RAM:/installer/payload.eynpkg", &total_entries, &total_work) == 0 && total_entries > 0) {
            if (total_work > 0u) progress_work_reset(s, total_work);
            else progress_reset(s, total_entries);
        } else {
            progress_reset(s, 0);
            s->progress_permille = 150;
        }
        installer_ui_pulse(s);
    }
    int payload_rc = install_from_payload_archive(s, "RAM:/installer/payload.eynpkg");
    if (payload_rc == 1) {
        status_set(s, "Payload archive missing; copying files from RAM:/ ...");
        {
            int total_entries = 0;
            if (count_tree_from_ram("/", &total_entries) == 0 && total_entries > 0) {
                progress_reset(s, total_entries);
            } else {
                progress_reset(s, 0);
                s->progress_permille = 200;
                installer_ui_pulse(s);
            }
        }
        if (copy_tree_from_ram(s, "/", "/") != 0) {
            if (s->error[0] == '\0') {
                error_set(s, "Copy from RAM:/ failed");
            }
            return -105;
        }
    } else if (payload_rc != 0) {
        if (s->error[0] == '\0') {
            error_set(s, "Payload install failed");
        }
        return -108;
    }

    status_set(s, "Writing GRUB config ...");
    s->step = STEP_BOOTLOADER;
    s->progress_total = 3;
    s->progress_done = 0;
    s->progress_permille = 0;
    strncpy(s->current_item, "Writing /boot/grub/grub.cfg", sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
    {
        int cfg_rc = write_grub_cfg();
        if (cfg_rc != 0) {
            error_set_path_code(s, "Failed writing /boot/grub/grub.cfg", "/boot/grub/grub.cfg", cfg_rc);
            return -106;
        }
        progress_step(s, "grub.cfg written");
    }

    status_set(s, "Installing GRUB (embedded core) ...");
    strncpy(s->current_item, "Embedding boot.img/core.img and kernel blob", sizeof(s->current_item) - 1);
    s->current_item[sizeof(s->current_item) - 1] = '\0';
    installer_ui_pulse(s);
    {
        int mbr_rc = install_mbr_boot_code(s->selected_drive);
        if (mbr_rc < 0) {
            error_set_path_code(s, "Bootloader install failed", "/installer/grub/{boot.img,core.img}", mbr_rc);
            return -107;
        }
        progress_step(s, "Bootloader installed");
    }

    status_set(s, "Installation complete");
    progress_step(s, "Done");
    s->progress_permille = 1000;
    installer_ui_pulse(s);
    s->step = STEP_DONE;
    return 0;
}

static void draw_center_text(int h, int y, const char* text, unsigned char r, unsigned char g, unsigned char b) {
    gui_text_t t;
    t.x = 12;
    t.y = y;
    t.r = r;
    t.g = g;
    t.b = b;
    t._pad = 0;
    t.text = text;
    (void)gui_draw_text(h, &t);
}

static void draw_ui(int h, installer_t* s) {
    gui_size_t sz;
    sz.w = 640;
    sz.h = 360;
    if (!gui_call_ok(gui_get_content_size(h, &sz), "gui_get_content_size", s)) {
        sz.w = 640;
        sz.h = 360;
    }

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    if (!gui_call_ok(gui_begin(h), "gui_begin", s)) return;
    if (!gui_call_ok(gui_clear(h, &bg), "gui_clear", s)) return;

    gui_rect_t header = {0, 0, sz.w, 24, GUI_PAL_HEADER_R, GUI_PAL_HEADER_G, GUI_PAL_HEADER_B, 0};
    if (!gui_call_ok(gui_fill_rect(h, &header), "gui_fill_rect(header)", s)) return;

    draw_center_text(h, 6, "EYN-OS Installer", 245, 245, 245);

    if (s->step == STEP_WELCOME) {
        draw_center_text(h, 52, "This will install EYN-OS to a selected disk.", 220, 220, 220);
        draw_center_text(h, 72, "Press Enter to continue or Q to cancel.", 170, 170, 170);
    } else if (s->step == STEP_SELECT_DRIVE) {
        draw_center_text(h, 44, "Select target drive (Up/Down + Enter)", 220, 220, 220);
        int y = 72;
        for (int i = 0; i < s->drive_count; ++i) {
            char line[64];
            snprintf(line, sizeof(line), "Drive %d", s->drives[i]);
            if (i == s->selected_idx) {
                gui_rect_t sel = {8, y - 2, sz.w - 16, 18, GUI_PAL_SEL_R, GUI_PAL_SEL_G, GUI_PAL_SEL_B, 0};
                if (!gui_call_ok(gui_fill_rect(h, &sel), "gui_fill_rect(select)", s)) return;
            }
            draw_center_text(h, y, line, 240, 240, 240);
            y += 20;
        }
        if (s->drive_count == 0) {
            draw_center_text(h, 72, "No drives detected", 255, 140, 140);
        }
    } else if (s->step == STEP_FORMAT) {
        char line[96];
        snprintf(line, sizeof(line), "Target drive: %d", s->selected_drive);
        draw_center_text(h, 52, line, 235, 235, 235);
        draw_center_text(h, 76, "Press Enter to FORMAT + INSTALL", 255, 200, 130);
        draw_center_text(h, 96, "Warning: this erases target disk contents.", 255, 120, 120);
    } else if (s->step == STEP_COPY || s->step == STEP_BOOTLOADER) {
        char c1[96];
        char ptxt[64];
        int bar_x = 14;
        int bar_y = 120;
        int bar_w = sz.w - 28;
        int bar_h = 12;
        int fill_w = (bar_w * clamp_permille(s->progress_permille)) / 1000;
        snprintf(c1, sizeof(c1), "Files copied: %d   Dirs: %d", s->copied_files, s->copied_dirs);
        snprintf(ptxt, sizeof(ptxt), "Progress: %d%%", clamp_permille(s->progress_permille) / 10);
        draw_center_text(h, 56, s->status[0] ? s->status : "Working...", 200, 230, 255);
        draw_center_text(h, 78, c1, 185, 185, 185);

        gui_rect_t bar_bg = {bar_x, bar_y, bar_w, bar_h, 58, 61, 72, 0};
        if (!gui_call_ok(gui_fill_rect(h, &bar_bg), "gui_fill_rect(bar_bg)", s)) return;

        if (fill_w > 0) {
            gui_rect_t bar_fg = {bar_x, bar_y, fill_w, bar_h, 86, 176, 240, 0};
            if (!gui_call_ok(gui_fill_rect(h, &bar_fg), "gui_fill_rect(bar_fg)", s)) return;
        }

        draw_center_text(h, 138, ptxt, 190, 210, 230);
        if (s->current_item[0]) {
            draw_center_text(h, 158, s->current_item, 170, 170, 170);
        } else {
            draw_center_text(h, 158, "Please wait...", 170, 170, 170);
        }
    } else if (s->step == STEP_DONE) {
        char c1[96];
        snprintf(c1, sizeof(c1), "Copied files: %d   directories: %d", s->copied_files, s->copied_dirs);
        draw_center_text(h, 56, "Install finished successfully.", 140, 255, 160);
        draw_center_text(h, 78, c1, 220, 220, 220);
        if (s->warning[0]) {
            draw_center_text(h, 102, s->warning, 255, 200, 120);
            draw_center_text(h, 126, "Payload installed; bootloader integration is pending.", 190, 190, 190);
            draw_center_text(h, 150, "Press Q to close installer.", 170, 170, 170);
        } else {
            draw_center_text(h, 102, "Reboot and boot from installed disk.", 190, 190, 190);
            draw_center_text(h, 126, "Press Q to close installer.", 170, 170, 170);
        }
    } else if (s->step == STEP_ERROR) {
        draw_center_text(h, 56, "Install failed", 255, 120, 120);
        draw_center_text(h, 78, s->error, 230, 190, 190);
        draw_center_text(h, 102, "Press Q to close installer.", 170, 170, 170);
    } else {
        draw_center_text(h, 56, "Installer state error", 255, 140, 140);
        draw_center_text(h, 78, "Unexpected installer step; operation aborted.", 210, 180, 180);
        draw_center_text(h, 102, "Press Q to close installer.", 170, 170, 170);
    }

    (void)gui_call_ok(gui_present(h), "gui_present", s);
}

static void refresh_drives(installer_t* s) {
    s->drive_count = 0;
    int n = eyn_sys_drive_get_count();
    if (n < 0) n = 0;
    for (int i = 0; i < n && s->drive_count < MAX_DRIVES; ++i) {
        if (eyn_sys_drive_is_present((uint32_t)i) > 0) {
            s->drives[s->drive_count++] = i;
        }
    }
    if (s->selected_idx >= s->drive_count) s->selected_idx = 0;
}

int main(void) {
    installer_t st;
    memset(&st, 0, sizeof(st));
    st.running = 1;
    st.step = STEP_WELCOME;
    status_set(&st, "Ready");

    int h = gui_attach("Installer", "EYN-OS setup");
    if (h < 0) {
        puts("installer: gui_attach failed");
        return 1;
    }
    /*
     * UI-INVARIANT: Installer text must remain visible while current drive
     * switches from RAM:/ to the target disk during install.
     *
     * Why: The default GUI font path may resolve against the current logical
     * drive. During target formatting/copy, that drive can be empty, causing
     * glyph lookups to fail and the window to appear blank.
     *
     * Contract: Use built-in kernel font (empty path) so rendering does not
     * depend on filesystem availability or current-drive context.
     */
    (void)gui_set_font(h, "");
    g_installer_gui_handle = h;

    refresh_drives(&st);
    draw_ui(h, &st);

    while (st.running) {
        gui_event_t ev;
        int rc = gui_wait_event(h, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        if (ev.type == GUI_EVENT_CLOSE) break;

        if (ev.type == GUI_EVENT_KEY) {
            unsigned ch = (unsigned)ev.a & 0xFFu;
            if (ch == 'q' || ch == 'Q') break;

            if (st.step == STEP_WELCOME) {
                if (ch == '\r' || ch == '\n') st.step = STEP_SELECT_DRIVE;
            } else if (st.step == STEP_SELECT_DRIVE) {
                if (ev.a == GUI_KEY_UP && st.selected_idx > 0) st.selected_idx--;
                if (ev.a == GUI_KEY_DOWN && st.selected_idx + 1 < st.drive_count) st.selected_idx++;
                if ((ch == '\r' || ch == '\n') && st.drive_count > 0) {
                    st.selected_drive = st.drives[st.selected_idx];
                    st.step = STEP_FORMAT;
                }
            } else if (st.step == STEP_FORMAT) {
                if (ch == '\r' || ch == '\n') {
                    st.step = STEP_COPY;
                    status_set(&st, "Preparing disk ...");
                    draw_ui(h, &st);
                    int install_rc = run_install(&st);
                    if (install_rc != 0) {
                        if (st.error[0] == '\0') {
                            snprintf(st.error, sizeof(st.error), "Installer failed (rc=%d)", install_rc);
                            st.error[sizeof(st.error) - 1] = '\0';
                        }
                        printf("[installer] failure rc=%d msg=%s\n", install_rc, st.error);
                        st.step = STEP_ERROR;
                    }
                }
            }
        }

        draw_ui(h, &st);
    }

    g_installer_gui_handle = -1;
    return 0;
}
