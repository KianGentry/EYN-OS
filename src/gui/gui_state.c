/*
 * Shared state: all static globals, constants, inline helpers, and forward
 * declarations. Every other gui_*.c module depends on what is defined here.
 * Covers: capacity budget, tile_t/window_t types, callback typedefs,
 * screen/GUI arrays, cursor state, theme, background helpers, WM globals.
*/

static int tm_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void tm_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

#define MAX_TILES 4
#define MAX_WINDOWS 8
#define PROG_NAME_MAX 20

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
    int minimized;
    int maximized;
    int pressed_button;
    int prev_x, prev_y, prev_w, prev_h;
    int desktop;             /* which virtual desktop this tile belongs to */
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

static int tile_find_first_visible(void) {
    for (int i = 0; i < tile_count; ++i) {
        if (tiles[i].type != TILE_EMPTY && !tiles[i].minimized) return i;
    }
    return -1;
}

static void tile_ensure_valid_focus(void) {
    if (tile_count <= 0) {
        focused = 0;
        return;
    }
    if (focused < 0 || focused >= tile_count || tiles[focused].type == TILE_EMPTY || tiles[focused].minimized) {
        int vis = tile_find_first_visible();
        focused = (vis >= 0) ? vis : 0;
    }
}

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
    uint32 hz = sched_get_tick_hz();
    if (!hz) hz = 50;
    uint32 ticks = sched_get_tick_count();
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
static int g_fb_w = 640;   // physical framebuffer width
static int g_fb_h = 480;   // physical framebuffer height
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

/*
 * SECURITY-INVARIANT: Cursor visibility flag.
 *
 * When set to 1, the cursor REI sprite is suppressed in draw_cursor_overlay
 * call sites (gui_input.c and tiling_manager.c).  Used by the gui_warp_mouse
 * syscall to hide the cursor for applications that grab the mouse (e.g. games).
 *
 * Invariant: automatically cleared when the owning ring-3 task exits or when
 *            gui_warp_mouse is not active for the focused tile.
 * Breakage if removed: cursor would always be visible even in grabbed mode,
 *                      causing distracting sprite jitter at the centre of the
 *                      screen during gameplay.
 * ABI-sensitive: No (kernel-internal only; userland controls it via
 *                gui_set_cursor_visible syscall).
 */
static int g_cursor_hidden = 0;

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

// --- Runtime theme (tile/window chrome colours) ---
// Classic gray palette with high-contrast beveled edges.
static wm_theme_t g_wm_theme = {
    .title_focused_r = 112, .title_focused_g = 112, .title_focused_b = 112,
    .title_unfocused_r = 86, .title_unfocused_g = 86, .title_unfocused_b = 86,
    .status_r = 74, .status_g = 74, .status_b = 74,
    .status_text_r = 240, .status_text_g = 240, .status_text_b = 240,
};

static const int UI_SURFACE_R = 100;
static const int UI_SURFACE_G = 100;
static const int UI_SURFACE_B = 100;
static const int UI_SURFACE_DARK_R = 84;
static const int UI_SURFACE_DARK_G = 84;
static const int UI_SURFACE_DARK_B = 84;
static const int UI_HILITE_OUTER_R = 196;
static const int UI_HILITE_OUTER_G = 196;
static const int UI_HILITE_OUTER_B = 196;
static const int UI_HILITE_INNER_R = 154;
static const int UI_HILITE_INNER_G = 154;
static const int UI_HILITE_INNER_B = 154;
static const int UI_SHADOW_INNER_R = 60;
static const int UI_SHADOW_INNER_G = 60;
static const int UI_SHADOW_INNER_B = 60;
static const int UI_SHADOW_OUTER_R = 34;
static const int UI_SHADOW_OUTER_G = 34;
static const int UI_SHADOW_OUTER_B = 34;
static const int UI_TEXT_R = 240;
static const int UI_TEXT_G = 240;
static const int UI_TEXT_B = 240;
static const int UI_TEXT_DIM_R = 206;
static const int UI_TEXT_DIM_G = 206;
static const int UI_TEXT_DIM_B = 206;

static void draw_raised_box(int x, int y, int w, int h, int fill_r, int fill_g, int fill_b) {
    if (w <= 0 || h <= 0) return;
    drawRect(x, y, w, h, fill_r, fill_g, fill_b);
    if (w < 2 || h < 2) return;

    drawRect(x, y, w, 1, UI_HILITE_OUTER_R, UI_HILITE_OUTER_G, UI_HILITE_OUTER_B);
    drawRect(x, y, 1, h, UI_HILITE_OUTER_R, UI_HILITE_OUTER_G, UI_HILITE_OUTER_B);
    drawRect(x + w - 1, y, 1, h, UI_SHADOW_OUTER_R, UI_SHADOW_OUTER_G, UI_SHADOW_OUTER_B);
    drawRect(x, y + h - 1, w, 1, UI_SHADOW_OUTER_R, UI_SHADOW_OUTER_G, UI_SHADOW_OUTER_B);

    if (w >= 4 && h >= 4) {
        drawRect(x + 1, y + 1, w - 2, 1, UI_HILITE_INNER_R, UI_HILITE_INNER_G, UI_HILITE_INNER_B);
        drawRect(x + 1, y + 1, 1, h - 2, UI_HILITE_INNER_R, UI_HILITE_INNER_G, UI_HILITE_INNER_B);
        drawRect(x + w - 2, y + 1, 1, h - 2, UI_SHADOW_INNER_R, UI_SHADOW_INNER_G, UI_SHADOW_INNER_B);
        drawRect(x + 1, y + h - 2, w - 2, 1, UI_SHADOW_INNER_R, UI_SHADOW_INNER_G, UI_SHADOW_INNER_B);
    }
}

static void draw_sunken_box(int x, int y, int w, int h, int fill_r, int fill_g, int fill_b) {
    if (w <= 0 || h <= 0) return;
    drawRect(x, y, w, h, fill_r, fill_g, fill_b);
    if (w < 2 || h < 2) return;

    drawRect(x, y, w, 1, UI_SHADOW_OUTER_R, UI_SHADOW_OUTER_G, UI_SHADOW_OUTER_B);
    drawRect(x, y, 1, h, UI_SHADOW_OUTER_R, UI_SHADOW_OUTER_G, UI_SHADOW_OUTER_B);
    drawRect(x + w - 1, y, 1, h, UI_HILITE_OUTER_R, UI_HILITE_OUTER_G, UI_HILITE_OUTER_B);
    drawRect(x, y + h - 1, w, 1, UI_HILITE_OUTER_R, UI_HILITE_OUTER_G, UI_HILITE_OUTER_B);

    if (w >= 4 && h >= 4) {
        drawRect(x + 1, y + 1, w - 2, 1, UI_SHADOW_INNER_R, UI_SHADOW_INNER_G, UI_SHADOW_INNER_B);
        drawRect(x + 1, y + 1, 1, h - 2, UI_SHADOW_INNER_R, UI_SHADOW_INNER_G, UI_SHADOW_INNER_B);
        drawRect(x + w - 2, y + 1, 1, h - 2, UI_HILITE_INNER_R, UI_HILITE_INNER_G, UI_HILITE_INNER_B);
        drawRect(x + 1, y + h - 2, w - 2, 1, UI_HILITE_INNER_R, UI_HILITE_INNER_G, UI_HILITE_INNER_B);
    }
}

static void draw_button_box(int x, int y, int w, int h, int pressed, int fill_r, int fill_g, int fill_b) {
    int hovered = (cursor_prev_x >= x && cursor_prev_x < x + w && cursor_prev_y >= y && cursor_prev_y < y + h);
    if (pressed) {
        draw_sunken_box(x, y, w, h, fill_r, fill_g, fill_b);
    } else if (hovered) {
        int hr = fill_r + 12; if (hr > 255) hr = 255;
        int hg = fill_g + 12; if (hg > 255) hg = 255;
        int hb = fill_b + 12; if (hb > 255) hb = 255;
        draw_raised_box(x, y, w, h, hr, hg, hb);
    } else {
        draw_raised_box(x, y, w, h, fill_r, fill_g, fill_b);
    }
}

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
    g_wm_theme.title_focused_r = 112; g_wm_theme.title_focused_g = 112; g_wm_theme.title_focused_b = 112;
    g_wm_theme.title_unfocused_r = 86; g_wm_theme.title_unfocused_g = 86; g_wm_theme.title_unfocused_b = 86;
    g_wm_theme.status_r = 74; g_wm_theme.status_g = 74; g_wm_theme.status_b = 74;
    g_wm_theme.status_text_r = 240; g_wm_theme.status_text_g = 240; g_wm_theme.status_text_b = 240;
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
static int g_desktop_bg_tile_idx = -1;

void tile_desktop_background_set(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    g_desktop_bg_tile_idx = tile_idx;
}

void tile_desktop_background_clear(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    if (g_desktop_bg_tile_idx == tile_idx) g_desktop_bg_tile_idx = -1;
}

void tile_desktop_background_remap(int old_idx, int new_idx) {
    if (old_idx < 0 || old_idx >= MAX_TILES || new_idx < 0 || new_idx >= MAX_TILES) return;
    if (g_desktop_bg_tile_idx == old_idx) {
        g_desktop_bg_tile_idx = new_idx;
    } else if (g_desktop_bg_tile_idx > old_idx) {
        g_desktop_bg_tile_idx--;
    }
}

// GCC -fanalyzer can miss that some allocations intentionally escape via
// struct fields. Keep a volatile escape sink so the analyzer treats these
// transfers as non-leaking without affecting runtime behavior.
static volatile const void* g_analyzer_escape_sink;

// Convert RGBA image to RGB in-place for background use (alpha<128 becomes black)
static uint8_t* bg_convert_rgba_to_rgb_alloc(const rei_image_t* im, size_t* out_sz) {
    if (out_sz) *out_sz = 0;
    if (!im || !im->data) return NULL;
    if (im->header.depth != REI_DEPTH_RGBA) return NULL;

    int w = im->header.width, h = im->header.height;
    size_t rgb_sz = (size_t)w * (size_t)h * (size_t)REI_DEPTH_RGB;
    uint8_t* rgb = (uint8_t*)malloc(rgb_sz);
    if (!rgb) return NULL;

    const uint8_t* src = im->data;
    size_t si = 0, di = 0;
    size_t total = (size_t)w * (size_t)h;
    for (size_t i = 0; i < total; ++i) {
        if ((i & 0x3FF) == 0) tm_ctx_account(SCHED_COST_ALLOC);
        uint8_t r8 = src[si + 0], g8 = src[si + 1], b8 = src[si + 2], a = src[si + 3];
        if (a < 128) { r8 = g8 = b8 = 0; }
        rgb[di + 0] = r8;
        rgb[di + 1] = g8;
        rgb[di + 2] = b8;
        si += 4;
        di += 3;
    }

    if (out_sz) *out_sz = rgb_sz;
    return rgb;
}

static void bg_convert_rgba_to_rgb(rei_image_t* im) {
    if (!im || !im->data) return;
    if (im->header.depth != REI_DEPTH_RGBA) return;
    if (!tm_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return;

    size_t out_sz = 0;
    uint8_t* rgb = bg_convert_rgba_to_rgb_alloc(im, &out_sz);
    if (!rgb) return;
    g_analyzer_escape_sink = rgb;

    uint8_t* old = im->data;
    im->data = rgb;
    im->data_size = out_sz;
    im->header.depth = REI_DEPTH_RGB;
    free(old);
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

static void draw_desktop_background(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_desktop_bg_tile_idx >= 0 && g_desktop_bg_tile_idx < MAX_TILES) {
        tile_bg_t* bg = &g_tile_bg[g_desktop_bg_tile_idx];
        if (bg->img && bg->img->data && bg->mode != BG_NONE) {
            const rei_image_t* im = bg->img;
            int iw = im->header.width;
            int ih = im->header.height;
            int depth = im->header.depth;
            const uint8_t* base = im->data;
            if (iw > 0 && ih > 0) {
                if (bg->mode == BG_TILE) {
                    for (int yy = 0; yy < h; ++yy) {
                        int sy = yy % ih;
                        const uint8_t* row = base + sy * iw * depth;
                        int py = y + yy;
                        for (int xx = 0; xx < w; ++xx) {
                            int sx = xx % iw;
                            int px = x + xx;
                            if (depth == REI_DEPTH_MONO) {
                                uint8_t v = row[sx];
                                apply_darken(&v, &v, &v, bg->darken);
                                drawPixel(px, py, v, v, v);
                            } else if (depth == REI_DEPTH_RGB) {
                                const uint8_t* p = row + sx * 3;
                                uint8_t r8 = p[0], g8 = p[1], b8 = p[2];
                                apply_darken(&r8, &g8, &b8, bg->darken);
                                drawPixel(px, py, r8, g8, b8);
                            } else if (depth == REI_DEPTH_RGBA) {
                                const uint8_t* p = row + sx * 4;
                                if (p[3] >= 128) {
                                    uint8_t r8 = p[0], g8 = p[1], b8 = p[2];
                                    apply_darken(&r8, &g8, &b8, bg->darken);
                                    drawPixel(px, py, r8, g8, b8);
                                }
                            }
                        }
                    }
                    return;
                }
                if (bg->mode == BG_CENTER || bg->mode == BG_SCALE) {
                    int draw_w = iw;
                    int draw_h = ih;
                    if (bg->mode == BG_SCALE) {
                        int sx_num = w, sx_den = iw;
                        int sy_num = h, sy_den = ih;
                        int use_x = ((long long)sx_num * sy_den <= (long long)sy_num * sx_den);
                        int num = use_x ? sx_num : sy_num;
                        int den = use_x ? sx_den : sy_den;
                        if (den > 0 && num > 0) {
                            draw_w = (iw * num) / den;
                            draw_h = (ih * num) / den;
                        }
                    }
                    int ox = x + (w - draw_w) / 2;
                    int oy = y + (h - draw_h) / 2;
                    for (int yy = 0; yy < draw_h; ++yy) {
                        int sy = (bg->mode == BG_SCALE) ? ((yy * ih) / draw_h) : yy;
                        if (sy < 0 || sy >= ih) continue;
                        const uint8_t* row = base + sy * iw * depth;
                        int py = oy + yy;
                        if (py < y || py >= y + h) continue;
                        for (int xx = 0; xx < draw_w; ++xx) {
                            int sx = (bg->mode == BG_SCALE) ? ((xx * iw) / draw_w) : xx;
                            if (sx < 0 || sx >= iw) continue;
                            int px = ox + xx;
                            if (px < x || px >= x + w) continue;
                            if (depth == REI_DEPTH_MONO) {
                                uint8_t v = row[sx];
                                apply_darken(&v, &v, &v, bg->darken);
                                drawPixel(px, py, v, v, v);
                            } else if (depth == REI_DEPTH_RGB) {
                                const uint8_t* p = row + sx * 3;
                                uint8_t r8 = p[0], g8 = p[1], b8 = p[2];
                                apply_darken(&r8, &g8, &b8, bg->darken);
                                drawPixel(px, py, r8, g8, b8);
                            } else if (depth == REI_DEPTH_RGBA) {
                                const uint8_t* p = row + sx * 4;
                                if (p[3] >= 128) {
                                    uint8_t r8 = p[0], g8 = p[1], b8 = p[2];
                                    apply_darken(&r8, &g8, &b8, bg->darken);
                                    drawPixel(px, py, r8, g8, b8);
                                }
                            }
                        }
                    }
                    return;
                }
            }
        }
    }
    drawRect(x, y, w, h, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
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
    tile_gui_close_cb close_cb;
    void* userdata;
    int needs_redraw;
    int continuous_redraw;
    // state
    int minimized;
    int maximized;
    int pressed_button;
    int prev_x, prev_y, prev_w, prev_h; // for restore after maximize
    int desktop;             // which virtual desktop this window belongs to
} window_t;

static window_t g_windows[MAX_WINDOWS];
static int g_window_order[MAX_WINDOWS]; // z-order back->front indices into g_windows
static int g_window_count = 0;
static int g_win_focused = -1; // index into g_windows, not order array

/*
 * Desktop switcher state (declared early so helper functions like
 * wm_hit_test and tile_index_at can filter by the active desktop).
 */
#define MAX_DESKTOPS 4
static int g_current_desktop = 0;
static int g_desktop_count   = 2;  // start with 2 virtual desktops

/*
 * Right-click context menu state (declared early so draw_ctx_menu
 * can reference it from its definition site near the other modals).
 */
static int  g_ctx_active = 0;
static int  g_ctx_x = 0, g_ctx_y = 0;
static int  g_ctx_kind = 0;
static int  g_ctx_index = -1;
static int  g_ctx_hover = -1;
#define CTX_ITEM_COUNT 3

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
    char key[24];
    rei_image_t img;
    int loaded;
} icon_cache_entry_t;

static icon_cache_entry_t g_icon_cache[ICON_CACHE_MAX];
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
    if (!tm_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return NULL;

    want16 = want16 ? 1 : 0;
    char cache_key[24];
    snprintf(cache_key, sizeof(cache_key), "%s|%s", want16 ? "16" : "8", ext);
    cache_key[sizeof(cache_key) - 1] = '\0';

    // Check cache first
    for (int i = 0; i < g_icon_cache_count; ++i) {
        if ((i & 0x7) == 0) tm_ctx_account(SCHED_COST_FS);
        if (strcmp(g_icon_cache[i].key, cache_key) == 0) {
            return g_icon_cache[i].loaded ? &g_icon_cache[i].img : NULL;
        }
    }
    if (g_icon_cache_count >= ICON_CACHE_MAX) return NULL;

    // Candidate filename patterns.
    // ext is treated as the full icon base name (without .rei), e.g. "file_txt", "file_none", "dir_empty", "dir_full".
    const char* patterns16[] = {
        "/icons16/%s.rei",
        "/testdir/icons16/%s.rei",
        // Legacy fallbacks
        "/icons16/file_%s.rei",
        "/testdir/icons16/file_%s.rei",
        NULL
    };
    const char* patterns8[] = {
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
            if ((pi & 0x3) == 0) tm_ctx_account(SCHED_COST_FS);
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

/*
 * Non-static accessor so kernel code (isr.c syscall handlers) can control
 * cursor visibility without reaching into the gui module's static state.
 */
void gui_cursor_set_hidden(int hidden)
{
    g_cursor_hidden = hidden;
}

/*
 * Draw a file-type icon at (x, y) by looking up the icon name
 * (e.g. "file_c", "dir_empty") in the icon cache. If not cached,
 * attempts to load from /icons16/ or /icons/.
 * Called from user_gui_draw_cb (isr.c) for USER_GUI_CMD_ICON commands.
 */
void tile_draw_file_icon(const char* icon_name, int x, int y) {
    if (!icon_name || !icon_name[0]) return;
    int want16 = (vga_text_cell_h() >= 16) ? 1 : 0;
    rei_image_t* im = load_icon_for_ext_mode(icon_name, want16);
    if (im) {
        draw_rei_at(im, x, y);
    }
}

