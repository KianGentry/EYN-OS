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
// global scroll tick used for marquee/scrolling text
static int g_tile_scroll_tick = 0;
// Force a full redraw of all tiles once (used after layout changes)
static int g_force_full_redraw = 1;
// Fullscreen state: -1 means normal, otherwise index of tile that is fullscreened
static int fullscreen_tile = -1;

// GUI client storage
static tile_gui_draw_cb gui_draw_cb[MAX_TILES];
static tile_gui_key_cb gui_key_cb[MAX_TILES];
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
static int cursor_w = 12, cursor_h = 18; // fallback size if no image
// Track a union of regions touched by the cursor this frame to minimize blits
// (legacy) damage tracking no longer needed when we explicitly mark
// previous and current cursor rects dirty each frame.
// Track if any content/decor dirty rect intersects the previous cursor region.
static int g_dirty_hits_prev_cursor = 0;

static inline int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    int ax2 = ax + aw, ay2 = ay + ah;
    int bx2 = bx + bw, by2 = by + bh;
    return (ax < bx2 && ax2 > bx && ay < by2 && ay2 > by);
}

static void draw_cursor_overlay(int x, int y) {
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
    // Draw REI image (supports MONO/RGB/RGBA). Treat alpha==0 as transparent; no blending.
    const uint8_t* data = g_cursor_img.data;
    int depth = g_cursor_img.header.depth;
    int stride = w * depth;
    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= screen_h) continue;
        const uint8_t* row = data + yy * stride;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= screen_w) continue;
            if (depth == REI_DEPTH_MONO) {
                uint8_t v = row[xx];
                if (v) vga_drawPixel_fb(px, py, v, v, v);
            } else if (depth == REI_DEPTH_RGB) {
                const uint8_t* p3 = row + xx * 3;
                vga_drawPixel_fb(px, py, p3[0], p3[1], p3[2]);
            } else if (depth == REI_DEPTH_RGBA) {
                const uint8_t* p4 = row + xx * 4;
                uint8_t a = p4[3];
                if (a) vga_drawPixel_fb(px, py, p4[0], p4[1], p4[2]);
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
    const char* paths[] = { "/cursor.rei", "/testdir/cursor.rei" };
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(disk, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    for (int pi = 0; pi < 2; ++pi) {
        eynfs_dir_entry_t entry;
        if (eynfs_traverse_path(disk, &sb, paths[pi], &entry, NULL, NULL) == 0 && entry.type == EYNFS_TYPE_FILE) {
            int size = entry.size;
            uint8_t* buf = (uint8_t*)malloc(size);
            if (!buf) return;
            int n = eynfs_read_file(disk, &sb, &entry, (char*)buf, size, 0);
            if (n == size) {
                if (rei_parse_image(buf, size, &g_cursor_img) == 0) {
                    g_cursor_loaded = 1;
                    free(buf); // image parser duplicated into its own buffer if needed
                    return;
                }
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
        // Determine the absolute last row to show (cursor row)
        int end_row = vterm_get_cursor_row(t->term_idx);
        // Build visual rows backward: fill from bottom up
        int vis_row = max_lines - 1;
        int abs_row = end_row;
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
                    drawCharAt(px, py, (int)(unsigned char)ch, rr, gg, bb);
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
    gui_userdata[term] = userdata;
    gui_needs_redraw[term] = 1;
}

void tile_unregister_gui_client(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    // Determine the terminal slot associated with this tile and clear callbacks there
    int term = tiles[tile_idx].term_idx;
    if (term >= 0 && term < MAX_TILES) {
        gui_draw_cb[term] = NULL;
        gui_key_cb[term] = NULL;
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
            if (!tiles[i].static_drawn) draw_static_tile(&tiles[i], i == focused);
            // Redraw decorations only if something changed (focus, title/status pointers, or show_status flag)
            int need_redraw_decor = 0;
            extern int tui_alt_pressed;
            int th_now_dec = get_title_height(&tiles[i]);
            int show_status_now_dec = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
            if (tiles[i].last_focused != (i == focused) ||
                tiles[i].last_title_ptr != tiles[i].title ||
                tiles[i].last_status_left_ptr != tiles[i].status_left ||
                tiles[i].last_status_right_ptr != tiles[i].status_right ||
                tiles[i].last_show_status != show_status_now_dec) {
                need_redraw_decor = 1;
            }
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
            if (g_force_full_redraw || (has_gui && gui_needs_redraw[term_for_i]) || tiles[i].last_drawn_version != cur_ver || rect_changed) {
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
            if (need_redraw_decor) {
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
        if (g_force_full_redraw) g_force_full_redraw = 0;
        tui_refresh();
        // Before swapping, exclude the previous cursor rect only if that area didn't change.
        if (!g_dirty_hits_prev_cursor && prev_saved_w > 0 && prev_saved_h > 0) {
            vga_set_swap_exclude(prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h);
        } else {
            vga_clear_swap_exclude();
        }
        // swap backbuffer to framebuffer (if backbuffer in use)
        vga_swap_buffers();
        // After swap, clear exclusion so future swaps can choose a new region
        vga_clear_swap_exclude();
    // Draw/restore cursor during vblank to minimize tearing/flicker
    vga_wait_vblank();

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
                    } else {
                        prev_saved_w = prev_saved_h = 0;
                    }
                } else {
                    prev_saved_w = prev_saved_h = 0;
                }
            }
            draw_cursor_overlay(cur_mx, cur_my);
            cursor_prev_x = cur_mx; cursor_prev_y = cur_my;
        } else {
            // No valid current position: clear prev so we don't try to restore garbage next frame
            cursor_prev_x = cursor_prev_y = -1000;
            prev_saved_w = prev_saved_h = 0;
        }
        // No swap exclusion used; overlay erase is handled proactively via backbuffer blit each loop

        // Handle mouse click-to-focus. Read a lightweight event snapshot.
        mouse_event_t me;
        if (mouse_read_event(&me) == 0) {
            // On left button press edge, set focus to the tile under cursor
            uint8 changes = me.button_changes;
            uint8 downMask = (me.buttons & MOUSE_BUTTON_LEFT);
            uint8 prevWasDown = (g_mouse_state.prev_buttons & MOUSE_BUTTON_LEFT);
            if ((changes & MOUSE_BUTTON_LEFT) && downMask && !prevWasDown) {
                int hit = tile_index_at(me.x, me.y);
                if (hit >= 0 && hit < tile_count && hit != focused) {
                    focused = hit;
                    g_force_full_redraw = 1;
                    if (gui_draw_cb[tiles[focused].term_idx]) gui_needs_redraw[tiles[focused].term_idx] = 1;
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
            // Super+Q to close focused tile
            if ((base & 0xFF) == 'q') {
                // If more than one tile, close the focused tile; otherwise exit tiling manager (return to main shell)
                if (tile_count > 1) {
                    tile_close(focused);
                    g_force_full_redraw = 1;
                    layout_tiles();
                } else {
                    running = 0; // exit tiling manager and return to main shell
                    break;
                }
                continue;
            }
        }

        // Esc to exit
        if (key == 27) {
            running = 0;
            break;
        }

        // Route input to focused vterm using full key handling
        // If a GUI client is registered for the focused tile, forward key to it
        int term = tiles[focused].term_idx;
        if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
            gui_key_cb[term](focused, key & 0xFFFF, gui_userdata[term]);
            gui_needs_redraw[term] = 1;
        } else {
            vterm_handle_key(tiles[focused].term_idx, key & 0xFFFF);
        }
    }
}
