#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui.h>
#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Open the draw canvas editor.", "draw /images/sketch.rei");

#define REI_MAGIC 0x52454900u
#define REI_DEPTH_RGB 3u
#define DRAW_BG_R GUI_PAL_BG_R
#define DRAW_BG_G GUI_PAL_BG_G
#define DRAW_BG_B GUI_PAL_BG_B
#define STATUS_H 18
#define PANEL_W 190

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t reserved1;
    uint16_t reserved2;
} rei_header_t;

typedef enum {
    MODAL_NONE = 0,
    MODAL_CANVAS_SETUP = 1,
    MODAL_EXIT_CONFIRM = 2,
} modal_t;

typedef struct {
    int handle;
    int running;

    int content_w;
    int content_h;
    int viewport_w;
    int viewport_h;

    int canvas_w;
    int canvas_h;
    uint16_t* canvas;

    int canvas_area_x;
    int canvas_area_y;
    int canvas_area_w;
    int canvas_area_h;
    int canvas_draw_x;
    int canvas_draw_y;
    int canvas_draw_w;
    int canvas_draw_h;
    int canvas_scale;

    int left_down;
    int prev_left_down;
    int mouse_x;
    int mouse_y;
    int last_canvas_x;
    int last_canvas_y;
    int has_last_canvas;

    uint16_t brush;
    int brush_radius;
    int dirty;
    int saved_once;
    char save_path[256];
    char status_msg[128];

    modal_t modal;
    int setup_active_field;
    char setup_width[8];
    char setup_height[8];
} app_t;

static const int WHEEL_X24[24] = {
    1000, 966, 866, 707, 500, 259, 0, -259, -500, -707, -866, -966,
    -1000, -966, -866, -707, -500, -259, 0, 259, 500, 707, 866, 966
};

static const int WHEEL_Y24[24] = {
    0, 259, 500, 707, 866, 966, 1000, 966, 866, 707, 500, 259,
    0, -259, -500, -707, -866, -966, -1000, -966, -866, -707, -500, -259
};

static const uint16_t WHEEL_COLOURS[24] = {
    0xF800, 0xF940, 0xFA80, 0xFBC0, 0xFD40, 0xFEC0,
    0xFFE0, 0xCFE0, 0x9FE0, 0x5FE0, 0x1FE0, 0x07E0,
    0x07F0, 0x07FF, 0x069F, 0x053F, 0x03DF, 0x029F,
    0x019F, 0x009F, 0x209F, 0x409F, 0x809F, 0xC09F
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static uint8_t rgb565_r8(uint16_t c) {
    uint8_t v = (uint8_t)((c >> 11) & 0x1Fu);
    return (uint8_t)((v * 255u + 15u) / 31u);
}

static uint8_t rgb565_g8(uint16_t c) {
    uint8_t v = (uint8_t)((c >> 5) & 0x3Fu);
    return (uint8_t)((v * 255u + 31u) / 63u);
}

static uint8_t rgb565_b8(uint16_t c) {
    uint8_t v = (uint8_t)(c & 0x1Fu);
    return (uint8_t)((v * 255u + 15u) / 31u);
}

static int clampi(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int iabs(int value) {
    return value < 0 ? -value : value;
}

static void str_copy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_append(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0 || !src) return;
    int n = (int)strlen(dst);
    int i = 0;
    if (n >= cap - 1) return;
    while (src[i] && (n + i) < cap - 1) {
        dst[n + i] = src[i];
        ++i;
    }
    dst[n + i] = '\0';
}

static void str_append_int(char* dst, int cap, int value) {
    char tmp[16];
    int pos = 0;
    unsigned v;
    if (value < 0) {
        str_append(dst, cap, "-");
        v = (unsigned)(-value);
    } else {
        v = (unsigned)value;
    }
    if (v == 0u) {
        str_append(dst, cap, "0");
        return;
    }
    while (v > 0u && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (pos > 0) {
        char c[2];
        c[0] = tmp[--pos];
        c[1] = '\0';
        str_append(dst, cap, c);
    }
}

static void set_status(app_t* app, const char* msg) {
    if (!app) return;
    if (!msg) {
        app->status_msg[0] = '\0';
        return;
    }
    str_copy(app->status_msg, (int)sizeof(app->status_msg), msg);
}

static void clear_canvas(app_t* app) {
    if (!app || !app->canvas) return;
    size_t total = (size_t)app->canvas_w * (size_t)app->canvas_h;
    uint16_t white = rgb565(255, 255, 255);
    for (size_t i = 0; i < total; ++i) app->canvas[i] = white;
}

static int init_canvas(app_t* app, int width, int height) {
    if (!app) return -1;
    if (width < 16 || height < 16 || width > 640 || height > 480) return -1;

    size_t pixels = (size_t)width * (size_t)height;
    uint16_t* buf = (uint16_t*)realloc(app->canvas, pixels * sizeof(uint16_t));
    if (!buf) return -1;

    app->canvas = buf;
    app->canvas_w = width;
    app->canvas_h = height;
    clear_canvas(app);
    app->dirty = 0;
    app->saved_once = 0;
    app->has_last_canvas = 0;
    return 0;
}

static void paint_pixel(app_t* app, int x, int y, uint16_t colour) {
    if (!app || !app->canvas) return;
    if (x < 0 || y < 0 || x >= app->canvas_w || y >= app->canvas_h) return;
    app->canvas[(size_t)y * (size_t)app->canvas_w + (size_t)x] = colour;
}

static void paint_brush(app_t* app, int x, int y) {
    if (!app || !app->canvas) return;
    int r = app->brush_radius;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy <= r * r) {
                paint_pixel(app, x + dx, y + dy, app->brush);
            }
        }
    }
    app->dirty = 1;
}

static void paint_line(app_t* app, int x0, int y0, int x1, int y1) {
    int dx = iabs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -iabs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        paint_brush(app, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int screen_to_canvas(const app_t* app, int sx, int sy, int* out_x, int* out_y) {
    if (!app || !out_x || !out_y) return 0;
    if (sx < app->canvas_draw_x || sy < app->canvas_draw_y) return 0;
    if (sx >= app->canvas_draw_x + app->canvas_draw_w) return 0;
    if (sy >= app->canvas_draw_y + app->canvas_draw_h) return 0;
    int rel_x = sx - app->canvas_draw_x;
    int rel_y = sy - app->canvas_draw_y;
    int cx = rel_x / app->canvas_scale;
    int cy = rel_y / app->canvas_scale;
    if (cx < 0 || cy < 0 || cx >= app->canvas_w || cy >= app->canvas_h) return 0;
    *out_x = cx;
    *out_y = cy;
    return 1;
}

static void compute_layout(app_t* app) {
    if (!app) return;
    app->viewport_w = app->content_w;
    app->viewport_h = app->content_h - STATUS_H;
    if (app->viewport_h < 60) app->viewport_h = 60;

    app->canvas_area_x = 8;
    app->canvas_area_y = 8;
    app->canvas_area_w = app->viewport_w - PANEL_W - 24;
    app->canvas_area_h = app->viewport_h - 16;
    if (app->canvas_area_w < 60) app->canvas_area_w = 60;
    if (app->canvas_area_h < 60) app->canvas_area_h = 60;

    int scale_w = (app->canvas_area_w - 4) / app->canvas_w;
    int scale_h = (app->canvas_area_h - 4) / app->canvas_h;
    int scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    app->canvas_scale = scale;
    app->canvas_draw_w = app->canvas_w * scale;
    app->canvas_draw_h = app->canvas_h * scale;
    app->canvas_draw_x = app->canvas_area_x + (app->canvas_area_w - app->canvas_draw_w) / 2;
    app->canvas_draw_y = app->canvas_area_y + (app->canvas_area_h - app->canvas_draw_h) / 2;
}

static int save_canvas_rei(app_t* app) {
    if (!app || !app->canvas || !app->save_path[0]) return -1;

    rei_header_t hdr;
    hdr.magic = REI_MAGIC;
    hdr.width = (uint16_t)app->canvas_w;
    hdr.height = (uint16_t)app->canvas_h;
    hdr.depth = REI_DEPTH_RGB;
    hdr.reserved1 = 0;
    hdr.reserved2 = 0;

    int stream = eynfs_stream_begin(app->save_path);
    if (stream < 0) {
        set_status(app, "save failed (stream begin)");
        return -1;
    }

    if (eynfs_stream_write(stream, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        (void)eynfs_stream_end(stream);
        set_status(app, "save failed (header write)");
        return -1;
    }

    size_t row_bytes = (size_t)app->canvas_w * 3u;
    uint8_t* row = (uint8_t*)malloc(row_bytes);
    if (!row) {
        (void)eynfs_stream_end(stream);
        set_status(app, "save failed (oom)");
        return -1;
    }

    for (int y = 0; y < app->canvas_h; ++y) {
        for (int x = 0; x < app->canvas_w; ++x) {
            uint16_t c = app->canvas[(size_t)y * (size_t)app->canvas_w + (size_t)x];
            row[(size_t)x * 3u + 0u] = rgb565_r8(c);
            row[(size_t)x * 3u + 1u] = rgb565_g8(c);
            row[(size_t)x * 3u + 2u] = rgb565_b8(c);
        }
        if (eynfs_stream_write(stream, row, row_bytes) != (ssize_t)row_bytes) {
            free(row);
            (void)eynfs_stream_end(stream);
            set_status(app, "save failed (pixel write)");
            return -1;
        }
    }

    free(row);
    if (eynfs_stream_end(stream) != 0) {
        set_status(app, "save failed (stream end)");
        return -1;
    }

    app->dirty = 0;
    app->saved_once = 1;
    set_status(app, "saved");
    return 0;
}

static void draw_canvas_pixels(app_t* app) {
    if (!app || !app->canvas) return;
    for (int y = 0; y < app->canvas_h; ++y) {
        for (int x = 0; x < app->canvas_w; ++x) {
            uint16_t c = app->canvas[(size_t)y * (size_t)app->canvas_w + (size_t)x];
            gui_rect_t px = {
                .x = app->canvas_draw_x + x * app->canvas_scale,
                .y = app->canvas_draw_y + y * app->canvas_scale,
                .w = app->canvas_scale,
                .h = app->canvas_scale,
                .r = rgb565_r8(c),
                .g = rgb565_g8(c),
                .b = rgb565_b8(c),
                ._pad = 0,
            };
            (void)gui_fill_rect(app->handle, &px);
        }
    }
}

static int colour_wheel_pick(app_t* app, int mx, int my) {
    int panel_x = app->canvas_area_x + app->canvas_area_w + 8;
    int wheel_cx = panel_x + (PANEL_W - 24) / 2;
    int wheel_cy = 96;

    int best = -1;
    int best_dist = 1 << 30;
    for (int i = 0; i < 24; ++i) {
        int sx = wheel_cx + (WHEEL_X24[i] * 54) / 1000;
        int sy = wheel_cy + (WHEEL_Y24[i] * 54) / 1000;
        int dx = mx - sx;
        int dy = my - sy;
        int dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    if (best >= 0 && best_dist <= (14 * 14)) {
        app->brush = WHEEL_COLOURS[best];
        return 1;
    }
    return 0;
}

static int key_is_ctrl_q(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    return ch == 17u || key == 0x2101;
}

static void draw_setup_modal(app_t* app) {
    int box_w = 360;
    int box_h = 180;
    if (box_w > app->viewport_w - 20) box_w = app->viewport_w - 20;
    if (box_h > app->viewport_h - 20) box_h = app->viewport_h - 20;
    if (box_w < 220) box_w = 220;
    if (box_h < 140) box_h = 140;
    int x = (app->viewport_w - box_w) / 2;
    int y = (app->viewport_h - box_h) / 2;

    gui_rect_t fade = { .x = 0, .y = 0, .w = app->viewport_w, .h = app->viewport_h, .r = 0, .g = 0, .b = 0, ._pad = 0 };
    (void)gui_fill_rect(app->handle, &fade);

    gui_rect_t box = { .x = x, .y = y, .w = box_w, .h = box_h, .r = 40, .g = 48, .b = 70, ._pad = 0 };
    (void)gui_fill_rect(app->handle, &box);

    gui_text_t title = { .x = x + 12, .y = y + 10, .r = 255, .g = 232, .b = 160, ._pad = 0, .text = "Canvas size" };
    gui_text_t hint = { .x = x + 12, .y = y + 30, .r = 200, .g = 214, .b = 238, ._pad = 0, .text = "Type width/height, Tab to switch, Enter to start" };
    (void)gui_draw_text(app->handle, &title);
    (void)gui_draw_text(app->handle, &hint);

    gui_rect_t wf = { .x = x + 20, .y = y + 62, .w = 130, .h = 28,
                      .r = app->setup_active_field == 0 ? 86 : 62,
                      .g = app->setup_active_field == 0 ? 112 : 78,
                      .b = app->setup_active_field == 0 ? 170 : 110,
                      ._pad = 0 };
    gui_rect_t hf = { .x = x + 188, .y = y + 62, .w = 130, .h = 28,
                      .r = app->setup_active_field == 1 ? 86 : 62,
                      .g = app->setup_active_field == 1 ? 112 : 78,
                      .b = app->setup_active_field == 1 ? 170 : 110,
                      ._pad = 0 };
    (void)gui_fill_rect(app->handle, &wf);
    (void)gui_fill_rect(app->handle, &hf);

    gui_text_t wl = { .x = x + 20, .y = y + 50, .r = 220, .g = 225, .b = 235, ._pad = 0, .text = "Width" };
    gui_text_t hl = { .x = x + 188, .y = y + 50, .r = 220, .g = 225, .b = 235, ._pad = 0, .text = "Height" };
    gui_text_t wt = { .x = x + 28, .y = y + 71, .r = 255, .g = 255, .b = 255, ._pad = 0, .text = app->setup_width };
    gui_text_t ht = { .x = x + 196, .y = y + 71, .r = 255, .g = 255, .b = 255, ._pad = 0, .text = app->setup_height };
    (void)gui_draw_text(app->handle, &wl);
    (void)gui_draw_text(app->handle, &hl);
    (void)gui_draw_text(app->handle, &wt);
    (void)gui_draw_text(app->handle, &ht);

    gui_text_t lim = { .x = x + 20, .y = y + 108, .r = 186, .g = 204, .b = 226, ._pad = 0,
                       .text = "Allowed range: 16..640 x 16..480" };
    gui_text_t esc = { .x = x + 20, .y = y + 130, .r = 186, .g = 204, .b = 226, ._pad = 0,
                       .text = "Esc to cancel" };
    (void)gui_draw_text(app->handle, &lim);
    (void)gui_draw_text(app->handle, &esc);
}

static void draw_exit_modal(app_t* app) {
    int box_w = 380;
    int box_h = 120;
    int x = (app->viewport_w - box_w) / 2;
    int y = (app->viewport_h - box_h) / 2;

    gui_rect_t fade = { .x = 0, .y = 0, .w = app->viewport_w, .h = app->viewport_h, .r = 0, .g = 0, .b = 0, ._pad = 0 };
    (void)gui_fill_rect(app->handle, &fade);

    gui_rect_t box = { .x = x, .y = y, .w = box_w, .h = box_h, .r = 50, .g = 36, .b = 54, ._pad = 0 };
    (void)gui_fill_rect(app->handle, &box);

    gui_text_t t1 = { .x = x + 12, .y = y + 14, .r = 255, .g = 220, .b = 160, ._pad = 0,
                      .text = "Unsaved changes" };
    gui_text_t t2 = { .x = x + 12, .y = y + 40, .r = 235, .g = 235, .b = 240, ._pad = 0,
                      .text = "Press S to save and exit, D to discard, C to continue editing" };
    (void)gui_draw_text(app->handle, &t1);
    (void)gui_draw_text(app->handle, &t2);
}

static void draw_ui(app_t* app) {
    (void)gui_begin(app->handle);
    gui_rgb_t bg = { .r = DRAW_BG_R, .g = DRAW_BG_G, .b = DRAW_BG_B, ._pad = 0 };
    (void)gui_clear(app->handle, &bg);

    gui_rect_t canvas_area = {
        .x = app->canvas_area_x,
        .y = app->canvas_area_y,
        .w = app->canvas_area_w,
        .h = app->canvas_area_h,
        .r = 34,
        .g = 36,
        .b = 44,
        ._pad = 0,
    };
    (void)gui_fill_rect(app->handle, &canvas_area);

    gui_rect_t canvas_border = {
        .x = app->canvas_draw_x - 1,
        .y = app->canvas_draw_y - 1,
        .w = app->canvas_draw_w + 2,
        .h = app->canvas_draw_h + 2,
        .r = 90,
        .g = 98,
        .b = 122,
        ._pad = 0,
    };
    (void)gui_fill_rect(app->handle, &canvas_border);

    draw_canvas_pixels(app);

    int panel_x = app->canvas_area_x + app->canvas_area_w + 8;
    gui_rect_t panel = {
        .x = panel_x,
        .y = 8,
        .w = PANEL_W - 8,
        .h = app->viewport_h - 16,
        .r = 26,
        .g = 31,
        .b = 46,
        ._pad = 0,
    };
    (void)gui_fill_rect(app->handle, &panel);

    gui_text_t ptitle = { .x = panel_x + 10, .y = 14, .r = 250, .g = 225, .b = 160, ._pad = 0, .text = "Colour wheel" };
    (void)gui_draw_text(app->handle, &ptitle);

    int wheel_cx = panel_x + (PANEL_W - 24) / 2;
    int wheel_cy = 96;
    for (int i = 0; i < 24; ++i) {
        int sx = wheel_cx + (WHEEL_X24[i] * 54) / 1000;
        int sy = wheel_cy + (WHEEL_Y24[i] * 54) / 1000;
        uint16_t c = WHEEL_COLOURS[i];
        gui_rect_t sw = {
            .x = sx - 6,
            .y = sy - 6,
            .w = 12,
            .h = 12,
            .r = rgb565_r8(c),
            .g = rgb565_g8(c),
            .b = rgb565_b8(c),
            ._pad = 0,
        };
        (void)gui_fill_rect(app->handle, &sw);
    }

    gui_rect_t preview = {
        .x = panel_x + 36,
        .y = 176,
        .w = 80,
        .h = 44,
        .r = rgb565_r8(app->brush),
        .g = rgb565_g8(app->brush),
        .b = rgb565_b8(app->brush),
        ._pad = 0,
    };
    (void)gui_fill_rect(app->handle, &preview);

    gui_text_t plabel = { .x = panel_x + 10, .y = 162, .r = 210, .g = 220, .b = 240, ._pad = 0, .text = "Current brush" };
    gui_text_t bsize = { .x = panel_x + 10, .y = 232, .r = 186, .g = 204, .b = 226, ._pad = 0, .text = "[ / ] brush size" };
    (void)gui_draw_text(app->handle, &plabel);
    (void)gui_draw_text(app->handle, &bsize);

    char bline[64];
    bline[0] = '\0';
    str_append(bline, (int)sizeof(bline), "size: ");
    str_append_int(bline, (int)sizeof(bline), app->brush_radius * 2 + 1);
    str_append(bline, (int)sizeof(bline), " px");
    gui_text_t bval = { .x = panel_x + 10, .y = 246, .r = 220, .g = 228, .b = 244, ._pad = 0, .text = bline };
    (void)gui_draw_text(app->handle, &bval);

    gui_rect_t status_bg = {
        .x = 0,
        .y = app->viewport_h,
        .w = app->content_w,
        .h = STATUS_H,
        .r = 24,
        .g = 30,
        .b = 44,
        ._pad = 0,
    };
    (void)gui_fill_rect(app->handle, &status_bg);

    char status[220];
    status[0] = '\0';
    str_copy(status, (int)sizeof(status), app->save_path);
    str_append(status, (int)sizeof(status), " | ");
    str_append(status, (int)sizeof(status), app->dirty ? "modified" : "saved");
    str_append(status, (int)sizeof(status), " | Ctrl+S save | Esc/Ctrl+Q exit | click+drag paint");
    gui_text_t st1 = { .x = 6, .y = app->viewport_h + 5, .r = 216, .g = 226, .b = 244, ._pad = 0, .text = status };
    (void)gui_draw_text(app->handle, &st1);

    gui_text_t st2 = { .x = app->content_w / 2, .y = app->viewport_h + 5, .r = 170, .g = 188, .b = 218, ._pad = 0,
                       .text = app->status_msg[0] ? app->status_msg : "ready" };
    (void)gui_draw_text(app->handle, &st2);

    if (app->modal == MODAL_CANVAS_SETUP) {
        draw_setup_modal(app);
    } else if (app->modal == MODAL_EXIT_CONFIRM) {
        draw_exit_modal(app);
    }

    (void)gui_present(app->handle);
}

static void setup_append_char(char* dst, size_t cap, char ch) {
    size_t n;
    if (!dst || cap == 0) return;
    n = strlen(dst);
    if (n + 1 >= cap) return;
    dst[n] = ch;
    dst[n + 1] = '\0';
}

static void setup_backspace(char* dst) {
    size_t n;
    if (!dst) return;
    n = strlen(dst);
    if (n == 0) return;
    dst[n - 1] = '\0';
}

static int parse_positive_int(const char* s, int* out) {
    if (!s || !s[0] || !out) return -1;
    int value = 0;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        value = value * 10 + (s[i] - '0');
        if (value > 2000) return -1;
    }
    *out = value;
    return 0;
}

static int key_to_ascii(int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    if (ch >= 32u && ch <= 126u) return (int)ch;
    if (ch == '\t' || ch == '\n' || ch == '\r' || ch == 8u || ch == 127u || ch == 27u) return (int)ch;
    return 0;
}

static void handle_setup_key(app_t* app, int key) {
    if (!app) return;
    int ch = key_to_ascii(key);
    int base = key & 0x0FFF;
    char* active = app->setup_active_field == 0 ? app->setup_width : app->setup_height;

    if (ch == 27 || key == 27) {
        app->running = 0;
        return;
    }

    if (ch == '\t') {
        app->setup_active_field = 1 - app->setup_active_field;
        return;
    }

    if (key == 0x1003 || base == 0x1003) {
        app->setup_active_field = 0;
        return;
    }
    if (key == 0x1004 || base == 0x1004) {
        app->setup_active_field = 1;
        return;
    }

    if (ch == 8 || ch == 127) {
        setup_backspace(active);
        return;
    }

    if (ch == '\n' || ch == '\r' || key == 10 || key == 13) {
        int w = 0;
        int h = 0;
        if (parse_positive_int(app->setup_width, &w) != 0 || parse_positive_int(app->setup_height, &h) != 0) {
            set_status(app, "invalid width/height");
            return;
        }
        if (init_canvas(app, w, h) != 0) {
            set_status(app, "canvas must be 16..640 x 16..480");
            return;
        }
        app->modal = MODAL_NONE;
        set_status(app, "canvas ready");
        return;
    }

    if (ch >= '0' && ch <= '9') {
        setup_append_char(active, 8, (char)ch);
    }
}

static void handle_exit_confirm_key(app_t* app, int key) {
    if (!app) return;
    unsigned ch = (unsigned)key & 0xFFu;
    if (ch == 's' || ch == 'S') {
        if (save_canvas_rei(app) == 0) {
            app->running = 0;
        }
    } else if (ch == 'd' || ch == 'D') {
        app->running = 0;
    } else if (ch == 'c' || ch == 'C' || ch == 27u) {
        app->modal = MODAL_NONE;
        set_status(app, "exit cancelled");
    }
}

static void request_exit(app_t* app) {
    if (!app) return;
    if (app->dirty) {
        app->modal = MODAL_EXIT_CONFIRM;
        return;
    }
    app->running = 0;
}

static void handle_main_key(app_t* app, int key) {
    if (!app) return;
    unsigned ch = (unsigned)key & 0xFFu;

    if (ch == 19u || key == 0x2001) {
        (void)save_canvas_rei(app);
        return;
    }
    if (ch == 27u || key_is_ctrl_q(key)) {
        request_exit(app);
        return;
    }

    if (ch == '[') {
        app->brush_radius = clampi(app->brush_radius - 1, 1, 8);
    } else if (ch == ']') {
        app->brush_radius = clampi(app->brush_radius + 1, 1, 8);
    }
}

static void handle_main_mouse(app_t* app, const gui_event_t* ev) {
    if (!app || !ev) return;

    int left = (ev->c & 0x1) != 0;
    int press_edge = left && !app->prev_left_down;
    int release_edge = (!left) && app->prev_left_down;
    app->prev_left_down = left;
    app->left_down = left;
    app->mouse_x = ev->a;
    app->mouse_y = ev->b;

    if (press_edge) {
        if (colour_wheel_pick(app, ev->a, ev->b)) {
            set_status(app, "brush colour changed");
            app->has_last_canvas = 0;
            return;
        }
    }

    int cx = 0;
    int cy = 0;
    if (!screen_to_canvas(app, ev->a, ev->b, &cx, &cy)) {
        if (release_edge) app->has_last_canvas = 0;
        return;
    }

    if (press_edge) {
        paint_brush(app, cx, cy);
        app->last_canvas_x = cx;
        app->last_canvas_y = cy;
        app->has_last_canvas = 1;
    } else if (left && app->has_last_canvas) {
        paint_line(app, app->last_canvas_x, app->last_canvas_y, cx, cy);
        app->last_canvas_x = cx;
        app->last_canvas_y = cy;
    } else if (release_edge) {
        app->has_last_canvas = 0;
    }
}

static void usage(void) {
    puts("Usage: draw [output.rei]");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    app_t app;
    memset(&app, 0, sizeof(app));
    app.brush = rgb565(0, 0, 0);
    app.brush_radius = 1;
    app.handle = -1;
    app.modal = MODAL_CANVAS_SETUP;
    app.running = 1;
    app.setup_active_field = 0;
    str_copy(app.setup_width, (int)sizeof(app.setup_width), "320");
    str_copy(app.setup_height, (int)sizeof(app.setup_height), "200");
    str_copy(app.save_path, (int)sizeof(app.save_path), (argc >= 2 && argv[1] && argv[1][0]) ? argv[1] : "/images/draw.rei");
    set_status(&app, "set canvas size");

    app.handle = gui_attach("Draw", "Ctrl+S save | Esc/Ctrl+Q exit | drag to paint");
    if (app.handle < 0) {
        puts("draw: gui_attach failed");
        return 1;
    }
    (void)gui_set_continuous_redraw(app.handle, 1);

    while (app.running) {
        gui_size_t sz = {0, 0};
        (void)gui_get_content_size(app.handle, &sz);
        if (sz.w <= 0) sz.w = 980;
        if (sz.h <= 0) sz.h = 620;
        app.content_w = sz.w;
        app.content_h = sz.h;

        if (app.canvas_w <= 0 || app.canvas_h <= 0) {
            app.canvas_w = 320;
            app.canvas_h = 200;
        }
        compute_layout(&app);

        gui_event_t ev;
        while (gui_poll_event(app.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                if (app.modal == MODAL_CANVAS_SETUP) {
                    handle_setup_key(&app, ev.a);
                } else if (app.modal == MODAL_EXIT_CONFIRM) {
                    handle_exit_confirm_key(&app, ev.a);
                } else {
                    handle_main_key(&app, ev.a);
                }
            } else if (ev.type == GUI_EVENT_CLOSE) {
                if (app.modal == MODAL_NONE) {
                    request_exit(&app);
                }
            } else if (ev.type == GUI_EVENT_MOUSE) {
                if (app.modal == MODAL_NONE) {
                    handle_main_mouse(&app, &ev);
                }
            }
        }

        draw_ui(&app);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(app.handle, 0);
    if (app.canvas) free(app.canvas);
    return 0;
}
