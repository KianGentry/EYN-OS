/*
 * Input pump: tile public registration/unregistration API,
 * tile_render_once() for use while ring-3 tasks are running,
 * and tile_pump_input_once() for background mouse/key dispatch.
*/

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
        tiles[i].minimized = 0; tiles[i].maximized = 0;
        tiles[i].prev_x = 0; tiles[i].prev_y = 0; tiles[i].prev_w = 0; tiles[i].prev_h = 0;
    }
    // Reassign term_idx for compacted tiles. Do NOT clear vterm buffers here - keep terminal content intact.
    for (int i = 0; i < n; ++i) {
        int old_idx = tiles[i].term_idx;
        tiles[i].term_idx = i;
        // if the term index changed, update vterm active mapping
        if (old_idx != i) {
            // Move terminal state with the tile so scrollback/input/history remain intact.
            vterm_move_state(i, old_idx);
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
    if (n <= 0) {
        tile_count = 0;
        focused = 0;
        return;
    }

    tile_count = n;
    tile_ensure_valid_focus();
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
    tiles[idx].minimized = 0;
    tiles[idx].maximized = 0;
    tiles[idx].prev_x = 0;
    tiles[idx].prev_y = 0;
    tiles[idx].prev_w = 0;
    tiles[idx].prev_h = 0;
    tiles[idx].desktop = g_current_desktop;
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
    g_tiles_full_content_redraw = 1;
    tiles[tile_idx].static_drawn = 0;
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
    static volatile int g_in_render_once = 0;
    if (g_in_render_once) return;
    g_in_render_once = 1;
    if (!g_tm_initialized) { g_in_render_once = 0; return; }

    // Best-effort: if framebuffer dimensions are unknown, avoid drawing.
    if (screen_w <= 0 || screen_h <= 0) { g_in_render_once = 0; return; }

    // Advance effects that are otherwise driven per-frame.
    g_tile_scroll_tick++;

    // Ensure UI/icon assets match current font metrics (8x8 vs 16x16 icon sets).
    maybe_update_icon_mode(0);

    vga_begin_frame();

    // While the main WM loop is paused (ring3 task active), render a full
    // compositor frame so taskbar, menus, windows, and tile content remain in sync.
    g_force_full_redraw = 1;
    g_tiles_full_content_redraw = 1;

    // Paint desktop background first so exposed regions never leave trails.
    drawRect(0, 0, screen_w, screen_h, 38, 38, 38);
    vga_mark_dirty_rect(0, 0, screen_w, screen_h);

    // Draw tiles.
    for (int i = 0; i < tile_count; ++i) {
        if (tiles[i].type == TILE_EMPTY || tiles[i].minimized) continue;
        if (tiles[i].desktop != g_current_desktop) continue;
        if (g_force_full_redraw) tiles[i].static_drawn = 0;
        if (!tiles[i].static_drawn) {
            draw_static_tile(&tiles[i], i == focused);
        }
        draw_tile_content(&tiles[i]);
        if (status_overlay_visible()) draw_tile_status_overlay(&tiles[i]);
        tiles[i].last_drawn_version = vterm_get_version(tiles[i].term_idx);
    }

    // Draw floating windows on top of tiles.
    if (g_window_count > 0) {
        for (int oi = 0; oi < g_window_count; ++oi) {
            int wi = g_window_order[oi];
            if (wi < 0 || wi >= MAX_WINDOWS) continue;
            window_t* w = &g_windows[wi];
            if (!w->used || w->minimized) continue;
            if (w->desktop != g_current_desktop) continue;

            int is_focused_win = (wi == g_win_focused);
            if (!w->static_drawn || w->last_focused != is_focused_win || g_force_full_redraw) {
                wm_draw_decor(w, is_focused_win);
                wm_mark_decor_dirty(w);
                w->static_drawn = 1;
                w->last_focused = is_focused_win;
            }

            if (w->needs_redraw || w->continuous_redraw || g_force_full_redraw) {
                wm_draw_content(w);
                if (!w->continuous_redraw) w->needs_redraw = 0;
            }

            if (status_overlay_visible()) {
                wm_draw_status_overlay(w);
            }
        }
    }

    if (g_bg_modal.active) draw_bg_modal();
    if (g_ctx_active) draw_ctx_menu();
    draw_taskbar();

    if (g_force_full_redraw) g_force_full_redraw = 0;
    if (g_tiles_full_content_redraw) g_tiles_full_content_redraw = 0;

    // Ensure the swap copies something even if dirty tracking is disabled.
    vga_mark_dirty_rect(0, 0, screen_w, screen_h);

    tui_refresh();
    // Force an immediate present. Prefer a direct backbuffer->fb blit so we
    // don’t depend on dirty-rect bookkeeping in this one-shot path.
    if (vga_get_vsync_enabled()) vga_wait_vblank();
    vga_blit_backbuffer_region_to_fb(0, 0, screen_w, screen_h);

    // Draw mouse cursor overlay so user-task GUI apps have a visible cursor.
    if (!g_cursor_hidden) {
        int cur_mx = -1000, cur_my = -1000;
        mouse_get_position(&cur_mx, &cur_my);
        if (cur_mx > -100 && cur_my > -100) {
            int draw_x = cur_mx, draw_y = cur_my;
            cursor_get_draw_pos_for_kind(cur_mx, cur_my, &draw_x, &draw_y);
            draw_cursor_overlay(draw_x, draw_y);
        }
    }

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
            uint8 changes = me.button_changes;
            int left_press = (changes & MOUSE_BUTTON_LEFT) && (me.buttons & MOUSE_BUTTON_LEFT);
            int right_press = (changes & MOUSE_BUTTON_RIGHT) && (me.buttons & MOUSE_BUTTON_RIGHT);

            int task_tile = focused;
            if (g_user_task_term >= 0) {
                int tt = tile_find_by_term(g_user_task_term);
                if (tt >= 0 && tt < tile_count) task_tile = tt;
            }

            /* While ring3 is active, emulate WM close affordances for the running app:
             * - titlebar close button click
             * - right-click taskbar button (kill shortcut)
             */
            if (task_tile >= 0 && task_tile < tile_count) {
                if (left_press) {
                    int t_th = get_title_height(&tiles[task_tile]);
                    if (t_th >= 12 && me.y >= tiles[task_tile].y && me.y < tiles[task_tile].y + t_th) {
                        /* Button layout matches draw_decorations exactly */
                        int t_btn_side = 12;
                        if (g_close_icon_loaded) {
                            int iw2 = g_close_icon.header.width;
                            int ih2 = g_close_icon.header.height;
                            int m2 = (iw2 > ih2) ? iw2 : ih2;
                            if (m2 > t_btn_side) t_btn_side = m2;
                        }
                        if (t_btn_side > t_th) t_btn_side = t_th;
                        int t_btn_margin = 2;
                        int t_btn_gap    = 2;
                        int t_btn_pad_y = (t_th - t_btn_side) / 2;
                        int t_close_bx = tiles[task_tile].x + tiles[task_tile].width - t_btn_side - t_btn_margin;
                        int t_close_by = tiles[task_tile].y + t_btn_pad_y;
                        int t_max_bx   = t_close_bx - t_btn_side - t_btn_gap;
                        int t_max_by   = t_close_by;
                        int t_min_bx   = t_max_bx - t_btn_side - t_btn_gap;
                        int t_min_by   = t_close_by;

                        /* Close button: send close event or interrupt */
                        if (point_in_rect(me.x, me.y, t_close_bx, t_close_by, t_btn_side, t_btn_side)) {
                            /* Invoke close callback if registered; only force-kill if allowed */
                            int term_c = tiles[task_tile].term_idx;
                            if (term_c >= 0 && term_c < MAX_TILES && gui_close_cb[term_c]) {
                                int allow = gui_close_cb[term_c](task_tile, gui_userdata[term_c]);
                                if (allow) g_user_interrupt = 1;
                                /* If vetoed (allow==0), the callback pushed a CLOSE event to the app */
                            } else {
                                g_user_interrupt = 1;
                            }
                            return 1;
                        }
                        /* Maximize button: toggle tile between full-screen and natural size */
                        if (point_in_rect(me.x, me.y, t_max_bx, t_max_by, t_btn_side, t_btn_side)) {
                            fullscreen_tile = -1;
                            int taskbar_h_tile = vga_text_cell_h() + 6;
                            if (taskbar_h_tile < 0) taskbar_h_tile = 0;
                            if (taskbar_h_tile > screen_h - 32) taskbar_h_tile = screen_h - 32;
                            if (!tiles[task_tile].maximized) {
                                tiles[task_tile].prev_x = tiles[task_tile].x;
                                tiles[task_tile].prev_y = tiles[task_tile].y;
                                tiles[task_tile].prev_w = tiles[task_tile].width;
                                tiles[task_tile].prev_h = tiles[task_tile].height;
                                tiles[task_tile].x = 0;
                                tiles[task_tile].y = taskbar_h_tile;
                                tiles[task_tile].width  = screen_w;
                                tiles[task_tile].height = screen_h - taskbar_h_tile;
                                tiles[task_tile].maximized = 1;
                                tiles[task_tile].minimized = 0;
                            } else {
                                if (tiles[task_tile].prev_w > 0 && tiles[task_tile].prev_h > 0) {
                                    tiles[task_tile].x      = tiles[task_tile].prev_x;
                                    tiles[task_tile].y      = tiles[task_tile].prev_y;
                                    tiles[task_tile].width  = tiles[task_tile].prev_w;
                                    tiles[task_tile].height = tiles[task_tile].prev_h;
                                }
                                tiles[task_tile].maximized = 0;
                            }
                            tiles[task_tile].static_drawn = 0;
                            focused = task_tile;
                            g_force_full_redraw = 1;
                            return 1;
                        }
                        /* Minimize button: hide tile (collapses to taskbar entry) */
                        if (point_in_rect(me.x, me.y, t_min_bx, t_min_by, t_btn_side, t_btn_side)) {
                            fullscreen_tile = -1;
                            tiles[task_tile].minimized = 1;
                            tiles[task_tile].maximized = 0;
                            tiles[task_tile].static_drawn = 0;
                            tile_ensure_valid_focus();
                            g_force_full_redraw = 1;
                            return 1;
                        }
                    }
                }

                if (right_press) {
                    int taskbar_h_r = vga_text_cell_h() + 6;
                    if (me.y < taskbar_h_r) {
                        for (int bi = 0; bi < g_tb_button_count; ++bi) {
                            if (me.x >= g_tb_buttons[bi].x && me.x < g_tb_buttons[bi].x + g_tb_buttons[bi].w) {
                                if (g_tb_buttons[bi].kind == 0 && g_tb_buttons[bi].index == task_tile) {
                                    g_user_interrupt = 1;
                                    return 1;
                                }
                            }
                        }
                    }
                }
            }

            int left_down = (me.buttons & MOUSE_BUTTON_LEFT) ? 1 : 0;
            int left_release = (changes & MOUSE_BUTTON_LEFT) && !left_down;

            if (left_release) {
                drag_active = 0; drag_win = -1;
                resize_active = 0; resize_win = -1; resize_edges = 0;
                tile_drag_active = 0; tile_drag_idx = -1;
                tile_border_resize_active = 0; tile_border_resize_idx = -1; tile_border_resize_edges = 0;
            }

            /*
             * Update cursor style on every mouse event while ring3 is active.
             *
             * The main tiler loop (which normally drives cursor changes) is paused whenever
             * a user task is running.  This block mirrors the cursor-selection logic in the
             * main loop so that resize/drag feedback reaches the user during app execution.
             *
             * Priority order mirrors the main loop:
             *   1. Active border-resize: show the matching resize cursor.
             *   2. Active titlebar-drag: keep the normal cursor (no special drag cursor).
             *   3. Hover over a window border: show the matching resize cursor.
             *   4. Otherwise: restore the normal cursor.
             */
            {
                cursor_set_style(CURSOR_NORMAL, 0);
                if (resize_active) {
                    if ((resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                        int xform = 0;
                        if ((resize_edges & RESIZE_EDGE_L) && (resize_edges & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                        if ((resize_edges & RESIZE_EDGE_R) && (resize_edges & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                        cursor_set_style(CURSOR_RES_DIAG, xform);
                    } else if (resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B)) {
                        cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                    } else if (resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) {
                        cursor_set_style(CURSOR_RES_HOR, 0);
                    }
                } else if (tile_border_resize_active) {
                    if ((tile_border_resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (tile_border_resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                        int xform = 0;
                        if ((tile_border_resize_edges & RESIZE_EDGE_L) && (tile_border_resize_edges & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                        if ((tile_border_resize_edges & RESIZE_EDGE_R) && (tile_border_resize_edges & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                        cursor_set_style(CURSOR_RES_DIAG, xform);
                    } else if (tile_border_resize_edges & (RESIZE_EDGE_T | RESIZE_EDGE_B)) {
                        cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                    } else if (tile_border_resize_edges & (RESIZE_EDGE_L | RESIZE_EDGE_R)) {
                        cursor_set_style(CURSOR_RES_HOR, 0);
                    }
                } else if (!drag_active && !tile_drag_active) {
                    /* Hover: check floating window borders then tile borders */
                    int w_hit = wm_hit_test(me.x, me.y);
                    if (w_hit >= 0) {
                        int e = wm_resize_hit_test_edges(&g_windows[w_hit], me.x, me.y);
                        if (e) {
                            if ((e & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (e & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                                int xform = 0;
                                if ((e & RESIZE_EDGE_L) && (e & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                                if ((e & RESIZE_EDGE_R) && (e & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                                cursor_set_style(CURSOR_RES_DIAG, xform);
                            } else if (e & (RESIZE_EDGE_T | RESIZE_EDGE_B)) {
                                cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                            } else if (e & (RESIZE_EDGE_L | RESIZE_EDGE_R)) {
                                cursor_set_style(CURSOR_RES_HOR, 0);
                            }
                        }
                    } else {
                        /* No floating window hit: check individual tile borders */
                        int t_hit_c = tile_index_at(me.x, me.y);
                        if (t_hit_c >= 0 && t_hit_c < tile_count) {
                            int te = tile_border_resize_hit_test(&tiles[t_hit_c], me.x, me.y);
                            if (te) {
                                if ((te & (RESIZE_EDGE_L | RESIZE_EDGE_R)) && (te & (RESIZE_EDGE_T | RESIZE_EDGE_B))) {
                                    int xform = 0;
                                    if ((te & RESIZE_EDGE_L) && (te & RESIZE_EDGE_T)) xform |= CURSOR_XFORM_FLIP_X;
                                    if ((te & RESIZE_EDGE_R) && (te & RESIZE_EDGE_B)) xform |= CURSOR_XFORM_FLIP_X;
                                    cursor_set_style(CURSOR_RES_DIAG, xform);
                                } else if (te & (RESIZE_EDGE_T | RESIZE_EDGE_B)) {
                                    cursor_set_style(CURSOR_RES_VERT, CURSOR_XFORM_ROT_90);
                                } else if (te & (RESIZE_EDGE_L | RESIZE_EDGE_R)) {
                                    cursor_set_style(CURSOR_RES_HOR, 0);
                                }
                            }
                        }
                    }
                }
            }

            if (resize_active && resize_win >= 0 && g_windows[resize_win].used) {
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

                if (new_w < 64) {
                    if (resize_edges & RESIZE_EDGE_L) new_x -= (64 - new_w);
                    new_w = 64;
                }
                if (new_h < 48) {
                    if (resize_edges & RESIZE_EDGE_T) new_y -= (48 - new_h);
                    new_h = 48;
                }
                if (new_x < 0) { if (resize_edges & RESIZE_EDGE_L) new_w += new_x; new_x = 0; }
                if (new_y < 0) { if (resize_edges & RESIZE_EDGE_T) new_h += new_y; new_y = 0; }
                if (new_x + new_w > screen_w) new_w = screen_w - new_x;
                if (new_y + new_h > screen_h) new_h = screen_h - new_y;
                if (new_w < 64) new_w = 64;
                if (new_h < 48) new_h = 48;

                w->x = new_x;
                w->y = new_y;
                w->w = new_w;
                w->h = new_h;
                w->static_drawn = 0;
                w->needs_redraw = 1;
                g_tiles_full_content_redraw = 1;
                return 1;
            }

            if (drag_active && drag_win >= 0 && g_windows[drag_win].used) {
                window_t* w = &g_windows[drag_win];
                int taskbar_h_drag = vga_text_cell_h() + 6;
                if (taskbar_h_drag < 0) taskbar_h_drag = 0;
                w->x = clampi(me.x - drag_off_x, 0, screen_w - w->w);
                w->y = clampi(me.y - drag_off_y, taskbar_h_drag, screen_h - w->h);
                w->static_drawn = 0;
                w->needs_redraw = 1;
                g_tiles_full_content_redraw = 1;
                return 1;
            }

            if (tile_border_resize_active && tile_border_resize_idx >= 0 && tile_border_resize_idx < tile_count) {
                tile_t* t = &tiles[tile_border_resize_idx];
                int dx = me.x - tile_border_resize_start_mx;
                int dy = me.y - tile_border_resize_start_my;
                int new_x = tile_border_resize_start_x;
                int new_y = tile_border_resize_start_y;
                int new_w = tile_border_resize_start_w;
                int new_h = tile_border_resize_start_h;
                if (tile_border_resize_edges & RESIZE_EDGE_L) { new_x = tile_border_resize_start_x + dx; new_w = tile_border_resize_start_w - dx; }
                if (tile_border_resize_edges & RESIZE_EDGE_R) { new_w = tile_border_resize_start_w + dx; }
                if (tile_border_resize_edges & RESIZE_EDGE_T) { new_y = tile_border_resize_start_y + dy; new_h = tile_border_resize_start_h - dy; }
                if (tile_border_resize_edges & RESIZE_EDGE_B) { new_h = tile_border_resize_start_h + dy; }
                const int t_minw2 = 200, t_minh2 = 120;
                if (new_w < t_minw2) { if (tile_border_resize_edges & RESIZE_EDGE_L) new_x -= (t_minw2 - new_w); new_w = t_minw2; }
                if (new_h < t_minh2) { if (tile_border_resize_edges & RESIZE_EDGE_T) new_y -= (t_minh2 - new_h); new_h = t_minh2; }
                int tbh_p = vga_text_cell_h() + 6;
                if (new_x < 0) { if (tile_border_resize_edges & RESIZE_EDGE_L) new_w += new_x; new_x = 0; }
                if (new_y < tbh_p) { if (tile_border_resize_edges & RESIZE_EDGE_T) new_h += (new_y - tbh_p); new_y = tbh_p; }
                if (new_x + new_w > screen_w) new_w = screen_w - new_x;
                if (new_y + new_h > screen_h) new_h = screen_h - new_y;
                if (new_w < t_minw2) new_w = t_minw2;
                if (new_h < t_minh2) new_h = t_minh2;
                t->x = new_x; t->y = new_y; t->width = new_w; t->height = new_h;
                t->static_drawn = 0;
                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
                return 1;
            }

            if (tile_drag_active && tile_drag_idx >= 0 && tile_drag_idx < tile_count) {
                tile_t* t = &tiles[tile_drag_idx];
                int tbh_tdp = vga_text_cell_h() + 6;
                t->x = clampi(me.x - tile_drag_off_x, 0, screen_w - t->width);
                t->y = clampi(me.y - tile_drag_off_y, tbh_tdp, screen_h - t->height);
                t->static_drawn = 0;
                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
                return 1;
            }

            if (left_press) {
                int w_hit = wm_hit_test(me.x, me.y);
                if (w_hit >= 0) {
                    wm_bring_to_front(w_hit);
                    g_win_focused = w_hit;

                    /* Check close / maximize / minimize buttons before drag/resize */
                    int did_btn = 0;
                    {
                        int bx, by, bw, bh;
                        wm_get_close_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            wm_close_window(w_hit);
                            g_force_full_redraw = 1;
                            g_tiles_full_content_redraw = 1;
                            did_btn = 1;
                        }
                    }
                    if (!did_btn) {
                        int bx, by, bw, bh;
                        wm_get_max_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            window_t* w = &g_windows[w_hit];
                            int taskbar_h_win = vga_text_cell_h() + 6;
                            if (taskbar_h_win < 0) taskbar_h_win = 0;
                            if (taskbar_h_win > screen_h - 32) taskbar_h_win = screen_h - 32;
                            if (!w->maximized) {
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                w->prev_x = w->x; w->prev_y = w->y; w->prev_w = w->w; w->prev_h = w->h;
                                w->x = 0; w->y = taskbar_h_win; w->w = screen_w; w->h = screen_h - taskbar_h_win;
                                w->maximized = 1; w->minimized = 0;
                            } else {
                                vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                                w->x = w->prev_x; w->y = w->prev_y; w->w = w->prev_w; w->h = w->prev_h;
                                w->maximized = 0;
                            }
                            w->static_drawn = 0; w->needs_redraw = 1;
                            g_tiles_full_content_redraw = 1;
                            did_btn = 1;
                        }
                    }
                    if (!did_btn) {
                        int bx, by, bw, bh;
                        wm_get_min_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            window_t* w = &g_windows[w_hit];
                            w->minimized = !w->minimized; w->needs_redraw = 1; w->static_drawn = 0;
                            g_tiles_full_content_redraw = 1;
                            did_btn = 1;
                        }
                    }
                    if (!did_btn) {
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
                        } else {
                            int th = win_title_height(&g_windows[w_hit]);
                            if (point_in_rect(me.x, me.y, g_windows[w_hit].x, g_windows[w_hit].y, g_windows[w_hit].w, th)) {
                                drag_active = 1;
                                drag_win = w_hit;
                                drag_off_x = me.x - g_windows[w_hit].x;
                                drag_off_y = me.y - g_windows[w_hit].y;
                            }
                        }
                    }
                    return 1;
                }
                /* No floating window hit: check tile border resize and titlebar drag. */
                {
                    int t_hit_p = tile_index_at(me.x, me.y);
                    if (t_hit_p >= 0 && t_hit_p < tile_count && !tiles[t_hit_p].maximized) {
                        int te = tile_border_resize_hit_test(&tiles[t_hit_p], me.x, me.y);
                        if (te) {
                            tile_border_resize_active = 1;
                            tile_border_resize_idx = t_hit_p;
                            tile_border_resize_edges = te;
                            tile_border_resize_start_mx = me.x;
                            tile_border_resize_start_my = me.y;
                            tile_border_resize_start_x = tiles[t_hit_p].x;
                            tile_border_resize_start_y = tiles[t_hit_p].y;
                            tile_border_resize_start_w = tiles[t_hit_p].width;
                            tile_border_resize_start_h = tiles[t_hit_p].height;
                            return 1;
                        }
                        int th_p = get_title_height(&tiles[t_hit_p]);
                        if (th_p > 0 && point_in_rect(me.x, me.y, tiles[t_hit_p].x, tiles[t_hit_p].y, tiles[t_hit_p].width, th_p)) {
                            tile_drag_active = 1;
                            tile_drag_idx = t_hit_p;
                            tile_drag_off_x = me.x - tiles[t_hit_p].x;
                            tile_drag_off_y = me.y - tiles[t_hit_p].y;
                            return 1;
                        }
                    }
                }
            }

            // Route to focused window if any; else to the active user task's tile.
            if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                window_t* wfocus = &g_windows[g_win_focused];
                if (wfocus->mouse_cb) {
                    wfocus->mouse_cb(g_win_focused, (const mouse_event_t*)&me, wfocus->userdata);
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

    /*
     * Key release: tui_read_key() returns negative values for key releases.
     * Route directly to the gui_key_cb (with the negative value preserved)
     * bypassing all hotkey / stdin processing — release events should not
     * trigger shortcuts or echo characters.
     */
    if (key < 0) {
        int target_tile = focused;
        if (g_user_task_active && g_user_task_term >= 0) {
            int tt = tile_find_by_term(g_user_task_term);
            if (tt >= 0 && tt < tile_count) target_tile = tt;
        }
        if (target_tile >= 0 && target_tile < tile_count) {
            int term = tiles[target_tile].term_idx;
            if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
                gui_key_cb[term](target_tile, key, gui_userdata[term]);
            }
        }
        return 1;
    }

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

    // While a ring3 task is running, Ctrl+C is reserved as the user-task abort
    // UNLESS the task has a GUI key handler (e.g. text editor wants Ctrl+C for copy).
    if (g_user_task_active && key == 0x2206) {
        int term = g_user_task_term;
        if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
            /* GUI app: deliver Ctrl+C as keypress, not interrupt */
        } else {
            g_user_interrupt = 1;
            return 1;
        }
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

    /* ── Super-alone (0x6001): toggle start menu ── */
    if (key == 0x6001) {
        g_start_active = !g_start_active;
        g_dropdown_active = 0;
        g_ctx_active = 0;
        g_start_hover = -1;
        g_programs_active = 0;
        g_programs_hover = -1;
        g_programs_scanned = 0;
        g_force_full_redraw = 1;
        return 1;
    }

    /* ── Start menu keyboard navigation ── */
    if (g_start_active) {
        int base_nav = key & 0x1FFF;
        if (base_nav == 0x1001) { /* Up */
            if (g_programs_active && g_programs_hover >= 0) {
                if (g_programs_hover > 0) {
                    g_programs_hover--;
                } else if (g_programs_scroll > 0) {
                    g_programs_scroll--;
                }
            } else {
                if (g_start_hover <= 0) g_start_hover = 5;
                else g_start_hover--;
            }
            g_force_full_redraw = 1;
            return 1;
        }
        if (base_nav == 0x1002) { /* Down */
            if (g_programs_active && g_programs_hover >= 0) {
                int item_h_nav = vga_text_cell_h() + 6;
                int avail_h_nav = screen_h - (vga_text_cell_h() + 6);
                int max_vis_nav = (avail_h_nav - 4) / item_h_nav;
                if (max_vis_nav < 1) max_vis_nav = 1;
                if (max_vis_nav > g_program_count) max_vis_nav = g_program_count;
                if (g_programs_hover < max_vis_nav - 1) {
                    g_programs_hover++;
                } else {
                    int max_scroll_nav = g_program_count - max_vis_nav;
                    if (max_scroll_nav < 0) max_scroll_nav = 0;
                    if (g_programs_scroll < max_scroll_nav) g_programs_scroll++;
                }
            } else {
                if (g_start_hover >= 5) g_start_hover = 0;
                else g_start_hover++;
            }
            g_force_full_redraw = 1;
            return 1;
        }
        if (base_nav == 0x1004) { /* Right: enter Programs submenu */
            if (g_start_hover == 0) {
                g_programs_active = 1;
                if (!g_programs_scanned) scan_programs();
                g_programs_hover = 0;
                g_force_full_redraw = 1;
            }
            return 1;
        }
        if (base_nav == 0x1003) { /* Left: leave Programs submenu */
            if (g_programs_active) {
                g_programs_active = 0;
                g_programs_hover = -1;
                g_force_full_redraw = 1;
            }
            return 1;
        }
        if ((key & 0xFF) == '\n' || (key & 0xFF) == '\r') { /* Enter: activate item */
            if (g_programs_active && g_programs_hover >= 0) {
                int pidx_nav = g_programs_hover + g_programs_scroll;
                if (pidx_nav >= 0 && pidx_nav < g_program_count) {
                    launch_program(g_program_names[pidx_nav]);
                }
                g_start_active = 0; g_programs_active = 0;
                g_force_full_redraw = 1;
                return 1;
            }
            if (g_start_hover >= 0 && g_start_hover < 6) {
                if (g_start_hover == 0) { /* Programs */
                    g_programs_active = 1;
                    if (!g_programs_scanned) scan_programs();
                    g_programs_hover = 0;
                } else if (g_start_hover == 1) { /* Shell */
                    g_start_active = 0; g_programs_active = 0;
                    launch_shell_tile();
                } else if (g_start_hover == 2) { /* Files */
                    g_start_active = 0; g_programs_active = 0;
                    launch_program("files");
                } else if (g_start_hover == 3) { /* Settings */
                    g_start_active = 0;
                } else if (g_start_hover == 4) { /* Reboot */
                    g_start_active = 0;
                    outportb(0x64, 0xFE);
                } else if (g_start_hover == 5) { /* Shutdown */
                    g_start_active = 0;
                    Shutdown();
                }
                g_force_full_redraw = 1;
                return 1;
            }
        }
        if ((key & 0xFF) == 27) { /* Escape: close menu */
            if (g_programs_active) {
                g_programs_active = 0;
                g_programs_hover = -1;
            } else {
                g_start_active = 0;
            }
            g_force_full_redraw = 1;
            return 1;
        }
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
                tiles[idx].desktop = g_current_desktop;
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

            tile_close(focused);
            g_force_full_redraw = 1;
            layout_tiles();
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
            wfocus->key_cb(g_win_focused, key & 0xFFFF, wfocus->userdata);
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

