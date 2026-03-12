/*
 * Tile windows: layout algorithm, decoration and content drawing,
 * tile lifecycle (create_shell_tile, launch helpers, gui_window_closed),
 * and the background-wallpaper selection modal.
 *
*/

static void layout_tiles() {
    int th = vga_text_cell_h() + 6;  // taskbar height
    int maxw = screen_w - 60;
    int maxh = screen_h - th - 60;
    if (tile_count <= 0) return;
    
    // Instead of tiling, we do stacking positions
    for (int i = 0; i < tile_count; ++i) {
        // If not initialized yet or was reset
        if (tiles[i].width == 0 || tiles[i].height == 0 || tiles[i].width == screen_w) {
            int w = 640;
            int h = 400;
            if (w > maxw) w = maxw;
            if (h > maxh) h = maxh;
            int offset = (i * 30) % 200;
            tiles[i].x = 30 + offset;
            tiles[i].y = th + 30 + offset;
            tiles[i].width = w;
            tiles[i].height = h;
        }
    }
}
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

    /* --- Helper: compute tile button rects (close, max, min) --- */
    /* These mirror the layout used by wm_get_close/max/min_rect for floating windows. */
    int btn_side = 12;
    if (g_close_icon_loaded) {
        int iw = g_close_icon.header.width;
        int ih = g_close_icon.header.height;
        int m2 = (iw > ih) ? iw : ih;
        if (m2 > btn_side) btn_side = m2;
    }
    if (btn_side > title_h) btn_side = title_h;
    int btn_margin = 2;
    int btn_pad_y = (title_h - btn_side) / 2;
    int btn_gap   = 2;
    int close_bx = t->x + t->width - btn_side - btn_margin;
    int close_by = t->y + btn_pad_y;
    int max_bx   = close_bx - btn_side - btn_gap;
    int max_by   = close_by;
    int min_bx   = max_bx - btn_side - btn_gap;
    int min_by   = close_by;

    /* Reserve right side for buttons so title text doesn't overlap */
    int btn_zone = 3 * (btn_side + btn_gap) + btn_margin;
    // In low mode, draw simpler title/borders (status is an Alt-held overlay drawn later)
    if (g_gui_low_mode) {
        if (title_h > 0) {
            int title_y = t->y;
            int title_colour_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
            int title_colour_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
            int title_colour_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
            drawRect(t->x, title_y, t->width, title_h, title_colour_r, title_colour_g, title_colour_b);
            if (t->title && t->title[0]) {
                int title_len = (int)strlen(t->title);
                int avail_chars = (t->width - 8 - btn_zone) / cw;
                if (avail_chars < 0) avail_chars = 0;
                int text_y = title_y + (title_h - fh) / 2;
                int cr = is_focused ? 230 : 160, cg = is_focused ? 230 : 160, cb = is_focused ? 230 : 160;
                /* Left-aligned title */
                int draw_len = title_len;
                if (draw_len > avail_chars) draw_len = avail_chars;
                int start_x = t->x + 6;
                for (int i = 0; i < draw_len; ++i) {
                    int cx = start_x + i * cw;
                    if (cx < 0 || cx + cw > screen_w) break;
                    drawCharAt(cx, text_y, (int)(unsigned char)t->title[i], cr, cg, cb);
                }
            }
            /* Low-mode tile buttons: simple fallback glyphs */
            if (title_h >= 12) {
                /* Close: X */
                drawCharAt(close_bx + 2, close_by + 1, (unsigned char)'X', 255, 255, 255);
                /* Maximize: square outline */
                drawRect(max_bx + 3, max_by + 3, btn_side - 6, 1, 255, 255, 255);
                drawRect(max_bx + 3, max_by + btn_side - 4, btn_side - 6, 1, 255, 255, 255);
                drawRect(max_bx + 3, max_by + 3, 1, btn_side - 6, 255, 255, 255);
                drawRect(max_bx + btn_side - 4, max_by + 3, 1, btn_side - 6, 255, 255, 255);
                /* Minimize: horizontal line */
                drawRect(min_bx + 3, min_by + btn_side / 2, btn_side - 6, 1, 255, 255, 255);
            }
        }
        // status overlay is drawn after content
        int border_r = is_focused ? 80 : 50, border_g = is_focused ? 80 : 50, border_b = is_focused ? 80 : 50;
        drawRect(t->x, t->y, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y + t->height - 1, t->width, 1, border_r, border_g, border_b);
        drawRect(t->x, t->y, 1, t->height, border_r, border_g, border_b);
        drawRect(t->x + t->width - 1, t->y, 1, t->height, border_r, border_g, border_b);
        return;
    }
    if (title_h > 0) {
        int title_y = t->y;
        int title_colour_r = is_focused ? g_wm_theme.title_focused_r : g_wm_theme.title_unfocused_r;
        int title_colour_g = is_focused ? g_wm_theme.title_focused_g : g_wm_theme.title_unfocused_g;
        int title_colour_b = is_focused ? g_wm_theme.title_focused_b : g_wm_theme.title_unfocused_b;
        drawRect(t->x, title_y, t->width, title_h, title_colour_r, title_colour_g, title_colour_b);
        if (t->title && t->title[0]) {
            int title_len = (int)strlen(t->title);
            int avail_chars = (t->width - 8 - btn_zone) / cw;
            if (avail_chars < 0) avail_chars = 0;
            int text_y = title_y + (title_h - fh) / 2;
            int colour_r = is_focused ? 230 : 160;
            int colour_g = is_focused ? 230 : 160;
            int colour_b = is_focused ? 230 : 160;
            /* Left-aligned title (modern desktop style) */
            int draw_len = title_len;
            if (draw_len > avail_chars) draw_len = avail_chars;
            int start_x = t->x + 6;
            for (int i = 0; i < draw_len; ++i) {
                int cx = start_x + i * cw;
                if (cx < 0 || cx + cw > screen_w) break;
                drawCharAt(cx, text_y, (int)(unsigned char)t->title[i], colour_r, colour_g, colour_b);
            }
        }
        /* Tile title bar buttons (close, maximize, minimize) -- same icons as floating windows */
        if (title_h >= 12) {
            /* Close */
            if (g_close_icon_loaded && g_close_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_close_icon, g_close_icon_loaded, close_bx, close_by, btn_side, btn_side);
                } else {
                    if (g_close_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_close_icon_unf, g_close_icon_unf_loaded, close_bx, close_by, btn_side, btn_side);
                    else
                        wm_draw_icon_into_button(&g_close_icon, g_close_icon_loaded, close_bx, close_by, btn_side, btn_side);
                }
            } else {
                for (int fi = 0; fi < 6; ++fi) {
                    drawRect(close_bx + 3 + fi, close_by + 3 + fi, 1, 1, 255, 255, 255);
                    drawRect(close_bx + btn_side - 4 - fi, close_by + 3 + fi, 1, 1, 255, 255, 255);
                }
            }
            /* Maximize */
            if (g_max_icon_loaded && g_max_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_max_icon, g_max_icon_loaded, max_bx, max_by, btn_side, btn_side);
                } else {
                    if (g_max_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_max_icon_unf, g_max_icon_unf_loaded, max_bx, max_by, btn_side, btn_side);
                    else
                        wm_draw_icon_into_button(&g_max_icon, g_max_icon_loaded, max_bx, max_by, btn_side, btn_side);
                }
            } else {
                drawRect(max_bx + 3, max_by + 3, btn_side - 6, 1, 255, 255, 255);
                drawRect(max_bx + 3, max_by + btn_side - 4, btn_side - 6, 1, 255, 255, 255);
                drawRect(max_bx + 3, max_by + 3, 1, btn_side - 6, 255, 255, 255);
                drawRect(max_bx + btn_side - 4, max_by + 3, 1, btn_side - 6, 255, 255, 255);
            }
            /* Minimize */
            if (g_min_icon_loaded && g_min_icon.data) {
                if (is_focused) {
                    wm_draw_icon_into_button(&g_min_icon, g_min_icon_loaded, min_bx, min_by, btn_side, btn_side);
                } else {
                    if (g_min_icon_unf_loaded)
                        wm_draw_icon_into_button(&g_min_icon_unf, g_min_icon_unf_loaded, min_bx, min_by, btn_side, btn_side);
                    else
                        wm_draw_icon_into_button(&g_min_icon, g_min_icon_loaded, min_bx, min_by, btn_side, btn_side);
                }
            } else {
                drawRect(min_bx + 3, min_by + btn_side / 2, btn_side - 6, 1, 255, 255, 255);
            }
        }
    }
    // status overlay is drawn after content
    // borders -- subtle Materia gray
    int border_r = is_focused ? 80 : 50;
    int border_g = is_focused ? 80 : 50;
    int border_b = is_focused ? 80 : 50;
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
                        vterm_get_char_colour_abs(t->term_idx, abs_row, src_col, &rr, &gg, &bb);
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
                            // Choose shadow colour opposing the text brightness slightly for contrast
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

static int create_shell_tile_for_launch(void) {
    if (tile_count >= 4) return -1;

    int idx = tile_count++;
    tiles[idx].type = TILE_SHELL;
    tiles[idx].title = "EYN-OS Shell";
    tiles[idx].status_left = NULL;
    tiles[idx].status_right = NULL;
    tiles[idx].term_idx = idx;
    tiles[idx].active = 1;
    tiles[idx].static_drawn = 0;
    tiles[idx].minimized = 0;
    tiles[idx].maximized = 0;
    tiles[idx].prev_x = 0;
    tiles[idx].prev_y = 0;
    tiles[idx].prev_w = 0;
    tiles[idx].prev_h = 0;
    tiles[idx].desktop = g_current_desktop;
    vterm_clear(idx);
    vterm_set_active(idx, 1);
    vterm_print_prompt(idx);
    focused = idx;
    g_force_full_redraw = 1;
    layout_tiles();
    if (gui_draw_cb[tiles[idx].term_idx]) gui_needs_redraw[tiles[idx].term_idx] = 1;
    return idx;
}

static int find_launch_term(void) {
    if (tile_count <= 0) {
        int created = create_shell_tile_for_launch();
        return (created >= 0) ? tiles[created].term_idx : -1;
    }

    if (focused >= 0 && focused < tile_count) {
        int term = tiles[focused].term_idx;
        if (term >= 0 && term < MAX_TILES && gui_draw_cb[term] == NULL) return term;
    }

    for (int i = 0; i < tile_count; ++i) {
        int term = tiles[i].term_idx;
        if (term >= 0 && term < MAX_TILES && gui_draw_cb[term] == NULL) return term;
    }

    for (int i = 0; i < tile_count; ++i) {
        int term = tiles[i].term_idx;
        if (term >= 0 && term < MAX_TILES) return term;
    }

    if (tile_count < 4) {
        int created = create_shell_tile_for_launch();
        return (created >= 0) ? tiles[created].term_idx : -1;
    }
    return -1;
}

static void launch_shell_tile(void) {
    if (tile_count < 4) {
        (void)create_shell_tile_for_launch();
        return;
    }

    for (int i = 0; i < tile_count; ++i) {
        int term = tiles[i].term_idx;
        if (term >= 0 && term < MAX_TILES && gui_draw_cb[term] == NULL) {
            if (tiles[i].minimized) {
                tiles[i].minimized = 0;
                tiles[i].static_drawn = 0;
            }
            focused = i;
            g_force_full_redraw = 1;
            return;
        }
    }

    focused = 0;
    g_force_full_redraw = 1;
}

/*
 * Launch a program from the Programs submenu.
 *
 * All /binaries programs are userland UELFs. They write to stdout via
 * write(1,...) which goes through the vterm, so the command wrapper
 * (which captures kernel printf) cannot intercept their output.
 * Therefore we always type the command into the focused shell tile.
 *
 * GUI-aware programs (gui_attach/gui_create) will open their own
 * window automatically once the shell tile executes them.
 */
static void launch_program(const char* name) {
    if (g_user_task_active) return;

    int term = find_launch_term();
    if (term >= 0) {
        for (int ci = 0; name[ci]; ++ci)
            vterm_handle_key(term, (int)(unsigned char)name[ci]);
        vterm_handle_key(term, '\n');
    }
}

/*
 * Command wrapper UI was removed; program launch now routes through shell tiles.
 * Keep this hook for wm_close_window() callers.
 */
static void gui_window_closed(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    /* Clear stale callbacks to prevent zombie rendering / input routing */
    g_windows[win_id].draw_cb = NULL;
    g_windows[win_id].key_cb = NULL;
    g_windows[win_id].mouse_cb = NULL;
    g_windows[win_id].close_cb = NULL;
    g_windows[win_id].userdata = NULL;
    if (g_win_focused == win_id) g_win_focused = -1;
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

// Right-click context menu drawing
static void draw_ctx_menu(void) {
    if (!g_ctx_active) return;

    int cw = vga_text_cell_w();
    int ch = vga_text_cell_h();
    int item_h = ch + 6;
    int menu_w = 14 * cw;  /* wide enough for "Desktop 1" etc. */
    int menu_h = CTX_ITEM_COUNT * item_h + 4;

    /* Clamp to screen */
    int mx = g_ctx_x;
    int my = g_ctx_y;
    if (mx + menu_w > screen_w) mx = screen_w - menu_w;
    if (my + menu_h > screen_h) my = screen_h - menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    /* Background */
    drawRect(mx, my, menu_w, menu_h, 42, 42, 42);
    /* Top accent line */
    drawRect(mx, my, menu_w, 1, 90, 90, 90);

    const char* items[CTX_ITEM_COUNT] = {"Desktop 1", "Desktop 2", "Kill"};
    int fg = 230;

    int dy = my + 2;
    for (int i = 0; i < CTX_ITEM_COUNT; ++i) {
        if (i == g_ctx_hover)
            drawRect(mx, dy, menu_w, item_h, 60, 60, 60);

        /* Checkmark for current desktop assignment */
        if (i < g_desktop_count) {
            int assigned_desktop = -1;
            if (g_ctx_kind == 0 && g_ctx_index >= 0 && g_ctx_index < tile_count)
                assigned_desktop = tiles[g_ctx_index].desktop;
            else if (g_ctx_kind == 1 && g_ctx_index >= 0 && g_ctx_index < MAX_WINDOWS && g_windows[g_ctx_index].used)
                assigned_desktop = g_windows[g_ctx_index].desktop;
            if (assigned_desktop == i)
                drawCharAt(mx + 4, dy + (item_h - ch) / 2, (unsigned char)'>', fg, fg, fg);
        }

        int len = (int)strlen(items[i]);
        for (int j = 0; j < len; ++j)
            drawCharAt(mx + 14 + j * cw, dy + (item_h - ch) / 2,
                       (unsigned char)items[i][j], fg, fg, fg);
        dy += item_h;
    }

    vga_mark_dirty_rect(mx, my, menu_w, menu_h);
}

