/*
 * Floating window manager: chrome drawing, button-rect helpers, hit-testing,
 * drag/resize state and execution, public wm_* API functions, cursor image
 * loading and save-under rendering, and titlebar-icon asset loading.
*/

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
        int title_colour_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
        int title_colour_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
        int title_colour_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
        drawRect(w->x, w->y, w->w, th, title_colour_r, title_colour_g, title_colour_b);
        if (w->title && w->title[0]) {
            int len = (int)strlen(w->title);
            int max_chars = (w->w - 12) / cw; if (max_chars < 0) max_chars = 0;
            int colour_r = is_focused ? 230 : 160, colour_g = is_focused ? 230 : 160, colour_b = is_focused ? 230 : 160;
            int text_y = w->y + (th - fh) / 2;
            if (len > max_chars) len = max_chars;
            int start_x = w->x + 6;
            for (int i = 0; i < len; ++i) drawCharAt(start_x + i * cw, text_y, (unsigned char)w->title[i], colour_r, colour_g, colour_b);
        }
        (void)sh;
        // border -- Materia gray
        int br = is_focused ? 80 : 56, bg2 = is_focused ? 80 : 56, bb = is_focused ? 80 : 56;
        drawRect(w->x, w->y, w->w, 1, br, bg2, bb);
        drawRect(w->x, w->y + w->h - 1, w->w, 1, br, bg2, bb);
        drawRect(w->x, w->y, 1, w->h, br, bg2, bb);
        drawRect(w->x + w->w - 1, w->y, 1, w->h, br, bg2, bb);
        return;
    }
    int th = win_title_height(w);
    int sh = 0;
    // frame background
    drawRect(w->x, w->y, w->w, w->h, 0, 0, 0);
    // title bar
    int title_colour_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
    int title_colour_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
    int title_colour_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
    drawRect(w->x, w->y, w->w, th, title_colour_r, title_colour_g, title_colour_b);
    if (w->title && w->title[0]) {
        int len = (int)strlen(w->title);
        // Reserve space for the three titlebar buttons on the right
        int btn_zone = 3 * 14 + 8; // approximate width for min+max+close
        int avail_chars = (w->w - 8 - btn_zone) / cw;
        if (avail_chars < 0) avail_chars = 0;
        int text_y = w->y + (th - fh) / 2;
        int colour_r = is_focused ? 230 : 160;
        int colour_g = is_focused ? 230 : 160;
        int colour_b = is_focused ? 230 : 160;
        // Left-aligned title (with small left padding)
        int draw_len = len;
        if (draw_len > avail_chars) draw_len = avail_chars;
        int start_x = w->x + 6;
        for (int i = 0; i < draw_len; ++i)
            drawCharAt(start_x + i * cw, text_y, (unsigned char)w->title[i], colour_r, colour_g, colour_b);
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
    // border -- subtle gray, slightly brighter when focused
    int br = is_focused ? 80 : 56, wbg = is_focused ? 80 : 56, bb = is_focused ? 80 : 56;
    drawRect(w->x, w->y, w->w, 1, br, wbg, bb);
    drawRect(w->x, w->y + w->h - 1, w->w, 1, br, wbg, bb);
    drawRect(w->x, w->y, 1, w->h, br, wbg, bb);
    drawRect(w->x + w->w - 1, w->y, 1, w->h, br, wbg, bb);
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
    // Determine colour key for non-alpha formats by sampling corners
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
            if (w->draw_cb) w->draw_cb((int)(w - g_windows), cx, cy, cw, ch, w->userdata);
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
        if (g_windows[wi].minimized) continue;
        if (g_windows[wi].desktop != g_current_desktop) continue;
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

/*
 * tile_border_resize_hit_test -- hit-test the edge/corner grab zones of a tile
 * window, mirroring wm_resize_hit_test_edges() for floating windows.
 *
 * Returns a bitmask of RESIZE_EDGE_* flags, or 0 if the point is not inside
 * any grab zone.  Only fires for non-maximized, non-minimized tiles; maximized
 * tiles use the maximize-toggle button rather than border drag.
 *
 * Grab-zone geometry:
 *   m      = 4 px band along each edge (single-axis resize)
 *   corner = 10 px square at each corner (diagonal resize, larger target)
 */
static int tile_border_resize_hit_test(const tile_t* t, int x, int y) {
    if (!t || t->type == TILE_EMPTY || t->minimized || t->maximized) return 0;
    const int m = 4;
    const int corner = 10;
    if (!point_in_rect(x, y, t->x, t->y, t->width, t->height)) return 0;
    /* Prefer diagonal corners -- they give a larger, easier grab target. */
    if (x < t->x + corner && y < t->y + corner) return RESIZE_EDGE_L | RESIZE_EDGE_T;
    if (x >= t->x + t->width - corner && y < t->y + corner) return RESIZE_EDGE_R | RESIZE_EDGE_T;
    if (x < t->x + corner && y >= t->y + t->height - corner) return RESIZE_EDGE_L | RESIZE_EDGE_B;
    if (x >= t->x + t->width - corner && y >= t->y + t->height - corner) return RESIZE_EDGE_R | RESIZE_EDGE_B;
    /* Single-axis edge strips */
    int edges = 0;
    if (x < t->x + m)               edges |= RESIZE_EDGE_L;
    if (x >= t->x + t->width - m)   edges |= RESIZE_EDGE_R;
    if (y < t->y + m)               edges |= RESIZE_EDGE_T;
    if (y >= t->y + t->height - m)  edges |= RESIZE_EDGE_B;
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

/*
 * Tile titlebar drag: move an individual tile window by dragging its title bar.
 *
 * Invariant: tile.x/y are updated directly; layout_tiles() will not clobber
 * them because it guards on (width==0 || height==0 || width==screen_w).
 * Dragging is disallowed on maximized tiles (they use restore/unmaximize instead).
 */
static int tile_drag_active = 0;
static int tile_drag_idx = -1;
static int tile_drag_off_x = 0, tile_drag_off_y = 0;

/*
 * Tile border resize: resize an individual tile window by dragging an edge or
 * corner grab zone.
 *
 * Invariant: operates on tile.width/height, which layout_tiles() leaves alone
 * once they are non-zero and not equal to screen_w.  Disallowed on maximized
 * tiles (use the maximize toggle instead).
 */
static int tile_border_resize_active = 0;
static int tile_border_resize_idx = -1;
static int tile_border_resize_edges = 0;
static int tile_border_resize_start_mx = 0, tile_border_resize_start_my = 0;
static int tile_border_resize_start_x = 0, tile_border_resize_start_y = 0;
static int tile_border_resize_start_w = 0, tile_border_resize_start_h = 0;

// Public APIs
int wm_create_window(const char* title, int x, int y, int w, int h, const char* status_left) {
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) {
            g_windows[i].used = 1;
            int tb_h_clamp = vga_text_cell_h() + 6;
            g_windows[i].x = clampi(x, 0, screen_w - 32);
            g_windows[i].y = clampi(y, tb_h_clamp, screen_h - 32);
            g_windows[i].w = clampi(w, 64, screen_w);
            g_windows[i].h = clampi(h, 48, screen_h);
            g_windows[i].title = title ? title : "";
            g_windows[i].status_left = status_left;
            g_windows[i].status_right = NULL;
            g_windows[i].draw_cb = NULL;
            g_windows[i].key_cb = NULL;
            g_windows[i].mouse_cb = NULL;
            g_windows[i].close_cb = NULL;
            g_windows[i].userdata = NULL;
            g_windows[i].needs_redraw = 1;
            g_windows[i].continuous_redraw = 0;
            g_windows[i].static_drawn = 0;
            g_windows[i].last_focused = 0;
            g_windows[i].minimized = 0;
            g_windows[i].maximized = 0;
            g_windows[i].prev_x = g_windows[i].x; g_windows[i].prev_y = g_windows[i].y;
            g_windows[i].prev_w = g_windows[i].w; g_windows[i].prev_h = g_windows[i].h;
            g_windows[i].desktop = g_current_desktop;
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
    g_windows[win_id].close_cb = NULL;
    g_windows[win_id].userdata = NULL;
    g_windows[win_id].needs_redraw = 0;
    g_windows[win_id].continuous_redraw = 0;
}

/*
 * Register an optional close-request callback for a floating window.
 * Semantics mirror tile_register_gui_close_cb: if the callback returns 0,
 * the close is vetoed (the user program is expected to call exit() when ready).
 * If the callback returns non-zero (or is NULL), the window closes immediately.
 */
void wm_register_gui_close_cb(int win_id, tile_gui_close_cb close_cb) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].close_cb = close_cb;
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

/*
 * Returns the content area (inside decorations) for a floating window.
 * Matches the layout computed in wm_draw_content: the title bar consumes
 * win_title_height(w) pixels at the top; an optional status overlay may
 * consume status_bar_height_px() pixels immediately below; a 1-pixel border
 * surrounds the remaining area on all four sides.
 *
 * ABI-INVARIANT: The content rect is kernel-internal; the same geometry is
 * used by draw_cb, mouse coordinate translation, and GET_CONTENT_SIZE.
 */
void wm_get_content_rect(int win_id, int* cx, int* cy, int* cw, int* ch) {
    if (cx) *cx = 0;
    if (cy) *cy = 0;
    if (cw) *cw = 0;
    if (ch) *ch = 0;
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    const window_t* w = &g_windows[win_id];
    int th = win_title_height(w);
    int sh = status_overlay_visible() ? status_bar_height_px() : 0;
    if (cx) *cx = w->x + 1;
    if (cy) *cy = w->y + th + sh + 1;
    if (cw) *cw = w->w - 2;
    if (ch) *ch = w->h - (th + sh) - 2;
}

void wm_set_continuous_redraw(int win_id, int enabled) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    g_windows[win_id].continuous_redraw = enabled ? 1 : 0;
    if (g_windows[win_id].continuous_redraw) g_windows[win_id].needs_redraw = 1;
}

/* Forward declaration: clean up GUI windows on close */
static void gui_window_closed(int win_id);

void wm_close_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    /*
     * If a close callback is registered, give it a chance to veto the close.
     * Zero return from the callback means "veto" -- the user program is expected
     * to eventually call exit() which will re-invoke wm_close_window().
     * Clear close_cb before invoking it to prevent infinite recursion on re-close.
     */
    if (g_windows[win_id].close_cb) {
        tile_gui_close_cb cb = g_windows[win_id].close_cb;
        void* ud = g_windows[win_id].userdata;
        g_windows[win_id].close_cb = NULL;
        if (cb(win_id, ud) == 0) return; /* vetoed */
    }
    // remove from order
    int pos = -1;
    for (int i = 0; i < g_window_count; ++i) if (g_window_order[i] == win_id) { pos = i; break; }
    if (pos >= 0) {
        for (int i = pos; i < g_window_count - 1; ++i) g_window_order[i] = g_window_order[i + 1];
        g_window_count--;
    }
    g_windows[win_id].used = 0;
    if (g_win_focused == win_id) g_win_focused = -1;
    gui_window_closed(win_id);
    g_force_full_redraw = 1;
    g_tiles_full_content_redraw = 1;
}

void wm_force_close_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !g_windows[win_id].used) return;
    // Forceful close path: strip callbacks (including close veto) before close.
    wm_unregister_gui_client(win_id);
    wm_close_window(win_id);
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
    // - RGB/MONO: if the four corner pixels are the same value/colour, treat that as a colour key
    //   and skip any pixels matching it. This removes solid background boxes on assets lacking alpha.
    const uint8_t* data = cim->data;
    int depth = cim->header.depth;
    int sw = cim->header.width;
    int sh = cim->header.height;
    int stride = sw * depth;
    // Determine colour key for non-alpha formats by sampling corners
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
        if (t->minimized) return -1;
        if (t->desktop != g_current_desktop) return -1;
        if (x >= t->x && x < t->x + t->width && y >= t->y && y < t->y + t->height) return fullscreen_tile;
        return -1;
    }
    for (int i = tile_count - 1; i >= 0; --i) {
        tile_t* t = &tiles[i];
        if (t->type == TILE_EMPTY) continue;
        if (t->minimized) continue;
        if (t->desktop != g_current_desktop) continue;
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

static void load_cursor_variant_try_paths(uint8 disk, const char* name, rei_image_t* out_img, int* out_loaded) {
    if (out_loaded) *out_loaded = 0;
    if (!name || !out_img || !out_loaded) return;
    const char* paths[] = {
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
    const char* paths16[] = { "/ui16/close.rei", "/testdir/ui16/close.rei", NULL };
    const char* paths8[] = { "/close.rei", "/ui/close.rei", "/testdir/close.rei", "/testdir/ui/close.rei", NULL };
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
    const char* paths16[] = { "/ui16/min.rei", "/testdir/ui16/min.rei", "/ui16/minimize.rei", "/testdir/ui16/minimize.rei", NULL };
    const char* paths8[] = { "/min.rei", "/ui/min.rei", "/minimize.rei", "/ui/minimize.rei", "/testdir/min.rei", "/testdir/ui/min.rei", NULL };
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
    const char* paths16[] = { "/ui16/max.rei", "/testdir/ui16/max.rei", "/ui16/maximize.rei", "/testdir/ui16/maximize.rei", NULL };
    const char* paths8[] = { "/max.rei", "/ui/max.rei", "/maximize.rei", "/ui/maximize.rei", "/testdir/max.rei", "/testdir/ui/max.rei", NULL };
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
    const char* paths16[] = { "/ui16/close_unfocused.rei", "/testdir/ui16/close_unfocused.rei", NULL };
    const char* paths8[] = { "/close_unfocused.rei", "/ui/close_unfocused.rei", "/testdir/close_unfocused.rei", "/testdir/ui/close_unfocused.rei", NULL };
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
    const char* paths16[] = { "/ui16/min_unfocused.rei", "/testdir/ui16/min_unfocused.rei", "/ui16/minimize_unfocused.rei", "/testdir/ui16/minimize_unfocused.rei", NULL };
    const char* paths8[] = { "/min_unfocused.rei", "/ui/min_unfocused.rei", "/minimize_unfocused.rei", "/ui/minimize_unfocused.rei", "/testdir/min_unfocused.rei", "/testdir/ui/min_unfocused.rei", NULL };
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
    const char* paths16[] = { "/ui16/max_unfocused.rei", "/testdir/ui16/max_unfocused.rei", "/ui16/maximize_unfocused.rei", "/testdir/ui16/maximize_unfocused.rei", NULL };
    const char* paths8[] = { "/max_unfocused.rei", "/ui/max_unfocused.rei", "/maximize_unfocused.rei", "/ui/maximize_unfocused.rei", "/testdir/max_unfocused.rei", "/testdir/ui/max_unfocused.rei", NULL };
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

