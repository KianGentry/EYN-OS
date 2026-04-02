#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <gui.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

#include "view_backend_protocol.h"

EYN_CMDMETA_V1("Dispatch a file to a view backend from /.view.", "view /images/picture.rei");

#define VIEW_BACKEND_DIR "/.view"
#define VIEW_MAX_BACKENDS 32
#define VIEW_NAME_MAX 32
#define VIEW_EXTS_MAX 128
#define VIEW_EXEC_MAX 128
#define VIEW_PROTO_MAX 16
#define VIEW_MANIFEST_MAX 1024
#define VIEW_HEAD_PROBE_MAX 64

#define VIEW_STATUS_H 18
#define VIEW_BG_R GUI_PAL_BG_R
#define VIEW_BG_G GUI_PAL_BG_G
#define VIEW_BG_B GUI_PAL_BG_B

typedef struct {
    char name[VIEW_NAME_MAX];
    char exts[VIEW_EXTS_MAX];
    char exec_path[VIEW_EXEC_MAX];
    char protocol[VIEW_PROTO_MAX];
    int has_magic16;
    uint16_t magic16;
    int has_magic32;
    uint32_t magic32;
    int has_submagic32;
    uint32_t submagic32;
    uint32_t submagic_off;
} view_backend_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t* pixels;
} view_image_t;

typedef struct {
    int handle;
    int running;

    int content_w;
    int content_h;
    int viewport_w;
    int viewport_h;
    int blit_w;
    int blit_h;

    uint16_t* viewbuf;
    size_t viewbuf_cap;

    int zoom_permille;
    int origin_x;
    int origin_y;

    int prev_left_down;
    int dragging;
    int drag_last_x;
    int drag_last_y;
} view_app_t;

static void usage(void) {
    puts("Usage: view <file>");
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static int clampi(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void str_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_trim_inplace(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[n - 1] = '\0';
        --n;
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
}

static int str_ends_with(const char* s, const char* suffix) {
    size_t a = s ? strlen(s) : 0;
    size_t b = suffix ? strlen(suffix) : 0;
    if (a < b) return 0;
    return strcmp(s + a - b, suffix) == 0;
}

static int read_small_file(const char* path, char* out, size_t cap) {
    if (!path || !out || cap == 0) return -1;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t got = read(fd, out + total, cap - 1 - total);
        if (got < 0) {
            close(fd);
            return -1;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(fd);
    out[total] = '\0';
    return 0;
}

static int read_exact_fd(int fd, void* out_buf, size_t len) {
    uint8_t* out = (uint8_t*)out_buf;
    size_t total = 0;
    while (total < len) {
        ssize_t got = read(fd, out + total, len - total);
        if (got <= 0) return -1;
        total += (size_t)got;
    }
    return 0;
}

static int parse_u32_hex(const char* s, uint32_t* out) {
    if (!s || !out) return -1;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_backend_manifest(const char* manifest_path, view_backend_t* out) {
    if (!manifest_path || !out) return -1;

    char buf[VIEW_MANIFEST_MAX];
    if (read_small_file(manifest_path, buf, sizeof(buf)) != 0) return -1;

    memset(out, 0, sizeof(*out));
    str_copy(out->protocol, sizeof(out->protocol), "legacy");

    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        str_trim_inplace(line);
        if (line[0] && line[0] != '#') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char* key = line;
                char* val = eq + 1;
                str_trim_inplace(key);
                str_trim_inplace(val);

                if (strcmp(key, "name") == 0) {
                    str_copy(out->name, sizeof(out->name), val);
                } else if (strcmp(key, "extensions") == 0) {
                    str_copy(out->exts, sizeof(out->exts), val);
                } else if (strcmp(key, "exec") == 0) {
                    str_copy(out->exec_path, sizeof(out->exec_path), val);
                } else if (strcmp(key, "protocol") == 0) {
                    str_copy(out->protocol, sizeof(out->protocol), val);
                } else if (strcmp(key, "magic16") == 0) {
                    uint32_t v = 0;
                    if (parse_u32_hex(val, &v) == 0) {
                        out->has_magic16 = 1;
                        out->magic16 = (uint16_t)v;
                    }
                } else if (strcmp(key, "magic32") == 0) {
                    uint32_t v = 0;
                    if (parse_u32_hex(val, &v) == 0) {
                        out->has_magic32 = 1;
                        out->magic32 = v;
                    }
                } else if (strncmp(key, "submagic32@", 11) == 0) {
                    uint32_t off = 0;
                    uint32_t val32 = 0;
                    if (parse_u32_hex(key + 11, &off) == 0 && parse_u32_hex(val, &val32) == 0) {
                        out->has_submagic32 = 1;
                        out->submagic_off = off;
                        out->submagic32 = val32;
                    }
                }
            }
        }

        if (!nl) break;
        line = nl + 1;
    }

    if (!out->name[0]) return -1;
    if (!out->exec_path[0]) return -1;
    if (!out->exts[0] && !out->has_magic16 && !out->has_magic32 && !out->has_submagic32) return -1;
    return 0;
}

static int file_matches_extensions(const char* path, const char* csv_exts) {
    if (!path || !csv_exts || !csv_exts[0]) return 1;

    char work[VIEW_EXTS_MAX];
    str_copy(work, sizeof(work), csv_exts);

    char* tok = work;
    while (tok && *tok) {
        char* comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        str_trim_inplace(tok);
        if (tok[0] && str_ends_with(path, tok)) return 1;
        tok = comma ? (comma + 1) : NULL;
    }
    return 0;
}

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int load_probe_head(const char* path, uint8_t* head, size_t cap, size_t* out_len) {
    if (!path || !head || cap == 0 || !out_len) return -1;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    ssize_t n = read(fd, head, cap);
    close(fd);
    if (n < 0) return -1;
    *out_len = (size_t)n;
    return 0;
}

static int backend_matches_file(const view_backend_t* b, const char* path, const uint8_t* head, size_t head_len) {
    if (!b || !path) return 0;

    int has_rule = 0;
    int matched = 0;

    if (b->exts[0]) {
        has_rule = 1;
        if (file_matches_extensions(path, b->exts)) matched = 1;
    }

    if (b->has_magic16) {
        has_rule = 1;
        if (head_len >= 2 && read_u16_le(head) == b->magic16) matched = 1;
    }

    if (b->has_magic32) {
        has_rule = 1;
        if (head_len >= 4 && read_u32_le(head) == b->magic32) matched = 1;
    }

    if (b->has_submagic32) {
        has_rule = 1;
        if (head_len >= (size_t)b->submagic_off + 4u &&
            read_u32_le(head + b->submagic_off) == b->submagic32) {
            matched = 1;
        }
    }

    return (has_rule && matched) ? 1 : 0;
}

static int scan_backends(view_backend_t* out, int max_out) {
    if (!out || max_out <= 0) return 0;

    int fd = open(VIEW_BACKEND_DIR, O_RDONLY, 0);
    if (fd < 0) return 0;

    typedef struct {
        uint8_t is_dir;
        uint8_t _pad[3];
        uint32_t size;
        char name[56];
    } view_dirent_t;

    view_dirent_t ents[16];
    int count = 0;

    for (;;) {
        int rc = getdents(fd, (void*)ents, sizeof(ents));
        if (rc <= 0) break;
        int n = rc / (int)sizeof(view_dirent_t);
        for (int i = 0; i < n && count < max_out; ++i) {
            if (ents[i].is_dir) continue;
            if (!ents[i].name[0]) continue;
            if (!str_ends_with(ents[i].name, ".backend")) continue;

            char manifest_path[160];
            snprintf(manifest_path, sizeof(manifest_path), "%s/%s", VIEW_BACKEND_DIR, ents[i].name);

            if (parse_backend_manifest(manifest_path, &out[count]) == 0) {
                count++;
            }
        }
    }

    close(fd);
    return count;
}

static int run_backend_spawn(const char* exec_path, const char* arg1, const char* arg2) {
    if (!exec_path || !exec_path[0]) return -1;

    const char* argv_local[3];
    int argc_local = 0;
    if (arg1 && arg1[0]) argv_local[argc_local++] = arg1;
    if (arg2 && arg2[0]) argv_local[argc_local++] = arg2;

    int pid = spawn(exec_path, argv_local, argc_local);
    if (pid <= 0) return -1;

    int status = 0;
    int waited = waitpid(pid, &status, 0);
    if (waited <= 0) return -1;
    return (status == 0) ? 0 : -1;
}

static int run_legacy_backend(const view_backend_t* b, const char* path) {
    return run_backend_spawn(b->exec_path, path, NULL);
}

static int launch_frame_backend(const view_backend_t* b, const char* path, const char* out_frame_path) {
    if (!b || !b->exec_path[0] || !path || !out_frame_path) return -1;

    const char* argv_local[2];
    argv_local[0] = path;
    argv_local[1] = out_frame_path;

    int pid = spawn(b->exec_path, argv_local, 2);
    return (pid > 0) ? pid : -1;
}

static int load_frame_file(const char* path, view_image_t* out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    view_frame_header_t hdr;
    if (read_exact_fd(fd, &hdr, sizeof(hdr)) != 0) {
        close(fd);
        return -1;
    }

    if (hdr.magic != VIEW_FRAME_MAGIC || hdr.format != VIEW_FRAME_FMT_RGB565) {
        close(fd);
        return -1;
    }

    if (hdr.width == 0 || hdr.height == 0) {
        close(fd);
        return -1;
    }

    uint32_t expect = (uint32_t)hdr.width * (uint32_t)hdr.height * 2u;
    if (expect == 0 || hdr.data_bytes != expect) {
        close(fd);
        return -1;
    }

    uint16_t* pixels = (uint16_t*)malloc((size_t)hdr.data_bytes);
    if (!pixels) {
        close(fd);
        return -1;
    }

    if (read_exact_fd(fd, pixels, (size_t)hdr.data_bytes) != 0) {
        free(pixels);
        close(fd);
        return -1;
    }

    close(fd);
    out->width = hdr.width;
    out->height = hdr.height;
    out->pixels = pixels;
    return 0;
}

static int ensure_viewbuf(view_app_t* app) {
    if (!app) return -1;
    if (app->blit_w <= 0 || app->blit_h <= 0) return -1;

    size_t need_px = (size_t)app->blit_w * (size_t)app->blit_h;
    if (need_px <= app->viewbuf_cap && app->viewbuf) return 0;

    uint16_t* grown = (uint16_t*)realloc(app->viewbuf, need_px * sizeof(uint16_t));
    if (!grown) return -1;
    app->viewbuf = grown;
    app->viewbuf_cap = need_px;
    return 0;
}

static void reset_view_transform(view_app_t* app, int src_w, int src_h) {
    if (!app || src_w <= 0 || src_h <= 0 || app->viewport_w <= 0 || app->viewport_h <= 0) return;

    int zoom_w = (app->viewport_w * 1000) / src_w;
    int zoom_h = (app->viewport_h * 1000) / src_h;
    int zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom < 50) zoom = 50;
    if (zoom > 6000) zoom = 6000;

    app->zoom_permille = zoom;
    int scaled_w = (src_w * zoom + 999) / 1000;
    int scaled_h = (src_h * zoom + 999) / 1000;
    app->origin_x = (app->viewport_w - scaled_w) / 2;
    app->origin_y = (app->viewport_h - scaled_h) / 2;
}

static void zoom_view(view_app_t* app, int src_w, int src_h, int zoom_in) {
    if (!app || src_w <= 0 || src_h <= 0) return;
    int old_zoom = app->zoom_permille;
    int next_zoom = zoom_in ? (old_zoom * 5) / 4 : (old_zoom * 4) / 5;
    next_zoom = clampi(next_zoom, 50, 8000);
    if (next_zoom == old_zoom) return;

    int cx = app->viewport_w / 2;
    int cy = app->viewport_h / 2;

    int src_cx = ((cx - app->origin_x) * 1000) / old_zoom;
    int src_cy = ((cy - app->origin_y) * 1000) / old_zoom;

    app->zoom_permille = next_zoom;
    app->origin_x = cx - (src_cx * next_zoom) / 1000;
    app->origin_y = cy - (src_cy * next_zoom) / 1000;
}

static void render_framebuffer(view_app_t* app, const uint16_t* src, int src_w, int src_h) {
    if (!app || !src || !app->viewbuf) return;
    if (app->blit_w <= 0 || app->blit_h <= 0) return;

    uint16_t bg = rgb565(VIEW_BG_R, VIEW_BG_G, VIEW_BG_B);
    size_t total = (size_t)app->blit_w * (size_t)app->blit_h;
    for (size_t i = 0; i < total; ++i) app->viewbuf[i] = bg;

    int zoom = app->zoom_permille;
    if (zoom <= 0) zoom = 1000;

    int sx_table[320];
    int ncols = (app->blit_w < 320) ? app->blit_w : 320;
    for (int bx = 0; bx < ncols; ++bx) {
        int vpx = (app->blit_w == app->viewport_w)
                  ? bx
                  : (bx * app->viewport_w / app->blit_w);
        int sx = ((vpx - app->origin_x) * 1000) / zoom;
        sx_table[bx] = (sx >= 0 && sx < src_w) ? sx : -1;
    }

    for (int by = 0; by < app->blit_h; ++by) {
        int vpy = (app->blit_h == app->viewport_h)
                  ? by
                  : (by * app->viewport_h / app->blit_h);
        int sy = ((vpy - app->origin_y) * 1000) / zoom;
        if (sy < 0 || sy >= src_h) continue;

        size_t row_off = (size_t)by * (size_t)app->blit_w;
        size_t src_row = (size_t)sy * (size_t)src_w;
        for (int bx = 0; bx < ncols; ++bx) {
            int sx = sx_table[bx];
            if (sx < 0) continue;
            app->viewbuf[row_off + (size_t)bx] = src[src_row + (size_t)sx];
        }
    }
}

static int key_is_zoom_in(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '+' || ch == '=' || key == 0x2102;
}

static int key_is_zoom_out(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == '-' || ch == '_' || key == 0x2103;
}

static void draw_status(view_app_t* app, const char* path, const view_backend_t* backend) {
    gui_rect_t status_bg = {
        .x = 0,
        .y = app->viewport_h,
        .w = app->content_w,
        .h = VIEW_STATUS_H,
        .r = GUI_PAL_STATUS_R,
        .g = GUI_PAL_STATUS_G,
        .b = GUI_PAL_STATUS_B,
        ._pad = 0
    };
    (void)gui_fill_rect(app->handle, &status_bg);

    char left[128];
    char right[128];
    snprintf(left, sizeof(left), "%s | backend %s | zoom %d%%", path, backend->name, app->zoom_permille / 10);
    str_copy(right, sizeof(right), "+/- zoom  wheel zoom  arrows/mouse pan  Esc quit");

    gui_text_t t1 = { .x = 6, .y = app->viewport_h + 5, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = left };
    gui_text_t t2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = right };
    (void)gui_draw_text(app->handle, &t1);
    (void)gui_draw_text(app->handle, &t2);
}

static int render_frame_image(const char* path, const view_backend_t* backend, const view_image_t* image) {
    if (!path || !backend || !image || !image->pixels || image->width == 0 || image->height == 0) return -1;

    view_app_t app;
    memset(&app, 0, sizeof(app));

    app.handle = gui_create("View", "Esc quit | +/- zoom | wheel zoom | arrows/mouse pan");
    if (app.handle < 0) {
        puts("view: gui_create failed");
        return -1;
    }
    printf("view: gui_create ok (handle=%d)\n", app.handle);

    (void)gui_set_continuous_redraw(app.handle, 1);
    app.running = 1;
    int loop_count = 0;

    while (app.running) {
        loop_count++;
        gui_size_t sz = {0, 0};
        (void)gui_get_content_size(app.handle, &sz);
        if (sz.w <= 0) sz.w = 920;
        if (sz.h <= 0) sz.h = 560;

        app.content_w = sz.w;
        app.content_h = sz.h;
        app.viewport_w = sz.w;
        app.viewport_h = sz.h - VIEW_STATUS_H;
        if (app.viewport_h < 40) app.viewport_h = 40;

        app.blit_w = app.viewport_w > 320 ? 320 : app.viewport_w;
        app.blit_h = app.viewport_h > 200 ? 200 : app.viewport_h;

        if (ensure_viewbuf(&app) != 0) {
            puts("view: failed to allocate render buffer");
            (void)gui_set_continuous_redraw(app.handle, 0);
            if (app.viewbuf) free(app.viewbuf);
            return -1;
        }

        if (app.zoom_permille <= 0) {
            reset_view_transform(&app, (int)image->width, (int)image->height);
        }

        gui_event_t ev;
        while (gui_poll_event(app.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                puts("view: GUI_EVENT_CLOSE received");
                app.running = 0;
                break;
            }

            if (ev.type == GUI_EVENT_KEY) {
                int base = ev.a & 0x0FFF;
                unsigned ch = (unsigned)ev.a & 0xFFu;

                if (ev.a == 27 || ch == 27u || ch == 'q' || ch == 'Q') {
                    app.running = 0;
                    break;
                }

                if (key_is_zoom_in(ev.a)) {
                    zoom_view(&app, (int)image->width, (int)image->height, 1);
                } else if (key_is_zoom_out(ev.a)) {
                    zoom_view(&app, (int)image->width, (int)image->height, 0);
                } else if (ev.a == 0x1001 || base == 0x1001) {
                    app.origin_y += 20;
                } else if (ev.a == 0x1002 || base == 0x1002) {
                    app.origin_y -= 20;
                } else if (ev.a == 0x1003 || base == 0x1003) {
                    app.origin_x += 20;
                } else if (ev.a == 0x1004 || base == 0x1004) {
                    app.origin_x -= 20;
                } else if (ch == '0') {
                    reset_view_transform(&app, (int)image->width, (int)image->height);
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                int left_down = (ev.c & 0x1) != 0;
                int press_edge = left_down && !app.prev_left_down;
                int release_edge = (!left_down) && app.prev_left_down;

                if (ev.d > 0) {
                    zoom_view(&app, (int)image->width, (int)image->height, 1);
                } else if (ev.d < 0) {
                    zoom_view(&app, (int)image->width, (int)image->height, 0);
                }

                if (press_edge) {
                    app.dragging = 1;
                    app.drag_last_x = ev.a;
                    app.drag_last_y = ev.b;
                } else if (left_down && app.dragging) {
                    int dx = ev.a - app.drag_last_x;
                    int dy = ev.b - app.drag_last_y;
                    app.origin_x += dx;
                    app.origin_y += dy;
                    app.drag_last_x = ev.a;
                    app.drag_last_y = ev.b;
                } else if (release_edge) {
                    app.dragging = 0;
                }

                app.prev_left_down = left_down;
            }
        }

        render_framebuffer(&app, image->pixels, (int)image->width, (int)image->height);

        if (gui_begin(app.handle) != 0) {
            puts("view: gui_begin failed");
            (void)gui_set_continuous_redraw(app.handle, 0);
            if (app.viewbuf) free(app.viewbuf);
            return -1;
        }
        gui_blit_rgb565_t blit = {
            .src_w = app.blit_w,
            .src_h = app.blit_h,
            .pixels = app.viewbuf,
            .dst_w = app.viewport_w,
            .dst_h = app.viewport_h,
        };
        if (gui_blit_rgb565(app.handle, &blit) != 0) {
            puts("view: gui_blit_rgb565 failed");
            (void)gui_set_continuous_redraw(app.handle, 0);
            if (app.viewbuf) free(app.viewbuf);
            return -1;
        }
        draw_status(&app, path, backend);
        if (gui_present(app.handle) != 0) {
            puts("view: gui_present failed");
            (void)gui_set_continuous_redraw(app.handle, 0);
            if (app.viewbuf) free(app.viewbuf);
            return -1;
        }

        if (loop_count == 1) {
            puts("view: first frame presented");
        }

        usleep(16000);
    }

    (void)gui_set_continuous_redraw(app.handle, 0);
    if (app.viewbuf) free(app.viewbuf);
    printf("view: render loop exited after %d iterations\n", loop_count);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    const char* path = argv[1];

    view_backend_t backends[VIEW_MAX_BACKENDS];
    int backend_count = scan_backends(backends, VIEW_MAX_BACKENDS);
    if (backend_count <= 0) {
        puts("view: no valid backends found in /.view");
        return 1;
    }

    uint8_t head[VIEW_HEAD_PROBE_MAX];
    size_t head_len = 0;
    if (load_probe_head(path, head, sizeof(head), &head_len) != 0) {
        head_len = 0;
    }

    for (int i = 0; i < backend_count; ++i) {
        view_backend_t* b = &backends[i];
        if (!backend_matches_file(b, path, head, head_len)) continue;

        if (strcmp(b->exec_path, "/binaries/view") == 0) {
            printf("view: backend '%s' misconfigured (exec points to /binaries/view)\n", b->name);
            return 1;
        }

        if (strcmp(b->protocol, "frame-v1") == 0) {
            printf("view: using frame-v1 backend '%s'\n", b->name);
            const char* frame_path = "/images/view.frame";
            view_image_t image;
            memset(&image, 0, sizeof(image));

            printf("view: trying frame path %s\n", frame_path);
            int pid = launch_frame_backend(b, path, frame_path);
            if (pid <= 0) {
                printf("view: backend '%s' failed to launch\n", b->name);
                return 1;
            }

            int succeeded = 0;
            for (int retry = 0; retry < 2000; ++retry) {
                if (load_frame_file(frame_path, &image) == 0) {
                    printf("view: loaded frame %ux%u from %s\n", (unsigned)image.width, (unsigned)image.height, frame_path);
                    succeeded = 1;
                    break;
                }
                usleep(5000);
            }

            if (!succeeded) {
                printf("view: backend '%s' failed\n", b->name);
                return 1;
            }

            int rc = render_frame_image(path, b, &image);
            free(image.pixels);
            if (rc != 0) {
                printf("view: render failed for backend '%s'\n", b->name);
                return 1;
            }
            return 0;
        }

        if (run_legacy_backend(b, path) != 0) {
            printf("view: backend '%s' failed\n", b->name);
            return 1;
        }
        return 0;
    }

    puts("view: no installed backend supports this file");
    puts("view: available backends:");
    for (int i = 0; i < backend_count; ++i) {
        printf("  - %s\n", backends[i].name);
    }
    return 1;
}
