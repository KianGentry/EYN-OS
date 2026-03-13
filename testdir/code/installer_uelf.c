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
    char status[MAX_STATUS];
    char warning[MAX_STATUS];
    char error[MAX_STATUS];
} installer_t;

static void status_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "";
    strncpy(s->status, msg, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = '\0';
}

static void error_set(installer_t* s, const char* msg) {
    if (!s) return;
    if (!msg) msg = "Unknown error";
    strncpy(s->error, msg, sizeof(s->error) - 1);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
}

static void error_set_path(installer_t* s, const char* prefix, const char* path) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s", prefix, path);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
}

static void error_set_path_code(installer_t* s, const char* prefix, const char* path, int code) {
    if (!s) return;
    if (!prefix) prefix = "Error";
    if (!path) path = "?";
    snprintf(s->error, sizeof(s->error), "%s: %s (rc=%d)", prefix, path, code);
    s->error[sizeof(s->error) - 1] = '\0';
    printf("[installer] error: %s\n", s->error);
    s->step = STEP_ERROR;
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

static int copy_file_stream(const char* src_ram_path, const char* dst_abs_path) {
    int fd = open(src_ram_path, O_RDONLY, 0);
    if (fd < 0) return -10;

    int sh = eynfs_stream_begin(dst_abs_path);
    if (sh < 0) {
        close(fd);
        return sh;
    }

    char buf[COPY_BUF_SZ];
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
                if (copy_tree_from_ram(s, child_rel, child_dst) != 0) {
                    close(dfd);
                    return -1;
                }
            } else {
                int cfr = copy_file_stream(child_src_ram, child_dst);
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
            }
        }
    }

    close(dfd);
    return 0;
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

    int fd_kernel = open("RAM:/boot/kernel.bin", O_RDONLY, 0);
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
    if (installer_ram_preflight(s) != 0) return -101;

    if (eyn_sys_installer_prepare_drive((uint32_t)s->selected_drive) != 0) {
        error_set(s, "Drive partition/format failed");
        return -102;
    }

    if (eyn_sys_drive_set_logical((uint32_t)s->selected_drive) < 0) {
        error_set(s, "Could not switch to target drive");
        return -103;
    }

    status_set(s, "Verifying target filesystem ...");
    if (installer_target_preflight(s) != 0) return -104;

    (void)mkdir("/binaries", 0);

    status_set(s, "Copying files from RAM:/ ...");
    if (copy_tree_from_ram(s, "/", "/") != 0) {
        if (s->error[0] == '\0') {
            error_set(s, "Copy from RAM:/ failed");
        }
        return -105;
    }

    status_set(s, "Writing GRUB config ...");
    {
        int cfg_rc = write_grub_cfg();
        if (cfg_rc != 0) {
            error_set_path_code(s, "Failed writing /boot/grub/grub.cfg", "/boot/grub/grub.cfg", cfg_rc);
            return -106;
        }
    }

    status_set(s, "Installing GRUB (embedded core) ...");
    {
        int mbr_rc = install_mbr_boot_code(s->selected_drive);
        if (mbr_rc < 0) {
            error_set_path_code(s, "Bootloader install failed", "/installer/grub/{boot.img,core.img}", mbr_rc);
            return -107;
        }
    }

    status_set(s, "Installation complete");
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
    gui_draw_text(h, &t);
}

static void draw_ui(int h, installer_t* s) {
    gui_size_t sz;
    sz.w = 640;
    sz.h = 360;
    gui_get_content_size(h, &sz);

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    gui_clear(h, &bg);

    gui_rect_t header = {0, 0, sz.w, 24, GUI_PAL_HEADER_R, GUI_PAL_HEADER_G, GUI_PAL_HEADER_B, 0};
    gui_fill_rect(h, &header);

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
                gui_fill_rect(h, &sel);
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
        snprintf(c1, sizeof(c1), "Files copied: %d   Dirs: %d", s->copied_files, s->copied_dirs);
        draw_center_text(h, 56, s->status, 200, 230, 255);
        draw_center_text(h, 78, c1, 185, 185, 185);
        draw_center_text(h, 102, "Please wait...", 170, 170, 170);
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
    }

    gui_present(h);
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

    return 0;
}
