// Allow safely re-entering the tiling-manager loop after aborting a user task.
static int g_tm_initialized = 0;
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
// allow sleeping the CPU when tiling manager is idle
#include <sched.h>
#include <watchdog.h>

#define MAX_TILES 4
#define MAX_WINDOWS 8

// Provided by the bootloader; used for framebuffer size and memory totals
extern multiboot_info_t* g_mbi;

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
// Periodic GUI heartbeat to allow time-driven GUI apps (like stats) to refresh without busy loops
static uint32 g_last_gui_heartbeat_tick = 0;
// Low-spec mode: enabled automatically on <8MB systems; also tunes frame pacing
static int g_gui_low_mode = 0;           // global toggle for low-memory/slow-CPU optimizations
static uint32 g_frame_target_us = 0;     // frame pacing target in microseconds (0 = unlimited)
static uint32 g_last_frame_tick = 0;     // last frame start tick for limiter
// Wireframe drag overlay state (low mode): previous outline rect to erase cheaply
static int prev_outline_x = -1, prev_outline_y = -1, prev_outline_w = 0, prev_outline_h = 0;
// Drag throttle (ticks between visual updates) to avoid overdraw on slow CPUs
static uint32 g_drag_throttle_ticks = 0; // computed from HZ (e.g., ~20-33ms)
static uint32 g_last_drag_tick = 0;

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

// ---------------- Per-tile background images ----------------
typedef enum { BG_NONE=0, BG_TILE=1, BG_SCALE=2, BG_CENTER=3 } bg_mode_t;
typedef struct {
    rei_image_t* img;   // owned; freed when replaced/cleared
    bg_mode_t mode;     // how to draw
    uint8_t darken;     // 0..255 darken factor (0=none)
    uint8_t adapt_text; // 1=auto switch terminal text brightness based on bg
    uint8_t text_shadow; // 1=draw a small shadow/glow behind terminal text for readability
    uint8_t local_darken; // 1=darken only behind visible terminal text cells instead of the whole background
} tile_bg_t;
static tile_bg_t g_tile_bg[MAX_TILES];

// Convert RGBA image to RGB in-place for background use (alpha<128 becomes black)
static void bg_convert_rgba_to_rgb(rei_image_t* im) {
    if (!im || !im->data) return;
    if (im->header.depth != REI_DEPTH_RGBA) return;
    int w = im->header.width, h = im->header.height;
    size_t out_sz = (size_t)w * (size_t)h * (size_t)REI_DEPTH_RGB;
    uint8_t* rgb = (uint8_t*)malloc(out_sz);
    if (!rgb) return;
    const uint8_t* src = im->data;
    size_t si = 0, di = 0; size_t total = (size_t)w * (size_t)h;
    for (size_t i = 0; i < total; ++i) {
        uint8_t r8 = src[si + 0], g8 = src[si + 1], b8 = src[si + 2], a = src[si + 3];
        if (a < 128) { r8 = g8 = b8 = 0; }
        rgb[di + 0] = r8; rgb[di + 1] = g8; rgb[di + 2] = b8;
        si += 4; di += 3;
    }
    free(im->data);
    im->data = rgb;
    im->data_size = (int)out_sz;
    im->header.depth = REI_DEPTH_RGB;
}

static inline void apply_darken(uint8_t* pr, uint8_t* pg, uint8_t* pb, uint8_t factor) {
    if (!factor) return; // 0=no darken
    *pr = (uint8_t)((uint16_t)(*pr) * (255 - factor) / 255);
    *pg = (uint8_t)((uint16_t)(*pg) * (255 - factor) / 255);
    *pb = (uint8_t)((uint16_t)(*pb) * (255 - factor) / 255);
}
static inline uint8_t rgb_luma(uint8_t r8, uint8_t g8, uint8_t b8) {
    return (uint8_t)((77*r8 + 150*g8 + 29*b8) >> 8);
}

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

// Use vga_fillRect_fb for overlay rect fills; local helper removed for simplicity

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

// ---------------- Icon cache for small file icons ----------------
#define ICON_CACHE_MAX 64
typedef struct {
    char ext[16];
    rei_image_t img;
    int loaded;
} icon_cache_entry_t;

static icon_cache_entry_t g_icon_cache[ICON_CACHE_MAX];
static int g_icon_cache_count = 0;

// Try a few candidate paths for an icon with given extension and load into cache entry
static rei_image_t* load_icon_for_ext(const char* ext) {
    if (!ext) return NULL;
    // Check cache first
    for (int i = 0; i < g_icon_cache_count; ++i) {
        if (strcmp(g_icon_cache[i].ext, ext) == 0) {
            return g_icon_cache[i].loaded ? &g_icon_cache[i].img : NULL;
        }
    }
    if (g_icon_cache_count >= ICON_CACHE_MAX) return NULL;

    // Candidate filename patterns.
    // ext is treated as the full icon base name (without .rei), e.g. "file_txt", "file_none", "dir_empty", "dir_full".
    const char* patterns[] = {
        "/icons/%s.rei",
        "/testdir/icons/%s.rei",
        // Legacy fallbacks (older naming conventions)
        "/icons/file_%s.rei",
        "/testdir/icons/file_%s.rei",
        NULL
    };

    char pathbuf[256];
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        return NULL;
    }

    rei_image_t tmp;
    int found = 0;
    for (int pi = 0; patterns[pi]; ++pi) {
        snprintf(pathbuf, sizeof(pathbuf), patterns[pi], ext);
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(0, &sb, pathbuf, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) break;
            int n = eynfs_read_file(0, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &tmp) == 0) {
                    found = 1;
                }
            }
            free(buf);
            if (found) break;
        }
    }
    // No special-case fallback needed here; callers can request "file_none" if desired.

    // Add to cache entry regardless (to avoid repeated failed lookups)
    int idx = g_icon_cache_count++;
    memset(&g_icon_cache[idx], 0, sizeof(g_icon_cache[idx]));
    strncpy(g_icon_cache[idx].ext, ext, sizeof(g_icon_cache[idx].ext)-1);
    if (found) {
        g_icon_cache[idx].img = tmp; // struct copy (contains allocated data)
        g_icon_cache[idx].loaded = 1;
        return &g_icon_cache[idx].img;
    }
    g_icon_cache[idx].loaded = 0;
    return NULL;
}

// Draw an REI image into the backbuffer at (x,y). Honor alpha for RGBA and use blending.
static void draw_rei_at(const rei_image_t* im, int x, int y) {
    if (!im || !im->data) return;
    int w = im->header.width;
    int h = im->header.height;
    int depth = im->header.depth;
    if (w <= 0 || h <= 0) return;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            int off = (py * w + px) * depth;
            if (off < 0 || (size_t)(off + depth) > (size_t)im->data_size) continue;
            uint8_t sr = 0, sg = 0, sb = 0, sa = 255;
            if (depth == REI_DEPTH_MONO) { sr = sg = sb = im->data[off]; }
            else if (depth == REI_DEPTH_RGB) { sr = im->data[off]; sg = im->data[off+1]; sb = im->data[off+2]; }
            else if (depth == REI_DEPTH_RGBA) { sr = im->data[off]; sg = im->data[off+1]; sb = im->data[off+2]; sa = im->data[off+3]; }
            if (depth == REI_DEPTH_RGBA) {
                if (sa == 0) continue; // fully transparent
                // Blend using vga_blendPixel_bb
                vga_blendPixel_bb(x + px, y + py, sr, sg, sb, (int)sa);
            } else {
                // opaque
                vga_drawPixel_bb(x + px, y + py, sr, sg, sb);
            }
        }
    }
    vga_mark_dirty_rect(x, y, w, h);
}

// Forward declarations for icon loaders (must appear before start_tiling_manager)
static void load_close_icon_try_paths(uint8 disk);
static void load_min_icon_try_paths(uint8 disk);
static void load_max_icon_try_paths(uint8 disk);

static void wm_draw_decor(window_t* w, int is_focused) {
    // In low mode, draw simpler decorations: skip icons and minimize overdraw
    if (g_gui_low_mode) {
        int th = win_title_height(w);
        int sh = win_status_height(w);
        // frame background
        drawRect(w->x, w->y, w->w, w->h, 0, 0, 0);
        // title bar
        int title_color_r = is_focused ? 160 : 90;
        int title_color_g = is_focused ? 160 : 90;
        int title_color_b = is_focused ? 50 : 50;
        drawRect(w->x, w->y, w->w, th, title_color_r, title_color_g, title_color_b);
        if (w->title && w->title[0]) {
            int len = strlen(w->title);
            int max_chars = (w->w / 8) - 2; if (max_chars < 0) max_chars = 0;
            int color_r = is_focused ? 0 : 255, color_g = is_focused ? 0 : 255, color_b = is_focused ? 0 : 255;
            int text_y = w->y + 4;
            if (len > max_chars) len = max_chars;
            int start_x = w->x + 4;
            for (int i = 0; i < len; ++i) drawCharAt(start_x + i * 8, text_y, (unsigned char)w->title[i], color_r, color_g, color_b);
        }
        // optional compact status
        if (sh > 0) {
            int sy = w->y + th;
            drawRect(w->x, sy, w->w, sh, 32, 32, 32);
            const char* left = w->status_left ? w->status_left : "";
            int max_chars = (w->w - 8) / 8; if (max_chars < 0) max_chars = 0;
            for (int i = 0; left[i] && i < max_chars; ++i) drawCharAt(w->x + 4 + i * 8, sy + 2, (unsigned char)left[i], 220, 220, 220);
        }
        // border
        int br = is_focused ? 120 : 60, bg = is_focused ? 120 : 60, bb = is_focused ? 80 : 60;
        drawRect(w->x, w->y, w->w, 1, br, bg, bb);
        drawRect(w->x, w->y + w->h - 1, w->w, 1, br, bg, bb);
        drawRect(w->x, w->y, 1, w->h, br, bg, bb);
        drawRect(w->x + w->w - 1, w->y, 1, w->h, br, bg, bb);
        return;
    }
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
                    int bpp_blend = vga_get_fb_bpp_bytes(); if (bpp_blend < 3) bpp_blend = 3;
                    int idx = (by * prev_saved_w + bx) * bpp_blend;
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
    // In low mode, draw simpler title/status without scrolling text or fancy clipping
    if (g_gui_low_mode) {
        if (title_h > 0) {
            int title_y = t->y;
            int title_color_r = is_focused ? 160 : 90;
            int title_color_g = is_focused ? 160 : 90;
            int title_color_b = is_focused ? 50 : 50;
            drawRect(t->x, title_y, t->width, title_h, title_color_r, title_color_g, title_color_b);
            if (t->title && t->title[0]) {
                int title_len = (int)strlen(t->title);
                int max_chars = t->width / 8; if (max_chars < 0) max_chars = 0;
                int text_y = title_y + 4;
                int cr = is_focused ? 0 : 255, cg = is_focused ? 0 : 255, cb = is_focused ? 0 : 255;
                if (title_len <= max_chars) {
                    int start_x = t->x + (t->width - (8 * title_len)) / 2;
                    for (int i = 0; i < title_len; ++i) {
                        int cx = start_x + i * 8;
                        int clip_left = t->x + 4;
                        int clip_right = t->x + t->width - 4;
                        if (cx < clip_left) continue;
                        if (cx + 8 > clip_right) break;
                        if (cx < 0 || cx + 8 > screen_w) break;
                        drawCharAt(cx, text_y, (int)(unsigned char)t->title[i], cr, cg, cb);
                    }
                } else {
                    int window = max_chars;
                    int speed = 6;
                    int period = title_len + window;
                    int pos = (g_tile_scroll_tick / speed) % period;
                    for (int i = 0; i < window; ++i) {
                        int idx = pos + i; char ch = ' ';
                        if (idx < title_len) ch = t->title[idx];
                        else { int wrap_idx = idx - title_len; if (wrap_idx < title_len) ch = t->title[wrap_idx]; }
                        int cx = t->x + i * 8;
                        int clip_left = t->x + 4;
                        int clip_right = t->x + t->width - 4;
                        if (cx < clip_left) continue;
                        if (cx + 8 > clip_right) break;
                        if (cx < 0 || cx + 8 > screen_w) break;
                        drawCharAt(cx, text_y, (int)(unsigned char)ch, cr, cg, cb);
                    }
                }
            }
        }
    int status_h = 0;
        int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
        if (show_status) {
            int status_y = t->y + title_h;
            status_h = 12;
            drawRect(t->x, status_y, t->width, status_h, 32, 32, 32);
            const char* left_text = t->status_left ? t->status_left : "";
            int left_len = (int)strlen(left_text);
            int max_chars = (t->width - 8) / 8; if (max_chars < 0) max_chars = 0;
            if (left_len > max_chars) left_len = max_chars;
            for (int i = 0; i < left_len; ++i) drawCharAt(t->x + 4 + i * 8, status_y + 2, (unsigned char)left_text[i], 220, 220, 220);
        }
        int border_r = is_focused ? 120 : 48, border_g = is_focused ? 120 : 48, border_b = is_focused ? 80 : 48;
        drawRect(t->x, t->y, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y + t->height - 1, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y, 1, t->height, border_r, border_g, border_b);
        drawRect(t->x + t->width - 1, t->y, 1, t->height, border_r, border_g, border_b);
        return;
    }
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
    int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
    if (show_status) status_h = 12;
    // Keep a 1px margin for the border so clears don't overwrite border pixels
    int content_x = t->x + 1;
    int content_y = t->y + title_h + status_h + 1;
    int line_h = 8;
    int max_lines = (t->height - (title_h + status_h + 2)) / line_h;
    int content_w = t->width - 2; // leave 1px border on left/right
    int content_h = t->height - (title_h + status_h) - 2; // leave 1px top/bottom border
    int bg_bright = -1; // -1 unknown; 0=dark; 1=bright
    if (content_w > 0 && content_h > 0) {
        // Always start with a clean content area to avoid any residuals around centered/scaled images
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
        // Debug: show number of registered redirect icons (non-invasive onscreen feedback)
        if (shell_redirect_icon_count > 0) {
            char label[8];
            int n = (shell_redirect_icon_count > 9) ? 9 : shell_redirect_icon_count;
            label[0] = 'I'; label[1] = 'c'; label[2] = ':'; label[3] = '0' + n; label[4] = '\0';
            for (int i = 0; label[i]; ++i) drawCharAt(content_x + i*8, content_y, (int)(unsigned char)label[i], 255, 200, 0);
        }
        // Draw configured background (if any); else clear to black
        int ti_bg = t - tiles;
        tile_bg_t* bg = (ti_bg >= 0 && ti_bg < MAX_TILES) ? &g_tile_bg[ti_bg] : NULL;
        int drew_bg = 0;
        if (bg && bg->img && bg->img->data && bg->mode != BG_NONE) {
            const rei_image_t* im = bg->img; int iw = im->header.width, ih = im->header.height; int depth = im->header.depth; const uint8_t* base = im->data;
            if (bg->mode == BG_TILE) {
                for (int yy = 0; yy < content_h; ++yy) {
                    int sy = yy % ih; const uint8_t* row = base + sy * iw * depth; int py = content_y + yy;
                    for (int xx = 0; xx < content_w; ++xx) {
                        int sx = xx % iw; int px = content_x + xx;
                        if (depth == REI_DEPTH_MONO) { uint8_t v=row[sx]; apply_darken(&v,&v,&v,bg->darken); vga_drawPixel_bb(px, py, v, v, v); }
                        else if (depth == REI_DEPTH_RGB) { const uint8_t* p=row+sx*3; uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8); }
                        else if (depth == REI_DEPTH_RGBA) { const uint8_t* p=row+sx*4; uint8_t a=p[3]; if (a>=128){ uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8);} }
                    }
                }
                drew_bg = 1;
            } else if (bg->mode == BG_CENTER) {
                int ox = content_x + (content_w - iw)/2; int oy = content_y + (content_h - ih)/2;
                for (int yy=0; yy<ih; ++yy) { int py = oy + yy; if (py < content_y || py >= content_y + content_h) continue; const uint8_t* row = base + yy * iw * depth;
                    for (int xx=0; xx<iw; ++xx) { int px = ox + xx; if (px < content_x || px >= content_x + content_w) continue;
                        if (depth == REI_DEPTH_MONO) { uint8_t v=row[xx]; apply_darken(&v,&v,&v,bg->darken); vga_drawPixel_bb(px, py, v,v,v); }
                        else if (depth == REI_DEPTH_RGB) { const uint8_t* p=row+xx*3; uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8);} 
                        else if (depth == REI_DEPTH_RGBA) { const uint8_t* p=row+xx*4; uint8_t a=p[3]; if (a>=128){ uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8);} }
                    }
                }
                drew_bg = 1;
            } else if (bg->mode == BG_SCALE) {
                int sx_num = content_w, sx_den = iw; int sy_num = content_h, sy_den = ih;
                int use_x = ( (long long)sx_num * sy_den <= (long long)sy_num * sx_den );
                int num = use_x ? sx_num : sy_num; int den = use_x ? sx_den : sy_den;
                if (den > 0 && num > 0) {
                    int dw = (iw * num) / den; int dh = (ih * num) / den;
                    int ox = content_x + (content_w - dw)/2; int oy = content_y + (content_h - dh)/2;
                    for (int yy=0; yy<dh; ++yy) { int syi = (yy * den) / num; if (syi >= ih) syi = ih - 1; const uint8_t* row = base + syi * iw * depth; int py = oy + yy; if (py < content_y || py >= content_y + content_h) continue;
                        for (int xx=0; xx<dw; ++xx) { int sxi = (xx * den) / num; if (sxi >= iw) sxi = iw - 1; int px = ox + xx; if (px < content_x || px >= content_x + content_w) continue;
                            if (depth == REI_DEPTH_MONO) { uint8_t v=row[sxi]; apply_darken(&v,&v,&v,bg->darken); vga_drawPixel_bb(px, py, v,v,v); }
                            else if (depth == REI_DEPTH_RGB) { const uint8_t* p=row+sxi*3; uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8);} 
                            else if (depth == REI_DEPTH_RGBA) { const uint8_t* p=row+sxi*4; uint8_t a=p[3]; if (a>=128){ uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); vga_drawPixel_bb(px, py, r8,g8,b8);} }
                        }
                    }
                    drew_bg = 1;
                }
            }
            if (drew_bg && bg->adapt_text) {
                // Estimate brightness from source image center after darken
                int sx = im->header.width/2; if (sx>=iw) sx=iw-1; int sy = im->header.height/2; if (sy>=ih) sy=ih-1; const uint8_t* row = base + sy * iw * depth;
                int luma = 128;
                if (depth == REI_DEPTH_MONO) { uint8_t v=row[sx]; apply_darken(&v,&v,&v,bg->darken); luma = rgb_luma(v,v,v); }
                else if (depth == REI_DEPTH_RGB) { const uint8_t* p=row+sx*3; uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); luma = rgb_luma(r8,g8,b8); }
                else if (depth == REI_DEPTH_RGBA) { const uint8_t* p=row+sx*4; if (p[3]>=128){ uint8_t r8=p[0],g8=p[1],b8=p[2]; apply_darken(&r8,&g8,&b8,bg->darken); luma = rgb_luma(r8,g8,b8);} }
                bg_bright = (luma >= 128) ? 1 : 0;
            }
        }
        if (!drew_bg) {
            // No background configured
            drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
        }
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
                int line_indent_px = 0;
                int line_anchor_col = 0;
                const char* line_icon_key = vterm_get_line_icon_key(t->term_idx, abs_row, &line_indent_px, &line_anchor_col);
                if (!line_icon_key) line_indent_px = 0;

                // If this wrapped segment contains the anchor column, draw the icon once on this visual row.
                if (line_icon_key && line_anchor_col >= start_col && line_anchor_col < start_col + cols) {
                    int py_icon = content_y + vis_row * line_h;
                    int x_text0 = content_x + (line_anchor_col - start_col) * 8 + line_indent_px;
                    int icon_x = x_text0 - 9;
                    rei_image_t* icon = load_icon_for_ext(line_icon_key);
                    if (!icon) icon = load_icon_for_ext("file_none");
                    if (icon) draw_rei_at(icon, icon_x, py_icon);
                }

                for (int cc = 0; cc < cols; ++cc) {
                    int px = content_x + cc * 8 + line_indent_px;
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
                    // Adaptive default text brightness against background
                    if (bg_bright != -1 && rr == 200 && gg == 200 && bb == 200) {
                        if (bg_bright) { rr = 40; gg = 40; bb = 40; } else { rr = 230; gg = 230; bb = 230; }
                    } else {
                        // If a background is set but adaptive is off, lift default gray to white-ish
                        int ti_bg_idx2 = t - tiles;
                        if (ti_bg_idx2 >= 0 && ti_bg_idx2 < MAX_TILES) {
                            tile_bg_t* bgp2 = &g_tile_bg[ti_bg_idx2];
                            if (bgp2->img && bgp2->mode != BG_NONE && !bgp2->adapt_text) {
                                if (rr == 200 && gg == 200 && bb == 200) { rr = 240; gg = 240; bb = 240; }
                            }
                        }
                    }
                    // Optional local darken: semi-transparent darken behind non-space glyphs
                    {
                        int ti_bg_idx3 = t - tiles;
                        if (ti_bg_idx3 >= 0 && ti_bg_idx3 < MAX_TILES) {
                            tile_bg_t* bgp3 = &g_tile_bg[ti_bg_idx3];
                            if (bgp3->img && bgp3->mode != BG_NONE && bgp3->local_darken && ch != ' ') {
                                // Multiply darken by ~50% for a softer look
                                vga_darkenRect_bb(px, py, 8, 8, 128);
                            }
                        }
                    }
                    int is_sel = vterm_is_selected(t->term_idx, abs_row, src_col);
                    if (is_sel) {
                        // selection background teal-ish
                        drawRect(px, py, 8, 8, 0, 128, 128);
                    }
                    // Optional text shadow: draw small dark offset before main glyph
                    int ti_bg_idx = t - tiles;
                    int use_shadow = 0; int sr = 0, sg = 0, sb = 0;
                    if (ti_bg_idx >= 0 && ti_bg_idx < MAX_TILES) {
                        tile_bg_t* bgp = &g_tile_bg[ti_bg_idx];
                        if (bgp->img && bgp->text_shadow) {
                            use_shadow = 1;
                            // Choose shadow color opposing the text brightness slightly for contrast
                            // If text is bright, use darker shadow; if text is dark, use very dark gray
                            if (rr + gg + bb > 380) { sr = 0; sg = 0; sb = 0; }
                            else { sr = 10; sg = 10; sb = 10; }
                        }
                    }
                    if (use_shadow) {
                        // 1px down-right shadow
                        drawCharAt(px + 1, py + 1, (int)(unsigned char)ch, sr, sg, sb);
                    }

                    // Draw main character
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

// Modal state for background choice
static struct { int active; int tile; rei_image_t* img; int selected; int only_scale; } g_bg_modal = {0, -1, NULL, 0, 0};

// Very small modal: draws a centered box with options Tile/Scale/Center
static void draw_bg_modal() {
    if (!g_bg_modal.active) return;
    const char* opts_all[3] = {"Tile", "Scale", "Center"};
    int opt_count = g_bg_modal.only_scale ? 1 : 3;
    const char** opts = opts_all;
    int box_w = 18 * 8; int box_h = (opt_count + 4) * 8; // simple size
    int bx = (screen_w - box_w) / 2; int by = (screen_h - box_h) / 2;
    // Dim background by drawing a translucent-like dark box using backbuffer primitives
    // We avoid full-screen framebuffer fills to prevent flicker; draw a padded backdrop instead.
    int pad = 12;
    int dbx = bx - pad; if (dbx < 0) dbx = 0;
    int dby = by - pad; if (dby < 0) dby = 0;
    int dbw = box_w + pad*2; if (dbx + dbw > screen_w) dbw = screen_w - dbx;
    int dbh = box_h + pad*2; if (dby + dbh > screen_h) dbh = screen_h - dby;
    drawRect(dbx, dby, dbw, dbh, 0, 0, 0);
    // border and title
    drawRect(bx, by, box_w, box_h, 32, 32, 32);
    const char* title = "Background Mode";
    for (int i = 0; title[i]; ++i) drawCharAt(bx + 8 + i*8, by + 8, (int)(unsigned char)title[i], 255, 255, 0);
    const char* hint = "Enter=Select  Esc=Cancel";
    for (int i = 0; hint[i]; ++i) drawCharAt(bx + 8 + i*8, by + 24, (int)(unsigned char)hint[i], 200, 200, 200);
    int y0 = by + 40;
    for (int i = 0; i < opt_count; ++i) {
        int rr = (i == g_bg_modal.selected) ? 255 : 200;
        int gg = (i == g_bg_modal.selected) ? 255 : 200;
        int bb = (i == g_bg_modal.selected) ? 0 : 200;
        for (int j = 0; opts[i][j]; ++j) drawCharAt(bx + 16 + j*8, y0 + i*12, (int)(unsigned char)opts[i][j], rr, gg, bb);
    }
    // mark only the modal region dirty to keep the blit minimal
    vga_mark_dirty_rect(dbx, dby, dbw, dbh);
}

// Handle keys for the modal; return 1 if consumed
static int handle_bg_modal_key(int key) {
    if (!g_bg_modal.active) return 0;
    int opt_count = g_bg_modal.only_scale ? 1 : 3;
    if (key == 0x1001) { // up
        if (g_bg_modal.selected > 0) {
            g_bg_modal.selected--;
        }
        return 1;
    }
    if (key == 0x1002) { // down
        if (g_bg_modal.selected < opt_count - 1) {
            g_bg_modal.selected++;
        }
        return 1;
    }
    if (key == 27) { // Esc
        // cancel
        if (g_bg_modal.img) { rei_free_image(g_bg_modal.img); free(g_bg_modal.img); }
        g_bg_modal.active = 0; g_bg_modal.tile = -1; g_bg_modal.img = NULL; return 1;
    }
    if (key == '\n' || key == 10) {
        int mode = g_bg_modal.only_scale ? BG_SCALE : (g_bg_modal.selected==0?BG_TILE:(g_bg_modal.selected==1?BG_SCALE:BG_CENTER));
        int ti = g_bg_modal.tile;
        if (ti >= 0 && ti < MAX_TILES) {
            // Clear old
            if (g_tile_bg[ti].img) { rei_free_image(g_tile_bg[ti].img); free(g_tile_bg[ti].img); g_tile_bg[ti].img = NULL; }
            g_tile_bg[ti].img = g_bg_modal.img; g_bg_modal.img = NULL;
            // For backgrounds, if image has alpha, convert to RGB with alpha->black
            if (g_tile_bg[ti].img && g_tile_bg[ti].img->header.depth == REI_DEPTH_RGBA) {
                bg_convert_rgba_to_rgb(g_tile_bg[ti].img);
            }
            g_tile_bg[ti].mode = (bg_mode_t)mode;
            g_tile_bg[ti].darken = 16; // keep global darken light; rely on local darken under text
            g_tile_bg[ti].adapt_text = 0; // keep terminal default colors (often white) consistent
            g_tile_bg[ti].text_shadow = 1; // default: enable shadow for readability
            g_tile_bg[ti].local_darken = 1; // darken only behind text
            // Force content redraw
            g_tiles_full_content_redraw = 1;
        } else {
            if (g_bg_modal.img) { rei_free_image(g_bg_modal.img); free(g_bg_modal.img); }
        }
        g_bg_modal.active = 0; g_bg_modal.tile = -1; g_bg_modal.img = NULL; return 1;
    }
    return 1;
}

// Optional: allow mouse to click options to avoid keyboard-only selection
static int handle_bg_modal_mouse(const mouse_event_t* me) {
    if (!g_bg_modal.active || !me) return 0;
    const char* opts_all[3] = {"Tile", "Scale", "Center"};
    int opt_count = g_bg_modal.only_scale ? 1 : 3;
    int box_w = 18 * 8; int box_h = (opt_count + 4) * 8;
    int bx = (screen_w - box_w) / 2; int by = (screen_h - box_h) / 2;
    int y0 = by + 40;
    int left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    for (int i = 0; i < opt_count; ++i) {
        int text_w = (int)strlen(opts_all[i]) * 8;
        int ox = bx + 16; int oy = y0 + i * 12;
        int ow = text_w; int oh = 10;
        if (me->x >= ox && me->x < ox + ow && me->y >= oy && me->y < oy + oh) {
            // Hover highlights
            if (g_bg_modal.selected != i) {
                g_bg_modal.selected = i;
            }
            if (left_down) {
                // Synthesize Enter
                (void)handle_bg_modal_key('\n');
            }
            return 1; // consumed by modal
        }
    }
    return 1; // consume mouse while modal active even if not over an option
}

int tile_begin_set_background_from_rei(int tile_idx, rei_image_t* image) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES || !image) return -1;
    // If image is larger than screen in either dimension, restrict to Scale only
    int only_scale = (image->header.width > screen_w) || (image->header.height > screen_h);
    g_bg_modal.active = 1; g_bg_modal.tile = tile_idx; g_bg_modal.img = image; g_bg_modal.selected = only_scale ? 0 : 1; g_bg_modal.only_scale = only_scale;
    // Ensure next frame draws modal
    g_force_full_redraw = 1; g_tiles_full_content_redraw = 1;
    return 0;
}

void tile_clear_background(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    if (g_tile_bg[tile_idx].img) { rei_free_image(g_tile_bg[tile_idx].img); free(g_tile_bg[tile_idx].img); g_tile_bg[tile_idx].img = NULL; }
    g_tile_bg[tile_idx].mode = BG_NONE; g_tile_bg[tile_idx].darken = 0; g_tile_bg[tile_idx].adapt_text = 0; g_tile_bg[tile_idx].text_shadow = 0; g_tile_bg[tile_idx].local_darken = 0;
    g_tiles_full_content_redraw = 1;
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

void tile_render_once(void) {
    // This is intentionally a minimal subset of the main tiler loop: it only
    // redraws tiles and swaps buffers. It does not poll keyboard/mouse or run
    // modal/window interaction logic.
    static volatile int g_in_render_once = 0;
    if (g_in_render_once) return;
    g_in_render_once = 1;
    if (!g_tm_initialized) { g_in_render_once = 0; return; }
    if (tile_count <= 0) { g_in_render_once = 0; return; }

    // Best-effort: if framebuffer dimensions are unknown, avoid drawing.
    if (screen_w <= 0 || screen_h <= 0) { g_in_render_once = 0; return; }

    // Advance effects that are otherwise driven per-frame.
    g_tile_scroll_tick++;

    vga_begin_frame();

    // Conservatively force a full redraw so the on-screen output is guaranteed
    // to reflect vterm content changes while the main loop is paused.
    g_force_full_redraw = 1;

    for (int i = 0; i < tile_count; ++i) {
        if (g_force_full_redraw) tiles[i].static_drawn = 0;
        if (!tiles[i].static_drawn) {
            draw_static_tile(&tiles[i], i == focused);
        }
        draw_tile_content(&tiles[i]);
        tiles[i].last_drawn_version = vterm_get_version(tiles[i].term_idx);
    }

    // Ensure the swap copies something even if dirty tracking is disabled.
    vga_mark_dirty_rect(0, 0, screen_w, screen_h);

    tui_refresh();
    // Force an immediate present. Prefer a direct backbuffer->fb blit so we
    // don’t depend on dirty-rect bookkeeping in this one-shot path.
    if (vga_get_vsync_enabled()) vga_wait_vblank();
    vga_blit_backbuffer_region_to_fb(0, 0, screen_w, screen_h);
    // Fallback when backbuffer is unavailable: drawing helpers already wrote
    // directly to the framebuffer.

    g_in_render_once = 0;
}

int tile_pump_input_once(void) {
    if (!g_tm_initialized) return 0;
    if (tile_count <= 0) return 0;

    int key = tui_read_key();
    if (!key) return 0;

    // While a ring3 task is running, Ctrl+C is reserved as the user-task abort.
    // Important: do NOT have a separate poller consume scancodes concurrently.
    if (g_user_task_active && key == 0x2206) {
        g_user_interrupt = 1;
        return 1;
    }

    // When a ring3 user task is active and waiting for stdin input,
    // route printable characters, backspace, and Enter to the stdin buffer
    // instead of the normal vterm command handling.
    if (g_user_task_active && g_user_task_term >= 0) {
        int term = g_user_task_term;
        // Skip Super-modified keys (they go to tiler hotkeys below)
        if (!(key & 0x4000)) {
            char ch = 0;
            // Printable ASCII
            if (key >= 32 && key <= 126) {
                ch = (char)key;
            }
            // Enter/newline (scancode 28 returns '\n' = 10)
            else if (key == 10) {
                ch = '\n';
            }
            // Backspace (scancode 14 returns '\b' = 8)
            else if (key == 8) {
                ch = '\b';
            }
            
            if (ch) {
                // Echo the character to the vterm display
                if (ch == '\b') {
                    // For backspace, we could implement echo but for now just update stdin buffer
                } else if (ch == '\n') {
                    vterm_write_char(term, '\n');
                } else {
                    vterm_write_char(term, ch);
                }
                // Add to stdin buffer
                vterm_stdin_putchar(term, ch);
                g_user_task_ui_dirty = 1;
                return 1;
            }
        }
    }

    // If modal is active, handle it first.
    if (g_bg_modal.active) {
        (void)handle_bg_modal_key(key);
        return 1;
    }

    // Super-modified keys (tui_read_key encodes Super by OR'ing 0x4000).
    // Note: when a ring3 task is running, we rely on this pump path (invoked
    // from the PIT IRQ) to keep the UI responsive; it must therefore implement
    // the same global hotkeys as the full tiler loop.
    if ((key & 0x4000)) {
        int base = key & (~0x4000);

        // Super+n -> new shell
        if ((base & 0xFF) == 'n') {
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
            return 1;
        }

        // Super+F toggle fullscreen for focused tile
        if ((base & 0xFF) == 'f') {
            if (fullscreen_tile == -1) {
                fullscreen_tile = focused;
            } else {
                fullscreen_tile = -1;
                layout_tiles();
            }
            g_force_full_redraw = 1;
            if (gui_draw_cb[tiles[focused].term_idx]) gui_needs_redraw[tiles[focused].term_idx] = 1;
            return 1;
        }

        // Super+Q to close focused tile or focused window
        if ((base & 0xFF) == 'q') {
            if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                wm_close_window(g_win_focused);
                g_win_focused = -1;
                g_force_full_redraw = 1;
                return 1;
            }

            // If user task is tied to this tile, prefer abort-to-shell over closing.
            if (g_user_task_active && g_user_task_term == tiles[focused].term_idx) {
                g_user_interrupt = 1;
                return 1;
            }

            if (tile_count > 1) {
                tile_close(focused);
                g_force_full_redraw = 1;
                layout_tiles();
            } else {
                static char last_tile_msg[64];
                snprintf(last_tile_msg, sizeof(last_tile_msg), "Last tile cannot be closed");
                tiles[focused].status_left = last_tile_msg;
                tiles[focused].status_visible = 1;
                g_force_full_redraw = 1;
            }
            return 1;
        }

        // Super + arrows to move focus (arrow codes are 0x1001..0x1004)
        if (base >= 0x1001 && base <= 0x1004) {
            if (base == 0x1001) { // up
                if (tile_count == 4) {
                    if (focused == 2) focused = 0;
                    else if (focused == 3) focused = 1;
                } else if (tile_count == 3) {
                    // layout: 0 = left tall, 1 = top-right, 2 = bottom-right
                    if (focused == 2) focused = 1;
                    else if (focused == 1) focused = 0;
                } else if (tile_count == 2) focused = 0;
            } else if (base == 0x1002) { // down
                if (tile_count == 4) {
                    if (focused == 0) focused = 2;
                    else if (focused == 1) focused = 3;
                } else if (tile_count == 3) {
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

            g_force_full_redraw = 1;
            return 1;
        }

        // Other Super combos are ignored in this pump path.
        return 1;
    }

    // Route input to focused window if any; else focused tile.
    if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
        window_t* wfocus = &g_windows[g_win_focused];
        if (wfocus->key_cb) {
            wfocus->key_cb(-1, key & 0xFFFF, wfocus->userdata);
            wfocus->needs_redraw = 1;
        }
        return 1;
    }

    int term = tiles[focused].term_idx;
    if (term < 0 || term >= MAX_TILES) term = 0;

    if (gui_key_cb[term]) {
        gui_key_cb[term](focused, key & 0xFFFF, gui_userdata[term]);
        gui_needs_redraw[term] = 1;
        return 1;
    }

    vterm_handle_key(term, key & 0xFFFF);
    return 1;
}

void start_tiling_manager() {
    if (!g_tm_initialized) {
        // initialize screen dimensions from global framebuffer if available
        if (g_mbi) {
            screen_w = g_mbi->framebuffer_width;
            screen_h = g_mbi->framebuffer_height;
        }
        // Decide low-memory/slow-CPU mode early so we can avoid loading heavy assets
        if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEMORY)) {
            uint32 total_kb = g_mbi->mem_lower + g_mbi->mem_upper; // in KB
            if (total_kb < 8192) g_gui_low_mode = 1; // <8MB
        }
        // Configure pacing and drag throttle
        if (g_gui_low_mode) {
            g_frame_target_us = 50000; // ~20 FPS cap
            // Keep vsync enabled to reduce tearing even on slow CPUs; can be overridden via gui command
            vga_set_vsync_enabled(1);
            vga_set_dirty_strategy(1); // single-rect blit to minimize tearing
        } else {
            g_frame_target_us = 0;     // unlimited
            vga_set_vsync_enabled(1);
            vga_set_dirty_strategy(0); // smart multi-rect blit for throughput
        }
        uint32 hz_tmp0 = sched_get_tick_hz(); if (!hz_tmp0) hz_tmp0 = 50;
        g_drag_throttle_ticks = g_gui_low_mode ? (hz_tmp0 / 40 + 1) : (hz_tmp0 / 100 + 1);
        g_last_frame_tick = sched_get_tick_count();
        // initialize virtual terminals
        vterm_init_all();
        // prime GUI heartbeat
        g_last_gui_heartbeat_tick = sched_get_tick_count();

        // initialize simple double-buffer (best-effort)
        vga_init_double_buffer();

        // Initialize mouse and bounds to pixel space
        mouse_init();
        mouse_set_bounds(0, 0, screen_w - 1, screen_h - 1);
        // Try to load UI assets unless in low mode (saves memory and draw time)
        // Always try to load the cursor image (small asset); other window icons are skipped in low mode.
        load_cursor_image_try_paths(0);
        if (!g_gui_low_mode) {
            // window button icons (focused/unfocused)
            load_close_icon_try_paths(0);
            load_close_icon_unf_try_paths(0);
            load_min_icon_try_paths(0);
            load_max_icon_try_paths(0);
            load_min_icon_unf_try_paths(0);
            load_max_icon_unf_try_paths(0);
        }
        // Allocate save-under buffer once we know cursor size
        int cw0 = g_cursor_loaded ? g_cursor_img.header.width : cursor_w;
        int ch0 = g_cursor_loaded ? g_cursor_img.header.height : cursor_h;
        int bpp0 = vga_get_fb_bpp_bytes(); if (bpp0 < 3) bpp0 = 3;
        cursor_save_w = cw0; cursor_save_h = ch0;
        cursor_save_len = cw0 * ch0 * bpp0;
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

        g_tm_initialized = 1;
    } else {
        // Re-entry path (e.g. aborting a ring3 task): keep all UI state, just ensure
        // a prompt is visible and force redraw so VGA updates immediately.
        if (tile_count <= 0) tile_count = 1;
        if (focused < 0 || focused >= tile_count) focused = 0;
        int ti = focused;
        int term = tiles[ti].term_idx;
        if (term < 0 || term >= MAX_TILES) term = 0;
        vterm_write_char(term, '\n');
        vterm_print_prompt(term);
        tiles[ti].static_drawn = 0;
        tiles[ti].last_drawn_version = 0;
        g_force_full_redraw = 1;
    }

    int running = 1;
    while (running) {
        // UI loop heartbeat for the watchdog
        watchdog_kick("wm-loop");
        // 1 Hz GUI heartbeat: invalidate GUI tiles once per second so they can update time-based views
        {
            uint32 now_ticks_hb = sched_get_tick_count();
            uint32 hz_hb = sched_get_tick_hz(); if (!hz_hb) hz_hb = 50;
            if (now_ticks_hb - g_last_gui_heartbeat_tick >= hz_hb) {
                for (int ti = 0; ti < MAX_TILES; ++ti) {
                    if (gui_draw_cb[ti]) {
                        gui_needs_redraw[ti] = 1;
                    }
                }
                g_last_gui_heartbeat_tick = now_ticks_hb;
            }
        }
        // Poll mouse in case IRQ12 is not firing (QEMU config)
    mouse_poll();
    watchdog_kick("wm-mouse");
        // Read current mouse position once per loop
        int cur_mx = -1000, cur_my = -1000;
        if (mouse_get_position(&cur_mx, &cur_my) != 0) {
            // Mouse may not be initialized yet under some emulators; show a default cursor so UI looks alive
            if (cursor_prev_x <= -900 || cursor_prev_y <= -900) {
                cur_mx = screen_w / 2; cur_my = screen_h / 2;
            } else {
                cur_mx = cursor_prev_x; cur_my = cursor_prev_y;
            }
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
            // Redraw decorations: in low mode, only when changed; otherwise each frame for freshness
            int need_redraw_decor = 1;
            if (g_gui_low_mode) {
                need_redraw_decor = 0;
                int th_now_dec = get_title_height(&tiles[i]);
                int show_status_now_dec = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
                if (!tiles[i].static_drawn || tiles[i].last_title_ptr != tiles[i].title ||
                    tiles[i].last_status_left_ptr != tiles[i].status_left || tiles[i].last_status_right_ptr != tiles[i].status_right ||
                    tiles[i].last_show_status != show_status_now_dec || tiles[i].last_focused != (i == focused)) {
                    need_redraw_decor = 1;
                }
            }
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
        // If a modal is active, draw it now on top of everything
        if (g_bg_modal.active) { draw_bg_modal(); }
    if (g_force_full_redraw) g_force_full_redraw = 0;
    if (g_tiles_full_content_redraw) g_tiles_full_content_redraw = 0;
        tui_refresh();
        // Build swap exclusion to preserve overlays (cursor and live-drag)
        int ex_x = -1, ex_y = -1, ex_w = 0, ex_h = 0;
        // Always exclude the previous cursor rectangle from the swap; we'll refresh it
        // from the backbuffer after the swap to avoid flicker on borders.
        if (prev_saved_w > 0 && prev_saved_h > 0) {
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
        // Ensure excluded cursor rectangle now reflects updated backbuffer content
        if (prev_saved_w > 0 && prev_saved_h > 0) {
            vga_blit_backbuffer_region_to_fb(prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h);
        }
        // Draw overlays near vblank; if swap already waited for vblank, skip extra wait
        if (!vga_get_vsync_enabled()) vga_wait_vblank();

        // Live-drag overlay
    if (!g_gui_low_mode) {
            // High/normal mode: blit full window area as overlay for smoothness
            if (prev_drag_w > 0 && prev_drag_h > 0) {
                vga_blit_backbuffer_region_to_fb(prev_drag_x, prev_drag_y, prev_drag_w, prev_drag_h);
                prev_drag_w = prev_drag_h = 0;
            }
            if (drag_active && drag_win >= 0 && g_windows[drag_win].used) {
                window_t* dw = &g_windows[drag_win];
                vga_blit_backbuffer_region_to_fb(dw->x, dw->y, dw->w, dw->h);
                prev_drag_x = dw->x; prev_drag_y = dw->y; prev_drag_w = dw->w; prev_drag_h = dw->h;
            }
        } else {
            // Low mode: wireframe dragging to reduce copies
            // Erase previous wireframe by restoring only border segments from backbuffer
            if (prev_outline_w > 0 && prev_outline_h > 0) {
                // top
                vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y, prev_outline_w, 1);
                // bottom
                vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y + prev_outline_h - 1, prev_outline_w, 1);
                // left
                vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y, 1, prev_outline_h);
                // right
                vga_blit_backbuffer_region_to_fb(prev_outline_x + prev_outline_w - 1, prev_outline_y, 1, prev_outline_h);
                prev_outline_w = prev_outline_h = 0;
            }
            if (drag_active && drag_win >= 0 && g_windows[drag_win].used) {
                window_t* dw = &g_windows[drag_win];
                int bx = dw->x, by = dw->y, bw = dw->w, bh = dw->h;
                // draw 2px border wireframe
                int rr = 255, gg = 255, bb = 0;
                vga_fillRect_fb(bx, by, bw, 2, rr, gg, bb);                   // top
                vga_fillRect_fb(bx, by + bh - 2, bw, 2, rr, gg, bb);          // bottom
                vga_fillRect_fb(bx, by, 2, bh, rr, gg, bb);                   // left
                vga_fillRect_fb(bx + bw - 2, by, 2, bh, rr, gg, bb);          // right
                prev_outline_x = bx; prev_outline_y = by; prev_outline_w = bw; prev_outline_h = bh;
            }
        }

        // No need to restore the previous cursor from a saved-under buffer; we refreshed
        // the excluded area directly from the backbuffer post-swap above.

    // Draw mouse cursor overlay on top of the freshly swapped framebuffer
        if (cur_mx > -100 && cur_my > -100) {
            // Ensure save-under buffer matches current cursor size (if image changed)
            int nw = g_cursor_loaded ? g_cursor_img.header.width : cursor_w;
            int nh = g_cursor_loaded ? g_cursor_img.header.height : cursor_h;
            int bpp1 = vga_get_fb_bpp_bytes(); if (bpp1 < 3) bpp1 = 3;
            if (!cursor_savebuf || nw != cursor_save_w || nh != cursor_save_h) {
                int newlen = nw * nh * bpp1;
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
            // If the background mode modal is active, handle mouse clicks there first
            if (g_bg_modal.active) {
                if (handle_bg_modal_mouse(&me)) {
                    // consumed; modal handled selection. Skip further mouse routing this frame.
                    goto after_mouse_handling;
                }
            }
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
            if (release_edge) { 
                drag_active = 0; drag_win = -1; 
                // On drag end in low mode, force a full reconcile since we only drew wireframes
                if (g_gui_low_mode) { 
                    if (prev_outline_w > 0 && prev_outline_h > 0) {
                        // erase leftover outline
                        vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y, prev_outline_w, 1);
                        vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y + prev_outline_h - 1, prev_outline_w, 1);
                        vga_blit_backbuffer_region_to_fb(prev_outline_x, prev_outline_y, 1, prev_outline_h);
                        vga_blit_backbuffer_region_to_fb(prev_outline_x + prev_outline_w - 1, prev_outline_y, 1, prev_outline_h);
                        prev_outline_w = prev_outline_h = 0;
                    }
                    g_force_full_redraw = 1; g_tiles_full_content_redraw = 1; 
                }
            }
            if (drag_active && drag_win >= 0) {
                window_t* w = &g_windows[drag_win];
                // Drag throttling in low mode
                if (g_gui_low_mode) {
                    uint32 nowt = sched_get_tick_count();
                    if (nowt - g_last_drag_tick < g_drag_throttle_ticks) {
                        // skip updating geom this loop
                    } else {
                        g_last_drag_tick = nowt;
                        w->x = clampi(me.x - drag_off_x, 0, screen_w - w->w);
                        w->y = clampi(me.y - drag_off_y, 0, screen_h - w->h);
                    }
                    // Do not mark tiles/windows dirty per-move; wireframe overlay handles visuals
                    w->static_drawn = 0; // so final commit repaints decor at new spot
                } else {
                    // Normal mode: full overlay + underlying refresh
                    vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                    int old_x = w->x, old_y = w->y, old_w = w->w, old_h = w->h;
                    w->x = clampi(me.x - drag_off_x, 0, screen_w - w->w);
                    w->y = clampi(me.y - drag_off_y, 0, screen_h - w->h);
                    vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                    w->static_drawn = 0;
                    w->needs_redraw = 1;
                    g_tiles_full_content_redraw = 1;
                    // Invalidate other windows that intersect moved window's old or new rect
                    for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                        if (wi == drag_win) continue;
                        window_t* wo = &g_windows[wi];
                        if (!wo->used || wo->minimized) continue;
                        if (rects_intersect(wo->x, wo->y, wo->w, wo->h, old_x, old_y, old_w, old_h) ||
                            rects_intersect(wo->x, wo->y, wo->w, wo->h, w->x, w->y, w->w, w->h)) {
                            wo->needs_redraw = 1; wo->static_drawn = 0;
                        }
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
                int show_status2 = (tt->status_left != NULL) || (tt->status_right != NULL) || (tt->status_visible) || (tui_alt_pressed);
                if (show_status2) {
                    int sy = tt->y + title_h2;
                    if (me.y >= sy && me.y < sy + 12) {
                        vga_mark_dirty_rect(tt->x, sy, tt->width, 12);
                    }
                }
            }
        }

after_mouse_handling:

    int key = tui_read_key();
    if (key) { watchdog_kick("wm-key"); }
        // If modal is active, handle it first and skip routing
        if (g_bg_modal.active) {
            if (handle_bg_modal_key(key)) { continue; }
        }
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
                    // Allow closing focused floating window regardless of tile count
                    wm_close_window(g_win_focused);
                    g_win_focused = -1;
                    g_force_full_redraw = 1;
                } else {
                    // If more than one tile, close the focused tile.
                    // If only one tile remains, DO NOT close/exit — keep at least one tile to avoid OS instability.
                    if (tile_count > 1) {
                        tile_close(focused);
                        g_force_full_redraw = 1;
                        layout_tiles();
                    } else {
                        // Refuse to close the final tile; surface a brief status hint.
                        static char last_tile_msg[64];
                        snprintf(last_tile_msg, sizeof(last_tile_msg), "Last tile cannot be closed");
                        tiles[focused].status_left = last_tile_msg;
                        tiles[focused].status_visible = 1;
                        g_force_full_redraw = 1;
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

        /*
         * Idle optimization: when nothing changed this frame (no tile content redraws,
         * no pending GUI redraws, no windows pending, and not dragging), yield the CPU
         * briefly to reduce busy-loop CPU usage. We use sched_sleep_us which halts
         * the processor until the next timer/interrupt which keeps latency low.
         */
        int idle_ok = 1;
        if (g_any_tile_content_redrew) idle_ok = 0;
        if (g_force_full_redraw) idle_ok = 0;
        if (g_tiles_full_content_redraw) idle_ok = 0;
        if (drag_active) idle_ok = 0;
        // Any GUI clients asking for redraw? (per-term)
        for (int _ti = 0; _ti < MAX_TILES; ++_ti) {
            if (gui_needs_redraw[_ti]) { idle_ok = 0; break; }
        }
        // Any windows needing redraw?
        if (idle_ok && g_window_count > 0) {
            for (int _wi = 0; _wi < MAX_WINDOWS; ++_wi) {
                if (g_windows[_wi].used && g_windows[_wi].needs_redraw) { idle_ok = 0; break; }
            }
        }
        if (idle_ok) {
            /* Sleep ~2ms to yield CPU but remain responsive. This will execute
             * `hlt` inside sched_sleep_us and wake on interrupts (mouse/keyboard/timer).
             */
            sched_sleep_us(2000);
        }

        // Simple frame limiter to avoid overdraw on slow CPUs
        // If mouse is moving and we're in low mode, temporarily boost FPS cap for a snappier cursor.
        int boost_low = 0; if (g_gui_low_mode && (g_mouse_state.delta_x != 0 || g_mouse_state.delta_y != 0)) boost_low = 1;
        uint32 frame_target_us_effective = g_frame_target_us;
        if (boost_low && g_frame_target_us && g_frame_target_us > 0) {
            // Raise to ~30 FPS during motion if current cap is lower (e.g., 20 FPS)
            uint32 cap30 = 33333u; if (frame_target_us_effective > cap30) frame_target_us_effective = cap30;
        }
        if (frame_target_us_effective) {
            uint32 now_ticks = sched_get_tick_count();
            uint32 hz = sched_get_tick_hz(); if (!hz) hz = 50;
            // Convert target_us to ticks and enforce at least 1 tick
            uint32 target_ticks = (frame_target_us_effective * hz) / 1000000; if (target_ticks < 1) target_ticks = 1;
            uint32 elapsed = now_ticks - g_last_frame_tick;
            if (elapsed < target_ticks) {
                uint32 remaining_ticks = target_ticks - elapsed;
                // approx sleep
                uint32 us_per_tick = 1000000 / hz;
                sched_sleep_us(remaining_ticks * us_per_tick);
            }
            g_last_frame_tick = sched_get_tick_count();
        }
    }
}

// ---------------- Runtime GUI tuning (low-spec controls) ----------------
void tiler_gui_set_mode(int mode) {
    // mode: 0=high, 1=low, 2=auto
    int new_low = g_gui_low_mode;
    if (mode == 2) {
        if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEMORY)) {
            uint32 total_kb = g_mbi->mem_lower + g_mbi->mem_upper; // in KB
            new_low = (total_kb < 8192) ? 1 : 0;
        }
    } else if (mode == 0) {
        new_low = 0;
    } else if (mode == 1) {
        new_low = 1;
    }

    if (new_low != g_gui_low_mode) {
        g_gui_low_mode = new_low;
        // Default pacing for new mode
        if (g_gui_low_mode) {
            g_frame_target_us = 50000; // ~20 FPS
            vga_set_vsync_enabled(1);
            vga_set_dirty_strategy(1);
        } else {
            g_frame_target_us = 0; // unlimited
            vga_set_vsync_enabled(1);
            vga_set_dirty_strategy(0);
        }
        // Update drag throttle from current HZ
        uint32 hz0 = sched_get_tick_hz(); if (!hz0) hz0 = 50;
        g_drag_throttle_ticks = g_gui_low_mode ? (hz0 / 40 + 1) : (hz0 / 100 + 1);

        // Force visual refresh of everything
        g_force_full_redraw = 1;
        g_tiles_full_content_redraw = 1;
        for (int i = 0; i < MAX_TILES; ++i) {
            tiles[i].static_drawn = 0;
            if (gui_draw_cb[tiles[i].term_idx]) gui_needs_redraw[tiles[i].term_idx] = 1;
        }
        for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
            if (g_windows[wi].used) { g_windows[wi].static_drawn = 0; g_windows[wi].needs_redraw = 1; }
        }
    }
}

void tiler_gui_set_fps_cap(int fps) {
    if (fps <= 0) {
        g_frame_target_us = 0;
    } else {
        if (fps > 240) fps = 240; // clamp
        g_frame_target_us = 1000000u / (uint32)fps;
    }
}

void tiler_gui_set_drag_throttle_ms(int ms) {
    uint32 hz = sched_get_tick_hz(); if (!hz) hz = 50;
    if (ms <= 0) {
        g_drag_throttle_ticks = (hz / 100) + 1; // minimal throttle
    } else {
        uint32 ticks = ((uint32)ms * hz) / 1000u;
        if (ticks < 1) ticks = 1;
        g_drag_throttle_ticks = ticks;
    }
}

void tiler_gui_print_status(void) {
    int auto_low = 0;
    if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEMORY)) {
        uint32 total_kb = g_mbi->mem_lower + g_mbi->mem_upper;
        auto_low = (total_kb < 8192) ? 1 : 0;
    }
    int vs = vga_get_vsync_enabled();
    int strat = vga_get_dirty_strategy();
    // Compute FPS cap from microseconds
    int fps_cap = 0; if (g_frame_target_us) fps_cap = (int)(1000000u / g_frame_target_us);
    // Convert drag throttle to ms
    uint32 hz = sched_get_tick_hz(); if (!hz) hz = 50;
    int drag_ms = (int)((g_drag_throttle_ticks * 1000u) / hz);
    printf("%cGUI status:\n", 200, 200, 255);
    printf("%c  mode: %s (auto suggestion: %s)\n", 255, 255, 255,
           g_gui_low_mode ? "low" : "high",
           auto_low ? "low" : "high");
    printf("%c  vsync: %s\n", 255, 255, 255, vs ? "on" : "off");
    printf("%c  swap: %s\n", 255, 255, 255, strat ? "single" : "smart");
    if (fps_cap > 0)
        printf("%c  fps cap: %d\n", 255, 255, 255, fps_cap);
    else
        printf("%c  fps cap: off\n", 255, 255, 255);
    printf("%c  drag throttle: %d ms (%u ticks)\n", 255, 255, 255, drag_ms, (unsigned)g_drag_throttle_ticks);
}
