#include <types.h>
#include <vga.h>
#include <tui.h>
#include <terminals.h>
#include <string.h>
#include <tile_manager.h>
#include <mouse.h>
#include <rei.h>
#include <eynfs.h>
#include <utilities/util.h>

#define MAX_TILES 4
#define MAX_WINDOWS 8

typedef enum { TILE_EMPTY = 0, TILE_SHELL, TILE_EDITOR } tile_type_t;

typedef struct {
    tile_type_t type;
    const char* title;
    const char* status_left;
    const char* status_right;
    int x, y, width, height; // pixel coords
    int term_idx; // which virtual terminal index
    int active;
    int status_visible; // whether the status bar is normally visible for this tile
    int static_drawn; // whether static UI (title/status/border) has been drawn into backbuffer
    unsigned int last_drawn_version; // for incremental content redraw
    // cache of last content rect used for rendering (excludes title/status/borders)
    int last_cx, last_cy, last_cw, last_ch;
    // caches to detect decoration changes without redrawing every frame
    const char* last_title_ptr;
    const char* last_status_left_ptr;
    const char* last_status_right_ptr;
    int last_focused;
    int last_show_status;
} tile_t;

static tile_t tiles[MAX_TILES];
static int tile_count = 0;
static int focused = 0; // index of focused tile (0..tile_count-1)
// Track if any tile content was redrawn this frame so we can composite windows on top
static int g_any_tile_content_redrew = 0;
// global scroll tick used for marquee/scrolling text
static int g_tile_scroll_tick = 0;
// Force a full redraw of all tiles once (used after layout changes)
static int g_force_full_redraw = 1;
// Fullscreen state: -1 means normal, otherwise index of tile that is fullscreened
static int fullscreen_tile = -1;

// GUI client storage
static tile_gui_draw_cb gui_draw_cb[MAX_TILES];
static tile_gui_key_cb gui_key_cb[MAX_TILES];
static tile_gui_mouse_cb gui_mouse_cb[MAX_TILES];
static void* gui_userdata[MAX_TILES];
// Redraw gating for GUI clients: only redraw when invalidated or when rect/version/force changes
static int gui_needs_redraw[MAX_TILES];

static int screen_w = 640; // pixels
static int screen_h = 480; // pixels

// Mouse cursor image and overlay state
static rei_image_t g_cursor_img;
static int g_cursor_loaded = 0;
static int cursor_prev_x = -1000, cursor_prev_y = -1000;
static unsigned char* cursor_savebuf = NULL; // save-under buffer
static int cursor_save_w = 0, cursor_save_h = 0, cursor_save_len = 0;
// Dimensions of the last saved-under region (clipped to screen) used for restore
static int prev_saved_x = 0, prev_saved_y = 0, prev_saved_w = 0, prev_saved_h = 0;
// When the cursor is clipped at screen edges, remember the offset between the
// cursor's logical (x,y) and the captured region's top-left so we can index the
// save-under buffer correctly during overlay alpha blending.
static int prev_saved_offx = 0, prev_saved_offy = 0;
static int cursor_w = 12, cursor_h = 18; // fallback size if no image
// Track a union of regions touched by the cursor this frame to minimize blits
// (legacy) damage tracking no longer needed when we explicitly mark
// previous and current cursor rects dirty each frame.
// Track if any content/decor dirty rect intersects the previous cursor region.
static int g_dirty_hits_prev_cursor = 0;
// When set, force all tile contents to redraw next frame (used when windows move/close)
static int g_tiles_full_content_redraw = 0;
// Live-drag overlay state for smoother window moves
static int prev_drag_x = -1, prev_drag_y = -1, prev_drag_w = 0, prev_drag_h = 0;

// Optional REI close icon for window titlebars
static rei_image_t g_close_icon;
static int g_close_icon_loaded = 0;
// Optional REI minimize/maximize icons
static rei_image_t g_min_icon;
static int g_min_icon_loaded = 0;
static rei_image_t g_max_icon;
static int g_max_icon_loaded = 0;
// Unfocused variants
static rei_image_t g_close_icon_unf;
static int g_close_icon_unf_loaded = 0;
static rei_image_t g_min_icon_unf;
static int g_min_icon_unf_loaded = 0;
static rei_image_t g_max_icon_unf;
static int g_max_icon_unf_loaded = 0;

// Rectangle intersection helper (used widely; keep it early for prototypes)
static inline int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    int ax2 = ax + aw, ay2 = ay + ah;
    int bx2 = bx + bw, by2 = by + bh;
    return (ax < bx2 && ax2 > bx && ay < by2 && ay2 > by);
}

static inline void rect_union(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh,
                              int* rx, int* ry, int* rw, int* rh) {
    if (aw <= 0 || ah <= 0) { *rx = bx; *ry = by; *rw = bw; *rh = bh; return; }
    if (bw <= 0 || bh <= 0) { *rx = ax; *ry = ay; *rw = aw; *rh = ah; return; }
    int x1 = (ax < bx) ? ax : bx;
    int y1 = (ay < by) ? ay : by;
    int x2 = ((ax + aw) > (bx + bw)) ? (ax + aw) : (bx + bw);
    int y2 = ((ay + ah) > (by + bh)) ? (ay + ah) : (by + bh);
    *rx = x1; *ry = y1; *rw = x2 - x1; *rh = y2 - y1;
}

// ---------------- Floating Windows (experimental) ----------------
typedef struct {
    int used;
    int x, y, w, h; // outer rect including decorations
    const char* title;
    const char* status_left;
    const char* status_right;
    // caches
    int last_focused;
    int static_drawn;
    // callbacks
    tile_gui_draw_cb draw_cb;
    tile_gui_key_cb key_cb;
    tile_gui_mouse_cb mouse_cb;
    void* userdata;
    int needs_redraw;
    // state
    int minimized;
    int maximized;
    int prev_x, prev_y, prev_w, prev_h; // for restore after maximize
} window_t;

static window_t g_windows[MAX_WINDOWS];
static int g_window_order[MAX_WINDOWS]; // z-order back->front indices into g_windows
static int g_window_count = 0;
static int g_win_focused = -1; // index into g_windows, not order array

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

static int win_title_height(const window_t* w) { (void)w; return 16; }
static int win_status_height(const window_t* w) { (void)w; return (w->status_left || w->status_right) ? 12 : 0; }

// Forward declarations for window helpers used later in the main loop
static void wm_draw_decor(window_t* w, int is_focused);
static void wm_mark_decor_dirty(const window_t* w);
static void wm_draw_content(window_t* w);
// Close button geometry helper
static void wm_get_close_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh);
// Minimize/Maximize geometry helpers
static void wm_get_max_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh);
static void wm_get_min_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh);
// Small helper to draw an icon into a button rect with alpha support
static void wm_draw_icon_into_button(const rei_image_t* icon, int loaded, int bx, int by, int bw, int bh);
static void load_close_icon_unf_try_paths(uint8 disk);
static void load_min_icon_unf_try_paths(uint8 disk);
static void load_max_icon_unf_try_paths(uint8 disk);

// Forward declarations for icon loaders (must appear before start_tiling_manager)
static void load_close_icon_try_paths(uint8 disk);
static void load_min_icon_try_paths(uint8 disk);
static void load_max_icon_try_paths(uint8 disk);

static void wm_draw_decor(window_t* w, int is_focused) {
    int th = win_title_height(w);
    int sh = win_status_height(w);
    // frame background
    drawRect(w->x, w->y, w->w, w->h, 0, 0, 0);
    // title bar
    int title_color_r = is_focused ? 180 : 100;
    int title_color_g = is_focused ? 180 : 100;
    int title_color_b = is_focused ? 60 : 60;
    drawRect(w->x, w->y, w->w, th, title_color_r, title_color_g, title_color_b);
    if (w->title && w->title[0]) {
        int len = strlen(w->title);
        // Try to avoid drawing under the close button by shrinking max chars by ~2
        int max_chars = (w->w / 8) - 2;
        if (max_chars < 0) max_chars = 0;
        int text_y = w->y + 4;
        int color_r = is_focused ? 0 : 255;
        int color_g = is_focused ? 0 : 255;
        int color_b = is_focused ? 0 : 255;
        if (len <= max_chars) {
            int start_x = w->x + (w->w - len * 8) / 2;
            for (int i = 0; i < len; ++i) drawCharAt(start_x + i * 8, text_y, (unsigned char)w->title[i], color_r, color_g, color_b);
        } else {
            int window = max_chars;
            int speed = 6;
            int period = len + window;
            int pos = (g_tile_scroll_tick / speed) % period;
            for (int i = 0; i < window; ++i) {
                int idx = pos + i; char ch = ' ';
                if (idx < len) ch = w->title[idx]; else { int wrap = idx - len; if (wrap < len) ch = w->title[wrap]; }
                drawCharAt(w->x + i * 8, text_y, (unsigned char)ch, color_r, color_g, color_b);
            }
        }
    }
    // Titlebar buttons (minimize, maximize, close) on the right
    if (th >= 12) {
        // Close
        {
            int bx, by, bw, bh; wm_get_close_rect(w, &bx, &by, &bw, &bh);
            // button background and border
            drawRect(bx, by, bw, bh, 160, 40, 40);
            drawRect(bx, by, bw, 1, 220, 80, 80);
            drawRect(bx, by + bh - 1, bw, 1, 80, 20, 20);
            drawRect(bx, by, 1, bh, 220, 80, 80);
            drawRect(bx + bw - 1, by, 1, bh, 80, 20, 20);
            if (g_close_icon_loaded && g_close_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_close_icon, g_close_icon_loaded, bx, by, bw, bh);
                } else {
                    // prefer unfocused variant; fallback to focused icon if missing
                    if (g_close_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_close_icon_unf, g_close_icon_unf_loaded, bx, by, bw, bh);
                    else
                        wm_draw_icon_into_button(&g_close_icon, g_close_icon_loaded, bx, by, bw, bh);
                }
            } else {
                if (bw >= 8 && bh >= 8) {
                    for (int i = 0; i < 6; ++i) {
                        drawRect(bx + 3 + i, by + 3 + i, 1, 1, 255, 255, 255);
                        drawRect(bx + bw - 4 - i, by + 3 + i, 1, 1, 255, 255, 255);
                    }
                } else {
                    drawCharAt(bx + 2, by + 1, (unsigned char)'X', 255, 255, 255);
                }
            }
        }
        // Maximize
        {
            int bx, by, bw, bh; wm_get_max_rect(w, &bx, &by, &bw, &bh);
            drawRect(bx, by, bw, bh, 80, 80, 160);
            drawRect(bx, by, bw, 1, 120, 120, 220);
            drawRect(bx, by + bh - 1, bw, 1, 20, 20, 80);
            drawRect(bx, by, 1, bh, 120, 120, 220);
            drawRect(bx + bw - 1, by, 1, bh, 20, 20, 80);
            if (g_max_icon_loaded && g_max_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_max_icon, g_max_icon_loaded, bx, by, bw, bh);
                } else {
                    if (g_max_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_max_icon_unf, g_max_icon_unf_loaded, bx, by, bw, bh);
                    else
                        wm_draw_icon_into_button(&g_max_icon, g_max_icon_loaded, bx, by, bw, bh);
                }
            } else {
                // fallback: a square outline
                drawRect(bx + 3, by + 3, bw - 6, 1, 255, 255, 255);
                drawRect(bx + 3, by + bh - 4, bw - 6, 1, 255, 255, 255);
                drawRect(bx + 3, by + 3, 1, bh - 6, 255, 255, 255);
                drawRect(bx + bw - 4, by + 3, 1, bh - 6, 255, 255, 255);
            }
        }
        // Minimize
        {
            int bx, by, bw, bh; wm_get_min_rect(w, &bx, &by, &bw, &bh);
            drawRect(bx, by, bw, bh, 80, 160, 80);
            drawRect(bx, by, bw, 1, 120, 220, 120);
            drawRect(bx, by + bh - 1, bw, 1, 20, 80, 20);
            drawRect(bx, by, 1, bh, 120, 220, 120);
            drawRect(bx + bw - 1, by, 1, bh, 20, 80, 20);
            if (g_min_icon_loaded && g_min_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_min_icon, g_min_icon_loaded, bx, by, bw, bh);
                } else {
                    if (g_min_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_min_icon_unf, g_min_icon_unf_loaded, bx, by, bw, bh);
                    else
                        wm_draw_icon_into_button(&g_min_icon, g_min_icon_loaded, bx, by, bw, bh);
                }
            } else {
                // fallback: a horizontal line
                drawRect(bx + 3, by + bh/2, bw - 6, 1, 255, 255, 255);
            }
        }
    }
    // status bar
    if (sh > 0) {
        int sy = w->y + th;
        drawRect(w->x, sy, w->w, sh, 32, 32, 32);
        const char* left = w->status_left ? w->status_left : "";
        for (int i = 0; left[i] && (w->x + 4 + i * 8) < w->x + w->w - 4; ++i) {
            drawCharAt(w->x + 4 + i * 8, sy + 2, (unsigned char)left[i], 255, 255, 255);
        }
        if (w->status_right && w->status_right[0]) {
            int rl = strlen(w->status_right);
            int max_chars = (w->w - 8) / 8;
            if (rl > max_chars) rl = max_chars - 1;
            int start_x = w->x + w->w - rl * 8 - 4;
            for (int i = 0; i < rl; ++i) drawCharAt(start_x + i * 8, sy + 2, (unsigned char)w->status_right[i], 255, 0, 0);
        }
    }
    // border
    int br = is_focused ? 140 : 60, bg = is_focused ? 140 : 60, bb = is_focused ? 90 : 60;
    drawRect(w->x, w->y, w->w, 1, br, bg, bb);
    drawRect(w->x, w->y + w->h - 1, w->w, 1, br, bg, bb);
    drawRect(w->x, w->y, 1, w->h, br, bg, bb);
    drawRect(w->x + w->w - 1, w->y, 1, w->h, br, bg, bb);
}

static void wm_get_close_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    int btn_w = 12, btn_h = 12;
    int pad = 2;
    if (btn_w > w->w - 2 * pad) btn_w = (w->w > 2 * pad) ? (w->w - 2 * pad) : 0;
    if (btn_h > th - 2 * pad) btn_h = (th > 2 * pad) ? (th - 2 * pad) : 0;
    int bx = w->x + w->w - btn_w - pad;
    int by = w->y + pad;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_w;
    if (rh) *rh = btn_h;
}

static void wm_get_max_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    int btn_w = 12, btn_h = 12;
    int pad = 2;
    if (btn_w > w->w - 2 * pad) btn_w = (w->w > 2 * pad) ? (w->w - 2 * pad) : 0;
    if (btn_h > th - 2 * pad) btn_h = (th > 2 * pad) ? (th - 2 * pad) : 0;
    int bx_close, by_close, bw_close, bh_close; wm_get_close_rect(w, &bx_close, &by_close, &bw_close, &bh_close);
    int bx = bx_close - pad - btn_w;
    int by = w->y + pad;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_w;
    if (rh) *rh = btn_h;
}

static void wm_get_min_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    int btn_w = 12, btn_h = 12;
    int pad = 2;
    if (btn_w > w->w - 2 * pad) btn_w = (w->w > 2 * pad) ? (w->w - 2 * pad) : 0;
    if (btn_h > th - 2 * pad) btn_h = (th > 2 * pad) ? (th - 2 * pad) : 0;
    int bx_max, by_max, bw_max, bh_max; wm_get_max_rect(w, &bx_max, &by_max, &bw_max, &bh_max);
    int bx = bx_max - pad - btn_w;
    int by = w->y + pad;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_w;
    if (rh) *rh = btn_h;
}

static void wm_draw_icon_into_button(const rei_image_t* icon, int loaded, int bx, int by, int bw, int bh) {
    if (!loaded || !icon || !icon->data) return;
    int iw = icon->header.width;
    int ih = icon->header.height;
    int ox = bx + (bw - iw) / 2; if (ox < bx) ox = bx;
    int oy = by + (bh - ih) / 2; if (oy < by) oy = by;
    int maxw = bw - (ox - bx);
    int maxh = bh - (oy - by);
    const uint8_t* data = icon->data;
    int depth = icon->header.depth;
    // Mark the whole button area dirty once; we'll write many pixels without per-pixel marks
    vga_mark_dirty_rect(bx, by, bw, bh);
    // Determine color key for non-alpha formats by sampling corners
    int use_key = 0; uint8_t keyR = 0, keyG = 0, keyB = 0, keyM = 0;
    if (depth == REI_DEPTH_RGB && iw > 0 && ih > 0) {
        const uint8_t* tl = data + 0 * iw * depth + 0 * 3;
        const uint8_t* tr = data + 0 * iw * depth + (iw - 1) * 3;
        const uint8_t* bl = data + (ih - 1) * iw * depth + 0 * 3;
        const uint8_t* br = data + (ih - 1) * iw * depth + (iw - 1) * 3;
        if (tl[0]==tr[0] && tl[1]==tr[1] && tl[2]==tr[2] &&
            tl[0]==bl[0] && tl[1]==bl[1] && tl[2]==bl[2] &&
            tl[0]==br[0] && tl[1]==br[1] && tl[2]==br[2]) {
            use_key = 1; keyR = tl[0]; keyG = tl[1]; keyB = tl[2];
        }
    } else if (depth == REI_DEPTH_MONO && iw > 0 && ih > 0) {
        uint8_t tl = data[0];
        uint8_t tr = data[(iw - 1)];
        uint8_t bl = data[(ih - 1) * iw * depth + 0];
        uint8_t br = data[(ih - 1) * iw * depth + (iw - 1)];
        if (tl == tr && tl == bl && tl == br) { use_key = 1; keyM = tl; }
    }
    for (int yy = 0; yy < ih && yy < maxh; ++yy) {
        int py = oy + yy; if (py < by || py >= by + bh) continue;
        const uint8_t* row = data + yy * iw * depth;
        for (int xx = 0; xx < iw && xx < maxw; ++xx) {
            int px = ox + xx; if (px < bx || px >= bx + bw) continue;
            if (depth == REI_DEPTH_MONO) {
                uint8_t v = row[xx]; if (use_key && v == keyM) continue; if (v) vga_drawPixel_bb(px, py, v, v, v);
            } else if (depth == REI_DEPTH_RGB) {
                const uint8_t* p3 = row + xx * 3; if (use_key && p3[0]==keyR && p3[1]==keyG && p3[2]==keyB) continue; vga_drawPixel_bb(px, py, p3[0], p3[1], p3[2]);
            } else if (depth == REI_DEPTH_RGBA) {
                const uint8_t* p4 = row + xx * 4; uint8_t a = p4[3];
                if (a >= 128) vga_drawPixel_bb(px, py, p4[0], p4[1], p4[2]);
            }
        }
    }
}

static void wm_mark_decor_dirty(const window_t* w) {
    int th = win_title_height(w);
    int sh = win_status_height(w);
    if (rects_intersect(w->x, w->y, w->w, th, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x, w->y, w->w, th);
    if (sh) {
        if (rects_intersect(w->x, w->y + th, w->w, sh, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
        vga_mark_dirty_rect(w->x, w->y + th, w->w, sh);
    }
    if (rects_intersect(w->x, w->y, w->w, 1, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x, w->y, w->w, 1);
    if (rects_intersect(w->x, w->y + w->h - 1, w->w, 1, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x, w->y + w->h - 1, w->w, 1);
    if (rects_intersect(w->x, w->y, 1, w->h, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x, w->y, 1, w->h);
    if (rects_intersect(w->x + w->w - 1, w->y, 1, w->h, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x + w->w - 1, w->y, 1, w->h);
}

static void wm_draw_content(window_t* w) {
    int th = win_title_height(w);
    int sh = win_status_height(w);
    int cx = w->x + 1;
    int cy = w->y + th + sh + 1;
    int cw = w->w - 2;
    int ch = w->h - (th + sh) - 2;
    if (cw > 0 && ch > 0) {
        drawRect(cx, cy, cw, ch, 0, 0, 0);
        if (!w->minimized) {
            if (w->draw_cb) w->draw_cb(-1, cx, cy, cw, ch, w->userdata);
        }
        vga_mark_dirty_rect(cx, cy, cw, ch);
    }
}

static int wm_hit_test(int x, int y) {
    // return topmost window index under point, else -1
    for (int oi = g_window_count - 1; oi >= 0; --oi) {
        int wi = g_window_order[oi];
        if (!g_windows[wi].used) continue;
        if (point_in_rect(x, y, g_windows[wi].x, g_windows[wi].y, g_windows[wi].w, g_windows[wi].h)) return wi;
    }
    return -1;
}

static void wm_bring_to_front(int wi) {
    if (wi < 0 || wi >= MAX_WINDOWS || !g_windows[wi].used) return;
    int pos = -1;
    for (int i = 0; i < g_window_count; ++i) if (g_window_order[i] == wi) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < g_window_count - 1; ++i) g_window_order[i] = g_window_order[i + 1];
    g_window_order[g_window_count - 1] = wi;
    g_win_focused = wi;
}

// dragging state
static int drag_active = 0;
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;

// Public APIs
int wm_create_window(const char* title, int x, int y, int w, int h, const char* status_left) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) {
            g_windows[i].used = 1;
            g_windows[i].x = clampi(x, 0, screen_w - 32);
            g_windows[i].y = clampi(y, 0, screen_h - 32);
            g_windows[i].w = clampi(w, 64, screen_w);
            g_windows[i].h = clampi(h, 48, screen_h);
            g_windows[i].title = title ? title : "";
            g_windows[i].status_left = status_left;
            g_windows[i].status_right = NULL;
            g_windows[i].draw_cb = NULL;
            g_windows[i].key_cb = NULL;
            g_windows[i].mouse_cb = NULL;
            g_windows[i].userdata = NULL;
            g_windows[i].needs_redraw = 1;
            g_windows[i].static_drawn = 0;
            g_windows[i].last_focused = 0;
            g_windows[i].minimized = 0;
            g_windows[i].maximized = 0;
            g_windows[i].prev_x = g_windows[i].x; g_windows[i].prev_y = g_windows[i].y;
            g_windows[i].prev_w = g_windows[i].w; g_windows[i].prev_h = g_windows[i].h;
            g_window_order[g_window_count++] = i;
            g_win_focused = i;
            g_force_full_redraw = 1;
            return i;
        }
    }
    return -1;
}

void wm_register_gui_client2(int win_id, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, tile_gui_mouse_cb mouse_cb, void* userdata) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].draw_cb = draw_cb;
    g_windows[win_id].key_cb = key_cb;
    g_windows[win_id].mouse_cb = mouse_cb;
    g_windows[win_id].userdata = userdata;
    g_windows[win_id].needs_redraw = 1;
}

void wm_unregister_gui_client(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].draw_cb = NULL;
    g_windows[win_id].key_cb = NULL;
    g_windows[win_id].mouse_cb = NULL;
    g_windows[win_id].userdata = NULL;
    g_windows[win_id].needs_redraw = 0;
}

void wm_set_title_status(int win_id, const char* title, const char* status_left, const char* status_right) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].title = title ? title : "";
    g_windows[win_id].status_left = status_left;
    g_windows[win_id].status_right = status_right;
    g_windows[win_id].static_drawn = 0;
}

void wm_invalidate_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].needs_redraw = 1;
}

void wm_close_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    // remove from order
    int pos = -1;
    for (int i = 0; i < g_window_count; ++i) if (g_window_order[i] == win_id) { pos = i; break; }
    if (pos >= 0) {
        for (int i = pos; i < g_window_count - 1; ++i) g_window_order[i] = g_window_order[i + 1];
        g_window_count--;
    }
    g_windows[win_id].used = 0;
    if (g_win_focused == win_id) g_win_focused = -1;
    g_force_full_redraw = 1;
    g_tiles_full_content_redraw = 1;
}

// duplicate rects_intersect removed (defined earlier)

static void draw_cursor_overlay(int x, int y) {
    static int s_logged_cursor_once = 0;
    int w = g_cursor_loaded ? g_cursor_img.header.width : cursor_w;
    int h = g_cursor_loaded ? g_cursor_img.header.height : cursor_h;
    // Simple fallback: small yellow box if no image
    if (!g_cursor_loaded) {
        for (int yy = 0; yy < h; ++yy) {
            int py = y + yy;
            if (py < 0 || py >= screen_h) continue;
            for (int xx = 0; xx < w; ++xx) {
                int px = x + xx;
                if (px < 0 || px >= screen_w) continue;
                vga_drawPixel_fb(px, py, 255, 255, 0);
            }
        }
        cursor_w = w; cursor_h = h;
        return;
    }
    // Draw REI image (supports MONO/RGB/RGBA)
    if (g_cursor_loaded && !s_logged_cursor_once) {
        s_logged_cursor_once = 1;
    }
    // Transparency handling:
    // - RGBA: draw only if alpha >= 128 (skip low-alpha to avoid halos)
    // - RGB/MONO: if the four corner pixels are the same value/color, treat that as a color key
    //   and skip any pixels matching it. This removes solid background boxes on assets lacking alpha.
    const uint8_t* data = g_cursor_img.data;
    int depth = g_cursor_img.header.depth;
    int stride = w * depth;
    // Determine color key for non-alpha formats by sampling corners
    int use_key = 0; uint8_t keyR = 0, keyG = 0, keyB = 0, keyM = 0;
    if (depth == REI_DEPTH_RGB && w > 0 && h > 0) {
        const uint8_t* tl = data + 0 * stride + 0 * 3;
        const uint8_t* tr = data + 0 * stride + (w - 1) * 3;
        const uint8_t* bl = data + (h - 1) * stride + 0 * 3;
        const uint8_t* br = data + (h - 1) * stride + (w - 1) * 3;
        if (tl[0]==tr[0] && tl[1]==tr[1] && tl[2]==tr[2] &&
            tl[0]==bl[0] && tl[1]==bl[1] && tl[2]==bl[2] &&
            tl[0]==br[0] && tl[1]==br[1] && tl[2]==br[2]) {
            use_key = 1; keyR = tl[0]; keyG = tl[1]; keyB = tl[2];
        }
    } else if (depth == REI_DEPTH_MONO && w > 0 && h > 0) {
        uint8_t tl = data[0];
        uint8_t tr = data[(w - 1)];
        uint8_t bl = data[(h - 1) * stride + 0];
        uint8_t br = data[(h - 1) * stride + (w - 1)];
        if (tl == tr && tl == bl && tl == br) { use_key = 1; keyM = tl; }
    }
    // Offsets into the saved-under buffer if the capture was clipped at edges
    int soffx = prev_saved_offx;
    int soffy = prev_saved_offy;
    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= screen_h) continue;
        const uint8_t* row = data + yy * stride;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= screen_w) continue;
            if (depth == REI_DEPTH_MONO) {
                uint8_t v = row[xx];
                if (use_key && v == keyM) continue;
                if (v) vga_drawPixel_fb(px, py, v, v, v);
            } else if (depth == REI_DEPTH_RGB) {
                const uint8_t* p3 = row + xx * 3;
                if (use_key && p3[0]==keyR && p3[1]==keyG && p3[2]==keyB) continue;
                vga_drawPixel_fb(px, py, p3[0], p3[1], p3[2]);
            } else if (depth == REI_DEPTH_RGBA) {
                const uint8_t* p4 = row + xx * 4;
                uint8_t sr = p4[0], sg = p4[1], sb = p4[2], a = p4[3];
                if (a == 0) {
                    // fully transparent
                    continue;
                }
                // Clamp very low alpha up to improve visibility over bright backgrounds
                if (a < 200) a = 200;
                // Map (xx,yy) in cursor space to saved-under buffer coordinates
                // when the capture region was clipped at the screen boundaries.
                int bx = xx - soffx;
                int by = yy - soffy;
                if (cursor_savebuf && bx >= 0 && by >= 0 && bx < prev_saved_w && by < prev_saved_h) {
                    // alpha blend over saved-under framebuffer if available
                    int bpp = vga_get_fb_bpp_bytes(); if (bpp < 3) bpp = 3;
                    int idx = (by * prev_saved_w + bx) * bpp;
                    if (idx + 2 < cursor_save_len) {
                        uint8_t dr = cursor_savebuf[idx + 0];
                        uint8_t dg = cursor_savebuf[idx + 1];
                        uint8_t db = cursor_savebuf[idx + 2];
                        // out = (src*a + dst*(255-a))/255
                        int inva = 255 - a;
                        uint8_t rr = (uint8_t)((sr * a + dr * inva + 127) / 255);
                        uint8_t gg = (uint8_t)((sg * a + dg * inva + 127) / 255);
                        uint8_t bb = (uint8_t)((sb * a + db * inva + 127) / 255);
                        vga_drawPixel_fb(px, py, rr, gg, bb);
                    } else {
                        // Fallback if out of bounds
                        if (a >= 128) vga_drawPixel_fb(px, py, sr, sg, sb);
                    }
                } else {
                    // No saved-under region (edge cases). Use thresholded draw.
                    if (a >= 128) vga_drawPixel_fb(px, py, sr, sg, sb);
                }
            }
        }
    }
    cursor_w = w; cursor_h = h;
}

// Removed: we now cover cursor cleanup by marking rects dirty and letting swap copy

// Return the tile index at screen pixel (x,y), or -1 if none
static int tile_index_at(int x, int y) {
    if (fullscreen_tile >= 0 && fullscreen_tile < tile_count) {
        tile_t* t = &tiles[fullscreen_tile];
        if (x >= t->x && x < t->x + t->width && y >= t->y && y < t->y + t->height) return fullscreen_tile;
        return -1;
    }
    for (int i = 0; i < tile_count; ++i) {
        tile_t* t = &tiles[i];
        if (t->type == TILE_EMPTY) continue;
        if (x >= t->x && x < t->x + t->width && y >= t->y && y < t->y + t->height) return i;
    }
    return -1;
}

static void load_cursor_image_try_paths(uint8 disk) {
    const char* paths[] = { "/cursor.rei", "/ui/cursor.rei", "/testdir/cursor.rei", "/testdir/ui/cursor.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < 4; ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_cursor_img) == 0) {
                    g_cursor_loaded = 1;
                    printf("Loaded cursor image: %s (%dx%d depth=%d)\n", paths[pi], g_cursor_img.header.width, g_cursor_img.header.height, g_cursor_img.header.depth);
                    free(buf); // image parser duplicated into its own buffer if needed
                    return;
                }
                else {
                    printf("Failed to parse cursor image at %s (size=%d)\n", paths[pi], size);
                }
            }
            free(buf);
        }
    }
    printf("No cursor image found in EYNFS; using fallback box.\n");
}

// Try to load a close button icon from a few candidate paths in EYNFS
static void load_close_icon_try_paths(uint8 disk) {
    const char* paths[] = { "/close.rei", "/ui/close.rei", "/testdir/close.rei", "/testdir/ui/close.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < 4; ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_close_icon) == 0) {
                    g_close_icon_loaded = 1;
                    free(buf);
                    return;
                }
            }
            free(buf);
        }
    }
}

// Try to load a minimize button icon from candidate paths
static void load_min_icon_try_paths(uint8 disk) {
    const char* paths[] = { "/min.rei", "/ui/min.rei", "/minimize.rei", "/ui/minimize.rei", "/testdir/min.rei", "/testdir/ui/min.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < (int)(sizeof(paths)/sizeof(paths[0])); ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_min_icon) == 0) {
                    g_min_icon_loaded = 1;
                    free(buf);
                    return;
                }
            }
            free(buf);
        }
    }
}

// Try to load a maximize button icon from candidate paths
static void load_max_icon_try_paths(uint8 disk) {
    const char* paths[] = { "/max.rei", "/ui/max.rei", "/maximize.rei", "/ui/maximize.rei", "/testdir/max.rei", "/testdir/ui/max.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < (int)(sizeof(paths)/sizeof(paths[0])); ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_max_icon) == 0) {
                    g_max_icon_loaded = 1;
                    free(buf);
                    return;
                }
            }
            free(buf);
        }
    }
}

// Try to load unfocused variants of icons
static void load_close_icon_unf_try_paths(uint8 disk) {
    const char* paths[] = { "/close_unfocused.rei", "/ui/close_unfocused.rei", "/testdir/close_unfocused.rei", "/testdir/ui/close_unfocused.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < (int)(sizeof(paths)/sizeof(paths[0])); ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size; uint8_t* buf = (uint8_t*)malloc(size); if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_close_icon_unf) == 0) { g_close_icon_unf_loaded = 1; free(buf); return; }
            }
            free(buf);
        }
    }
}

static void load_min_icon_unf_try_paths(uint8 disk) {
    const char* paths[] = { "/min_unfocused.rei", "/ui/min_unfocused.rei", "/minimize_unfocused.rei", "/ui/minimize_unfocused.rei", "/testdir/min_unfocused.rei", "/testdir/ui/min_unfocused.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < (int)(sizeof(paths)/sizeof(paths[0])); ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size; uint8_t* buf = (uint8_t*)malloc(size); if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_min_icon_unf) == 0) { g_min_icon_unf_loaded = 1; free(buf); return; }
            }
            free(buf);
        }
    }
}

static void load_max_icon_unf_try_paths(uint8 disk) {
    const char* paths[] = { "/max_unfocused.rei", "/ui/max_unfocused.rei", "/maximize_unfocused.rei", "/ui/maximize_unfocused.rei", "/testdir/max_unfocused.rei", "/testdir/ui/max_unfocused.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < (int)(sizeof(paths)/sizeof(paths[0])); ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size; uint8_t* buf = (uint8_t*)malloc(size); if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_max_icon_unf) == 0) { g_max_icon_unf_loaded = 1; free(buf); return; }
            }
            free(buf);
        }
    }
}

static void layout_tiles() {
    // Convert layout rules into pixel rectangles
    if (tile_count <= 0) return;
    if (tile_count == 1) {
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = screen_w; tiles[0].height = screen_h;
    } else if (tile_count == 2) {
        int w = screen_w / 2;
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = w; tiles[0].height = screen_h;
        tiles[1].x = w; tiles[1].y = 0; tiles[1].width = screen_w - w; tiles[1].height = screen_h;
    } else if (tile_count == 3) {
        int w = screen_w / 2;
        int h = screen_h / 2;
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = w; tiles[0].height = screen_h;
        tiles[1].x = w; tiles[1].y = 0; tiles[1].width = screen_w - w; tiles[1].height = h;
        tiles[2].x = w; tiles[2].y = h; tiles[2].width = screen_w - w; tiles[2].height = screen_h - h;
    } else {
        int w = screen_w / 2;
        int h = screen_h / 2;
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = w; tiles[0].height = h;
        tiles[1].x = w; tiles[1].y = 0; tiles[1].width = screen_w - w; tiles[1].height = h;
        tiles[2].x = 0; tiles[2].y = h; tiles[2].width = w; tiles[2].height = screen_h - h;
        tiles[3].x = w; tiles[3].y = h; tiles[3].width = screen_w - w; tiles[3].height = screen_h - h;
    }
}

// Draw the static (rarely-changing) parts of a tile: background, titlebar, status bar, borders.
// Draw only the decorations (title bar, status bar and borders). This is cheap and
// safe to call every frame into the backbuffer so that decoration pixels are
// freshly present for the partial blit, while the large background fill can stay
// cached in the backbuffer and only redrawn when needed.
static int get_title_height(const tile_t* t) {
    if (fullscreen_tile == (t - tiles)) return 0; // hide titlebar in fullscreen
    return 16;
}

static void draw_decorations(tile_t* t, int is_focused) {
    if (t->type == TILE_EMPTY) return;
    int title_h = get_title_height(t);
    if (title_h > 0) {
        int title_y = t->y;
        int title_color_r = is_focused ? 200 : 120;
        int title_color_g = is_focused ? 200 : 120;
        int title_color_b = is_focused ? 50 : 50;
        drawRect(t->x, title_y, t->width, title_h, title_color_r, title_color_g, title_color_b);
        if (t->title && t->title[0]) {
            int title_len = (int)strlen(t->title);
            int max_chars = t->width / 8;
            int text_y = title_y + 4;
            int color_r = is_focused ? 0 : 255;
            int color_g = is_focused ? 0 : 255;
            int color_b = is_focused ? 0 : 255;
            if (title_len <= max_chars) {
                int start_x = t->x + (t->width - (8 * title_len)) / 2;
                for (int i = 0; i < title_len; ++i) {
                    int cx = start_x + i * 8;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + 8 > clip_right) break;
                    if (cx < 0 || cx + 8 > screen_w) break;
                    drawCharAt(cx, text_y, (int)(unsigned char)t->title[i], color_r, color_g, color_b);
                }
            } else {
                int window = max_chars;
                int speed = 6;
                int period = title_len + window;
                int pos = (g_tile_scroll_tick / speed) % period;
                for (int i = 0; i < window; ++i) {
                    int idx = pos + i;
                    char ch = ' ';
                    if (idx < title_len) ch = t->title[idx];
                    else {
                        int wrap_idx = idx - title_len;
                        if (wrap_idx < title_len) ch = t->title[wrap_idx];
                    }
                    int cx = t->x + i * 8;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + 8 > clip_right) break;
                    if (cx < 0 || cx + 8 > screen_w) break;
                    drawCharAt(cx, text_y, (int)(unsigned char)ch, color_r, color_g, color_b);
                }
            }
        }
    }
    // Status bar (repositioned at top if fullscreen hides title)
    int status_h = 0;
    extern int tui_alt_pressed;
    int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
    if (show_status) {
        int status_y = t->y + title_h; // if title_h==0 this is top
        status_h = 12;
        drawRect(t->x, status_y, t->width, status_h, 32, 32, 32);
        const char* left_text = t->status_left ? t->status_left : "Super+n:New | Super+Arrow:Switch | Super+Q:Close";
        int left_len = (int)strlen(left_text);
        int avail_chars = (t->width - 8) / 8;
        int text_y = status_y + 2;
        if (left_len <= avail_chars) {
            int clip_min = t->x + 4;
            int clip_max = t->x + t->width - 4;
            for (int i = 0; i < left_len; ++i) {
                int cx = clip_min + i * 8;
                if (cx < clip_min) continue;
                if (cx + 8 > clip_max) break;
                if (cx < 0 || cx + 8 > screen_w) break;
                drawCharAt(cx, text_y, (int)(unsigned char)left_text[i], 255, 255, 255);
            }
        } else {
            int window = avail_chars;
            int speed = 6;
            int period = left_len + window;
            int pos = (g_tile_scroll_tick / speed) % period;
            int clip_min = t->x + 4;
            int clip_max = t->x + t->width - 4;
            for (int i = 0; i < window; ++i) {
                int idx = pos + i;
                char ch = ' ';
                if (idx < left_len) ch = left_text[idx];
                else {
                    int wrap_idx = idx - left_len;
                    if (wrap_idx < left_len) ch = left_text[wrap_idx];
                }
                int cx = clip_min + i * 8;
                if (cx < clip_min) continue;
                if (cx + 8 > clip_max) break;
                if (cx < 0 || cx + 8 > screen_w) break;
                drawCharAt(cx, text_y, (int)(unsigned char)ch, 255, 255, 255);
            }
        }
        if (t->status_right && t->status_right[0]) {
            int right_len = strlen(t->status_right);
            int max_right_pixels = t->width - 8;
            int max_right_chars = max_right_pixels / 8;
            if (max_right_chars <= 0) max_right_chars = 0;
            const char* right_ptr = t->status_right;
            if (right_len > max_right_chars) {
                int take = max_right_chars - 1;
                if (take < 0) take = 0;
                if (take == 0) {
                    right_ptr = "";
                    right_len = 0;
                } else {
                    right_ptr = t->status_right + (right_len - take);
                    right_len = take;
                }
            }
            int right_len_visible = right_len;
            int max_chars = (t->width - 8) / 8;
            if (right_len > max_chars) right_len_visible = max_chars - 1;
            if (right_len_visible <= max_chars && right_len_visible > 0) {
                int start_x = t->x + t->width - right_len_visible * 8 - 4;
                for (int ri = 0; ri < right_len_visible; ++ri) {
                    int cx = start_x + ri * 8;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + 8 > clip_right) break;
                    if (cx < 0 || cx + 8 > screen_w) break;
                    drawCharAt(cx, status_y + 2, (int)(unsigned char)right_ptr[ri], 255, 0, 0);
                }
            } else {
                int tx = t->x + t->width - right_len * 8 - 4;
                if (tx < t->x + 4) tx = t->x + 4;
                for (int ri = 0; ri < right_len; ++ri) {
                    int cx = tx + ri * 8;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + 8 > clip_right) break;
                    if (cx < 0 || cx + 8 > screen_w) break;
                    drawCharAt(cx, status_y + 2, (int)(unsigned char)right_ptr[ri], 255, 0, 0);
                }
            }
        }
    }
    // borders
    int border_r = is_focused ? 120 : 48;
    int border_g = is_focused ? 120 : 48;
    int border_b = is_focused ? 80 : 48;
    drawRect(t->x, t->y, t->width, 1, border_r, border_g, border_b);
    drawRect(t->x, t->y + t->height - 1, t->width, 1, border_r, border_g, border_b);
    drawRect(t->x, t->y, 1, t->height, border_r, border_g, border_b);
    drawRect(t->x + t->width - 1, t->y, 1, t->height, border_r, border_g, border_b);
}

static void draw_static_tile(tile_t* t, int is_focused) {
    if (t->type == TILE_EMPTY) return;
    // Draw background once and then draw decorations on top
    drawRect(t->x, t->y, t->width, t->height, 0, 0, 0);
    draw_decorations(t, is_focused);
    t->static_drawn = 1;
    // Update decoration caches
    t->last_title_ptr = t->title;
    t->last_status_left_ptr = t->status_left;
    t->last_status_right_ptr = t->status_right;
    extern int tui_alt_pressed;
    int title_h = get_title_height(t);
    int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
    t->last_show_status = show_status;
    t->last_focused = is_focused;
}

// Draw the dynamic/content area for a tile (invoked each frame or when content changes)
static void draw_tile_content(const tile_t* t) {
    if (t->type == TILE_EMPTY) return;
    int title_h = get_title_height(t);
    int status_h = 0;
    extern int tui_alt_pressed;
    int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
    if (show_status) status_h = 12;
    // Keep a 1px margin for the border so clears don't overwrite border pixels
    int content_x = t->x + 1;
    int content_y = t->y + title_h + status_h + 1;
    int line_h = 8;
    int max_lines = (t->height - (title_h + status_h + 2)) / line_h;
    int content_w = t->width - 2; // leave 1px border on left/right
    int content_h = t->height - (title_h + status_h) - 2; // leave 1px top/bottom border
    if (content_w > 0 && content_h > 0) {
        // Clear content area to background so new frames don't draw over old content
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
    }
    if (gui_draw_cb[t->term_idx]) {
        gui_draw_cb[t->term_idx](t - tiles, content_x, content_y, content_w, content_h, gui_userdata[t->term_idx]);
    } else {
        // Soft-wrapped rendering: draw from the end of the buffer using visual rows.
        // Compute visible columns from content width.
        int cols = content_w / 8;
        if (cols < 1) cols = 1;
    // Determine the absolute last row to show (cursor row minus scrollback offset)
    int end_row = vterm_get_cursor_row(t->term_idx) - vterm_get_scroll(t->term_idx);
        // Build visual rows backward: fill from bottom up
        int vis_row = max_lines - 1;
        int abs_row = end_row;
        int cursor_row = vterm_get_cursor_row(t->term_idx);
        int cursor_col = vterm_get_cursor_col(t->term_idx);
        while (vis_row >= 0 && abs_row >= 0) {
            const char* src = vterm_get_line(t->term_idx, abs_row);
            int len = 0; while (src[len] && len < TERM_COLS) len++;
            // number of wrapped segments for this row (at least 1)
            int wraps = (len + cols - 1) / cols; if (wraps < 1) wraps = 1;
            for (int w = wraps - 1; w >= 0 && vis_row >= 0; --w) {
                int start_col = w * cols;
                for (int cc = 0; cc < cols; ++cc) {
                    int px = content_x + cc * 8;
                    int py = content_y + vis_row * line_h;
                    // Horizontal & vertical clipping
                    if (px + 7 < content_x || px >= content_x + content_w) continue;
                    if (py + 7 < content_y || py >= content_y + content_h) continue;
                    int src_col = start_col + cc;
                    char ch = ' ';
                    int rr = 200, gg = 200, bb = 200;
                    if (src_col < len) {
                        ch = src[src_col];
                        vterm_get_char_color_abs(t->term_idx, abs_row, src_col, &rr, &gg, &bb);
                    }
                    int is_sel = vterm_is_selected(t->term_idx, abs_row, src_col);
                    if (is_sel) {
                        // selection background teal-ish
                        drawRect(px, py, 8, 8, 0, 128, 128);
                    }
                    // Always draw underlying character first
                    drawCharAt(px, py, (int)(unsigned char)ch, rr, gg, bb);
                    // Then overlay underscore cursor at the logical caret position
                    if (abs_row == cursor_row && start_col + cc == cursor_col) {
                        drawCharAt(px, py, (int)'_', 255, 255, 0);
                    }
                }
                vis_row--;
            }
            abs_row--;
        }
    }
}

// Public API implementations (minimal, local helper functions)
void tile_set_title_status(int tile_idx, const char* title, const char* status_left, const char* status_right) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    tiles[tile_idx].title = title ? title : "";
    tiles[tile_idx].status_left = status_left;
    tiles[tile_idx].status_right = status_right;
    // If any status text is provided, mark the tile to show the status bar
    tiles[tile_idx].status_visible = (status_left != NULL) || (status_right != NULL);
    // Invalidate static UI so it will be redrawn next frame
    tiles[tile_idx].static_drawn = 0;
    // Also reset decoration cache pointers so changes are detected
    tiles[tile_idx].last_title_ptr = NULL;
    tiles[tile_idx].last_status_left_ptr = NULL;
    tiles[tile_idx].last_status_right_ptr = NULL;
}

void tile_invalidate_decorations(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    // Force decorations to redraw next frame
    tiles[tile_idx].static_drawn = 0;
    tiles[tile_idx].last_title_ptr = NULL;
    tiles[tile_idx].last_status_left_ptr = NULL;
    tiles[tile_idx].last_status_right_ptr = NULL;
}

void tile_close(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    if (tiles[tile_idx].type == TILE_EMPTY) return;
    // Mark empty
    tiles[tile_idx].type = TILE_EMPTY;
    tiles[tile_idx].title = "";
    tiles[tile_idx].status_left = NULL;
    tiles[tile_idx].status_right = NULL;
    vterm_set_active(tiles[tile_idx].term_idx, 0);
    // Compact remaining tiles: move non-empty tiles to front
    tile_t tmp[MAX_TILES]; int n = 0;
    for (int i = 0; i < MAX_TILES; ++i) {
        if (tiles[i].type != TILE_EMPTY) tmp[n++] = tiles[i];
    }
    // fill rest with empty
    for (int i = 0; i < n; ++i) tiles[i] = tmp[i];
    for (int i = n; i < MAX_TILES; ++i) {
        tiles[i].type = TILE_EMPTY; tiles[i].title = ""; tiles[i].status_left = NULL; tiles[i].status_right = NULL; tiles[i].term_idx = i; tiles[i].active = 0;
    }
    // Reassign term_idx for compacted tiles. Do NOT clear vterm buffers here — keep terminal content intact.
    for (int i = 0; i < n; ++i) {
        int old_idx = tiles[i].term_idx;
        tiles[i].term_idx = i;
        // if the term index changed, update vterm active mapping
        if (old_idx != i) {
            // move active flag: deactivate old index and activate new index
            vterm_set_active(old_idx, 0);
            vterm_set_active(i, 1);
            // Move any registered GUI callbacks associated with the old term index to the new index
            gui_draw_cb[i] = gui_draw_cb[old_idx];
            gui_key_cb[i] = gui_key_cb[old_idx];
            gui_userdata[i] = gui_userdata[old_idx];
            gui_needs_redraw[i] = gui_needs_redraw[old_idx];
            gui_draw_cb[old_idx] = NULL;
            gui_key_cb[old_idx] = NULL;
            gui_userdata[old_idx] = NULL;
            gui_needs_redraw[old_idx] = 0;
        }
    }
    tile_count = n > 0 ? n : 1; // ensure at least one tile exists
    if (focused >= tile_count) focused = tile_count - 1;
    layout_tiles();
}

int tile_get_focused() { return focused; }

int tile_is_tiling_active() {
    return tile_count > 0;
}

void tile_register_gui_client(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, void* userdata) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_draw_cb[term] = draw_cb;
    gui_key_cb[term] = key_cb;
    gui_mouse_cb[term] = NULL;
    gui_userdata[term] = userdata;
    gui_needs_redraw[term] = 1;
}

void tile_register_gui_client2(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, tile_gui_mouse_cb mouse_cb, void* userdata) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_draw_cb[term] = draw_cb;
    gui_key_cb[term] = key_cb;
    gui_mouse_cb[term] = mouse_cb;
    gui_userdata[term] = userdata;
    gui_needs_redraw[term] = 1;
}

int tile_create_gui_tile(const char* title, const char* status_left) {
    if (tile_count >= 4) return -1; // simple grid supports up to 4
    int idx = tile_count++;
    tiles[idx].type = TILE_SHELL; // reuse shell type slot for now
    tiles[idx].title = title ? title : "";
    tiles[idx].status_left = status_left;
    tiles[idx].status_right = NULL;
    tiles[idx].term_idx = idx;
    tiles[idx].active = 1;
    tiles[idx].static_drawn = 0;
    vterm_clear(idx);
    vterm_set_active(idx, 1);
    layout_tiles();
    focused = idx;
    g_force_full_redraw = 1;
    return idx;
}

void tile_unregister_gui_client(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    // Determine the terminal slot associated with this tile and clear callbacks there
    int term = tiles[tile_idx].term_idx;
    if (term >= 0 && term < MAX_TILES) {
        gui_draw_cb[term] = NULL;
        gui_key_cb[term] = NULL;
        gui_mouse_cb[term] = NULL;
        gui_userdata[term] = NULL;
        // No GUI anymore; clear any pending GUI invalidation
        gui_needs_redraw[term] = 0;
    }

    // If this tile is a shell, restore its title/status to the default shell values
    if (tiles[tile_idx].type == TILE_SHELL) {
        tiles[tile_idx].title = "EYN-OS Shell";
        tiles[tile_idx].status_left = NULL;
        tiles[tile_idx].status_right = NULL;
        tiles[tile_idx].status_visible = 0; // hide status unless set again by the shell
        // Ensure decorations are refreshed next frame
        tiles[tile_idx].static_drawn = 0;
        tiles[tile_idx].last_title_ptr = NULL;
        tiles[tile_idx].last_status_left_ptr = NULL;
        tiles[tile_idx].last_status_right_ptr = NULL;
    }

    // Force an immediate redraw so the terminal content replaces the last GUI frame
    g_force_full_redraw = 1;
}

void tile_invalidate_gui(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_needs_redraw[term] = 1;
}

void start_tiling_manager() {
    // initialize screen dimensions from global framebuffer if available
    extern multiboot_info_t* g_mbi;
    if (g_mbi) {
        screen_w = g_mbi->framebuffer_width;
        screen_h = g_mbi->framebuffer_height;
    }
    // initialize virtual terminals
    vterm_init_all();

    // initialize simple double-buffer (best-effort)
    vga_init_double_buffer();

    // Initialize mouse and bounds to pixel space
    mouse_init();
    mouse_set_bounds(0, 0, screen_w - 1, screen_h - 1);
    // Try to load cursor image from EYNFS disk 0
    load_cursor_image_try_paths(0);
    // Try to load a close button icon (optional)
    load_close_icon_try_paths(0);
    load_close_icon_unf_try_paths(0);
    // Try to load minimize/maximize icons (optional)
    load_min_icon_try_paths(0);
    load_max_icon_try_paths(0);
    load_min_icon_unf_try_paths(0);
    load_max_icon_unf_try_paths(0);
    // Allocate save-under buffer once we know cursor size
    int cw0 = g_cursor_loaded ? g_cursor_img.header.width : cursor_w;
    int ch0 = g_cursor_loaded ? g_cursor_img.header.height : cursor_h;
    int bpp = vga_get_fb_bpp_bytes(); if (bpp < 3) bpp = 3;
    cursor_save_w = cw0; cursor_save_h = ch0;
    cursor_save_len = cw0 * ch0 * bpp;
    cursor_savebuf = (unsigned char*)malloc(cursor_save_len);

    // Initialize tiles
    tile_count = 1;
    focused = 0;
    for (int i = 0; i < MAX_TILES; ++i) {
        tiles[i].type = TILE_EMPTY;
        tiles[i].title = "";
        tiles[i].status_left = NULL;
        tiles[i].status_right = NULL;
        tiles[i].static_drawn = 0;
        tiles[i].last_drawn_version = 0;
        tiles[i].last_cx = tiles[i].last_cy = tiles[i].last_cw = tiles[i].last_ch = -1;
        tiles[i].term_idx = i;
        tiles[i].active = 0;
        gui_needs_redraw[i] = 0;
    }
    tiles[0].type = TILE_SHELL;
    tiles[0].title = "EYN-OS Shell";
    static char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), "Fb: %dx%d", screen_w, screen_h);
    tiles[0].status_left = status_buf; // show framebuffer size for debug
    tiles[0].active = 1;
    vterm_set_active(0, 1);
    // Print initial prompt into vterm 0
    vterm_print_prompt(0);
    layout_tiles();

    int running = 1;
    while (running) {
        // Poll mouse in case IRQ12 is not firing (QEMU config)
        mouse_poll();
        // Read current mouse position once per loop
        int cur_mx = -1000, cur_my = -1000;
        if (mouse_get_position(&cur_mx, &cur_my) != 0) {
            cur_mx = -1000; cur_my = -1000; // invalid
        }
        // Do not erase cursor before swap; keep it visible during backbuffer rendering.
        // advance scroll tick each frame for marquee effects
        g_tile_scroll_tick++;
        // begin frame: reset dirty tracking for optimized blit
        vga_begin_frame();
    g_dirty_hits_prev_cursor = 0;
    g_any_tile_content_redrew = 0;
    // draw all tiles
    // Avoid clearing the entire screen each frame to reduce flicker. Only clear the regions we will redraw (each tile).
    // Quick heuristic: clear each tile's rectangle before drawing it.
        // If Alt held, draw a help/status bar on the main shell tile (index 0 if active)
        extern int tui_alt_pressed;
        if (tui_alt_pressed) {
            // Build status text
            static char alt_help[128];
            snprintf(alt_help, sizeof(alt_help), "Super+n: New | Super+Arrow: Switch | Super+Q: Close/Exit | Alt: Help");
            // Draw as status on tile 0 if it exists
                if (tile_count > 0 && tiles[0].type != TILE_EMPTY) {
                    int title_h = 16;
                    int status_y = tiles[0].y + title_h;
                    drawRect(tiles[0].x, status_y, tiles[0].width, 12, 32, 32, 32);
                    // draw alt_help clipped to tile 0 area
                    int clip_min = tiles[0].x + 4;
                    int clip_max = tiles[0].x + tiles[0].width - 4;
                    for (const char* p = alt_help; *p; ++p) {
                        int cx = clip_min + (int)(p - alt_help) * 8;
                        if (cx + 7 > clip_max) break;
                        drawCharAt(cx, status_y + 2, (int)(unsigned char)*p, 255, 255, 255);
                    }
                } else {
                    // fallback: draw at top of screen
                    drawRect(0, 0, screen_w, 12, 32, 32, 32);
                    int clip_min = 4;
                    int clip_max = screen_w - 4;
                    for (const char* p = alt_help; *p; ++p) {
                        int cx = clip_min + (int)(p - alt_help) * 8;
                        if (cx + 7 > clip_max) break;
                        drawCharAt(cx, 2, (int)(unsigned char)*p, 255, 255, 255);
                    }
                }
        }
    for (int i = 0; i < tile_count; ++i) {
        if (g_force_full_redraw) tiles[i].static_drawn = 0; // force static re-render
        int decor_fresh = 0;
        if (!tiles[i].static_drawn) { draw_static_tile(&tiles[i], i == focused); decor_fresh = 1; }
            // Redraw decorations each frame to ensure subtitle/title are always current
            int need_redraw_decor = 1;
            extern int tui_alt_pressed;
            int th_now_dec = get_title_height(&tiles[i]);
            int show_status_now_dec = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
            // We still maintain caches for potential future optimization
            if (need_redraw_decor) {
                draw_decorations(&tiles[i], i == focused);
                tiles[i].last_title_ptr = tiles[i].title;
                tiles[i].last_status_left_ptr = tiles[i].status_left;
                tiles[i].last_status_right_ptr = tiles[i].status_right;
                tiles[i].last_show_status = show_status_now_dec;
                tiles[i].last_focused = (i == focused);
            }
            // Compute current content rect for this tile
            extern int tui_alt_pressed;
            int show_status_now = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
            int th_now = get_title_height(&tiles[i]);
            int sh_now = 0;
            if (show_status_now) sh_now = 12;
            int cx_now = tiles[i].x + 1;
            int cy_now = tiles[i].y + th_now + sh_now + 1;
            int cw_now = tiles[i].width - 2;
            int ch_now = tiles[i].height - (th_now + sh_now) - 2;
            // Only redraw content if the vterm version changed since last draw, rect changed, or GUI was invalidated
            unsigned int cur_ver = vterm_get_version(tiles[i].term_idx);
            int content_redrew = 0;
            int rect_changed = (cx_now != tiles[i].last_cx) || (cy_now != tiles[i].last_cy) || (cw_now != tiles[i].last_cw) || (ch_now != tiles[i].last_ch);
            int term_for_i = tiles[i].term_idx;
            if (rect_changed && term_for_i >= 0 && term_for_i < MAX_TILES) {
                // Ensure a fresh render after geometry change
                gui_needs_redraw[term_for_i] = 1;
            }
            int has_gui = (gui_draw_cb[term_for_i] != NULL);
            if (g_force_full_redraw || g_tiles_full_content_redraw || (has_gui && gui_needs_redraw[term_for_i]) || tiles[i].last_drawn_version != cur_ver || rect_changed) {
                // For GUI tiles, pre-mark the entire content area so subsequent per-primitive dirty marks
                // merge into one big rect, ensuring a single bottom-up copy and avoiding visible sweeps.
                if (has_gui && cw_now > 0 && ch_now > 0) {
                    if (rects_intersect(cx_now, cy_now, cw_now, ch_now, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                    vga_mark_dirty_rect(cx_now, cy_now, cw_now, ch_now);
                }
                draw_tile_content(&tiles[i]);
                tiles[i].last_drawn_version = cur_ver;
                tiles[i].last_cx = cx_now; tiles[i].last_cy = cy_now; tiles[i].last_cw = cw_now; tiles[i].last_ch = ch_now;
                content_redrew = 1;
                g_any_tile_content_redrew = 1;
                if (has_gui) gui_needs_redraw[term_for_i] = 0;
            }
            // Mark only the UI decoration areas (titlebar, statusbar, and 1px borders)
            // so the blit stays small while ensuring decorations are kept in sync.
            int th = get_title_height(&tiles[i]);
            extern int tui_alt_pressed;
            int sh = 0;
            int show_status = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
            if (show_status) sh = 12;
            // Mark decorations dirty only if we redrew them this frame
            if (need_redraw_decor || decor_fresh) {
                if (th > 0) {
                    if (rects_intersect(tiles[i].x, tiles[i].y, tiles[i].width, th, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                    vga_mark_dirty_rect(tiles[i].x, tiles[i].y, tiles[i].width, th);
                }
                if (sh > 0) {
                    if (rects_intersect(tiles[i].x, tiles[i].y + th, tiles[i].width, sh, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                    vga_mark_dirty_rect(tiles[i].x, tiles[i].y + th, tiles[i].width, sh);
                }
                if (rects_intersect(tiles[i].x, tiles[i].y, tiles[i].width, 1, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                vga_mark_dirty_rect(tiles[i].x, tiles[i].y, tiles[i].width, 1); // top border
                if (rects_intersect(tiles[i].x, tiles[i].y + tiles[i].height - 1, tiles[i].width, 1, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                vga_mark_dirty_rect(tiles[i].x, tiles[i].y + tiles[i].height - 1, tiles[i].width, 1); // bottom border
                if (rects_intersect(tiles[i].x, tiles[i].y, 1, tiles[i].height, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                vga_mark_dirty_rect(tiles[i].x, tiles[i].y, 1, tiles[i].height); // left border
                if (rects_intersect(tiles[i].x + tiles[i].width - 1, tiles[i].y, 1, tiles[i].height, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                vga_mark_dirty_rect(tiles[i].x + tiles[i].width - 1, tiles[i].y, 1, tiles[i].height); // right border
            }
            // If content redrew, mark its content area dirty; otherwise skip to keep blit small
            if (content_redrew) {
                // For GUI tiles, rely on the GUI's draw calls (drawRect/drawCharAt) to mark precise dirty areas.
                // For plain terminals, mark the whole content area to ensure a full region copy.
                int term_for_i2 = tiles[i].term_idx;
                int has_gui2 = (term_for_i2 >= 0 && term_for_i2 < MAX_TILES && gui_draw_cb[term_for_i2] != NULL);
                if (!has_gui2) {
                    if (cw_now > 0 && ch_now > 0) {
                        if (rects_intersect(cx_now, cy_now, cw_now, ch_now, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
                        vga_mark_dirty_rect(cx_now, cy_now, cw_now, ch_now);
                    }
                }
            } else {
                // no new content, nothing to mark beyond borders/title/status
        }
        }
        // Draw floating windows on top of tiles (back-to-front)
        if (g_window_count > 0) {
            for (int oi = 0; oi < g_window_count; ++oi) {
                int wi = g_window_order[oi];
                if (wi < 0 || wi >= MAX_WINDOWS) continue;
                window_t* w = &g_windows[wi];
                if (!w->used) continue;
                int is_focused_win = (wi == g_win_focused);
                // Redraw decorations if first time, focus changed, forced, or underlying tiles changed
                if (!w->static_drawn || w->last_focused != is_focused_win || g_force_full_redraw || g_any_tile_content_redrew) {
                    wm_draw_decor(w, is_focused_win);
                    wm_mark_decor_dirty(w);
                    w->static_drawn = 1;
                    w->last_focused = is_focused_win;
                }
                // Redraw content if requested
                if (w->needs_redraw || g_force_full_redraw || g_any_tile_content_redrew) {
                    wm_draw_content(w);
                    w->needs_redraw = 0;
                }
            }
        }
    if (g_force_full_redraw) g_force_full_redraw = 0;
    if (g_tiles_full_content_redraw) g_tiles_full_content_redraw = 0;
        tui_refresh();
        // Build swap exclusion to preserve overlays (cursor and live-drag)
        int ex_x = -1, ex_y = -1, ex_w = 0, ex_h = 0;
        // Cursor exclusion only if that area was untouched this frame
        if (!g_dirty_hits_prev_cursor && prev_saved_w > 0 && prev_saved_h > 0) {
            ex_x = prev_saved_x; ex_y = prev_saved_y; ex_w = prev_saved_w; ex_h = prev_saved_h;
        }
        // If dragging a window, also exclude its current rect; union with cursor exclusion if both present
        if (drag_active && drag_win >= 0 && g_windows[drag_win].used) {
            window_t* dw = &g_windows[drag_win];
            int wx = dw->x, wy = dw->y, ww = dw->w, wh = dw->h;
            if (ex_w > 0 && ex_h > 0) {
                int ux, uy, uw, uh; rect_union(ex_x, ex_y, ex_w, ex_h, wx, wy, ww, wh, &ux, &uy, &uw, &uh);
                ex_x = ux; ex_y = uy; ex_w = uw; ex_h = uh;
            } else {
                ex_x = wx; ex_y = wy; ex_w = ww; ex_h = wh;
            }
        }
        if (ex_w > 0 && ex_h > 0) vga_set_swap_exclude(ex_x, ex_y, ex_w, ex_h); else vga_clear_swap_exclude();
        // swap backbuffer to framebuffer (if backbuffer in use)
        vga_swap_buffers();
        // After swap, clear exclusion so future swaps can choose a new region
        vga_clear_swap_exclude();
    // Draw overlays during vblank to minimize tearing/flicker
    vga_wait_vblank();

        // Live-drag overlay: erase previous overlay and draw the dragged window as an overlay from backbuffer
        if (prev_drag_w > 0 && prev_drag_h > 0) {
            // Blit background (current backbuffer content) over the previous overlay region
            vga_blit_backbuffer_region_to_fb(prev_drag_x, prev_drag_y, prev_drag_w, prev_drag_h);
            prev_drag_w = prev_drag_h = 0;
        }
        if (drag_active && drag_win >= 0 && g_windows[drag_win].used) {
            window_t* dw = &g_windows[drag_win];
            // Draw the window from backbuffer to framebuffer at its current position
            vga_blit_backbuffer_region_to_fb(dw->x, dw->y, dw->w, dw->h);
            prev_drag_x = dw->x; prev_drag_y = dw->y; prev_drag_w = dw->w; prev_drag_h = dw->h;
        }

        // After swap: restore previous cursor region only if that area did not change this frame
        if (!g_dirty_hits_prev_cursor && cursor_prev_x > -100 && cursor_prev_y > -100 && cursor_savebuf && prev_saved_w > 0 && prev_saved_h > 0) {
            vga_restore_fb_region(prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h, cursor_savebuf, prev_saved_w * prev_saved_h * vga_get_fb_bpp_bytes());
        }

    // Draw mouse cursor overlay on top of the freshly swapped framebuffer
        if (cur_mx > -100 && cur_my > -100) {
            // Ensure save-under buffer matches current cursor size (if image changed)
            int nw = g_cursor_loaded ? g_cursor_img.header.width : cursor_w;
            int nh = g_cursor_loaded ? g_cursor_img.header.height : cursor_h;
            int bpp = vga_get_fb_bpp_bytes(); if (bpp < 3) bpp = 3;
            if (!cursor_savebuf || nw != cursor_save_w || nh != cursor_save_h) {
                int newlen = nw * nh * bpp;
                if (cursor_savebuf) free(cursor_savebuf);
                cursor_savebuf = (unsigned char*)malloc(newlen);
                cursor_save_w = nw; cursor_save_h = nh; cursor_save_len = newlen;
            }
            if (cursor_savebuf) {
                // Clip capture to screen and remember exact saved region dims
                int cap_x = cur_mx;
                int cap_y = cur_my;
                int cap_w = cursor_save_w;
                int cap_h = cursor_save_h;
                if (cap_x < 0) { cap_w += cap_x; cap_x = 0; }
                if (cap_y < 0) { cap_h += cap_y; cap_y = 0; }
                if (cap_x + cap_w > screen_w) cap_w = screen_w - cap_x;
                if (cap_y + cap_h > screen_h) cap_h = screen_h - cap_y;
                if (cap_w > 0 && cap_h > 0) {
                    int need = cap_w * cap_h * vga_get_fb_bpp_bytes();
                    if (need <= cursor_save_len) {
                        vga_capture_fb_region(cap_x, cap_y, cap_w, cap_h, cursor_savebuf, cursor_save_len);
                        prev_saved_x = cap_x; prev_saved_y = cap_y; prev_saved_w = cap_w; prev_saved_h = cap_h;
                        // Store offsets from logical cursor (cur_mx,cur_my) to the capture top-left
                        prev_saved_offx = cap_x - cur_mx;
                        prev_saved_offy = cap_y - cur_my;
                    } else {
                        prev_saved_w = prev_saved_h = 0;
                        prev_saved_offx = prev_saved_offy = 0;
                    }
                } else {
                    prev_saved_w = prev_saved_h = 0;
                    prev_saved_offx = prev_saved_offy = 0;
                }
            }
            draw_cursor_overlay(cur_mx, cur_my);
            cursor_prev_x = cur_mx; cursor_prev_y = cur_my;
        } else {
            // No valid current position: clear prev so we don't try to restore garbage next frame
            cursor_prev_x = cursor_prev_y = -1000;
            prev_saved_w = prev_saved_h = 0;
        }
        // If drag just ended, ensure the last overlay region is reconciled by the next swap
        if (!drag_active && prev_drag_w > 0 && prev_drag_h > 0) {
            // The next frame's swap will copy the backbuffer which already has the window at its final position
            // Here we just clear the overlay bookkeeping so exclusion stops next frame
            prev_drag_w = prev_drag_h = 0;
        }
        // No swap exclusion used; overlay erase is handled proactively via backbuffer blit each loop

        // Handle mouse click-to-focus and forward events to focused GUI.
    mouse_event_t me;
    if (mouse_read_event(&me) == 0) {
            // On left button press edge, set focus to the tile under cursor
            uint8 changes = me.button_changes;
            uint8 downMask = (me.buttons & MOUSE_BUTTON_LEFT);
            uint8 prevWasDown = (g_mouse_state.prev_buttons & MOUSE_BUTTON_LEFT);
            int press_edge = (changes & MOUSE_BUTTON_LEFT) && downMask && !prevWasDown;
            int release_edge = (changes & MOUSE_BUTTON_LEFT) && !downMask && prevWasDown;
            if (press_edge) {
                int w_hit = wm_hit_test(me.x, me.y);
                if (w_hit >= 0) {
                    wm_bring_to_front(w_hit);
                    // Bringing a window to front changes composition; redraw all windows
                    for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                        if (g_windows[wi].used && !g_windows[wi].minimized) {
                            g_windows[wi].needs_redraw = 1;
                            g_windows[wi].static_drawn = 0;
                        }
                    }
                    int did_action = 0;
                    // Close
                    {
                        int bx, by, bw, bh; wm_get_close_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            wm_close_window(w_hit);
                            g_force_full_redraw = 1; g_tiles_full_content_redraw = 1;
                            drag_active = 0; drag_win = -1;
                            did_action = 1;
                        }
                    }
                    // Maximize toggle
                    if (!did_action) {
                        int bx, by, bw, bh; wm_get_max_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            window_t* w = &g_windows[w_hit];
                            if (!w->maximized) {
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                w->prev_x = w->x; w->prev_y = w->y; w->prev_w = w->w; w->prev_h = w->h;
                                w->x = 0; w->y = 0; w->w = screen_w; w->h = screen_h;
                                w->maximized = 1; w->minimized = 0;
                                w->static_drawn = 0; w->needs_redraw = 1;
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                g_tiles_full_content_redraw = 1;
                            } else {
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                w->x = w->prev_x; w->y = w->prev_y; w->w = w->prev_w; w->h = w->prev_h;
                                w->maximized = 0; w->static_drawn = 0; w->needs_redraw = 1;
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                g_tiles_full_content_redraw = 1;
                            }
                            did_action = 1;
                        }
                    }
                    // Minimize toggle
                    if (!did_action) {
                        int bx, by, bw, bh; wm_get_min_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            window_t* w = &g_windows[w_hit];
                            w->minimized = !w->minimized; w->needs_redraw = 1; w->static_drawn = 0;
                            int th2 = win_title_height(w); int sh2 = win_status_height(w);
                            int cx2 = w->x + 1; int cy2 = w->y + th2 + sh2 + 1; int cw2 = w->w - 2; int ch2 = w->h - (th2 + sh2) - 2;
                            if (cw2 > 0 && ch2 > 0) vga_mark_dirty_rect(cx2, cy2, cw2, ch2);
                            did_action = 1;
                        }
                    }
                    // Drag if titlebar and not already handled
                    if (!did_action) {
                        int th = win_title_height(&g_windows[w_hit]);
                        if (point_in_rect(me.x, me.y, g_windows[w_hit].x, g_windows[w_hit].y, g_windows[w_hit].w, th)) {
                            drag_active = 1; drag_win = w_hit; drag_off_x = me.x - g_windows[w_hit].x; drag_off_y = me.y - g_windows[w_hit].y;
                        }
                    }
                    if (!did_action) {
                        g_win_focused = w_hit;
                        if (g_windows[w_hit].used) g_windows[w_hit].static_drawn = 0;
                    }
                } else {
                    int hit = tile_index_at(me.x, me.y);
                    if (hit >= 0 && hit < tile_count && hit != focused) {
                        focused = hit;
                        g_force_full_redraw = 1;
                        // Redraw all windows since tiles will repaint under them
                        for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                            if (g_windows[wi].used && !g_windows[wi].minimized) {
                                g_windows[wi].needs_redraw = 1;
                                g_windows[wi].static_drawn = 0;
                            }
                        }
                        if (gui_draw_cb[tiles[focused].term_idx]) gui_needs_redraw[tiles[focused].term_idx] = 1;
                    }
                    // Clicking outside any window clears window focus so tiles receive keys
                    g_win_focused = -1;
                    // Tiles will repaint; ensure windows repaint on top next frame
                    for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                        if (g_windows[wi].used && !g_windows[wi].minimized) {
                            g_windows[wi].needs_redraw = 1;
                            g_windows[wi].static_drawn = 0;
                        }
                    }
                }
            }
            if (release_edge) { drag_active = 0; drag_win = -1; }
            if (drag_active && drag_win >= 0) {
                window_t* w = &g_windows[drag_win];
                // mark old rect dirty
                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                int old_x = w->x, old_y = w->y, old_w = w->w, old_h = w->h;
                w->x = clampi(me.x - drag_off_x, 0, screen_w - w->w);
                w->y = clampi(me.y - drag_off_y, 0, screen_h - w->h);
                // mark new rect dirty and request redraw
                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                w->static_drawn = 0;
                w->needs_redraw = 1;
                // Ensure underlying tiles are refreshed to erase old window frame
                g_tiles_full_content_redraw = 1;
                // Invalidate other windows that intersect moved window's old or new rect
                for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                    if (wi == drag_win) continue;
                    window_t* wo = &g_windows[wi];
                    if (!wo->used || wo->minimized) continue;
                    if (rects_intersect(wo->x, wo->y, wo->w, wo->h, old_x, old_y, old_w, old_h) ||
                        rects_intersect(wo->x, wo->y, wo->w, wo->h, w->x, w->y, w->w, w->h)) {
                        wo->needs_redraw = 1;
                        wo->static_drawn = 0;
                    }
                }
            }
            // Mouse routing: deliver to top window iff mouse is inside its rect (or dragging)
            if (g_window_count > 0) {
                int topwi = g_window_order[g_window_count - 1];
                window_t* wtop = &g_windows[topwi];
                int inside_top = point_in_rect(me.x, me.y, wtop->x, wtop->y, wtop->w, wtop->h);
                if (!wtop->minimized && (inside_top || drag_active) && wtop->mouse_cb) {
                    if (!drag_active) wtop->mouse_cb(-1, &me, wtop->userdata);
                    wtop->needs_redraw = 1;
                } else {
                    // Forward to tile if not interacting with window
                    int fterm = tiles[focused].term_idx;
                    if (fterm >= 0 && fterm < MAX_TILES && gui_mouse_cb[fterm]) {
                        const mouse_event_t* cme = (const mouse_event_t*)&me;
                        gui_mouse_cb[fterm](focused, cme, gui_userdata[fterm]);
                        gui_needs_redraw[fterm] = 1;
                    }
                    // If no GUI registered for the tile, map wheel to terminal scrollback
                    if (fterm >= 0 && !gui_draw_cb[fterm] && me.wheel_delta != 0) {
                        int cur = vterm_get_scroll(fterm);
                        // Map positive wheel (wheel up) to scroll up (decrease), negative to scroll down (increase)
                        int new_scroll = cur - me.wheel_delta;
                        if (new_scroll < 0) new_scroll = 0;
                        vterm_set_scroll(fterm, new_scroll);
                        // ensure content redraw
                        g_tiles_full_content_redraw = 1;
                    }
                }
            } else {
                // No windows: forward to focused GUI client if it registered a mouse callback
                int fterm = tiles[focused].term_idx;
                if (fterm >= 0 && fterm < MAX_TILES && gui_mouse_cb[fterm]) {
                    const mouse_event_t* cme = (const mouse_event_t*)&me;
                    gui_mouse_cb[fterm](focused, cme, gui_userdata[fterm]);
                    gui_needs_redraw[fterm] = 1;
                }
                // Also map wheel to terminal scrollback when tile is a plain terminal (no GUI)
                if (fterm >= 0 && !gui_draw_cb[fterm] && me.wheel_delta != 0) {
                    int cur = vterm_get_scroll(fterm);
                    int new_scroll = cur - me.wheel_delta;
                    if (new_scroll < 0) new_scroll = 0;
                    vterm_set_scroll(fterm, new_scroll);
                    g_tiles_full_content_redraw = 1;
                }
            }

            // Ensure subtitle/status bar under the mouse is refreshed to avoid trails
            int hit2 = tile_index_at(me.x, me.y);
            if (hit2 >= 0 && hit2 < tile_count) {
                tile_t* tt = &tiles[hit2];
                int title_h2 = get_title_height(tt);
                extern int tui_alt_pressed;
                int show_status2 = (tt->status_left != NULL) || (tt->status_right != NULL) || (tt->status_visible) || (tui_alt_pressed);
                if (show_status2) {
                    int sy = tt->y + title_h2;
                    if (me.y >= sy && me.y < sy + 12) {
                        vga_mark_dirty_rect(tt->x, sy, tt->width, 12);
                    }
                }
            }
        }

        int key = tui_read_key();
        // Super+n -> new shell
        if ((key & 0x4000) && (key & 0xFF) == 'n') {
            if (tile_count < 4) {
                int idx = tile_count++;
                tiles[idx].type = TILE_SHELL;
                tiles[idx].title = "EYN-OS Shell";
                tiles[idx].status_left = NULL;
                tiles[idx].status_right = NULL;
                tiles[idx].term_idx = idx;
                tiles[idx].active = 1;
                    // ensure a fresh vterm buffer so prompt isn't duplicated
                    vterm_clear(idx);
                    vterm_set_active(idx, 1);
                // print prompt for the new vterm
                vterm_print_prompt(idx);
                focused = idx;
                g_force_full_redraw = 1;
                layout_tiles();
                if (gui_draw_cb[tiles[idx].term_idx]) gui_needs_redraw[tiles[idx].term_idx] = 1;
            }
            continue;
        }

        // Super + arrows to move focus; arrow codes are 0x1001..0x1004
        // tui_read_key encodes Super by OR'ing 0x4000. Clear that bit to get the base arrow code.
        if ((key & 0x4000) ) {
            int base = key & (~0x4000); // remove Super modifier bit
            // Update debug status with last key seen (helps diagnose encoding at runtime)
            if (tiles[0].status_left) {
                static char dbg[128];
                snprintf(dbg, sizeof(dbg), "Fb: %dx%d Key:0x%04x Base:0x%04x", screen_w, screen_h, key, base);
                tiles[0].status_left = dbg;
            }
            // Super+F toggle fullscreen for focused tile
            if ((base & 0xFF) == 'f') {
                if (fullscreen_tile == -1) {
                    fullscreen_tile = focused;
                } else {
                    fullscreen_tile = -1; // exit fullscreen
                    layout_tiles();
                }
                g_force_full_redraw = 1;
                if (gui_draw_cb[tiles[focused].term_idx]) gui_needs_redraw[tiles[focused].term_idx] = 1;
                continue;
            }
            if (base >= 0x1001 && base <= 0x1004) {
                if (base == 0x1001) { // up
                    if (tile_count == 4) {
                        if (focused == 2) focused = 0;
                        else if (focused == 3) focused = 1;
                    } else if (tile_count == 3) {
                        // layout: 0 = left tall, 1 = top-right, 2 = bottom-right
                            if (focused == 2) focused = 1; // bottom-right -> top-right
                            else if (focused == 1) focused = 0; // top-right -> left
                    } else if (tile_count == 2) focused = 0;
                } else if (base == 0x1002) { // down
                    if (tile_count == 4) {
                        if (focused == 0) focused = 2;
                        else if (focused == 1) focused = 3;
                    } else if (tile_count == 3) {
                        // from top-right (1) go to bottom-right (2)
                        if (focused == 0) focused = 2;
                        else if (focused == 1) focused = 2;
                    } else if (tile_count == 2) focused = 1;
                } else if (base == 0x1003) { // left
                    if (tile_count >= 2) {
                        if (focused == 1) focused = 0;
                        else if (focused == 3) focused = 2;
                    }
                } else if (base == 0x1004) { // right
                    if (tile_count >= 2) {
                        if (focused == 0) focused = 1;
                        else if (focused == 2) focused = 3;
                        else if (tile_count == 2) focused = 1;
                    }
                }
                continue;
            }
            // Super+Q to close focused tile or focused window
            if ((base & 0xFF) == 'q') {
                // If a window is focused, close it; otherwise, close tile or exit
                if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                    wm_close_window(g_win_focused);
                    g_win_focused = -1;
                    g_force_full_redraw = 1;
                } else {
                    // If more than one tile, close the focused tile; otherwise exit tiling manager (return to main shell)
                    if (tile_count > 1) {
                        tile_close(focused);
                        g_force_full_redraw = 1;
                        layout_tiles();
                    } else {
                        running = 0; // exit tiling manager and return to main shell
                        break;
                    }
                }
                continue;
            }
        }

        // Esc to exit
        if (key == 27) {
            running = 0;
            break;
        }

        // Ctrl+X: close focused window (global) or default-exit focused GUI tile
        if (key == 0x2002) {
            if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                wm_close_window(g_win_focused);
                g_win_focused = -1;
                g_force_full_redraw = 1;
                continue;
            } else {
                // Default for GUI tiles: give the app a chance to handle Ctrl+X; if still registered, unregister
                int term = tiles[focused].term_idx;
                if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
                    tile_gui_key_cb cb = gui_key_cb[term];
                    void* ud = gui_userdata[term];
                    cb(focused, key & 0xFFFF, ud);
                    // If the GUI is still registered (app didn't close itself), default to unregister
                    if (gui_key_cb[term]) {
                        tile_unregister_gui_client(focused);
                        g_force_full_redraw = 1;
                    }
                    continue;
                }
            }
        }

        // Input routing: if a window is focused, it gets keys; else focused tile
        if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
            window_t* wfocus = &g_windows[g_win_focused];
            if (wfocus->key_cb) { wfocus->key_cb(-1, key & 0xFFFF, wfocus->userdata); wfocus->needs_redraw = 1; }
        } else {
            // Route input to focused vterm or GUI client
            int term = tiles[focused].term_idx;
            if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
                gui_key_cb[term](focused, key & 0xFFFF, gui_userdata[term]);
                gui_needs_redraw[term] = 1;
                g_any_tile_content_redrew = 1; // tile is likely to repaint
            } else {
                vterm_handle_key(tiles[focused].term_idx, key & 0xFFFF);
                // Shell/vterm content will change; ensure windows repaint afterward
                g_any_tile_content_redrew = 1;
            }
        }
    }
}
