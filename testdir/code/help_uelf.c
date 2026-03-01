#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Display command help.", "help");

typedef struct {
    char name[64];
    char desc[96];
    char usage[96];
    /*
     * Tracks whether metadata has been loaded for this entry.
     * 0 = not yet scanned; 1 = desc/usage populated (from binary or fallback).
     * Loading is deferred until the command is first displayed so that startup
     * requires only a directory listing, not a read of every binary.
     */
    int  meta_loaded;
} help_cmd_t;

#define HELP_BIN_DIR "/binaries"
#define HELP_MAX_CMDS 200

static help_cmd_t g_cmds[HELP_MAX_CMDS];
static int g_cmd_count = 0;

/*
 * ABI-INVARIANT: Marker embedded by EYN_CMDMETA_V1 in the .eynos.cmdmeta
 * section: 4-byte ASCII magic "ECMD", followed by a v1 header (u16 LE
 * version=1, u16 reserved=0), then two NUL-terminated strings: description
 * and example/usage.  This layout must match userland/include/eynos_cmdmeta.h
 * exactly; any divergence silently produces wrong or missing help text.
 *
 * The array is stored XOR'd with 0xFF so that the raw marker sequence is
 * never present in this binary's .rodata -- without this the scanner would
 * hit the constant itself as a false-positive match before reaching the
 * actual EYN_CMDMETA_V1 payload embedded in each scanned binary.
 */
static const unsigned char CMDMETA_MARKER_XOR[] = {
    'E'^0xFF, 'C'^0xFF, 'M'^0xFF, 'D'^0xFF,
    0x01^0xFF, 0x00^0xFF,  /* version = 1, little-endian u16 */
    0x00^0xFF, 0x00^0xFF   /* reserved */
};
#define CMDMETA_MARKER_LEN 8

static int cmdmeta_marker_match(const unsigned char* p) {
    for (int i = 0; i < CMDMETA_MARKER_LEN; i++)
        if (p[i] != (unsigned char)(CMDMETA_MARKER_XOR[i] ^ 0xFF)) return 0;
    return 1;
}

/*
 * Scan an already-open file descriptor for the ECMD metadata marker and
 * extract the description and example strings that follow it.
 *
 * Reads in SCAN_CHUNK-byte increments, retaining SCAN_OVERLAP bytes across
 * iterations so that a marker straddling a read boundary is still detected.
 * Once the marker is located, up to 256 subsequent bytes are buffered to
 * find both NUL terminators.
 *
 * Returns 1 on success (desc and example populated), 0 if not found or on
 * any I/O error.
 */
static int scan_for_meta(int fd,
                          char* desc, int desc_len,
                          char* example, int example_len)
{
    enum { SCAN_CHUNK = 512 };
    /* Overlap = marker length - 1 so a split marker is caught on the next
     * iteration without re-reading. */
    enum { SCAN_OVERLAP = CMDMETA_MARKER_LEN - 1 };

    unsigned char buf[SCAN_CHUNK + SCAN_OVERLAP];
    int buf_used = 0;

    for (;;) {
        int n = read(fd, buf + buf_used, SCAN_CHUNK);
        if (n <= 0) break;
        buf_used += n;

        for (int i = 0; i + CMDMETA_MARKER_LEN <= buf_used; i++) {
            if (!cmdmeta_marker_match(buf + i))
                continue;

            /* Marker found.  Collect the two NUL-terminated strings that
             * immediately follow into strbuf, reading more data if needed. */
            unsigned char strbuf[256];
            int have = buf_used - i - CMDMETA_MARKER_LEN;
            if (have < 0) have = 0;
            if (have > (int)sizeof(strbuf)) have = (int)sizeof(strbuf);
            memcpy(strbuf, buf + i + CMDMETA_MARKER_LEN, (unsigned int)have);

            while (have < (int)sizeof(strbuf) - 1) {
                int nuls = 0;
                for (int k = 0; k < have; k++) {
                    if (strbuf[k] == 0 && ++nuls >= 2) break;
                }
                if (nuls >= 2) break;
                int nn = read(fd, strbuf + have,
                              (unsigned int)(sizeof(strbuf) - (unsigned int)have - 1));
                if (nn <= 0) break;
                have += nn;
            }

            /* First NUL terminates the description. */
            int nul1 = -1;
            for (int k = 0; k < have; k++) {
                if (strbuf[k] == 0) { nul1 = k; break; }
            }
            if (nul1 < 0) return 0;

            int dlen = nul1 < desc_len - 1 ? nul1 : desc_len - 1;
            memcpy(desc, strbuf, (unsigned int)dlen);
            desc[dlen] = '\0';

            /* Second NUL terminates the example/usage string. */
            int nul2 = -1;
            for (int k = nul1 + 1; k < have; k++) {
                if (strbuf[k] == 0) { nul2 = k; break; }
            }
            int estart = nul1 + 1;
            int elen = (nul2 >= 0 ? nul2 - estart : have - estart);
            if (elen < 0) elen = 0;
            if (elen > example_len - 1) elen = example_len - 1;
            memcpy(example, strbuf + estart, (unsigned int)elen);
            example[elen] = '\0';

            return 1;
        }

        /* Retain the tail so a marker split across chunks is not missed. */
        if (buf_used > SCAN_OVERLAP) {
            memmove(buf, buf + buf_used - SCAN_OVERLAP, SCAN_OVERLAP);
            buf_used = SCAN_OVERLAP;
        }
    }

    return 0;
}

/*
 * Try to read EYN_CMDMETA_V1 metadata from the binary at bin_path.
 * Returns 1 and populates desc/example on success, 0 on failure.
 */
static int try_read_uelf_meta(const char* bin_path,
                               char* desc, int desc_len,
                               char* example, int example_len)
{
    int fd = open(bin_path, O_RDONLY, 0);
    if (fd < 0) return 0;
    int found = scan_for_meta(fd, desc, desc_len, example, example_len);
    close(fd);
    return found;
}

static void set_default_cmd_meta(help_cmd_t* cmd) {
    if (!cmd) return;

    /* Build the binary path and attempt to extract embedded metadata. */
    char bin_path[128];
    int plen = (int)strlen(HELP_BIN_DIR);
    int nlen = (int)strlen(cmd->name);
    if (plen + 1 + nlen < (int)sizeof(bin_path) - 1) {
        memcpy(bin_path, HELP_BIN_DIR, (unsigned int)plen);
        bin_path[plen] = '/';
        memcpy(bin_path + plen + 1, cmd->name, (unsigned int)(nlen + 1));
        if (try_read_uelf_meta(bin_path, cmd->desc, (int)sizeof(cmd->desc),
                               cmd->usage, (int)sizeof(cmd->usage))) {
            return;
        }
    }

    /* Fallback: generic description; usage defaults to the command name. */
    strncpy(cmd->desc, "Userland command.", sizeof(cmd->desc) - 1);
    cmd->desc[sizeof(cmd->desc) - 1] = '\0';
    strncpy(cmd->usage, cmd->name, sizeof(cmd->usage) - 1);
    cmd->usage[sizeof(cmd->usage) - 1] = '\0';
}

/*
 * Ensure metadata for cmd is available, scanning its binary on first access.
 * Idempotent: does nothing if meta_loaded is already set.  The deferred scan
 * means startup only does a directory listing; binary reads happen one at a
 * time as the user navigates to each command.
 */
static void ensure_cmd_meta(help_cmd_t* cmd) {
    if (!cmd || cmd->meta_loaded) return;
    cmd->meta_loaded = 1;
    set_default_cmd_meta(cmd);
}

static void load_commands_from_binaries(void) {
    g_cmd_count = 0;

    int fd = open(HELP_BIN_DIR, O_RDONLY, 0);
    if (fd < 0) return;

    eyn_dirent_t entries[16];
    for (;;) {
        int rc = getdents(fd, entries, sizeof(entries));
        if (rc <= 0) break;

        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (g_cmd_count >= HELP_MAX_CMDS) break;
            if (entries[i].name[0] == '\0' || entries[i].is_dir) continue;
            if (entries[i].name[0] == '.') continue;

            help_cmd_t* cmd = &g_cmds[g_cmd_count++];
            strncpy(cmd->name, entries[i].name, sizeof(cmd->name) - 1);
            cmd->name[sizeof(cmd->name) - 1] = '\0';
            cmd->meta_loaded = 0;
            cmd->desc[0] = '\0';
            cmd->usage[0] = '\0';
        }

        if (g_cmd_count >= HELP_MAX_CMDS) break;
    }

    (void)close(fd);

    for (int i = 0; i < g_cmd_count - 1; ++i) {
        for (int j = i + 1; j < g_cmd_count; ++j) {
            if (strcmp(g_cmds[i].name, g_cmds[j].name) > 0) {
                help_cmd_t tmp = g_cmds[i];
                g_cmds[i] = g_cmds[j];
                g_cmds[j] = tmp;
            }
        }
    }
}

static void draw_help_gui(int h, int selected) {
    (void)gui_begin(h);

    gui_size_t sz;
    sz.w = 0;
    sz.h = 0;
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 720;
    if (sz.h <= 0) sz.h = 420;

    gui_rgb_t bg = { .r = 8, .g = 8, .b = 10, ._pad = 0 };
    (void)gui_clear(h, &bg);

    int left_w = (sz.w * 32) / 100;
    if (left_w < 180) left_w = 180;
    int body_y = 0;
    int body_h = sz.h - body_y;

    gui_rect_t left_bg = { .x = 0, .y = body_y, .w = left_w, .h = body_h, .r = 14, .g = 14, .b = 18, ._pad = 0 };
    gui_rect_t right_bg = { .x = left_w + 1, .y = body_y, .w = sz.w - left_w - 1, .h = body_h, .r = 10, .g = 10, .b = 14, ._pad = 0 };
    (void)gui_fill_rect(h, &left_bg);
    (void)gui_fill_rect(h, &right_bg);

    gui_line_t sep = { .x1 = left_w, .y1 = body_y, .x2 = left_w, .y2 = sz.h - 1, .r = 70, .g = 70, .b = 80, ._pad = 0 };
    (void)gui_draw_line(h, &sep);

    int max_rows = (body_h - 8) / 12;
    if (max_rows < 3) max_rows = 3;
    if (selected < 0) selected = 0;
    if (selected >= g_cmd_count) selected = g_cmd_count - 1;
    int start = 0;
    if (selected >= max_rows) start = selected - max_rows + 1;

    for (int i = 0; i < max_rows; ++i) {
        int idx = start + i;
        if (idx >= g_cmd_count) break;
        int y = body_y + 4 + (i * 12);
        if (idx == selected) {
            gui_rect_t hi = { .x = 2, .y = y - 1, .w = left_w - 4, .h = 12, .r = 45, .g = 45, .b = 68, ._pad = 0 };
            (void)gui_fill_rect(h, &hi);
            gui_text_t t = { .x = 6, .y = y, .r = 255, .g = 255, .b = 160, ._pad = 0, .text = g_cmds[idx].name };
            (void)gui_draw_text(h, &t);
        } else {
            gui_text_t t = { .x = 6, .y = y, .r = 215, .g = 215, .b = 215, ._pad = 0, .text = g_cmds[idx].name };
            (void)gui_draw_text(h, &t);
        }
    }

    ensure_cmd_meta(&g_cmds[selected]);

    gui_text_t d0 = { .x = left_w + 10, .y = body_y + 8, .r = 255, .g = 220, .b = 120, ._pad = 0, .text = g_cmds[selected].name };
    gui_text_t d1 = { .x = left_w + 10, .y = body_y + 24, .r = 230, .g = 230, .b = 230, ._pad = 0, .text = g_cmds[selected].desc };
    gui_text_t d2 = { .x = left_w + 10, .y = body_y + 40, .r = 170, .g = 220, .b = 255, ._pad = 0, .text = "Usage:" };
    gui_text_t d3 = { .x = left_w + 10, .y = body_y + 52, .r = 200, .g = 200, .b = 200, ._pad = 0, .text = g_cmds[selected].usage };
    (void)gui_draw_text(h, &d0);
    (void)gui_draw_text(h, &d1);
    (void)gui_draw_text(h, &d2);
    (void)gui_draw_text(h, &d3);

    (void)gui_present(h);
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: help");
        return 0;
    }

    load_commands_from_binaries();
    if (g_cmd_count <= 0) {
        puts("help: no commands found in /binaries");
        return 1;
    }

    int h = gui_attach("EYN-OS Help", "Up/Down/mouse wheel: navigate | Click: select | q/ESC: quit");
    if (h < 0) {
        puts("help: gui_attach failed");
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    int selected = 0;
    int prev_left_down = 0;
    for (;;) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                int key = ev.a & 0xFFFF;
                int base = key & 0x0FFF;
                unsigned ch = (unsigned)key & 0xFFu;

                if (ch == (unsigned)'q' || ch == (unsigned)'Q' || ch == 27u) {
                    (void)gui_set_continuous_redraw(h, 0);
                    return 0;
                }

                if (ch == (unsigned)'j' || ch == (unsigned)'J') {
                    if (selected + 1 < g_cmd_count) selected++;
                } else if (ch == (unsigned)'k' || ch == (unsigned)'K') {
                    if (selected > 0) selected--;
                } else if (key == 0x1001 || base == 0x1001 || key == 0x4800 || key == 72) {
                    if (selected > 0) selected--;
                } else if (key == 0x1002 || base == 0x1002 || key == 0x5000 || key == 80) {
                    if (selected + 1 < g_cmd_count) selected++;
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                gui_size_t sz;
                sz.w = 0;
                sz.h = 0;
                (void)gui_get_content_size(h, &sz);
                if (sz.w <= 0) sz.w = 720;
                if (sz.h <= 0) sz.h = 420;

                int left_w = (sz.w * 32) / 100;
                if (left_w < 180) left_w = 180;

                int max_rows = (sz.h - 8) / 12;
                if (max_rows < 3) max_rows = 3;
                int start = 0;
                if (selected >= max_rows) start = selected - max_rows + 1;

                if (ev.d > 0 && selected > 0) {
                    selected--;
                } else if (ev.d < 0 && selected + 1 < g_cmd_count) {
                    selected++;
                }

                int left_down = (ev.c & 0x1) != 0;
                int press_edge = left_down && !prev_left_down;
                prev_left_down = left_down;

                if (press_edge && ev.a >= 0 && ev.a < left_w && ev.b >= 4) {
                    int row = (ev.b - 4) / 12;
                    int idx = start + row;
                    if (idx >= 0 && idx < g_cmd_count) {
                        selected = idx;
                    }
                }
            }
        }

        draw_help_gui(h, selected);
        usleep(16000);
    }
}
