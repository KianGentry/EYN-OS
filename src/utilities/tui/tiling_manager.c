#include <types.h>
#include <vga.h>
#include <tui.h>
#include <terminals.h>
#include <string.h>
#include <tile_manager.h>

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
} tile_t;

static tile_t tiles[MAX_TILES];
static int tile_count = 0;
static int focused = 0; // index of focused tile (0..tile_count-1)
// global scroll tick used for marquee/scrolling text
static int g_tile_scroll_tick = 0;
// Force a full redraw of all tiles once (used after layout changes)
static int g_force_full_redraw = 1;

// GUI client storage
static tile_gui_draw_cb gui_draw_cb[MAX_TILES];
static tile_gui_key_cb gui_key_cb[MAX_TILES];
static void* gui_userdata[MAX_TILES];

static int screen_w = 640; // pixels
static int screen_h = 480; // pixels

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
static void draw_decorations(tile_t* t, int is_focused) {
    if (t->type == TILE_EMPTY) return;
    // Titlebar
    int title_h = 16;
    int title_y = t->y;
    int title_color_r = is_focused ? 200 : 120;
    int title_color_g = is_focused ? 200 : 120;
    int title_color_b = is_focused ? 50 : 50;
    drawRect(t->x, title_y, t->width, title_h, title_color_r, title_color_g, title_color_b);
    // Title text - draw centered but clip / scroll if too long
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
    // Status bar
    int status_h = 0;
    extern int tui_alt_pressed;
    int show_status = (t->status_left != NULL) || (t->status_right != NULL) || (t->status_visible) || (tui_alt_pressed);
    if (show_status) {
        int status_y = t->y + title_h;
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
}

// Draw the dynamic/content area for a tile (invoked each frame or when content changes)
static void draw_tile_content(const tile_t* t) {
    if (t->type == TILE_EMPTY) return;
    int title_h = 16;
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
        for (int rr = 0; rr < max_lines; ++rr) {
            const char* line = vterm_get_tail_line(t->term_idx, rr, max_lines);
            if (!line) continue;
            for (int c = 0; c < TERM_COLS && line[c]; ++c) {
                int cr=200, cg=200, cb=200;
                vterm_get_tail_char_color(t->term_idx, rr, max_lines, c, &cr, &cg, &cb);
                drawCharAt(content_x + c * 8, content_y + rr * line_h, (int)(unsigned char)line[c], cr, cg, cb);
            }
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
            gui_draw_cb[old_idx] = NULL;
            gui_key_cb[old_idx] = NULL;
            gui_userdata[old_idx] = NULL;
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
    gui_draw_cb[tile_idx] = draw_cb;
    gui_key_cb[tile_idx] = key_cb;
    gui_userdata[tile_idx] = userdata;
}

void tile_unregister_gui_client(int tile_idx) {
    if (tile_idx < 0 || tile_idx >= MAX_TILES) return;
    // Determine the terminal slot associated with this tile and clear callbacks there
    int term = tiles[tile_idx].term_idx;
    if (term >= 0 && term < MAX_TILES) {
        gui_draw_cb[term] = NULL;
        gui_key_cb[term] = NULL;
        gui_userdata[term] = NULL;
    }

    // If this tile is a shell, restore its title/status to the default shell values
    if (tiles[tile_idx].type == TILE_SHELL) {
        tiles[tile_idx].title = "EYN-OS Shell";
        tiles[tile_idx].status_left = NULL;
        tiles[tile_idx].status_right = NULL;
    }
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

    // Initialize tiles
    tile_count = 1;
    focused = 0;
    for (int i = 0; i < MAX_TILES; ++i) {
        tiles[i].type = TILE_EMPTY;
        tiles[i].title = "";
        tiles[i].status_left = NULL;
        tiles[i].status_right = NULL;
        tiles[i].static_drawn = 0;
        tiles[i].term_idx = i;
        tiles[i].active = 0;
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
        // advance scroll tick each frame for marquee effects
        g_tile_scroll_tick++;
    // begin frame: reset dirty tracking for optimized blit
    vga_begin_frame();
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
            // Always redraw decorations on top of cached background so borders/title/status
            // pixels are present in the backbuffer for the partial blit.
            draw_decorations(&tiles[i], i == focused);
            draw_tile_content(&tiles[i]);
            // Mark only the UI decoration areas (titlebar, statusbar, and 1px borders)
            // so the blit stays small while ensuring decorations are kept in sync.
            int th = 16;
            extern int tui_alt_pressed;
            int sh = 0;
            int show_status = (tiles[i].status_left != NULL) || (tiles[i].status_right != NULL) || (tiles[i].status_visible) || (tui_alt_pressed);
            if (show_status) sh = 12;
            // titlebar
            vga_mark_dirty_rect(tiles[i].x, tiles[i].y, tiles[i].width, th);
            // statusbar (if shown)
            if (sh > 0) vga_mark_dirty_rect(tiles[i].x, tiles[i].y + th, tiles[i].width, sh);
            // 1px borders
            vga_mark_dirty_rect(tiles[i].x, tiles[i].y, tiles[i].width, 1); // top
            vga_mark_dirty_rect(tiles[i].x, tiles[i].y + tiles[i].height - 1, tiles[i].width, 1); // bottom
            vga_mark_dirty_rect(tiles[i].x, tiles[i].y, 1, tiles[i].height); // left
            vga_mark_dirty_rect(tiles[i].x + tiles[i].width - 1, tiles[i].y, 1, tiles[i].height); // right
            // Additionally, ensure the focused tile's content area is always blitted so
            // prompt/cursor updates and echoed characters don't cause flicker.
            if (i == focused) {
                int title_h = 16;
                int status_h = 0;
                if (show_status) status_h = sh;
                int cx = tiles[i].x + 1;
                int cy = tiles[i].y + title_h + status_h + 1;
                int cw = tiles[i].width - 2;
                int ch = tiles[i].height - (title_h + status_h) - 2;
                if (cw > 0 && ch > 0) vga_mark_dirty_rect(cx, cy, cw, ch);
            }
        }
        if (g_force_full_redraw) g_force_full_redraw = 0;
        tui_refresh();
        // swap backbuffer to framebuffer (if backbuffer in use)
        vga_swap_buffers();

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
        } else {
            vterm_handle_key(tiles[focused].term_idx, key & 0xFFFF);
        }
    }
}
