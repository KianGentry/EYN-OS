// Allow safely re-entering the tiling-manager loop after aborting a user task.
static int g_tm_initialized = 0;
#include <misc/types.h>
#include <vga.h>
#include <tui.h>
#include <terminals.h>
#include <string.h>
#include <tile_manager.h>
#include <mouse.h>
#include <rei.h>
#include <eynfs.h>
#include <utilities/util.h>
#include <misc/types.h>
// allow sleeping the CPU when tiling manager is idle
#include <sched.h>
#include <hal/time.h>
#include <watchdog.h>
#include <ui_prefs.h>
#if defined(__i386__)
#include <network/netstack.h>
#include <drivers/e1000.h>
#endif

extern uint8_t g_current_drive;

#define MAX_TILES 4
#define MAX_WINDOWS 8
#define ALIGN16 __attribute__((aligned(16)))

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
    int status_visible; // legacy: kept for compatibility; status overlay is Alt-held now
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

// --- Status bar overlay (Alt-held) ---
static inline int status_bar_height_px(void) {
    int fh = vga_text_cell_h();
    int h = fh + 4;
    if (h < 12) h = 12;
    return h;
}

static inline int status_overlay_visible(void) {
    return (tui_alt_pressed != 0) || (ui_prefs_get_status_bar_mode() != 0);
}

// Fixed-rate marquee: ~3 characters per second regardless of frame rate.
// Uses a bounce-with-pause pattern so the text reverses at ends.
static inline uint32 status_scroll_steps(void) {
    uint32 hz = hal_time_tick_hz();
    if (!hz) hz = 50;
    uint32 ticks = (uint32)hal_time_ticks();
    // 3 chars/sec => steps = floor(ticks * 3 / hz)
    uint32 q = ticks / hz;
    uint32 r = ticks % hz;
    return (q * 3u) + ((r * 3u) / hz);
}

static inline int status_bounce_offset(uint32 steps, int max_offset, int pause_steps) {
    if (max_offset <= 0) return 0;
    if (pause_steps < 0) pause_steps = 0;
    int cycle = (max_offset * 2) + (pause_steps * 2);
    if (cycle <= 0) return 0;
    int p = (int)(steps % (uint32)cycle);
    if (p < max_offset) return p;
    p -= max_offset;
    if (p < pause_steps) return max_offset;
    p -= pause_steps;
    if (p < max_offset) return (max_offset - p);
    return 0;
}

static void draw_status_overlay_text(int x, int y, int w, int h,
                                     const char* left_text,
                                     const char* right_base,
                                     const char* right_extra,
                                     int base_is_red) {
    wm_theme_t theme;
    wm_theme_get(&theme);

    int cw = vga_text_cell_w();
    int fh = vga_text_cell_h();
    if (w <= 8 || cw <= 0) return;

    drawRect(x, y, w, h, theme.status_r, theme.status_g, theme.status_b);

    int text_y = y + (h - fh) / 2;
    int clip_min = x + 4;
    int clip_max = x + w - 4;
    if (clip_max <= clip_min) return;

    int total_chars = (clip_max - clip_min) / cw;
    if (total_chars <= 0) return;

    // Right side: draw base controls (white), then optional extra (red) immediately to the left.
    const char* base = right_base ? right_base : "";
    const char* extra = right_extra ? right_extra : "";
    int base_len = (int)strlen(base);
    int extra_len = (int)strlen(extra);

    // Truncate to avoid negative placement.
    if (base_len > total_chars) base_len = total_chars;
    // Render base as the tail if it doesn't fit (keeps ": Exit" visible).
    const char* base_ptr = base;
    int full_base_len = (int)strlen(base);
    if (full_base_len > base_len) base_ptr = base + (full_base_len - base_len);

    int base_start_x = clip_max - base_len * cw;
    int base_r = base_is_red ? 255 : theme.status_text_r;
    int base_g = base_is_red ? 0 : theme.status_text_g;
    int base_b = base_is_red ? 0 : theme.status_text_b;

    for (int i = 0; i < base_len; ++i) {
        int cx = base_start_x + i * cw;
        if (cx < clip_min || cx + cw > clip_max) continue;
        drawCharAt(cx, text_y, (unsigned char)base_ptr[i], base_r, base_g, base_b);
    }

    int gap_chars = (base_len > 0 && extra_len > 0) ? 1 : 0;
    int extra_room = total_chars - base_len - gap_chars;
    if (extra_room < 0) extra_room = 0;

    int extra_draw_len = extra_len;
    if (extra_draw_len > extra_room) extra_draw_len = extra_room;
    const char* extra_ptr = extra;
    if (extra_len > extra_draw_len) {
        // Keep tail visible for short markers like "[Modified]".
        extra_ptr = extra + (extra_len - extra_draw_len);
    }
    int extra_start_x = base_start_x - gap_chars * cw - extra_draw_len * cw;
    if (extra_draw_len > 0) {
        int rr = base_is_red ? 255 : 255;
        int rg = base_is_red ? 0 : 255;
        int rb = base_is_red ? 0 : 255;
        (void)rr; (void)rg; (void)rb;
        // Extra is rendered in red to preserve existing warning/marker behavior.
        for (int i = 0; i < extra_draw_len; ++i) {
            int cx = extra_start_x + i * cw;
            if (cx < clip_min || cx + cw > clip_max) continue;
            drawCharAt(cx, text_y, (unsigned char)extra_ptr[i], 255, 0, 0);
        }
        // Optional gap
        if (gap_chars) {
            int gx = base_start_x - cw;
            if (gx >= clip_min && gx + cw <= clip_max) drawCharAt(gx, text_y, (unsigned char)' ', theme.status_text_r, theme.status_text_g, theme.status_text_b);
        }
    }

    // Left side: scroll if needed in remaining space.
    int left_clip_max = extra_start_x - cw; // leave at least 1 char gap before right area
    if (extra_draw_len == 0 && base_len > 0) left_clip_max = base_start_x - cw;
    if (base_len == 0 && extra_draw_len == 0) left_clip_max = clip_max;
    if (left_clip_max > clip_max) left_clip_max = clip_max;
    if (left_clip_max <= clip_min) return;

    int left_chars = (left_clip_max - clip_min) / cw;
    if (left_chars <= 0) return;

    const char* left = left_text ? left_text : "";
    int left_len = (int)strlen(left);
    if (left_len <= 0) return;

    int start_idx = 0;
    if (left_len > left_chars) {
        int max_off = left_len - left_chars;
        // Pause ~1s at each end (3 steps at 3 chars/sec).
        start_idx = status_bounce_offset(status_scroll_steps(), max_off, 3);
    }

    for (int i = 0; i < left_chars; ++i) {
        int idx = start_idx + i;
        if (idx >= left_len) break;
        int cx = clip_min + i * cw;
        if (cx < clip_min || cx + cw > left_clip_max) continue;
        drawCharAt(cx, text_y, (unsigned char)left[idx], theme.status_text_r, theme.status_text_g, theme.status_text_b);
    }
}

// GUI client storage
static tile_gui_draw_cb gui_draw_cb[MAX_TILES];
static tile_gui_key_cb gui_key_cb[MAX_TILES];
static tile_gui_mouse_cb gui_mouse_cb[MAX_TILES];
static tile_gui_close_cb gui_close_cb[MAX_TILES];
static void* gui_userdata[MAX_TILES];
// Redraw gating for GUI clients: only redraw when invalidated or when rect/version/force changes
static int gui_needs_redraw[MAX_TILES];
static int gui_continuous_redraw[MAX_TILES];

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

// Optional resize cursors (draw-time transforms used instead of multiple images)
static rei_image_t g_cursor_res_img;
static int g_cursor_res_loaded = 0;
static rei_image_t g_cursor_res_diag_img;
static int g_cursor_res_diag_loaded = 0;

typedef enum {
    CURSOR_NORMAL = 0,
    CURSOR_RES_VERT,
    CURSOR_RES_HOR,
    CURSOR_RES_DIAG,
} cursor_kind_t;

static cursor_kind_t g_cursor_kind = CURSOR_NORMAL;
// Cursor draw transform flags applied in draw_cursor_overlay()
#define CURSOR_XFORM_FLIP_X 1
#define CURSOR_XFORM_FLIP_Y 2
#define CURSOR_XFORM_ROT_90 4
static int g_cursor_xform = 0;
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

// --- Floating window resize state ---
static int resize_active = 0;
static int resize_win = -1;
static int resize_edges = 0;
static int resize_start_mx = 0, resize_start_my = 0;
static int resize_start_x = 0, resize_start_y = 0, resize_start_w = 0, resize_start_h = 0;

// --- Tiling resize state (dragging split borders) ---
static int g_tile_split_inited_for_count = -1;
static int g_tile_split_x = 0;
static int g_tile_split_y = 0;
static int tile_resize_active = 0;
static int tile_resize_mode = 0; // 1=vert,2=hor,3=both

#define RESIZE_EDGE_L 1
#define RESIZE_EDGE_R 2
#define RESIZE_EDGE_T 4
#define RESIZE_EDGE_B 8

static inline void cursor_set_kind(cursor_kind_t k) {
    g_cursor_kind = k;
    g_cursor_xform = 0;
}

static inline void cursor_set_style(cursor_kind_t k, int xform) {
    g_cursor_kind = k;
    g_cursor_xform = xform;
}

static inline int cursor_img_loaded_for_kind(cursor_kind_t k) {
    if (k == CURSOR_RES_VERT) return g_cursor_res_loaded;
    if (k == CURSOR_RES_HOR) return g_cursor_res_loaded;
    if (k == CURSOR_RES_DIAG) return g_cursor_res_diag_loaded;
    return g_cursor_loaded;
}

static inline const rei_image_t* cursor_img_for_kind(cursor_kind_t k) {
    if ((k == CURSOR_RES_VERT || k == CURSOR_RES_HOR) && g_cursor_res_loaded) return &g_cursor_res_img;
    if (k == CURSOR_RES_DIAG && g_cursor_res_diag_loaded) return &g_cursor_res_diag_img;
    return &g_cursor_img;
}

static inline void cursor_get_dims(int* out_w, int* out_h) {
    int w = cursor_w;
    int h = cursor_h;
    const rei_image_t* im = cursor_img_for_kind(g_cursor_kind);
    if (im && cursor_img_loaded_for_kind(g_cursor_kind)) {
        w = im->header.width;
        h = im->header.height;
        if (g_cursor_xform & CURSOR_XFORM_ROT_90) {
            int tmp = w; w = h; h = tmp;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

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

// --- Runtime theme (tile/window chrome colors) ---
static wm_theme_t g_wm_theme = {
    // Default: darker gray chrome
    .title_focused_r = 96, .title_focused_g = 96, .title_focused_b = 96,
    .title_unfocused_r = 64, .title_unfocused_g = 64, .title_unfocused_b = 64,
    .status_r = 32, .status_g = 32, .status_b = 32,
    .status_text_r = 255, .status_text_g = 255, .status_text_b = 255,
};

void wm_theme_get(wm_theme_t* out) {
    if (!out) return;
    *out = g_wm_theme;
}

void wm_theme_set(const wm_theme_t* in) {
    if (!in) return;
    g_wm_theme = *in;
    // Force redraw so chrome updates immediately.
    g_force_full_redraw = 1;
}

void wm_theme_reset_defaults(void) {
    g_wm_theme.title_focused_r = 96; g_wm_theme.title_focused_g = 96; g_wm_theme.title_focused_b = 96;
    g_wm_theme.title_unfocused_r = 64; g_wm_theme.title_unfocused_g = 64; g_wm_theme.title_unfocused_b = 64;
    g_wm_theme.status_r = 32; g_wm_theme.status_g = 32; g_wm_theme.status_b = 32;
    g_wm_theme.status_text_r = 255; g_wm_theme.status_text_g = 255; g_wm_theme.status_text_b = 255;
    g_force_full_redraw = 1;
}

// - Per-tile background images -
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

// - Floating Windows (experimental) -
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
    int continuous_redraw;
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

static int win_title_height(const window_t* w) {
    (void)w;
    int fh = vga_text_cell_h();
    int h = fh + 4;
    if (h < 16) h = 16;
    return h;
}
static int win_status_height(const window_t* w) {
    (void)w;
    // Status is an Alt-held overlay and does not reserve layout space.
    return 0;
}

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
static void load_close_icon_try_paths(uint8 disk);
static void load_min_icon_try_paths(uint8 disk);
static void load_max_icon_try_paths(uint8 disk);
static void load_close_icon_unf_try_paths(uint8 disk);
static void load_min_icon_unf_try_paths(uint8 disk);
static void load_max_icon_unf_try_paths(uint8 disk);

// - Icon cache for small file icons -
#define ICON_CACHE_MAX 64
typedef struct {
    char key[32];
    rei_image_t img ALIGN16;
    int loaded;
    int pad;
} icon_cache_entry_t;

static icon_cache_entry_t g_icon_cache[ICON_CACHE_MAX] ALIGN16;
static int g_icon_cache_count = 0;

static int g_last_icon_mode_16 = -1;

static void icon_cache_clear(void) {
    for (int i = 0; i < g_icon_cache_count; ++i) {
        if (g_icon_cache[i].loaded) {
            rei_free_image(&g_icon_cache[i].img);
        }
    }
    memset(g_icon_cache, 0, sizeof(g_icon_cache));
    g_icon_cache_count = 0;
}

static void free_window_icons(void) {
    if (g_close_icon_loaded) { rei_free_image(&g_close_icon); g_close_icon_loaded = 0; }
    if (g_min_icon_loaded) { rei_free_image(&g_min_icon); g_min_icon_loaded = 0; }
    if (g_max_icon_loaded) { rei_free_image(&g_max_icon); g_max_icon_loaded = 0; }

    if (g_close_icon_unf_loaded) { rei_free_image(&g_close_icon_unf); g_close_icon_unf_loaded = 0; }
    if (g_min_icon_unf_loaded) { rei_free_image(&g_min_icon_unf); g_min_icon_unf_loaded = 0; }
    if (g_max_icon_unf_loaded) { rei_free_image(&g_max_icon_unf); g_max_icon_unf_loaded = 0; }
}

static void maybe_update_icon_mode(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    if (g_last_icon_mode_16 == want16) return;
    g_last_icon_mode_16 = want16;

    // Switching fonts can change desired icon set (8x8 vs 16x16). Flush caches and reload UI icons.
    watchdog_kick("wm-iconmode");
    icon_cache_clear();
    free_window_icons();
    watchdog_kick("wm-iconmode");
    load_close_icon_try_paths(disk);
    watchdog_kick("wm-iconmode");
    load_min_icon_try_paths(disk);
    watchdog_kick("wm-iconmode");
    load_max_icon_try_paths(disk);
    watchdog_kick("wm-iconmode");
    load_close_icon_unf_try_paths(disk);
    watchdog_kick("wm-iconmode");
    load_min_icon_unf_try_paths(disk);
    watchdog_kick("wm-iconmode");
    load_max_icon_unf_try_paths(disk);

    g_force_full_redraw = 1;
}

// Try a few candidate paths for an icon with given extension and load into cache entry
static rei_image_t* load_icon_for_ext_mode(const char* ext, int want16) {
    if (!ext) return NULL;

    want16 = want16 ? 1 : 0;
    char cache_key[24];
    snprintf(cache_key, sizeof(cache_key), "%s|%s", want16 ? "16" : "8", ext);
    cache_key[sizeof(cache_key) - 1] = '\0';

    // Check cache first
    for (int i = 0; i < g_icon_cache_count; ++i) {
        if (strcmp(g_icon_cache[i].key, cache_key) == 0) {
            return g_icon_cache[i].loaded ? &g_icon_cache[i].img : NULL;
        }
    }
    if (g_icon_cache_count >= ICON_CACHE_MAX) return NULL;

    // Candidate filename patterns.
    // ext is treated as the full icon base name (without .rei), e.g. "file_txt", "file_none", "dir_empty", "dir_full".
    static const char* const patterns16[] ALIGN16 = {
        "/icons16/%s.rei",
        "/testdir/icons16/%s.rei",
        // Legacy fallbacks
        "/icons16/file_%s.rei",
        "/testdir/icons16/file_%s.rei",
        NULL
    };
    static const char* const patterns8[] ALIGN16 = {
        "/icons/%s.rei",
        "/testdir/icons/%s.rei",
        // Legacy fallbacks
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
    const char** primary = want16 ? patterns16 : patterns8;
    const char** fallback = want16 ? patterns8 : NULL;

    for (int pass = 0; pass < 2 && !found; ++pass) {
        const char** patterns = (pass == 0) ? primary : fallback;
        if (!patterns) continue;
        for (int pi = 0; patterns[pi]; ++pi) {
            watchdog_kick("wm-iconload");
            snprintf(pathbuf, sizeof(pathbuf), patterns[pi], ext);
            eynfs_dir_entry_t entry;
            if (eynfs_traverse_path(0, &sb, pathbuf, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
                int size = entry.size;
                uint8_t* buf = (uint8_t*)malloc(size);
                if (!buf) { found = 0; break; }
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
    }
    // No special-case fallback needed here; callers can request "file_none" if desired.

    // Add to cache entry regardless (to avoid repeated failed lookups)
    int idx = g_icon_cache_count++;
    memset(&g_icon_cache[idx], 0, sizeof(g_icon_cache[idx]));
    strncpy(g_icon_cache[idx].key, cache_key, sizeof(g_icon_cache[idx].key)-1);
    if (found) {
        g_icon_cache[idx].img = tmp; // struct copy (contains allocated data)
        g_icon_cache[idx].loaded = 1;
        return &g_icon_cache[idx].img;
    }
    g_icon_cache[idx].loaded = 0;
    return NULL;
}

static rei_image_t* load_icon_for_ext(const char* ext) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    return load_icon_for_ext_mode(ext, want16);
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

static void wm_draw_decor(window_t* w, int is_focused) {
    int cw = vga_text_cell_w();
    int fh = vga_text_cell_h();
    // In low mode, draw simpler decorations: skip icons and minimize overdraw
    if (g_gui_low_mode) {
        int th = win_title_height(w);
        int sh = 0;
        // frame background
        drawRect(w->x, w->y, w->w, w->h, 0, 0, 0);
        // title bar
        int title_color_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
        int title_color_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
        int title_color_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
        drawRect(w->x, w->y, w->w, th, title_color_r, title_color_g, title_color_b);
        if (w->title && w->title[0]) {
            int len = strlen(w->title);
            int max_chars = (w->w / cw) - 2; if (max_chars < 0) max_chars = 0;
            int color_r = is_focused ? 255 : 0, color_g = is_focused ? 255 : 0, color_b = is_focused ? 255 : 0;
            int text_y = w->y + (th - fh) / 2;
            if (len > max_chars) len = max_chars;
            int start_x = w->x + 4;
            for (int i = 0; i < len; ++i) drawCharAt(start_x + i * cw, text_y, (unsigned char)w->title[i], color_r, color_g, color_b);
        }
        (void)sh;
        // border
        int br = is_focused ? 120 : 60, bg = is_focused ? 120 : 60, bb = is_focused ? 80 : 60;
        drawRect(w->x, w->y, w->w, 1, br, bg, bb);
        drawRect(w->x, w->y + w->h - 1, w->w, 1, br, bg, bb);
        drawRect(w->x, w->y, 1, w->h, br, bg, bb);
        drawRect(w->x + w->w - 1, w->y, 1, w->h, br, bg, bb);
        return;
    }
    int th = win_title_height(w);
    int sh = 0;
    // frame background
    drawRect(w->x, w->y, w->w, w->h, 0, 0, 0);
    // title bar
    int title_color_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
    int title_color_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
    int title_color_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
    drawRect(w->x, w->y, w->w, th, title_color_r, title_color_g, title_color_b);
    if (w->title && w->title[0]) {
        int len = strlen(w->title);
        // Try to avoid drawing under the close button by shrinking max chars by ~2
        int max_chars = (w->w / cw) - 2;
        if (max_chars < 0) max_chars = 0;
        int text_y = w->y + (th - fh) / 2;
        int color_r = is_focused ? 255 : 0;
        int color_g = is_focused ? 255 : 0;
        int color_b = is_focused ? 255 : 0;
        if (len <= max_chars) {
            int start_x = w->x + (w->w - len * cw) / 2;
            for (int i = 0; i < len; ++i) drawCharAt(start_x + i * cw, text_y, (unsigned char)w->title[i], color_r, color_g, color_b);
        } else {
            int window = max_chars;
            int speed = 6;
            int period = len + window;
            int pos = (g_tile_scroll_tick / speed) % period;
            for (int i = 0; i < window; ++i) {
                int idx = pos + i; char ch = ' ';
                if (idx < len) ch = w->title[idx]; else { int wrap = idx - len; if (wrap < len) ch = w->title[wrap]; }
                drawCharAt(w->x + i * cw, text_y, (unsigned char)ch, color_r, color_g, color_b);
            }
        }
    }
    // Titlebar buttons (minimize, maximize, close) on the right
    if (th >= 12) {
        // Close
        {
            int bx, by, bw, bh; wm_get_close_rect(w, &bx, &by, &bw, &bh);
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
    (void)sh;
    // border
    int br = is_focused ? 140 : 60, bg = is_focused ? 140 : 60, bb = is_focused ? 90 : 60;
    drawRect(w->x, w->y, w->w, 1, br, bg, bb);
    drawRect(w->x, w->y + w->h - 1, w->w, 1, br, bg, bb);
    drawRect(w->x, w->y, 1, w->h, br, bg, bb);
    drawRect(w->x + w->w - 1, w->y, 1, w->h, br, bg, bb);
}

static void wm_get_close_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    // Size buttons to the icon (when available) and center them vertically within the titlebar.
    int btn_side = 12;
    if (g_close_icon_loaded) {
        int iw = g_close_icon.header.width;
        int ih = g_close_icon.header.height;
        int m = (iw > ih) ? iw : ih;
        if (m > btn_side) btn_side = m;
    }
    if (btn_side > th) btn_side = th;
    int margin = 2;
    int pad_y = (th - btn_side) / 2;
    if (btn_side > w->w - 2 * margin) btn_side = (w->w > 2 * margin) ? (w->w - 2 * margin) : 0;
    int bx = w->x + w->w - btn_side - margin;
    int by = w->y + pad_y;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_side;
    if (rh) *rh = btn_side;
}

static void wm_get_max_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    int btn_side = 12;
    if (g_max_icon_loaded) {
        int iw = g_max_icon.header.width;
        int ih = g_max_icon.header.height;
        int m = (iw > ih) ? iw : ih;
        if (m > btn_side) btn_side = m;
    }
    if (btn_side > th) btn_side = th;
    int gap = 2;
    int pad_y = (th - btn_side) / 2;
    int bx_close, by_close, bw_close, bh_close; wm_get_close_rect(w, &bx_close, &by_close, &bw_close, &bh_close);
    int bx = bx_close - gap - btn_side;
    int by = w->y + pad_y;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_side;
    if (rh) *rh = btn_side;
}

static void wm_get_min_rect(const window_t* w, int* rx, int* ry, int* rw, int* rh) {
    int th = win_title_height(w);
    int btn_side = 12;
    if (g_min_icon_loaded) {
        int iw = g_min_icon.header.width;
        int ih = g_min_icon.header.height;
        int m = (iw > ih) ? iw : ih;
        if (m > btn_side) btn_side = m;
    }
    if (btn_side > th) btn_side = th;
    int gap = 2;
    int pad_y = (th - btn_side) / 2;
    int bx_max, by_max, bw_max, bh_max; wm_get_max_rect(w, &bx_max, &by_max, &bw_max, &bh_max);
    int bx = bx_max - gap - btn_side;
    int by = w->y + pad_y;
    if (rx) *rx = bx;
    if (ry) *ry = by;
    if (rw) *rw = btn_side;
    if (rh) *rh = btn_side;
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
    int sh = 0;
    if (rects_intersect(w->x, w->y, w->w, th, prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h)) g_dirty_hits_prev_cursor = 1;
    vga_mark_dirty_rect(w->x, w->y, w->w, th);
    (void)sh;
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
    int sh = status_overlay_visible() ? status_bar_height_px() : 0;
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

static void wm_draw_status_overlay(window_t* w) {
    if (!w || !w->used) return;
    if (!status_overlay_visible()) return;
    int th = win_title_height(w);
    int sh = status_bar_height_px();
    int sy = w->y + th;
    if (sy + sh > w->y + w->h) return;
    draw_status_overlay_text(w->x, sy, w->w, sh,
                             w->status_left ? w->status_left : "",
                             "Ctrl+Q: Exit",
                             (w->status_right && w->status_right[0]) ? w->status_right : NULL,
                             0);
    vga_mark_dirty_rect(w->x, sy, w->w, sh);
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

static int wm_resize_hit_test_edges(const window_t* w, int x, int y) {
    if (!w || !w->used || w->minimized) return 0;
    if (w->maximized) return 0;
    // Grab zone thickness in pixels
    const int m = 4;
    const int corner = 10;
    int edges = 0;
    // Outside window => not resizable
    if (!point_in_rect(x, y, w->x, w->y, w->w, w->h)) return 0;

    // Avoid treating clicks in the titlebar button cluster as resize.
    // (The buttons live in the titlebar near the top-right.)
    int th = win_title_height(w);
    int bx0 = 0, by0 = 0, bw0 = 0, bh0 = 0;
    wm_get_close_rect(w, &bx0, &by0, &bw0, &bh0);
    int avoid_x = bx0 - (bw0 * 3);
    if (avoid_x < w->x) avoid_x = w->x;
    if (point_in_rect(x, y, avoid_x, w->y, (w->x + w->w) - avoid_x, th)) {
        // still allow resize on the extreme right edge in the titlebar
        if (x < w->x + w->w - m) return 0;
    }

    // Prefer diagonal resize when near corners (bigger target than just intersecting both edge strips).
    // This makes diagonal resizing much easier to grab.
    if (x < w->x + corner && y < w->y + corner) return RESIZE_EDGE_L | RESIZE_EDGE_T;
    if (x >= w->x + w->w - corner && y < w->y + corner) return RESIZE_EDGE_R | RESIZE_EDGE_T;
    if (x < w->x + corner && y >= w->y + w->h - corner) return RESIZE_EDGE_L | RESIZE_EDGE_B;
    if (x >= w->x + w->w - corner && y >= w->y + w->h - corner) return RESIZE_EDGE_R | RESIZE_EDGE_B;

    if (x < w->x + m) edges |= RESIZE_EDGE_L;
    if (x >= w->x + w->w - m) edges |= RESIZE_EDGE_R;
    if (y < w->y + m) edges |= RESIZE_EDGE_T;
    if (y >= w->y + w->h - m) edges |= RESIZE_EDGE_B;

    return edges;
}

static int tile_split_hit_test_mode(int x, int y) {
    if (fullscreen_tile >= 0) return 0;
    if (tile_count < 2) return 0;
    const int m = 3;
    int near_v = 0;
    int near_h = 0;

    // vertical split
    if (tile_count >= 2) {
        if (x >= g_tile_split_x - m && x <= g_tile_split_x + m) near_v = 1;
    }
    // horizontal split
    if (tile_count == 3) {
        if (x >= g_tile_split_x && (y >= g_tile_split_y - m && y <= g_tile_split_y + m)) near_h = 1;
    } else if (tile_count >= 4) {
        if (y >= g_tile_split_y - m && y <= g_tile_split_y + m) near_h = 1;
    }

    if (near_v && near_h) return 3;
    if (near_v) return 1;
    if (near_h) return 2;
    return 0;
}

// For input hit-testing, treat the cursor's "hot" point as its center rather than its top-left.
// This makes resize interactions feel centered even for arrow-shaped cursors.
static inline void cursor_get_draw_pos_for_kind(int tip_x, int tip_y, int* out_x, int* out_y) {
    // Keep the normal cursor behavior unchanged: (tip_x,tip_y) is the draw origin.
    if (g_cursor_kind == CURSOR_NORMAL) {
        if (out_x) *out_x = tip_x;
        if (out_y) *out_y = tip_y;
        return;
    }
    // For resize cursors, center the sprite on the cursor tip.
    int w = 0, h = 0;
    cursor_get_dims(&w, &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (out_x) *out_x = tip_x - (w / 2);
    if (out_y) *out_y = tip_y - (h / 2);
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
            g_windows[i].continuous_redraw = 0;
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
    g_windows[win_id].continuous_redraw = 0;
}

void wm_unregister_gui_client(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].draw_cb = NULL;
    g_windows[win_id].key_cb = NULL;
    g_windows[win_id].mouse_cb = NULL;
    g_windows[win_id].userdata = NULL;
    g_windows[win_id].needs_redraw = 0;
    g_windows[win_id].continuous_redraw = 0;
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

void wm_set_continuous_redraw(int win_id, int enabled) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].continuous_redraw = enabled ? 1 : 0;
    if (g_windows[win_id].continuous_redraw) g_windows[win_id].needs_redraw = 1;
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
    int w = cursor_w;
    int h = cursor_h;
    const rei_image_t* cim = cursor_img_for_kind(g_cursor_kind);
    int cim_loaded = cursor_img_loaded_for_kind(g_cursor_kind);
    if (cim && cim_loaded) {
        w = cim->header.width;
        h = cim->header.height;
        if (g_cursor_xform & CURSOR_XFORM_ROT_90) {
            int tmp = w; w = h; h = tmp;
        }
    }
    // Simple fallback: small box if no image
    if (!cim_loaded || !cim || !cim->data) {
        for (int yy = 0; yy < h; ++yy) {
            int py = y + yy;
            if (py < 0 || py >= screen_h) continue;
            for (int xx = 0; xx < w; ++xx) {
                int px = x + xx;
                if (px < 0 || px >= screen_w) continue;
                vga_drawPixel_fb(px, py, 200, 200, 200);
            }
        }
        cursor_w = w; cursor_h = h;
        return;
    }
    // Draw REI image (supports MONO/RGB/RGBA)
    if (cim_loaded && !s_logged_cursor_once) {
        s_logged_cursor_once = 1;
    }
    // Transparency handling:
    // - RGBA: draw only if alpha >= 128 (skip low-alpha to avoid halos)
    // - RGB/MONO: if the four corner pixels are the same value/color, treat that as a color key
    //   and skip any pixels matching it. This removes solid background boxes on assets lacking alpha.
    const uint8_t* data = cim->data;
    int depth = cim->header.depth;
    int sw = cim->header.width;
    int sh = cim->header.height;
    int stride = sw * depth;
    // Determine color key for non-alpha formats by sampling corners
    int use_key = 0; uint8_t keyR = 0, keyG = 0, keyB = 0, keyM = 0;
    if (depth == REI_DEPTH_RGB && sw > 0 && sh > 0) {
        const uint8_t* tl = data + 0 * stride + 0 * 3;
        const uint8_t* tr = data + 0 * stride + (sw - 1) * 3;
        const uint8_t* bl = data + (sh - 1) * stride + 0 * 3;
        const uint8_t* br = data + (sh - 1) * stride + (sw - 1) * 3;
        if (tl[0]==tr[0] && tl[1]==tr[1] && tl[2]==tr[2] &&
            tl[0]==bl[0] && tl[1]==bl[1] && tl[2]==bl[2] &&
            tl[0]==br[0] && tl[1]==br[1] && tl[2]==br[2]) {
            use_key = 1; keyR = tl[0]; keyG = tl[1]; keyB = tl[2];
        }
    } else if (depth == REI_DEPTH_MONO && sw > 0 && sh > 0) {
        uint8_t tl = data[0];
        uint8_t tr = data[(sw - 1)];
        uint8_t bl = data[(sh - 1) * stride + 0];
        uint8_t br = data[(sh - 1) * stride + (sw - 1)];
        if (tl == tr && tl == bl && tl == br) { use_key = 1; keyM = tl; }
    }
    // Offsets into the saved-under buffer if the capture was clipped at edges
    int soffx = prev_saved_offx;
    int soffy = prev_saved_offy;
    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= screen_h) continue;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= screen_w) continue;
            // Apply transforms in destination space (so flips work consistently with rotation)
            int dx = xx;
            int dy = yy;
            if (g_cursor_xform & CURSOR_XFORM_FLIP_X) dx = (w - 1) - dx;
            if (g_cursor_xform & CURSOR_XFORM_FLIP_Y) dy = (h - 1) - dy;

            int sx = dx;
            int sy = dy;
            if (g_cursor_xform & CURSOR_XFORM_ROT_90) {
                // 90 degrees clockwise: dest(w=sh,h=sw) maps to source(sw,sh)
                // sx = sw - 1 - dy; sy = dx
                sx = (sw - 1) - dy;
                sy = dx;
            }
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
            const uint8_t* row = data + sy * stride;
            if (depth == REI_DEPTH_MONO) {
                uint8_t v = row[sx];
                if (use_key && v == keyM) continue;
                if (v) vga_drawPixel_fb(px, py, v, v, v);
            } else if (depth == REI_DEPTH_RGB) {
                const uint8_t* p3 = row + sx * 3;
                if (use_key && p3[0]==keyR && p3[1]==keyG && p3[2]==keyB) continue;
                vga_drawPixel_fb(px, py, p3[0], p3[1], p3[2]);
            } else if (depth == REI_DEPTH_RGBA) {
                const uint8_t* p4 = row + sx * 4;
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
    static const char* const paths[] ALIGN16 = { "/cursor.rei", "/ui/cursor.rei", "/testdir/cursor.rei", "/testdir/ui/cursor.rei" };
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

static void load_cursor_variant_try_paths(uint8 disk, const char* name, rei_image_t* out_img, int* out_loaded) {
    if (out_loaded) *out_loaded = 0;
    if (!name || !out_img || !out_loaded) return;
    static const char* const paths[] ALIGN16 = {
        "/ui/",            // primary
        "/testdir/ui/",    // compatibility
        NULL
    };
    char full[64];
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; paths[pi]; ++pi) {
        watchdog_kick("wm-iconload");
        snprintf(full, sizeof(full), "%s%s", paths[pi], name);
        full[sizeof(full) - 1] = '\0';
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, full, &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, out_img) == 0) {
                    *out_loaded = 1;
                    free(buf);
                    return;
                }
            }
            free(buf);
        }
    }
}

// Try to load a close button icon from a few candidate paths in EYNFS
static void load_close_icon_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/close.rei", "/testdir/ui16/close.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/close.rei", "/ui/close.rei", "/testdir/close.rei", "/testdir/ui/close.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;

    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

// Try to load a minimize button icon from candidate paths
static void load_min_icon_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/min.rei", "/testdir/ui16/min.rei", "/ui16/minimize.rei", "/testdir/ui16/minimize.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/min.rei", "/ui/min.rei", "/minimize.rei", "/ui/minimize.rei", "/testdir/min.rei", "/testdir/ui/min.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;

    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

// Try to load a maximize button icon from candidate paths
static void load_max_icon_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/max.rei", "/testdir/ui16/max.rei", "/ui16/maximize.rei", "/testdir/ui16/maximize.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/max.rei", "/ui/max.rei", "/maximize.rei", "/ui/maximize.rei", "/testdir/max.rei", "/testdir/ui/max.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;

    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

// Try to load unfocused variants of icons
static void load_close_icon_unf_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/close_unfocused.rei", "/testdir/ui16/close_unfocused.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/close_unfocused.rei", "/ui/close_unfocused.rei", "/testdir/close_unfocused.rei", "/testdir/ui/close_unfocused.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;

    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

static void load_min_icon_unf_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/min_unfocused.rei", "/testdir/ui16/min_unfocused.rei", "/ui16/minimize_unfocused.rei", "/testdir/ui16/minimize_unfocused.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/min_unfocused.rei", "/ui/min_unfocused.rei", "/minimize_unfocused.rei", "/ui/minimize_unfocused.rei", "/testdir/min_unfocused.rei", "/testdir/ui/min_unfocused.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;

    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

static void load_max_icon_unf_try_paths(uint8 disk) {
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    static const char* const paths16[] ALIGN16 = { "/ui16/max_unfocused.rei", "/testdir/ui16/max_unfocused.rei", "/ui16/maximize_unfocused.rei", "/testdir/ui16/maximize_unfocused.rei", NULL };
    static const char* const paths8[] ALIGN16 = { "/max_unfocused.rei", "/ui/max_unfocused.rei", "/maximize_unfocused.rei", "/ui/maximize_unfocused.rei", "/testdir/max_unfocused.rei", "/testdir/ui/max_unfocused.rei", NULL };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    const char** primary = want16 ? paths16 : paths8;
    const char** fallback = want16 ? paths8 : NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const char** paths = (pass == 0) ? primary : fallback;
        if (!paths) continue;
        for (int pi = 0; paths[pi]; ++pi) {
            watchdog_kick("wm-iconload");
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
}

static void layout_tiles() {
    // Convert layout rules into pixel rectangles
    if (tile_count <= 0) return;

    // Initialize splits when tile count changes
    if (g_tile_split_inited_for_count != tile_count) {
        g_tile_split_inited_for_count = tile_count;
        g_tile_split_x = screen_w / 2;
        g_tile_split_y = screen_h / 2;
    }

    // Clamp splits to keep tiles usable
    int minw = 64;
    int minh = 48;
    if (g_tile_split_x < minw) g_tile_split_x = minw;
    if (g_tile_split_x > screen_w - minw) g_tile_split_x = screen_w - minw;
    if (g_tile_split_y < minh) g_tile_split_y = minh;
    if (g_tile_split_y > screen_h - minh) g_tile_split_y = screen_h - minh;

    if (tile_count == 1) {
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = screen_w; tiles[0].height = screen_h;
    } else if (tile_count == 2) {
        int w = g_tile_split_x;
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = w; tiles[0].height = screen_h;
        tiles[1].x = w; tiles[1].y = 0; tiles[1].width = screen_w - w; tiles[1].height = screen_h;
    } else if (tile_count == 3) {
        int w = g_tile_split_x;
        int h = g_tile_split_y;
        tiles[0].x = 0; tiles[0].y = 0; tiles[0].width = w; tiles[0].height = screen_h;
        tiles[1].x = w; tiles[1].y = 0; tiles[1].width = screen_w - w; tiles[1].height = h;
        tiles[2].x = w; tiles[2].y = h; tiles[2].width = screen_w - w; tiles[2].height = screen_h - h;
    } else {
        int w = g_tile_split_x;
        int h = g_tile_split_y;
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
    int fh = vga_text_cell_h();
    int h = fh + 4;
    if (h < 16) h = 16;
    return h;
}

static void draw_decorations(tile_t* t, int is_focused) {
    if (t->type == TILE_EMPTY) return;
    int cw = vga_text_cell_w();
    int fh = vga_text_cell_h();
    int title_h = get_title_height(t);
    // In low mode, draw simpler title/borders (status is an Alt-held overlay drawn later)
    if (g_gui_low_mode) {
        if (title_h > 0) {
            int title_y = t->y;
            int title_color_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
            int title_color_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
            int title_color_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
            drawRect(t->x, title_y, t->width, title_h, title_color_r, title_color_g, title_color_b);
            if (t->title && t->title[0]) {
                int title_len = (int)strlen(t->title);
                int max_chars = t->width / cw; if (max_chars < 0) max_chars = 0;
                int text_y = title_y + (title_h - fh) / 2;
                int cr = is_focused ? 255 : 0, cg = is_focused ? 255 : 0, cb = is_focused ? 255 : 0;
                if (title_len <= max_chars) {
                    int start_x = t->x + (t->width - (cw * title_len)) / 2;
                    for (int i = 0; i < title_len; ++i) {
                        int cx = start_x + i * cw;
                        int clip_left = t->x + 4;
                        int clip_right = t->x + t->width - 4;
                        if (cx < clip_left) continue;
                        if (cx + cw > clip_right) break;
                        if (cx < 0 || cx + cw > screen_w) break;
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
                        int cx = t->x + i * cw;
                        int clip_left = t->x + 4;
                        int clip_right = t->x + t->width - 4;
                        if (cx < clip_left) continue;
                        if (cx + cw > clip_right) break;
                        if (cx < 0 || cx + cw > screen_w) break;
                        drawCharAt(cx, text_y, (int)(unsigned char)ch, cr, cg, cb);
                    }
                }
            }
        }
        // status overlay is drawn after content
        int border_r = is_focused ? 255 : 48, border_g = is_focused ? 255 : 48, border_b = is_focused ? 255 : 48;
        drawRect(t->x, t->y, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y + t->height - 1, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y, 1, t->height, border_r, border_g, border_b);
        drawRect(t->x + t->width - 1, t->y, 1, t->height, border_r, border_g, border_b);
        return;
    }
    if (title_h > 0) {
        int title_y = t->y;
        int title_color_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
        int title_color_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
        int title_color_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
        drawRect(t->x, title_y, t->width, title_h, title_color_r, title_color_g, title_color_b);
        if (t->title && t->title[0]) {
            int title_len = (int)strlen(t->title);
            int max_chars = t->width / cw;
            int text_y = title_y + (title_h - fh) / 2;
            int color_r = is_focused ? 255 : 0;
            int color_g = is_focused ? 255 : 0;
            int color_b = is_focused ? 255 : 0;
            if (title_len <= max_chars) {
                int start_x = t->x + (t->width - (cw * title_len)) / 2;
                for (int i = 0; i < title_len; ++i) {
                    int cx = start_x + i * cw;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + cw > clip_right) break;
                    if (cx < 0 || cx + cw > screen_w) break;
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
                    int cx = t->x + i * cw;
                    int clip_left = t->x + 4;
                    int clip_right = t->x + t->width - 4;
                    if (cx < clip_left) continue;
                    if (cx + cw > clip_right) break;
                    if (cx < 0 || cx + cw > screen_w) break;
                    drawCharAt(cx, text_y, (int)(unsigned char)ch, color_r, color_g, color_b);
                }
            }
        }
    }
    // status overlay is drawn after content
    // borders
    int border_r = is_focused ? 225 : 40;
    int border_g = is_focused ? 225 : 40;
    int border_b = is_focused ? 225 : 40;
    drawRect(t->x, t->y, t->width, 1, border_r, border_g, border_b);
    drawRect(t->x, t->y + t->height - 1, t->width, 1, border_r, border_g, border_b);
    drawRect(t->x, t->y, 1, t->height, border_r, border_g, border_b);
    drawRect(t->x + t->width - 1, t->y, 1, t->height, border_r, border_g, border_b);
}

static void draw_tile_status_overlay(tile_t* t) {
    if (!t) return;
    if (!status_overlay_visible()) return;
    if (t->type == TILE_EMPTY) return;
    if (fullscreen_tile == (t - tiles)) {
        // Fullscreen hides titlebar; keep status directly at top.
    }

    int title_h = get_title_height(t);
    int sh = status_bar_height_px();
    int sy = t->y + title_h;
    if (sy + sh > t->y + t->height) return;

    const char* left = t->status_left;
    if (!left || !left[0]) {
        if (t->type == TILE_SHELL) {
            left = "Super+n: New | Super+Arrow: Switch | Super+Q: Close/Exit";
        } else {
            left = "";
        }
    }

    const char* base = (t->type == TILE_SHELL) ? "" : "Ctrl+Q: Exit";
    const char* extra = (t->status_right && t->status_right[0]) ? t->status_right : NULL;

    draw_status_overlay_text(t->x, sy, t->width, sh, left, base, extra, 0);
    vga_mark_dirty_rect(t->x, sy, t->width, sh);
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
    (void)title_h;
    t->last_show_status = status_overlay_visible();
    t->last_focused = is_focused;
}

// Draw the dynamic/content area for a tile (invoked each frame or when content changes)
static void draw_tile_content(const tile_t* t) {
    if (t->type == TILE_EMPTY) return;
    int title_h = get_title_height(t);
    int sh = status_overlay_visible() ? status_bar_height_px() : 0;
    int cw = vga_text_cell_w();
    int fh = vga_text_cell_h();
    // Keep a 1px margin for the border so clears don't overwrite border pixels
    int content_x = t->x + 1;
    int content_y = t->y + title_h + sh + 1;
    int line_h = fh;
    int max_lines = (t->height - (title_h + sh + 2)) / line_h;
    int content_w = t->width - 2; // leave 1px border on left/right
    int content_h = t->height - (title_h + sh) - 2; // leave 1px top/bottom border
    int bg_bright = -1; // -1 unknown; 0=dark; 1=bright
    if (content_w > 0 && content_h > 0) {
        // Always start with a clean content area to avoid any residuals around centered/scaled images
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
        // Debug: show number of registered redirect icons (non-invasive onscreen feedback)
        if (shell_redirect_icon_count > 0) {
            char label[8];
            int n = (shell_redirect_icon_count > 9) ? 9 : shell_redirect_icon_count;
            label[0] = 'I'; label[1] = 'c'; label[2] = ':'; label[3] = '0' + n; label[4] = '\0';
            for (int i = 0; label[i]; ++i) drawCharAt(content_x + i * cw, content_y, (int)(unsigned char)label[i], 255, 200, 0);
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
        int cols = content_w / cw;
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
                // Icons are anchored to character columns within this line.
                // Draw every icon whose anchor column falls within this wrapped segment.
                int icon_count = vterm_get_line_icon_count(t->term_idx, abs_row);
                if (icon_count > 0) {
                    int py_icon = content_y + vis_row * line_h;
                    for (int ii = 0; ii < icon_count; ++ii) {
                        int icon_anchor_col = 0;
                        const char* line_icon_key = vterm_get_line_icon_key_n(t->term_idx, abs_row, ii, &icon_anchor_col);
                        if (!line_icon_key) continue;
                        if (icon_anchor_col < start_col || icon_anchor_col >= start_col + cols) continue;

                        // If we're in 16x mode, only use 16x16 icons when the text stream
                        // reserved enough character cells (typically two spaces for 16px-wide icons
                        // in an 8px-wide text grid). This keeps older output readable after font/icon
                        // mode changes.
                        int want16_global = (vga_text_cell_h() >= 16) ? 1 : 0;
                        int want16_icon = want16_global;
                        if (want16_global) {
                            if (icon_anchor_col < 0 || icon_anchor_col + 1 >= TERM_COLS) {
                                want16_icon = 0;
                            } else {
                                char c0 = src[icon_anchor_col];
                                char c1 = src[icon_anchor_col + 1];
                                if (c0 != ' ' || c1 != ' ') want16_icon = 0;
                            }
                        }

                        rei_image_t* icon = load_icon_for_ext_mode(line_icon_key, want16_icon);
                        if (!icon) icon = load_icon_for_ext_mode("file_none", want16_icon);
                        if (icon) {
                            int icon_x = content_x + (icon_anchor_col - start_col) * cw;
                            int icon_y = py_icon;
                            int ih = icon->header.height;
                            if (fh > ih) icon_y = py_icon + (fh - ih) / 2;
                            draw_rei_at(icon, icon_x, icon_y);
                        }
                    }
                }

                for (int cc = 0; cc < cols; ++cc) {
                    int px = content_x + cc * cw + line_indent_px;
                    int py = content_y + vis_row * line_h;
                    // Horizontal & vertical clipping
                    if (px + (cw - 1) < content_x || px >= content_x + content_w) continue;
                    if (py + (fh - 1) < content_y || py >= content_y + content_h) continue;
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
                                vga_darkenRect_bb(px, py, cw, fh, 128);
                            }
                        }
                    }
                    int is_sel = vterm_is_selected(t->term_idx, abs_row, src_col);
                    if (is_sel) {
                        // selection background teal-ish
                        drawRect(px, py, cw, fh, 0, 128, 128);
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
                        drawCharAt(px, py, (int)'_', 220, 220, 220);
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
    static const char* const opts_all[3] ALIGN16 = {"Tile", "Scale", "Center"};
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
    for (int i = 0; title[i]; ++i) drawCharAt(bx + 8 + i*8, by + 8, (int)(unsigned char)title[i], 220, 220, 220);
    const char* hint = "Enter=Select  Esc=Cancel";
    for (int i = 0; hint[i]; ++i) drawCharAt(bx + 8 + i*8, by + 24, (int)(unsigned char)hint[i], 200, 200, 200);
    int y0 = by + 40;
    for (int i = 0; i < opt_count; ++i) {
        int rr = (i == g_bg_modal.selected) ? 220 : 200;
        int gg = (i == g_bg_modal.selected) ? 220 : 200;
        int bb = (i == g_bg_modal.selected) ? 220 : 200;
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
    static const char* const opts_all[3] ALIGN16 = {"Tile", "Scale", "Center"};
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

    // If a GUI client is registered, allow it to veto closing.
    int term = tiles[tile_idx].term_idx;
    if (term >= 0 && term < MAX_TILES && gui_close_cb[term]) {
        tile_gui_close_cb cb = gui_close_cb[term];
        void* ud = gui_userdata[term];
        if (!cb(tile_idx, ud)) {
            return; // vetoed
        }
    }

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
    // Reassign term_idx for compacted tiles. Do NOT clear vterm buffers here - keep terminal content intact.
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

int tile_find_by_term(int term_idx) {
    if (term_idx < 0 || term_idx >= MAX_TILES) return -1;
    for (int i = 0; i < tile_count; ++i) {
        if (tiles[i].active && tiles[i].term_idx == term_idx) return i;
    }
    return -1;
}

void tile_get_content_rect(int tile_idx, int* out_x, int* out_y, int* out_w, int* out_h) {
    if (out_x) *out_x = 0;
    if (out_y) *out_y = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (tile_idx < 0 || tile_idx >= tile_count) return;

    tile_t* t = &tiles[tile_idx];
    if (t->type == TILE_EMPTY) return;

    int title_h = (fullscreen_tile == tile_idx) ? 0 : 16;

    int cx = t->x + 1;
    int cy = t->y + title_h + 1;
    int cw = t->width - 2;
    int ch = t->height - (title_h) - 2;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;

    if (out_x) *out_x = cx;
    if (out_y) *out_y = cy;
    if (out_w) *out_w = cw;
    if (out_h) *out_h = ch;
}

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
    gui_close_cb[term] = NULL;
    gui_userdata[term] = userdata;
    gui_needs_redraw[term] = 1;
    gui_continuous_redraw[term] = 0;
}

void tile_register_gui_client2(int tile_idx, tile_gui_draw_cb draw_cb, tile_gui_key_cb key_cb, tile_gui_mouse_cb mouse_cb, void* userdata) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_draw_cb[term] = draw_cb;
    gui_key_cb[term] = key_cb;
    gui_mouse_cb[term] = mouse_cb;
    gui_close_cb[term] = NULL;
    gui_userdata[term] = userdata;
    gui_needs_redraw[term] = 1;
    gui_continuous_redraw[term] = 0;
}

void tile_register_gui_close_cb(int tile_idx, tile_gui_close_cb close_cb_fn) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_close_cb[term] = close_cb_fn;
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
        gui_close_cb[term] = NULL;
        gui_userdata[term] = NULL;
        // No GUI anymore; clear any pending GUI invalidation
        gui_needs_redraw[term] = 0;
        gui_continuous_redraw[term] = 0;
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

void tile_set_gui_continuous_redraw(int tile_idx, int enabled) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    int term = tiles[tile_idx].term_idx;
    if (term < 0 || term >= MAX_TILES) term = tile_idx;
    gui_continuous_redraw[term] = enabled ? 1 : 0;
    if (gui_continuous_redraw[term]) gui_needs_redraw[term] = 1;
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

    // Ensure UI/icon assets match current font metrics (8x8 vs 16x16 icon sets).
    maybe_update_icon_mode(0);

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
        if (status_overlay_visible()) draw_tile_status_overlay(&tiles[i]);
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

    // Also poll + route mouse while a ring3 task is running (the main tiler loop is paused).
    // This enables GUI user programs to receive GUI_EVENT_MOUSE events.
    if (g_user_task_active) {
        mouse_poll();
        // mouse_read_event() always succeeds and clears deltas, so only read/dispatch
        // when something actually changed.
        int have_mouse = 0;
        if (g_mouse_state.delta_x || g_mouse_state.delta_y || g_mouse_state.wheel_delta) {
            have_mouse = 1;
        }
        if ((g_mouse_state.buttons ^ g_mouse_state.prev_buttons) != 0) {
            have_mouse = 1;
        }

        mouse_event_t me;
        if (have_mouse && mouse_read_event(&me) == 0) {
            // Route to focused window if any; else to the active user task's tile.
            if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                window_t* wfocus = &g_windows[g_win_focused];
                if (wfocus->mouse_cb) {
                    wfocus->mouse_cb(-1, (const mouse_event_t*)&me, wfocus->userdata);
                    wfocus->needs_redraw = 1;
                }
                return 1;
            }

            int target_tile = focused;
            if (g_user_task_term >= 0) {
                int tt = tile_find_by_term(g_user_task_term);
                if (tt >= 0 && tt < tile_count) target_tile = tt;
            }

            int term = tiles[target_tile].term_idx;
            if (term < 0 || term >= MAX_TILES) term = 0;

            if (gui_mouse_cb[term]) {
                gui_mouse_cb[term](target_tile, (const mouse_event_t*)&me, gui_userdata[term]);
                gui_needs_redraw[term] = 1;
                return 1;
            }
        }
    }

    int key = tui_read_key();

    // Shift+Alt toggles pinned status overlay (persisted). Must run even if key==0.
    {
        static int prev_alt = 0;
        static int prev_shift = 0;
        int cur_alt = tui_alt_pressed ? 1 : 0;
        int cur_shift = tui_shift_pressed ? 1 : 0;
        int alt_rise = (cur_alt && !prev_alt);
        int shift_rise = (cur_shift && !prev_shift);
        if ((alt_rise && cur_shift) || (shift_rise && cur_alt)) {
            int new_mode = ui_prefs_get_status_bar_mode() ? 0 : 1;
            ui_prefs_set_status_bar_mode(new_mode);
            (void)ui_prefs_save((uint8)g_current_drive);

            g_force_full_redraw = 1;
            g_tiles_full_content_redraw = 1;
            for (int ti = 0; ti < tile_count; ++ti) tiles[ti].static_drawn = 0;
            for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                if (g_windows[wi].used && !g_windows[wi].minimized) {
                    g_windows[wi].needs_redraw = 1;
                    g_windows[wi].static_drawn = 0;
                }
            }
        }
        prev_alt = cur_alt;
        prev_shift = cur_shift;
    }

    // If there's no actual key event to route, we're done.
    if (!key) return 0;

    // If status overlay visibility toggles, force redraw to restore any covered pixels.
    // This input pump path is called while ring3 tasks are running.
    static int last_overlay_state = -1;
    int overlay_now = status_overlay_visible();
    if (last_overlay_state != overlay_now) {
        last_overlay_state = overlay_now;
        g_force_full_redraw = 1;
        g_tiles_full_content_redraw = 1;
        for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
            if (g_windows[wi].used && !g_windows[wi].minimized) {
                g_windows[wi].needs_redraw = 1;
                g_windows[wi].static_drawn = 0;
            }
        }
    }

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
        // If this ring3 task has attached a GUI key handler, prefer routing keys
        // to the GUI event queue instead of hijacking them into stdin.
        if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
            // fall through to normal routing
        } else {
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
                    vterm_backspace_output(term);
                } else if (ch == '\n') {
                    vterm_write_char(term, '\n');
                } else {
                    vterm_write_char(term, ch);
                }
                // Add to stdin buffer
                vterm_stdin_putchar(term, ch);
                return 1;
            }
        }
        }
    }
    // If modal is active, handle it first.
    if (g_bg_modal.active) {
        (void)handle_bg_modal_key(key);
        return 1;
    }

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
                fullscreen_tile = -1; // exit fullscreen
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

    // While a ring3 task is active, route normal keys to the task's tile.
    // Mouse-based focus switching is not pumped in the PIT path, so relying on
    // 'focused' can starve the user task of input.
    int target_tile = focused;
    if (g_user_task_active && g_user_task_term >= 0) {
        int tt = tile_find_by_term(g_user_task_term);
        if (tt >= 0 && tt < tile_count) target_tile = tt;
    }

    int term = tiles[target_tile].term_idx;
    if (term < 0 || term >= MAX_TILES) term = 0;

    if (gui_key_cb[term]) {
        gui_key_cb[term](target_tile, key & 0xFFFF, gui_userdata[term]);
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
        uint32 hz_tmp0 = hal_time_tick_hz(); if (!hz_tmp0) hz_tmp0 = 50;
        g_drag_throttle_ticks = g_gui_low_mode ? (hz_tmp0 / 40 + 1) : (hz_tmp0 / 100 + 1);
        g_last_frame_tick = (uint32)hal_time_ticks();
        // initialize virtual terminals
        vterm_init_all();
        // prime GUI heartbeat
        g_last_gui_heartbeat_tick = (uint32)hal_time_ticks();

        // initialize simple double-buffer (best-effort)
        vga_init_double_buffer();

        // Initialize mouse and bounds to pixel space
        mouse_init();
        mouse_set_bounds(0, 0, screen_w - 1, screen_h - 1);
        // Try to load UI assets unless in low mode (saves memory and draw time)
        // Always try to load the cursor image (small asset); other window icons are skipped in low mode.
        load_cursor_image_try_paths(0);
        load_cursor_variant_try_paths(0, "cursor_res.rei", &g_cursor_res_img, &g_cursor_res_loaded);
        load_cursor_variant_try_paths(0, "cursor_res_diag.rei", &g_cursor_res_diag_img, &g_cursor_res_diag_loaded);
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

        // Background networking: pump RX/ARP/UDP enqueue at most once per tick.
        // This keeps host->guest delivery working even when the user isn't
        // running a dedicated udp-listen command.
        static uint32 last_net_tick = 0;
        uint32 now_net_tick = (uint32)hal_time_ticks();
#if defined(__i386__)
        if (net_is_inited() && now_net_tick != last_net_tick) {
            last_net_tick = now_net_tick;
            uint8 local_ip[4];
            net_get_local_ip(local_ip);
            (void)net_poll(local_ip, 32);
        }

        // Interrupt-assisted RX: poll immediately if NIC signaled data.
        if (net_is_inited() && e1000_irq_rx_pending()) {
            uint8 local_ip[4];
            net_get_local_ip(local_ip);
            (void)net_poll(local_ip, 128);
            e1000_irq_clear_rx_pending();
        }
#endif

        // If status overlay visibility toggles, force redraw to restore any covered pixels.
        static int last_overlay_state = -1;
        int overlay_now = status_overlay_visible();
        if (last_overlay_state != overlay_now) {
            last_overlay_state = overlay_now;
            g_force_full_redraw = 1;
            g_tiles_full_content_redraw = 1;
            for (int ti = 0; ti < tile_count; ++ti) {
                tiles[ti].static_drawn = 0;
            }
            for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                if (g_windows[wi].used && !g_windows[wi].minimized) {
                    g_windows[wi].needs_redraw = 1;
                    g_windows[wi].static_drawn = 0;
                }
            }
        }

        // Ensure UI/icon assets match current font metrics (8x8 vs 16x16 icon sets).
        maybe_update_icon_mode(0);
        // 1 Hz GUI heartbeat: invalidate GUI tiles once per second so they can update time-based views
        {
            uint32 now_ticks_hb = (uint32)hal_time_ticks();
            uint32 hz_hb = hal_time_tick_hz(); if (!hz_hb) hz_hb = 50;
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
    for (int i = 0; i < tile_count; ++i) {
        if (g_force_full_redraw) tiles[i].static_drawn = 0; // force static re-render
        int decor_fresh = 0;
        if (!tiles[i].static_drawn) { draw_static_tile(&tiles[i], i == focused); decor_fresh = 1; }
            // Redraw decorations: in low mode, only when changed; otherwise each frame for freshness
            int need_redraw_decor = 1;
            if (g_gui_low_mode) {
                need_redraw_decor = 0;
                int th_now_dec = get_title_height(&tiles[i]);
                if (!tiles[i].static_drawn || tiles[i].last_title_ptr != tiles[i].title ||
                    tiles[i].last_status_left_ptr != tiles[i].status_left || tiles[i].last_status_right_ptr != tiles[i].status_right ||
                    tiles[i].last_focused != (i == focused)) {
                    need_redraw_decor = 1;
                }
            }
            int th_now_dec = get_title_height(&tiles[i]);
            // We still maintain caches for potential future optimization
            if (need_redraw_decor) {
                draw_decorations(&tiles[i], i == focused);
                tiles[i].last_title_ptr = tiles[i].title;
                tiles[i].last_status_left_ptr = tiles[i].status_left;
                tiles[i].last_status_right_ptr = tiles[i].status_right;
                tiles[i].last_focused = (i == focused);
            }
            // Compute current content rect for this tile
            int th_now = get_title_height(&tiles[i]);
            int cx_now = tiles[i].x + 1;
            int cy_now = tiles[i].y + th_now + 1;
            int cw_now = tiles[i].width - 2;
            int ch_now = tiles[i].height - (th_now) - 2;
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
            if (g_force_full_redraw || g_tiles_full_content_redraw || (has_gui && (gui_needs_redraw[term_for_i] || gui_continuous_redraw[term_for_i])) || tiles[i].last_drawn_version != cur_ver || rect_changed) {
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
                if (has_gui && !gui_continuous_redraw[term_for_i]) gui_needs_redraw[term_for_i] = 0;
            }
            // Mark only the UI decoration areas (titlebar, statusbar, and 1px borders)
            // so the blit stays small while ensuring decorations are kept in sync.
            int th = get_title_height(&tiles[i]);
            int sh = status_overlay_visible() ? status_bar_height_px() : 0;
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

            // Draw status overlay on top of tile content.
            if (status_overlay_visible()) {
                draw_tile_status_overlay(&tiles[i]);
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
                if (w->needs_redraw || w->continuous_redraw || g_force_full_redraw || g_any_tile_content_redrew) {
                    wm_draw_content(w);
                    if (!w->continuous_redraw) w->needs_redraw = 0;
                }

                // Draw status overlay on top of window content.
                if (status_overlay_visible()) {
                    wm_draw_status_overlay(w);
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
            if (resize_active && resize_win >= 0 && g_windows[resize_win].used) {
                window_t* dw = &g_windows[resize_win];
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
            if ((drag_active && drag_win >= 0 && g_windows[drag_win].used) ||
                (resize_active && resize_win >= 0 && g_windows[resize_win].used)) {
                window_t* dw = drag_active ? &g_windows[drag_win] : &g_windows[resize_win];
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

        // Choose cursor kind (normal vs resize) based on hover/active state.
        cursor_set_style(CURSOR_NORMAL, 0);
        if (resize_active) {
            if ((resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                // Diagonal cursor base is NE-SW; flip for NW-SE corners.
                int xform = 0;
                if ((resize_edges & RESIZE_EDGE_L) && (resize_edges & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                if ((resize_edges & RESIZE_EDGE_R) && (resize_edges & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                cursor_set_style(CURSOR_RES_DIAG, xform);
            }
            else if (resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B)) cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
            else if (resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) cursor_set_style(CURSOR_RES_HOR, 0);
        } else if (tile_resize_active) {
            if (tile_resize_mode == 3) cursor_set_style(CURSOR_RES_DIAG, 0);
            else if (tile_resize_mode == 2) cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
            else if (tile_resize_mode == 1) cursor_set_style(CURSOR_RES_HOR, 0);
        } else if (cur_mx > -100 && cur_my > -100) {
            int w_hit = wm_hit_test(cur_mx, cur_my);
            if (w_hit >= 0) {
                int e = wm_resize_hit_test_edges(&g_windows[w_hit], cur_mx, cur_my);
                if (e) {
                    if ((e & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (e & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                        int xform = 0;
                        if ((e & RESIZE_EDGE_L) && (e & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                        if ((e & RESIZE_EDGE_R) && (e & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                        cursor_set_style(CURSOR_RES_DIAG, xform);
                    }
                    else if (e & (RESIZE_EDGE_T | RESIZE_EDGE_B)) cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                    else if (e & (RESIZE_EDGE_L | RESIZE_EDGE_R)) cursor_set_style(CURSOR_RES_HOR, 0);
                }
            } else {
                int m = tile_split_hit_test_mode(cur_mx, cur_my);
                if (m == 3) cursor_set_style(CURSOR_RES_DIAG, 0);
                else if (m == 2) cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                else if (m == 1) cursor_set_style(CURSOR_RES_HOR, 0);
            }
        }

        // Draw mouse cursor overlay on top of the freshly swapped framebuffer
        if (cur_mx > -100 && cur_my > -100) {
            int draw_x = cur_mx;
            int draw_y = cur_my;
            cursor_get_draw_pos_for_kind(cur_mx, cur_my, &draw_x, &draw_y);

            // Ensure save-under buffer matches current cursor size (if image changed)
            int nw = 0, nh = 0;
            cursor_get_dims(&nw, &nh);
            int bpp1 = vga_get_fb_bpp_bytes(); if (bpp1 < 3) bpp1 = 3;
            if (!cursor_savebuf || nw != cursor_save_w || nh != cursor_save_h) {
                int newlen = nw * nh * bpp1;
                if (cursor_savebuf) free(cursor_savebuf);
                cursor_savebuf = (unsigned char*)malloc(newlen);
                cursor_save_w = nw; cursor_save_h = nh; cursor_save_len = newlen;
            }
            if (cursor_savebuf) {
                // Clip capture to screen and remember exact saved region dims
                int cap_x = draw_x;
                int cap_y = draw_y;
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
                        // Store offsets from draw origin (draw_x,draw_y) to the capture top-left
                        prev_saved_offx = cap_x - draw_x;
                        prev_saved_offy = cap_y - draw_y;
                    } else {
                        prev_saved_w = prev_saved_h = 0;
                        prev_saved_offx = prev_saved_offy = 0;
                    }
                } else {
                    prev_saved_w = prev_saved_h = 0;
                    prev_saved_offx = prev_saved_offy = 0;
                }
            }
            draw_cursor_overlay(draw_x, draw_y);
            // Keep cursor_prev_* as the last mouse tip position.
            cursor_prev_x = cur_mx; cursor_prev_y = cur_my;
        } else {
            // No valid current position: clear prev so we don't try to restore garbage next frame
            cursor_prev_x = cursor_prev_y = -1000;
            prev_saved_w = prev_saved_h = 0;
        }
        // If drag just ended, ensure the last overlay region is reconciled by the next swap
        if (!drag_active && prev_drag_w > 0 && prev_drag_h > 0) {
            // The next frame's swap will copy the backbuffer which already has the window at its final position
            // Jjust clear the overlay bookkeeping so exclusion stops next frame
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
                        int e = wm_resize_hit_test_edges(&g_windows[w_hit], me.x, me.y);
                        if (e) {
                            resize_active = 1;
                            resize_win = w_hit;
                            resize_edges = e;
                            resize_start_mx = me.x;
                            resize_start_my = me.y;
                            resize_start_x = g_windows[w_hit].x;
                            resize_start_y = g_windows[w_hit].y;
                            resize_start_w = g_windows[w_hit].w;
                            resize_start_h = g_windows[w_hit].h;
                        } else if (point_in_rect(me.x, me.y, g_windows[w_hit].x, g_windows[w_hit].y, g_windows[w_hit].w, th)) {
                            drag_active = 1; drag_win = w_hit; drag_off_x = me.x - g_windows[w_hit].x; drag_off_y = me.y - g_windows[w_hit].y;
                        }
                    }
                    if (!did_action) {
                        g_win_focused = w_hit;
                        if (g_windows[w_hit].used) g_windows[w_hit].static_drawn = 0;
                    }
                } else {
                    // Tiling split resize start
                    int m = tile_split_hit_test_mode(me.x, me.y);
                    if (m) {
                        tile_resize_active = 1;
                        tile_resize_mode = m;
                    }
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
                resize_active = 0; resize_win = -1; resize_edges = 0;
                tile_resize_active = 0; tile_resize_mode = 0;
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
            if (tile_resize_active) {
                // Update splits based on mouse position and relayout tiles
                if (tile_resize_mode & 1) {
                    g_tile_split_x = me.x;
                }
                if (tile_resize_mode & 2) {
                    g_tile_split_y = me.y;
                }
                layout_tiles();
                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
                for (int ti = 0; ti < tile_count; ++ti) tiles[ti].static_drawn = 0;
                for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                    if (g_windows[wi].used && !g_windows[wi].minimized) {
                        g_windows[wi].needs_redraw = 1;
                        g_windows[wi].static_drawn = 0;
                    }
                }
            }

            if (resize_active && resize_win >= 0) {
                window_t* w = &g_windows[resize_win];
                int dx = me.x - resize_start_mx;
                int dy = me.y - resize_start_my;

                int new_x = resize_start_x;
                int new_y = resize_start_y;
                int new_w = resize_start_w;
                int new_h = resize_start_h;

                if (resize_edges & RESIZE_EDGE_L) { new_x = resize_start_x + dx; new_w = resize_start_w - dx; }
                if (resize_edges & RESIZE_EDGE_R) { new_w = resize_start_w + dx; }
                if (resize_edges & RESIZE_EDGE_T) { new_y = resize_start_y + dy; new_h = resize_start_h - dy; }
                if (resize_edges & RESIZE_EDGE_B) { new_h = resize_start_h + dy; }

                // Clamp size and position
                int minw = 64;
                int minh = 48;
                if (new_w < minw) {
                    if (resize_edges & RESIZE_EDGE_L) new_x -= (minw - new_w);
                    new_w = minw;
                }
                if (new_h < minh) {
                    if (resize_edges & RESIZE_EDGE_T) new_y -= (minh - new_h);
                    new_h = minh;
                }
                if (new_x < 0) { if (resize_edges & RESIZE_EDGE_L) { new_w += new_x; } new_x = 0; }
                if (new_y < 0) { if (resize_edges & RESIZE_EDGE_T) { new_h += new_y; } new_y = 0; }
                if (new_x + new_w > screen_w) new_w = screen_w - new_x;
                if (new_y + new_h > screen_h) new_h = screen_h - new_y;
                if (new_w < minw) new_w = minw;
                if (new_h < minh) new_h = minh;
                if (new_x + new_w > screen_w) new_x = screen_w - new_w;
                if (new_y + new_h > screen_h) new_y = screen_h - new_h;

                if (!g_gui_low_mode) {
                    vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                    int old_x = w->x, old_y = w->y, old_w = w->w, old_h = w->h;
                    w->x = new_x; w->y = new_y; w->w = new_w; w->h = new_h;
                    vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                    w->static_drawn = 0;
                    w->needs_redraw = 1;
                    g_tiles_full_content_redraw = 1;
                    for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                        if (wi == resize_win) continue;
                        window_t* wo = &g_windows[wi];
                        if (!wo->used || wo->minimized) continue;
                        if (rects_intersect(wo->x, wo->y, wo->w, wo->h, old_x, old_y, old_w, old_h) ||
                            rects_intersect(wo->x, wo->y, wo->w, wo->h, w->x, w->y, w->w, w->h)) {
                            wo->needs_redraw = 1; wo->static_drawn = 0;
                        }
                    }
                } else {
                    // Low mode: keep updates throttled via the existing drag throttle
                    uint32 nowt = (uint32)hal_time_ticks();
                    if (nowt - g_last_drag_tick >= g_drag_throttle_ticks) {
                        g_last_drag_tick = nowt;
                        w->x = new_x; w->y = new_y; w->w = new_w; w->h = new_h;
                    }
                    w->static_drawn = 0;
                }
            }

            if (drag_active && drag_win >= 0) {
                window_t* w = &g_windows[drag_win];
                // Drag throttling in low mode
                if (g_gui_low_mode) {
                    uint32 nowt = (uint32)hal_time_ticks();
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
                int sh2 = status_overlay_visible() ? status_bar_height_px() : 0;
                if (sh2 > 0) {
                    int sy = tt->y + title_h2;
                    if (me.y >= sy && me.y < sy + sh2) {
                        vga_mark_dirty_rect(tt->x, sy, tt->width, sh2);
                    }
                }
            }
        }

after_mouse_handling:

    int key = tui_read_key();
    if (key) { watchdog_kick("wm-key"); }

        // Shift+Alt toggles pinned status overlay (persisted in ui.cfg).
        // Note: modifier scancodes return 0 from tui_read_key(), so this must run even when key==0.
        {
            static int prev_alt = 0;
            static int prev_shift = 0;
            int cur_alt = tui_alt_pressed ? 1 : 0;
            int cur_shift = tui_shift_pressed ? 1 : 0;
            int alt_rise = (cur_alt && !prev_alt);
            int shift_rise = (cur_shift && !prev_shift);

            if ((alt_rise && cur_shift) || (shift_rise && cur_alt)) {
                int new_mode = ui_prefs_get_status_bar_mode() ? 0 : 1;
                ui_prefs_set_status_bar_mode(new_mode);
                (void)ui_prefs_save((uint8)g_current_drive);

                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
                for (int ti = 0; ti < tile_count; ++ti) tiles[ti].static_drawn = 0;
                for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                    if (g_windows[wi].used && !g_windows[wi].minimized) {
                        g_windows[wi].needs_redraw = 1;
                        g_windows[wi].static_drawn = 0;
                    }
                }
            }

            prev_alt = cur_alt;
            prev_shift = cur_shift;
        }
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
                    // If only one tile remains, DO NOT close/exit - keep at least one tile to avoid OS instability.
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

        // Esc: when a GUI client/window is focused, route it instead of exiting the WM.
        // This allows in-app dialogs (e.g., cancel prompts) to work.
        if (key == 27) {
            if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                window_t* wfocus = &g_windows[g_win_focused];
                if (wfocus->key_cb) { wfocus->key_cb(-1, key & 0xFFFF, wfocus->userdata); wfocus->needs_redraw = 1; }
                continue;
            }
            int term = tiles[focused].term_idx;
            if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
                gui_key_cb[term](focused, key & 0xFFFF, gui_userdata[term]);
                gui_needs_redraw[term] = 1;
                g_any_tile_content_redrew = 1;
                continue;
            }
            // No GUI/window focused: Esc exits WM
            running = 0;
            break;
        }

        // Ctrl+Q: close focused window (global) or default-exit focused GUI tile
        if (key == 0x2101) {
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
                    // If the GUI is still registered (app didn't close itself), consult close-veto callback.
                    if (gui_key_cb[term]) {
                        int allow_close = 1;
                        if (gui_close_cb[term]) {
                            allow_close = gui_close_cb[term](focused, gui_userdata[term]) ? 1 : 0;
                        }
                        if (allow_close) {
                            tile_unregister_gui_client(focused);
                            g_force_full_redraw = 1;
                        }
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
         * briefly to reduce busy-loop CPU usage. We use hal_sleep_us which halts
         * the processor until the next timer/interrupt which keeps latency low.
         */
        int idle_ok = 1;
        if (g_any_tile_content_redrew) idle_ok = 0;
        if (g_force_full_redraw) idle_ok = 0;
        if (g_tiles_full_content_redraw) idle_ok = 0;
        if (drag_active) idle_ok = 0;
        // Any GUI clients asking for redraw? (per-term)
        for (int _ti = 0; _ti < MAX_TILES; ++_ti) {
            if (gui_needs_redraw[_ti] || gui_continuous_redraw[_ti]) { idle_ok = 0; break; }
        }
        // Any windows needing redraw?
        if (idle_ok && g_window_count > 0) {
            for (int _wi = 0; _wi < MAX_WINDOWS; ++_wi) {
                if (g_windows[_wi].used && (g_windows[_wi].needs_redraw || g_windows[_wi].continuous_redraw)) { idle_ok = 0; break; }
            }
        }
        if (idle_ok) {
            /* Sleep ~2ms to yield CPU but remain responsive; wakes on interrupts
             * (mouse/keyboard/timer).
             */
            hal_sleep_us(2000);
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
            uint32 now_ticks = (uint32)hal_time_ticks();
            uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
            // Convert target_us to ticks and enforce at least 1 tick
            uint32 target_ticks = (frame_target_us_effective * hz) / 1000000; if (target_ticks < 1) target_ticks = 1;
            uint32 elapsed = now_ticks - g_last_frame_tick;
            if (elapsed < target_ticks) {
                uint32 remaining_ticks = target_ticks - elapsed;
                // approx sleep
                uint32 us_per_tick = 1000000 / hz;
                hal_sleep_us(remaining_ticks * us_per_tick);
            }
            g_last_frame_tick = (uint32)hal_time_ticks();
        }
    }
}

// - Runtime GUI tuning (low-spec controls) -
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
        uint32 hz0 = hal_time_tick_hz(); if (!hz0) hz0 = 50;
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
    uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
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
    uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
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
