#include <system.h>
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
#include <misc/sched.h>
#include <watchdog.h>
#include <ui_prefs.h>
#include <network/netstack.h>
#include <drivers/e1000.h>
#include <context.h>
#include <utilities/shell/shell.h>
#include <fs/vfs.h>
#include <cpu/user_elf.h>
#include <multiboot.h>
#include <ata.h>

extern uint8_t g_current_drive;
static int g_tm_boot_installer_attempted = 0;


/* 
 * Single compilation unit for the GUI subsystem. Sub-modules are pulled
 * in by #include in dependency order; they share this TU and all its state.
 *
 *   gui_state.c   -- shared globals, types, constants, inline helpers
 *   gui_wm.c      -- floating window manager (chrome, drag/resize, API)
 *   gui_tiles.c   -- tile windows (layout, draw, lifecycle)
 *   gui_taskbar.c -- taskbar and start menu
 *   gui_input.c   -- tile public API and ring-3 input pump
 *
 * To add a new sub-module:
 *   1. Create src/gui/gui_<name>.c with a comment block description such as this one.
 *   2. #include it below in the correct dependency order.
 *   3. Do not add it as a separate Makefile object target.
*/

#include "gui_state.c"
#include "gui_wm.c"
#include "gui_tiles.c"
#include "gui_taskbar.c"
#include "gui_input.c"

static int tm_str_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return 0;
    for (int i = 0; haystack[i]; ++i) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) {
            ++j;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int tm_boot_installer_requested(void) {
    if (!g_mbi) return 0;
    if (!(g_mbi->flags & MULTIBOOT_INFO_CMDLINE) || !g_mbi->cmdline) return 0;
    const char* cmdline = (const char*)(uintptr_t)g_mbi->cmdline;
    return tm_str_contains(cmdline, "installer=1") || tm_str_contains(cmdline, "installer");
}

static int tm_disk_has_installer_binary(void) {
    uint8 logical_count = ata_get_num_logical_drives();
    for (uint8 logical = 0; logical < logical_count; ++logical) {
        if (!ata_logical_drive_present(logical)) continue;
        uint8 physical = ata_logical_to_physical(logical);
        if (physical == 0xFFu) continue;
        if (vfs_detect(physical) != VFS_FS_EYNFS) continue;

        vfs_stat_t st;
        if (vfs_stat(physical, "/binaries/installer", &st) == 0 && st.type == VFS_NODE_FILE) {
            return 1;
        }
    }
    return 0;
}

static void tm_aspect_ratio_for_mode(int mode, int* out_num, int* out_den) {
    int num = 1;
    int den = 1;
    switch (mode) {
        case TILER_ASPECT_4_3:
            num = 4; den = 3;
            break;
        case TILER_ASPECT_16_10:
            num = 16; den = 10;
            break;
        case TILER_ASPECT_16_9:
            num = 16; den = 9;
            break;
        case TILER_ASPECT_21_9:
            num = 21; den = 9;
            break;
        case TILER_ASPECT_1_1:
            num = 1; den = 1;
            break;
        case TILER_ASPECT_NATIVE:
        default:
            num = 0; den = 0;
            break;
    }
    if (out_num) *out_num = num;
    if (out_den) *out_den = den;
}

static void tm_compute_workspace_dims(int fb_w, int fb_h, int scale_pct, int aspect_mode, int* out_w, int* out_h) {
    if (fb_w < 1) fb_w = 640;
    if (fb_h < 1) fb_h = 480;

    if (scale_pct < 50) scale_pct = 50;
    if (scale_pct > 100) scale_pct = 100;
    if (aspect_mode < TILER_ASPECT_NATIVE || aspect_mode > TILER_ASPECT_1_1) aspect_mode = TILER_ASPECT_NATIVE;

    int w = (fb_w * scale_pct) / 100;
    int h = (fb_h * scale_pct) / 100;
    if (w < 320) w = 320;
    if (h < 200) h = 200;
    if (w > fb_w) w = fb_w;
    if (h > fb_h) h = fb_h;

    int ratio_num = 0;
    int ratio_den = 0;
    tm_aspect_ratio_for_mode(aspect_mode, &ratio_num, &ratio_den);
    if (ratio_num > 0 && ratio_den > 0) {
        if ((w * ratio_den) > (h * ratio_num)) {
            w = (h * ratio_num) / ratio_den;
        } else {
            h = (w * ratio_den) / ratio_num;
        }
        if (w < 160) w = 160;
        if (h < 120) h = 120;
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void tm_apply_workspace_profile(int relayout) {
    int fbw = g_fb_w > 0 ? g_fb_w : screen_w;
    int fbh = g_fb_h > 0 ? g_fb_h : screen_h;

    int new_w = screen_w;
    int new_h = screen_h;
    tm_compute_workspace_dims(
        fbw,
        fbh,
        ui_prefs_get_workspace_scale_pct(),
        ui_prefs_get_workspace_aspect_mode(),
        &new_w,
        &new_h
    );

    if (new_w == screen_w && new_h == screen_h) {
        if (relayout) {
            mouse_set_bounds(0, 0, screen_w - 1, screen_h - 1);
        }
        return;
    }

    screen_w = new_w;
    screen_h = new_h;

    if (screen_w < 1) screen_w = 1;
    if (screen_h < 1) screen_h = 1;

    mouse_set_bounds(0, 0, screen_w - 1, screen_h - 1);

    // Keep areas outside the logical workspace black when using non-native profiles.
    drawRect(0, 0, fbw, fbh, 0, 0, 0);
    vga_mark_dirty_rect(0, 0, fbw, fbh);

    if (relayout) {
        layout_tiles();
        g_force_full_redraw = 1;
        g_tiles_full_content_redraw = 1;
    }
}

static char g_tm_status_buf[64];

static void tm_refresh_status_text(void) {
    snprintf(g_tm_status_buf, sizeof(g_tm_status_buf), "Fb: %dx%d UI:%dx%d", g_fb_w, g_fb_h, screen_w, screen_h);
}

static void tm_apply_saved_display_mode(void) {
    int req_w = ui_prefs_get_display_width();
    int req_h = ui_prefs_get_display_height();
    int req_bpp = ui_prefs_get_display_bpp();

    if (req_w < 320 || req_h < 200) return;
    if (req_bpp != 16 && req_bpp != 24 && req_bpp != 32) req_bpp = 32;

    int cur_w = (g_mbi && g_mbi->framebuffer_width > 0) ? (int)g_mbi->framebuffer_width : g_fb_w;
    int cur_h = (g_mbi && g_mbi->framebuffer_height > 0) ? (int)g_mbi->framebuffer_height : g_fb_h;
    int cur_bpp = (g_mbi && g_mbi->framebuffer_bpp > 0) ? (int)g_mbi->framebuffer_bpp : 32;

    if (cur_w == req_w && cur_h == req_h && cur_bpp == req_bpp) {
        return;
    }

    if (vga_set_mode(req_w, req_h, req_bpp) == 0) {
        printf("[display] switched to %dx%dx%d from ui.cfg\n", req_w, req_h, req_bpp);
    } else {
        printf("%c[display] runtime mode switch unsupported; keeping boot mode\n", 255, 165, 0);
    }
}

/* -- Main compositor loop and runtime configuration -------------------- */

void start_tiling_manager() {
    if (!g_tm_initialized) {
        command_context_t tm_boot_ctx;
        tm_boot_ctx.caps = CAP_ALL;
        tm_boot_ctx.wo = 0;
        tm_boot_ctx.det_seq = 0;
        tm_boot_ctx.drive = g_current_drive;
        int tm_boot_ctx_pushed = command_context_push(&tm_boot_ctx);

        // Installer-media fallback: the tiling manager boots before launch_shell().
        // In installer mode, prefer RAM:/ unconditionally when it is available.
        vfs_fs_type_t drive_fs = vfs_detect(g_current_drive);
        vfs_fs_type_t ram_fs = vfs_detect(VFS_DRIVE_RAM);
        int installer_requested = tm_boot_installer_requested();
        printf("[installer] tm boot check: current_drive=0x%X fs=%d ram_fs=%d installer=%d\n",
               (unsigned)g_current_drive, (int)drive_fs, (int)ram_fs, installer_requested);

        int disk_has_installer = tm_disk_has_installer_binary();
        if (installer_requested && ram_fs == VFS_FS_EYNFS && !disk_has_installer) {
            g_current_drive = VFS_DRIVE_RAM;
            printf("[installer] forcing current drive to RAM:/ due to installer boot flag\n");
        } else if (drive_fs == VFS_FS_NONE && ram_fs == VFS_FS_EYNFS && !disk_has_installer) {
            g_current_drive = VFS_DRIVE_RAM;
            printf("[installer] switched current drive to RAM:/ because disk fs is NONE\n");
        } else if (installer_requested && disk_has_installer) {
            printf("[installer] disk /binaries/installer present; skipping RAM auto-fallback\n");
        }

        // Initialize physical framebuffer dimensions from global multiboot info.
        if (g_mbi && g_mbi->framebuffer_width > 0 && g_mbi->framebuffer_height > 0) {
            g_fb_w = g_mbi->framebuffer_width;
            g_fb_h = g_mbi->framebuffer_height;
        }
        // Apply persisted runtime mode request (e.g. 1024x768) before UI sizing.
        tm_apply_saved_display_mode();
        if (g_mbi && g_mbi->framebuffer_width > 0 && g_mbi->framebuffer_height > 0) {
            g_fb_w = g_mbi->framebuffer_width;
            g_fb_h = g_mbi->framebuffer_height;
        }
        screen_w = g_fb_w;
        screen_h = g_fb_h;
        tm_apply_workspace_profile(0);
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
            tiles[i].minimized = 0;
            tiles[i].maximized = 0;
            tiles[i].prev_x = 0;
            tiles[i].prev_y = 0;
            tiles[i].prev_w = 0;
            tiles[i].prev_h = 0;
            tiles[i].desktop = 0;
            gui_needs_redraw[i] = 0;
        }
        tiles[0].type = TILE_SHELL;
        tiles[0].title = "EYN-OS Shell";
        tiles[0].desktop = 0;
        tm_refresh_status_text();
        tiles[0].status_left = g_tm_status_buf; // show framebuffer/workspace size for debug
        tiles[0].active = 1;
        vterm_set_active(0, 1);
        // Print initial prompt into vterm 0
        vterm_print_prompt(0);

        if (tm_boot_ctx_pushed) {
            command_context_pop();
        }

        layout_tiles();
        g_tm_initialized = 1;

        // Boot directly into installer when RAM media is present.
        // Do this only after layout+init so gui_attach() sees a live compositor.
        if (!g_tm_boot_installer_attempted) {
            g_tm_boot_installer_attempted = 1;
            command_context_t tm_launch_ctx;
            tm_launch_ctx.caps = CAP_ALL;
            tm_launch_ctx.wo = 0;
            tm_launch_ctx.det_seq = 0;
            tm_launch_ctx.drive = g_current_drive;
            int tm_launch_ctx_pushed = command_context_push(&tm_launch_ctx);

            vfs_stat_t st;
            int installer_stat = vfs_stat(VFS_DRIVE_RAM, "/binaries/installer", &st);
            printf("[installer] tm launch check: drive=0x%X stat=%d type=%d\n",
                   (unsigned)g_current_drive, installer_stat, (int)st.type);
            if (g_current_drive == VFS_DRIVE_RAM &&
                !tm_disk_has_installer_binary() &&
                installer_stat == 0 &&
                st.type == VFS_NODE_FILE) {
                printf("[installer] launching RAM:/binaries/installer\n");
                /* Spawn asynchronously to prevent UI blocking during installer load */
                int installer_pid = user_task_spawn_argv(VFS_DRIVE_RAM, "/binaries/installer", 0, NULL);
                printf("[installer] spawned installer pid=%d\n", installer_pid);
            }

            if (tm_launch_ctx_pushed) {
                command_context_pop();
            }
        }
    } else {
        // Re-entry path (e.g. aborting a ring3 task): keep all UI state, just ensure
        // a prompt is visible and force redraw so VGA updates immediately.
        if (tile_count <= 0) {
            tile_count = 0;
            (void)create_shell_tile_for_launch();
        }
        tile_ensure_valid_focus();
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
        if (tile_count <= 0) {
            tile_count = 0;
        }
        tile_ensure_valid_focus();

        // UI loop heartbeat for the watchdog
        watchdog_kick("wm-loop");

        // Background networking: pump RX/ARP/UDP enqueue at most once per tick.
        // This keeps host->guest delivery working even when the user isn't
        // running a dedicated udp-listen command.
        static uint32 last_net_tick = 0;
        uint32 now_net_tick = sched_get_tick_count();
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
    // On full redraws, paint the desktop background so exposed areas are not garbage.
    if (g_force_full_redraw) {
        draw_desktop_background(0, 0, screen_w, screen_h);
        vga_mark_dirty_rect(0, 0, screen_w, screen_h);
    }
    // draw all tiles
    // Avoid clearing the entire screen each frame to reduce flicker. Only clear the regions we will redraw (each tile).
    // Quick heuristic: clear each tile's rectangle before drawing it.
    for (int i = 0; i < tile_count; ++i) {
        if (tiles[i].type == TILE_EMPTY || tiles[i].minimized) continue;
        if (tiles[i].desktop != g_current_desktop) continue;
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
                if (w->desktop != g_current_desktop) continue;
                // Redraw content if requested
                if (w->needs_redraw || w->continuous_redraw || g_force_full_redraw || g_any_tile_content_redrew) {
                    wm_draw_content(w);
                    if (!w->continuous_redraw) w->needs_redraw = 0;
                }

                int is_focused_win = (wi == g_win_focused);
                // Redraw decorations if first time, focus changed, forced, or underlying tiles changed
                int need_decor = (!g_gui_low_mode) || !w->static_drawn || w->last_focused != is_focused_win || g_force_full_redraw || g_any_tile_content_redrew;
                if (need_decor) {
                    wm_draw_decor(w, is_focused_win);
                    wm_mark_decor_dirty(w);
                    w->static_drawn = 1;
                    w->last_focused = is_focused_win;
                }

                // Draw status overlay on top of window content.
                if (status_overlay_visible()) {
                    wm_draw_status_overlay(w);
                }
            }
        }
        // If a modal is active, draw it now on top of everything
        if (g_bg_modal.active) { draw_bg_modal(); }
        if (g_ctx_active) { draw_ctx_menu(); }

        // Track which redraw flags were consumed by this frame so requests
        // raised during draw_taskbar (for example, toast expiry) can persist
        // into the next frame.
        int clear_force_full_redraw = g_force_full_redraw ? 1 : 0;
        int clear_tiles_full_content_redraw = g_tiles_full_content_redraw ? 1 : 0;

        draw_taskbar();

        if (clear_force_full_redraw) g_force_full_redraw = 0;
        if (clear_tiles_full_content_redraw) g_tiles_full_content_redraw = 0;
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

        // With the software backbuffer enabled, the previous cursor rectangle is refreshed
        // from the backbuffer post-swap above. In low-RAM/no-backbuffer mode, that path is
        // a no-op, so we must restore from the save-under buffer to avoid cursor trails.

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
                if (!m) {
                    /* Hover over an individual tile border → show resize cursor */
                    int t_hit_h = tile_index_at(cur_mx, cur_my);
                    if (t_hit_h >= 0 && t_hit_h < tile_count) {
                        int te = tile_border_resize_hit_test(&tiles[t_hit_h], cur_mx, cur_my);
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

        // Draw mouse cursor overlay on top of the freshly swapped framebuffer
        if (cur_mx > -100 && cur_my > -100) {
            if (!vga_has_backbuffer() && cursor_savebuf && prev_saved_w > 0 && prev_saved_h > 0) {
                vga_restore_fb_region(prev_saved_x, prev_saved_y, prev_saved_w, prev_saved_h,
                                     cursor_savebuf, cursor_save_len);
            }

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
            if (!g_cursor_hidden)
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
            int press_edge = (changes & MOUSE_BUTTON_LEFT) && downMask;
            int release_edge = (changes & MOUSE_BUTTON_LEFT) && !downMask;

            /* Right-click detection for context menu */
            int right_press = (changes & MOUSE_BUTTON_RIGHT) && (me.buttons & MOUSE_BUTTON_RIGHT);

            /* --- Handle left-click on an active context menu --- */
            if (press_edge && g_ctx_active) {
                int cw_ctx = vga_text_cell_w();
                int ch_ctx = vga_text_cell_h();
                int item_h_ctx = ch_ctx + 6;
                int menu_w_ctx = 14 * cw_ctx;
                int menu_h_ctx = CTX_ITEM_COUNT * item_h_ctx + 4;
                int cmx = g_ctx_x; int cmy = g_ctx_y;
                if (cmx + menu_w_ctx > screen_w) cmx = screen_w - menu_w_ctx;
                if (cmy + menu_h_ctx > screen_h) cmy = screen_h - menu_h_ctx;
                if (cmx < 0) cmx = 0;
                if (cmy < 0) cmy = 0;

                if (me.x >= cmx && me.x < cmx + menu_w_ctx &&
                    me.y >= cmy && me.y < cmy + menu_h_ctx) {
                    int item = (me.y - cmy - 2) / item_h_ctx;
                    if (item >= 0 && item < CTX_ITEM_COUNT) {
                        if (item < g_desktop_count) {
                            /* Move to Desktop item */
                            if (g_ctx_kind == 0 && g_ctx_index >= 0 && g_ctx_index < tile_count) {
                                tiles[g_ctx_index].desktop = item;
                            } else if (g_ctx_kind == 1 && g_ctx_index >= 0 && g_ctx_index < MAX_WINDOWS && g_windows[g_ctx_index].used) {
                                g_windows[g_ctx_index].desktop = item;
                            }
                            g_force_full_redraw = 1;
                            layout_tiles();
                        } else if (item == CTX_ITEM_COUNT - 1) {
                            /* Kill */
                            if (g_ctx_kind == 0 && g_ctx_index >= 0 && g_ctx_index < tile_count) {
                                tile_close(g_ctx_index);
                                g_force_full_redraw = 1;
                                layout_tiles();
                            } else if (g_ctx_kind == 1 && g_ctx_index >= 0 && g_ctx_index < MAX_WINDOWS && g_windows[g_ctx_index].used) {
                                wm_force_close_window(g_ctx_index);
                                if (g_user_task_active) g_user_interrupt = 1;
                                g_force_full_redraw = 1;
                                g_tiles_full_content_redraw = 1;
                            }
                        }
                    }
                }
                g_ctx_active = 0;
                g_ctx_index = -1;
                g_force_full_redraw = 1;
                goto after_mouse_handling;
            }

            /* --- Right-click: open context menu --- */
            if (right_press) {
                /* Right-click on a window titlebar */
                int w_hit_r = wm_hit_test(me.x, me.y);
                if (w_hit_r >= 0) {
                    int th_r = win_title_height(&g_windows[w_hit_r]);
                    if (me.y >= g_windows[w_hit_r].y && me.y < g_windows[w_hit_r].y + th_r) {
                        g_ctx_active = 1;
                        g_ctx_x = me.x; g_ctx_y = me.y;
                        g_ctx_kind = 1; g_ctx_index = w_hit_r;
                        g_ctx_hover = -1;
                        g_force_full_redraw = 1;
                        goto after_mouse_handling;
                    }
                }
                /* Right-click on a tile titlebar */
                int t_hit_r = tile_index_at(me.x, me.y);
                if (t_hit_r >= 0 && t_hit_r < tile_count) {
                    int th_r = get_title_height(&tiles[t_hit_r]);
                    if (me.y >= tiles[t_hit_r].y && me.y < tiles[t_hit_r].y + th_r) {
                        g_ctx_active = 1;
                        g_ctx_x = me.x; g_ctx_y = me.y;
                        g_ctx_kind = 0; g_ctx_index = t_hit_r;
                        g_ctx_hover = -1;
                        g_force_full_redraw = 1;
                        goto after_mouse_handling;
                    }
                }
                /* Right-click on a taskbar entry */
                int taskbar_h_r = vga_text_cell_h() + 6;
                if (me.y < taskbar_h_r) {
                    for (int bi = 0; bi < g_tb_button_count; ++bi) {
                        if (me.x >= g_tb_buttons[bi].x && me.x < g_tb_buttons[bi].x + g_tb_buttons[bi].w) {
                            g_ctx_active = 1;
                            g_ctx_x = me.x; g_ctx_y = taskbar_h_r;
                            g_ctx_kind = g_tb_buttons[bi].kind;
                            g_ctx_index = g_tb_buttons[bi].index;
                            g_ctx_hover = -1;
                            g_force_full_redraw = 1;
                            goto after_mouse_handling;
                        }
                    }
                }
                /* Right-click elsewhere: dismiss any open context menu */
                if (g_ctx_active) {
                    g_ctx_active = 0;
                    g_force_full_redraw = 1;
                    goto after_mouse_handling;
                }
            }
            if (press_edge) {
                int taskbar_h = vga_text_cell_h() + 6;
                int tb_cw = vga_text_cell_w();
                int start_btn_w = tb_cw + 10;
                if (me.y < taskbar_h) {
                    /* --- Clicks on the taskbar --- */

                    /* Start button */
                    if (me.x >= 2 && me.x < 2 + start_btn_w) {
                        g_start_active = !g_start_active;
                        g_dropdown_active = 0;
                        g_ctx_active = 0;
                        g_start_hover = -1;
                        g_programs_active = 0;
                        g_programs_hover  = -1;
                        g_programs_scanned = 0; /* rescan /binaries on next open */
                        g_force_full_redraw = 1;
                        goto after_mouse_handling;
                    }

                    /* Overflow arrow button */
                    if (g_tb_overflow_w > 0 && me.x >= g_tb_overflow_x && me.x < g_tb_overflow_x + g_tb_overflow_w) {
                        g_dropdown_active = !g_dropdown_active;
                        g_start_active = 0;
                        g_dropdown_hover = -1;
                        g_force_full_redraw = 1;
                        goto after_mouse_handling;
                    }

                    /* App buttons -- click to focus/raise */
                    for (int bi = 0; bi < g_tb_button_count; ++bi) {
                        if (me.x >= g_tb_buttons[bi].x && me.x < g_tb_buttons[bi].x + g_tb_buttons[bi].w) {
                            if (g_tb_buttons[bi].kind == 0) {
                                /* Tile: switch focus */
                                int ti2 = g_tb_buttons[bi].index;
                                if (ti2 >= 0 && ti2 < tile_count) {
                                    if (tiles[ti2].minimized) {
                                        tiles[ti2].minimized = 0;
                                        tiles[ti2].static_drawn = 0;
                                    }
                                    focused = ti2;
                                    g_win_focused = -1;
                                    g_force_full_redraw = 1;
                                }
                            } else {
                                /* Window: bring to front + focus */
                                int wi2 = g_tb_buttons[bi].index;
                                if (wi2 >= 0 && wi2 < MAX_WINDOWS && g_windows[wi2].used) {
                                    wm_bring_to_front(wi2);
                                    g_win_focused = wi2;
                                    if (g_windows[wi2].minimized) {
                                        g_windows[wi2].minimized = 0;
                                        g_windows[wi2].needs_redraw = 1;
                                        g_windows[wi2].static_drawn = 0;
                                    }
                                    g_force_full_redraw = 1;
                                }
                            }
                            g_start_active = 0;
                            g_dropdown_active = 0;
                            goto after_mouse_handling;
                        }
                    }

                    /* Desktop switcher pills */
                    {
                        int pill_w2 = tb_cw + 6;
                        int pill_gap2 = 3;
                        int desk_total_w2 = g_desktop_count * (pill_w2 + pill_gap2) - pill_gap2;
                        int ds_x2 = screen_w - desk_total_w2 - 8;
                        for (int d = 0; d < g_desktop_count; ++d) {
                            int px2 = ds_x2 + d * (pill_w2 + pill_gap2);
                            if (me.x >= px2 && me.x < px2 + pill_w2) {
                                g_current_desktop = d;
                                /* Unfocus windows not on the new desktop */
                                if (g_win_focused >= 0 && g_windows[g_win_focused].desktop != d)
                                    g_win_focused = -1;
                                /* Move tile focus to a tile on the new desktop */
                                if (focused >= 0 && focused < tile_count &&
                                    tiles[focused].desktop != d) {
                                    int found_tile = -1;
                                    for (int t = 0; t < tile_count; ++t) {
                                        if (tiles[t].type != TILE_EMPTY &&
                                            !tiles[t].minimized &&
                                            tiles[t].desktop == d) {
                                            found_tile = t;
                                            break;
                                        }
                                    }
                                    if (found_tile >= 0) focused = found_tile;
                                }
                                g_force_full_redraw = 1;
                                layout_tiles();
                                goto after_mouse_handling;
                            }
                        }
                    }

                    /* Clicked elsewhere on taskbar -- dismiss menus */
                    g_start_active = 0;
                    g_dropdown_active = 0;
                    g_force_full_redraw = 1;
                    goto after_mouse_handling;

                } else if (g_start_active) {
                    /* Check clicks inside the Programs submenu first */
                    if (g_programs_active && g_program_count > 0) {
                        int sub_w2 = 160;
                        int sub_x2 = 150; /* menu_w */
                        int item_h2 = vga_text_cell_h() + 6;
                        int sub_top2 = taskbar_h;
                        int avail_h2 = screen_h - sub_top2;
                        int max_vis2 = (avail_h2 - 4) / item_h2;
                        if (max_vis2 < 1) max_vis2 = 1;
                        if (max_vis2 > g_program_count) max_vis2 = g_program_count;
                        int need_scroll2 = (g_program_count > max_vis2);
                        int sub_h2 = max_vis2 * item_h2 + 4;
                        if (need_scroll2) sub_h2 += item_h2 * 2; /* arrow rows */
                        if (me.x >= sub_x2 && me.x < sub_x2 + sub_w2 &&
                            me.y >= sub_top2 && me.y < sub_top2 + sub_h2) {
                            int rel_y = me.y - sub_top2 - 2;
                            if (need_scroll2) {
                                /* Click on scroll-up arrow */
                                if (rel_y < item_h2) {
                                    if (g_programs_scroll > 0) g_programs_scroll--;
                                    g_force_full_redraw = 1;
                                    goto after_mouse_handling;
                                }
                                rel_y -= item_h2;
                                /* Click on scroll-down arrow (after items) */
                                if (rel_y >= max_vis2 * item_h2) {
                                    int max_scroll2 = g_program_count - max_vis2;
                                    if (g_programs_scroll < max_scroll2) g_programs_scroll++;
                                    g_force_full_redraw = 1;
                                    goto after_mouse_handling;
                                }
                            }
                            int pidx = rel_y / item_h2 + g_programs_scroll;
                            if (pidx >= 0 && pidx < g_program_count) {
                                /* GUI programs open their own window; others use the wrapper */
                                launch_program(g_program_names[pidx]);
                            }
                            g_start_active = 0;
                            g_programs_active = 0;
                            g_force_full_redraw = 1;
                            goto after_mouse_handling;
                        }
                    }
                    /* Check clicks inside the start menu dropdown */
                    int menu_w2 = 150;
                    int item_h2 = vga_text_cell_h() + 6;
                    int menu_top = taskbar_h;
                    if (me.x >= 0 && me.x < menu_w2 && me.y >= menu_top && me.y < menu_top + 6 * item_h2 + 4) {
                        int idx2 = (me.y - menu_top - 2) / item_h2;
                        if (idx2 >= 0 && idx2 < 6) {
                            g_start_hover = idx2;
                            if (idx2 == 0) {
                                /* Programs: toggle submenu (handled by hover in draw) */
                                g_programs_active = !g_programs_active;
                                if (g_programs_active && !g_programs_scanned) scan_programs();
                                g_force_full_redraw = 1;
                            } else if (idx2 == 1) {
                                /* Shell: create/focus an EYN-OS shell tile */
                                g_start_active = 0;
                                g_programs_active = 0;
                                g_force_full_redraw = 1;
                                launch_shell_tile();
                            } else if (idx2 == 2) {
                                /* Files: run userland file explorer */
                                g_start_active = 0;
                                g_programs_active = 0;
                                g_force_full_redraw = 1;
                                launch_program("files");
                            } else if (idx2 == 3) {
                                /* Settings: run userland settings app */
                                g_start_active = 0;
                                g_programs_active = 0;
                                g_force_full_redraw = 1;
                                launch_program("settings");
                            } else if (idx2 == 4) {
                                /* Reboot: triple-fault via keyboard controller reset */
                                g_start_active = 0;
                                g_force_full_redraw = 1;
                                outportb(0x64, 0xFE); /* pulse CPU reset line */
                            } else if (idx2 == 5) {
                                /* Shutdown: use the system Shutdown() helper */
                                g_start_active = 0;
                                g_force_full_redraw = 1;
                                Shutdown();
                            }
                        }
                        goto after_mouse_handling;
                    }
                    g_start_active = 0;
                    g_programs_active = 0;
                    g_force_full_redraw = 1;

                } else if (g_dropdown_active && g_tb_overflow_count > 0) {
                    /* Check clicks inside the overflow dropdown */
                    int menu_w3 = 180;
                    int item_h3 = vga_text_cell_h() + 6;
                    int menu_h3 = g_tb_overflow_count * item_h3 + 4;
                    int menu_x3 = g_tb_overflow_x;
                    if (menu_x3 + menu_w3 > screen_w) menu_x3 = screen_w - menu_w3;
                    if (me.x >= menu_x3 && me.x < menu_x3 + menu_w3 &&
                        me.y >= taskbar_h && me.y < taskbar_h + menu_h3) {
                        int idx3 = (me.y - taskbar_h - 2) / item_h3;
                        if (idx3 >= 0 && idx3 < g_tb_overflow_count) {
                            /* Focus the chosen overflow app */
                            if (g_tb_overflow[idx3].kind == 0) {
                                int ti3 = g_tb_overflow[idx3].index;
                                if (ti3 >= 0 && ti3 < tile_count) {
                                    if (tiles[ti3].minimized) {
                                        tiles[ti3].minimized = 0;
                                        tiles[ti3].static_drawn = 0;
                                    }
                                    focused = ti3;
                                    g_win_focused = -1;
                                }
                            } else {
                                int wi3 = g_tb_overflow[idx3].index;
                                if (wi3 >= 0 && wi3 < MAX_WINDOWS && g_windows[wi3].used) {
                                    wm_bring_to_front(wi3);
                                    g_win_focused = wi3;
                                    if (g_windows[wi3].minimized) {
                                        g_windows[wi3].minimized = 0;
                                        g_windows[wi3].needs_redraw = 1;
                                        g_windows[wi3].static_drawn = 0;
                                    }
                                }
                            }
                            g_dropdown_active = 0;
                            g_force_full_redraw = 1;
                        }
                        goto after_mouse_handling;
                    }
                    g_dropdown_active = 0;
                    g_force_full_redraw = 1;

                } else {
                    if (g_start_active || g_dropdown_active) {
                        g_start_active = 0; g_dropdown_active = 0; g_force_full_redraw = 1;
                    }
                }
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
                    int bx, by, bw, bh;
                    wm_get_close_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                    if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                        g_windows[w_hit].pressed_button = 1;
                        g_windows[w_hit].static_drawn = 0;
                        did_action = 1;
                    }
                    if (!did_action) {
                        wm_get_max_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            g_windows[w_hit].pressed_button = 2;
                            g_windows[w_hit].static_drawn = 0;
                            did_action = 1;
                        }
                    }
                    if (!did_action) {
                        wm_get_min_rect(&g_windows[w_hit], &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            g_windows[w_hit].pressed_button = 3;
                            g_windows[w_hit].static_drawn = 0;
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
                    /* Check tile title bar button clicks (close/max/min) */
                    if (hit >= 0 && hit < tile_count) {
                        int t_th = get_title_height(&tiles[hit]);
                        if (t_th >= 12 && me.y >= tiles[hit].y && me.y < tiles[hit].y + t_th) {
                            /* Compute button rects for this tile (same layout as draw_decorations) */
                            int t_btn_side = 12;
                            if (g_close_icon_loaded) {
                                int iw2 = g_close_icon.header.width;
                                int ih2 = g_close_icon.header.height;
                                int m2 = (iw2 > ih2) ? iw2 : ih2;
                                if (m2 > t_btn_side) t_btn_side = m2;
                            }
                            if (t_btn_side > t_th) t_btn_side = t_th;
                            int t_btn_margin = 2, t_btn_gap = 2;
                            int t_btn_pad_y = (t_th - t_btn_side) / 2;
                            int t_close_bx = tiles[hit].x + tiles[hit].width - t_btn_side - t_btn_margin;
                            int t_close_by = tiles[hit].y + t_btn_pad_y;
                            int t_max_bx   = t_close_bx - t_btn_side - t_btn_gap;
                            int t_max_by   = t_close_by;
                            int t_min_bx   = t_max_bx - t_btn_side - t_btn_gap;
                            int t_min_by   = t_close_by;
                            if (point_in_rect(me.x, me.y, t_close_bx, t_close_by, t_btn_side, t_btn_side)) {
                                tiles[hit].pressed_button = 1;
                                tiles[hit].static_drawn = 0;
                                focused = hit;
                                g_force_full_redraw = 1;
                                goto after_mouse_handling;
                            }
                            if (point_in_rect(me.x, me.y, t_max_bx, t_max_by, t_btn_side, t_btn_side)) {
                                tiles[hit].pressed_button = 2;
                                tiles[hit].static_drawn = 0;
                                focused = hit;
                                g_force_full_redraw = 1;
                                goto after_mouse_handling;
                            }
                            if (point_in_rect(me.x, me.y, t_min_bx, t_min_by, t_btn_side, t_btn_side)) {
                                tiles[hit].pressed_button = 3;
                                tiles[hit].static_drawn = 0;
                                focused = hit;
                                g_force_full_redraw = 1;
                                goto after_mouse_handling;
                            }
                        }
                        /* Tile border resize: grab an edge or corner to resize the tile. */
                        if (!tiles[hit].maximized) {
                            int te = tile_border_resize_hit_test(&tiles[hit], me.x, me.y);
                            if (te) {
                                focused = hit;
                                g_win_focused = -1;
                                tile_border_resize_active = 1;
                                tile_border_resize_idx = hit;
                                tile_border_resize_edges = te;
                                tile_border_resize_start_mx = me.x;
                                tile_border_resize_start_my = me.y;
                                tile_border_resize_start_x = tiles[hit].x;
                                tile_border_resize_start_y = tiles[hit].y;
                                tile_border_resize_start_w = tiles[hit].width;
                                tile_border_resize_start_h = tiles[hit].height;
                                g_force_full_redraw = 1;
                                goto after_mouse_handling;
                            }
                            /* Tile titlebar drag: grab titlebar outside button cluster. */
                            int th_drag = get_title_height(&tiles[hit]);
                            if (th_drag > 0 && point_in_rect(me.x, me.y, tiles[hit].x, tiles[hit].y, tiles[hit].width, th_drag)) {
                                focused = hit;
                                g_win_focused = -1;
                                tile_drag_active = 1;
                                tile_drag_idx = hit;
                                tile_drag_off_x = me.x - tiles[hit].x;
                                tile_drag_off_y = me.y - tiles[hit].y;
                                g_force_full_redraw = 1;
                                goto after_mouse_handling;
                            }
                        }
                    }
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
                int release_hit = wm_hit_test(me.x, me.y);
                if (release_hit >= 0 && release_hit < MAX_WINDOWS && g_windows[release_hit].used) {
                    window_t* wrel = &g_windows[release_hit];
                    if (wrel->pressed_button == 1) {
                        int bx, by, bw, bh; wm_get_close_rect(wrel, &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            if (!wrel->close_cb || wrel->close_cb(release_hit, wrel->userdata)) {
                                wm_close_window(release_hit);
                                if (g_user_task_active) {
                                    g_user_interrupt = 1;
                                    g_force_full_redraw = 1;
                                }
                                layout_tiles();
                                g_force_full_redraw = 1;
                            }
                        }
                    } else if (wrel->pressed_button == 2) {
                        int bx, by, bw, bh; wm_get_max_rect(wrel, &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            fullscreen_tile = -1;
                            int taskbar_h_win = vga_text_cell_h() + 6;
                            if (taskbar_h_win < 0) taskbar_h_win = 0;
                            if (taskbar_h_win > screen_h - 32) taskbar_h_win = screen_h - 32;
                            if (!wrel->maximized) {
                                vga_mark_dirty_rect(wrel->x, wrel->y, wrel->w, wrel->h);
                                wrel->prev_x = wrel->x; wrel->prev_y = wrel->y; wrel->prev_w = wrel->w; wrel->prev_h = wrel->h;
                                wrel->x = 0; wrel->y = taskbar_h_win; wrel->w = screen_w; wrel->h = screen_h - taskbar_h_win;
                                wrel->maximized = 1; wrel->minimized = 0;
                                wrel->static_drawn = 0; wrel->needs_redraw = 1;
                                vga_mark_dirty_rect(wrel->x, wrel->y, wrel->w, wrel->h);
                                g_tiles_full_content_redraw = 1;
                            } else {
                                vga_mark_dirty_rect(wrel->x, wrel->y, wrel->w, wrel->h);
                                wrel->x = wrel->prev_x; wrel->y = wrel->prev_y; wrel->w = wrel->prev_w; wrel->h = wrel->prev_h;
                                wrel->maximized = 0; wrel->static_drawn = 0; wrel->needs_redraw = 1;
                                vga_mark_dirty_rect(wrel->x, wrel->y, wrel->w, wrel->h);
                                g_tiles_full_content_redraw = 1;
                            }
                        }
                    } else if (wrel->pressed_button == 3) {
                        int bx, by, bw, bh; wm_get_min_rect(wrel, &bx, &by, &bw, &bh);
                        if (point_in_rect(me.x, me.y, bx, by, bw, bh)) {
                            wrel->minimized = !wrel->minimized;
                            wrel->needs_redraw = 1;
                            wrel->static_drawn = 0;
                            if (wrel->minimized) g_win_focused = -1;
                            int th2 = win_title_height(wrel); int sh2 = win_status_height(wrel);
                            int cx2 = wrel->x + 1; int cy2 = wrel->y + th2 + sh2 + 1; int cw2 = wrel->w - 2; int ch2 = wrel->h - (th2 + sh2) - 2;
                            if (cw2 > 0 && ch2 > 0) vga_mark_dirty_rect(cx2, cy2, cw2, ch2);
                        }
                    }
                }

                for (int wi = 0; wi < MAX_WINDOWS; ++wi) {
                    if (g_windows[wi].used && g_windows[wi].pressed_button) {
                        g_windows[wi].pressed_button = 0;
                        g_windows[wi].static_drawn = 0;
                    }
                }

                for (int ti = 0; ti < tile_count; ++ti) {
                    if (tiles[ti].pressed_button == 1 || tiles[ti].pressed_button == 2 || tiles[ti].pressed_button == 3) {
                        int t_th = get_title_height(&tiles[ti]);
                        int t_btn_side = 12;
                        if (g_close_icon_loaded) {
                            int iw2 = g_close_icon.header.width;
                            int ih2 = g_close_icon.header.height;
                            int m2 = (iw2 > ih2) ? iw2 : ih2;
                            if (m2 > t_btn_side) t_btn_side = m2;
                        }
                        if (t_btn_side > t_th) t_btn_side = t_th;
                        int t_btn_margin = 2, t_btn_gap = 2;
                        int t_btn_pad_y = (t_th - t_btn_side) / 2;
                        int t_close_bx = tiles[ti].x + tiles[ti].width - t_btn_side - t_btn_margin;
                        int t_close_by = tiles[ti].y + t_btn_pad_y;
                        int t_max_bx = t_close_bx - t_btn_side - t_btn_gap;
                        int t_max_by = t_close_by;
                        int t_min_bx = t_max_bx - t_btn_side - t_btn_gap;
                        int t_min_by = t_close_by;

                        int fire = 0;
                        if (tiles[ti].pressed_button == 1 && point_in_rect(me.x, me.y, t_close_bx, t_close_by, t_btn_side, t_btn_side)) {
                            tile_close(ti);
                            layout_tiles();
                            g_force_full_redraw = 1;
                            fire = 1;
                        } else if (tiles[ti].pressed_button == 2 && point_in_rect(me.x, me.y, t_max_bx, t_max_by, t_btn_side, t_btn_side)) {
                            fullscreen_tile = -1;
                            int taskbar_h_tile = vga_text_cell_h() + 6;
                            if (taskbar_h_tile < 0) taskbar_h_tile = 0;
                            if (taskbar_h_tile > screen_h - 32) taskbar_h_tile = screen_h - 32;
                            if (!tiles[ti].maximized) {
                                tiles[ti].prev_x = tiles[ti].x;
                                tiles[ti].prev_y = tiles[ti].y;
                                tiles[ti].prev_w = tiles[ti].width;
                                tiles[ti].prev_h = tiles[ti].height;
                                tiles[ti].x = 0;
                                tiles[ti].y = taskbar_h_tile;
                                tiles[ti].width = screen_w;
                                tiles[ti].height = screen_h - taskbar_h_tile;
                                tiles[ti].maximized = 1;
                                tiles[ti].minimized = 0;
                            } else {
                                if (tiles[ti].prev_w > 0 && tiles[ti].prev_h > 0) {
                                    tiles[ti].x = tiles[ti].prev_x;
                                    tiles[ti].y = tiles[ti].prev_y;
                                    tiles[ti].width = tiles[ti].prev_w;
                                    tiles[ti].height = tiles[ti].prev_h;
                                }
                                tiles[ti].maximized = 0;
                            }
                            focused = ti;
                            g_force_full_redraw = 1;
                            fire = 1;
                        } else if (tiles[ti].pressed_button == 3 && point_in_rect(me.x, me.y, t_min_bx, t_min_by, t_btn_side, t_btn_side)) {
                            fullscreen_tile = -1;
                            tiles[ti].minimized = 1;
                            tiles[ti].maximized = 0;
                            tile_ensure_valid_focus();
                            g_force_full_redraw = 1;
                            fire = 1;
                        }

                        tiles[ti].pressed_button = 0;
                        tiles[ti].static_drawn = 0;
                        if (fire) break;
                    }
                }

                drag_active = 0; drag_win = -1;
                resize_active = 0; resize_win = -1; resize_edges = 0;
                tile_resize_active = 0; tile_resize_mode = 0;
                tile_drag_active = 0; tile_drag_idx = -1;
                tile_border_resize_active = 0; tile_border_resize_idx = -1; tile_border_resize_edges = 0;
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
                int taskbar_h_resize = vga_text_cell_h() + 6;
                int max_anchor_x = (screen_w > 0) ? (screen_w - 1) : 0;
                int max_anchor_y = (screen_h > 0) ? (screen_h - 1) : taskbar_h_resize;
                if (max_anchor_y < taskbar_h_resize) max_anchor_y = taskbar_h_resize;
                if (new_x < 0) { if (resize_edges & RESIZE_EDGE_L) { new_w += new_x; } new_x = 0; }
                if (new_y < taskbar_h_resize) { if (resize_edges & RESIZE_EDGE_T) { new_h += (new_y - taskbar_h_resize); } new_y = taskbar_h_resize; }
                if (new_x > max_anchor_x) new_x = max_anchor_x;
                if (new_y > max_anchor_y) new_y = max_anchor_y;
                if (new_w < minw) new_w = minw;
                if (new_h < minh) new_h = minh;

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
                    uint32 nowt = sched_get_tick_count();
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
                    uint32 nowt = sched_get_tick_count();
                    if (nowt - g_last_drag_tick < g_drag_throttle_ticks) {
                        // skip updating geom this loop
                    } else {
                        g_last_drag_tick = nowt;
                        int tb_h_drag = vga_text_cell_h() + 6;
                        int max_anchor_x = (screen_w > 0) ? (screen_w - 1) : 0;
                        int max_anchor_y = (screen_h > 0) ? (screen_h - 1) : tb_h_drag;
                        if (max_anchor_y < tb_h_drag) max_anchor_y = tb_h_drag;
                        w->x = clampi(me.x - drag_off_x, 0, max_anchor_x);
                        w->y = clampi(me.y - drag_off_y, tb_h_drag, max_anchor_y);
                    }
                    // Do not mark tiles/windows dirty per-move; wireframe overlay handles visuals
                    w->static_drawn = 0; // so final commit repaints decor at new spot
                } else {
                    // Normal mode: full overlay + underlying refresh
                    vga_mark_dirty_rect(w->x, w->y, w->w, w->h);
                    int old_x = w->x, old_y = w->y, old_w = w->w, old_h = w->h;
                    int tb_h_drag2 = vga_text_cell_h() + 6;
                    int max_anchor_x = (screen_w > 0) ? (screen_w - 1) : 0;
                    int max_anchor_y = (screen_h > 0) ? (screen_h - 1) : tb_h_drag2;
                    if (max_anchor_y < tb_h_drag2) max_anchor_y = tb_h_drag2;
                    w->x = clampi(me.x - drag_off_x, 0, max_anchor_x);
                    w->y = clampi(me.y - drag_off_y, tb_h_drag2, max_anchor_y);
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
            /* Tile border resize: update an individual tile's geometry via edge/corner drag */
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
                const int t_minw = 200, t_minh = 120;
                if (new_w < t_minw) { if (tile_border_resize_edges & RESIZE_EDGE_L) new_x -= (t_minw - new_w); new_w = t_minw; }
                if (new_h < t_minh) { if (tile_border_resize_edges & RESIZE_EDGE_T) new_y -= (t_minh - new_h); new_h = t_minh; }
                int tbh_tbr = vga_text_cell_h() + 6;
                int title_h = get_title_height(&tiles[tile_border_resize_idx]);
                int title_visible_w = titlebar_min_visible_width_px(title_h);
                int max_anchor_x = (screen_w > title_visible_w) ? (screen_w - title_visible_w) : 0;
                int max_anchor_y = (screen_h > title_h) ? (screen_h - title_h) : tbh_tbr;
                if (max_anchor_y < tbh_tbr) max_anchor_y = tbh_tbr;
                if (new_x < 0) { if (tile_border_resize_edges & RESIZE_EDGE_L) new_w += new_x; new_x = 0; }
                if (new_y < tbh_tbr) { if (tile_border_resize_edges & RESIZE_EDGE_T) new_h += (new_y - tbh_tbr); new_y = tbh_tbr; }
                if (new_x > max_anchor_x) new_x = max_anchor_x;
                if (new_y > max_anchor_y) new_y = max_anchor_y;
                if (new_w < t_minw) new_w = t_minw;
                if (new_h < t_minh) new_h = t_minh;
                t->x = new_x; t->y = new_y; t->width = new_w; t->height = new_h;
                t->static_drawn = 0;
                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
            }
            /* Tile titlebar drag: move an individual tile by dragging its titlebar */
            if (tile_drag_active && tile_drag_idx >= 0 && tile_drag_idx < tile_count) {
                tile_t* t = &tiles[tile_drag_idx];
                int tbh_td = vga_text_cell_h() + 6;
                int title_h = get_title_height(t);
                int title_visible_w = titlebar_min_visible_width_px(title_h);
                int max_anchor_x = (screen_w > title_visible_w) ? (screen_w - title_visible_w) : 0;
                int max_anchor_y = (screen_h > title_h) ? (screen_h - title_h) : tbh_td;
                if (max_anchor_y < tbh_td) max_anchor_y = tbh_td;
                t->x = clampi(me.x - tile_drag_off_x, 0, max_anchor_x);
                t->y = clampi(me.y - tile_drag_off_y, tbh_td, max_anchor_y);
                t->static_drawn = 0;
                g_force_full_redraw = 1;
                g_tiles_full_content_redraw = 1;
            }
            /* Scroll wheel: scroll the Programs submenu if it is open and cursor is inside it */
            if (me.wheel_delta != 0 && g_start_active && g_programs_active && g_program_count > 0) {
                int sub_x_sc = 150; /* menu_w */
                int sub_w_sc = 160;
                int taskbar_h_sc = vga_text_cell_h() + 6;
                if (me.x >= sub_x_sc && me.x < sub_x_sc + sub_w_sc && me.y >= taskbar_h_sc) {
                    g_programs_scroll -= me.wheel_delta;
                    if (g_programs_scroll < 0) g_programs_scroll = 0;
                    g_force_full_redraw = 1;
                }
            }
            // Mouse routing: deliver to top window iff mouse is inside its rect (or dragging)
            if (g_window_count > 0) {
                int topwi = g_window_order[g_window_count - 1];
                window_t* wtop = &g_windows[topwi];
                int inside_top = point_in_rect(me.x, me.y, wtop->x, wtop->y, wtop->w, wtop->h);
                if (!wtop->minimized && (inside_top || drag_active) && wtop->mouse_cb) {
                    if (!drag_active) wtop->mouse_cb(topwi, &me, wtop->userdata);
                    wtop->needs_redraw = 1;
                } else if (tile_count > 0 && focused >= 0 && focused < tile_count) {
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
            } else if (tile_count > 0 && focused >= 0 && focused < tile_count) {
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

            /* --- Hover tracking for context menu --- */
            if (g_ctx_active) {
                int cw_ctxh = vga_text_cell_w();
                int ch_ctxh = vga_text_cell_h();
                int item_h_ctxh = ch_ctxh + 6;
                int menu_w_ctxh = 14 * cw_ctxh;
                int menu_h_ctxh = CTX_ITEM_COUNT * item_h_ctxh + 4;
                int cmxh = g_ctx_x; int cmyh = g_ctx_y;
                if (cmxh + menu_w_ctxh > screen_w) cmxh = screen_w - menu_w_ctxh;
                if (cmyh + menu_h_ctxh > screen_h) cmyh = screen_h - menu_h_ctxh;
                if (cmxh < 0) cmxh = 0;
                if (cmyh < 0) cmyh = 0;
                int old_ctx_hover = g_ctx_hover;
                if (me.x >= cmxh && me.x < cmxh + menu_w_ctxh &&
                    me.y >= cmyh && me.y < cmyh + menu_h_ctxh) {
                    g_ctx_hover = (me.y - cmyh - 2) / item_h_ctxh;
                    if (g_ctx_hover < 0) g_ctx_hover = 0;
                    if (g_ctx_hover >= CTX_ITEM_COUNT) g_ctx_hover = CTX_ITEM_COUNT - 1;
                } else {
                    g_ctx_hover = -1;
                }
                if (g_ctx_hover != old_ctx_hover) g_force_full_redraw = 1;
            }

            /* --- Hover tracking for dropdown menus --- */
            {
                int taskbar_h2 = vga_text_cell_h() + 6;
                int item_h2 = vga_text_cell_h() + 6;
                if (g_start_active) {
                    int menu_w2 = 150;
                    int menu_top2 = taskbar_h2;
                    int old_hover = g_start_hover;
                    /* Check Programs submenu hover first */
                    if (g_programs_active && g_program_count > 0) {
                        int sub_w2 = 160;
                        int sub_x2 = menu_w2;
                        int sub_top2 = taskbar_h2;
                        int avail_hv = screen_h - sub_top2;
                        int max_vis_hv = (avail_hv - 4) / item_h2;
                        if (max_vis_hv < 1) max_vis_hv = 1;
                        if (max_vis_hv > g_program_count) max_vis_hv = g_program_count;
                        int need_scroll_hv = (g_program_count > max_vis_hv);
                        int sub_h2 = max_vis_hv * item_h2 + 4;
                        if (need_scroll_hv) sub_h2 += item_h2 * 2;
                        int old_ph = g_programs_hover;
                        if (me.x >= sub_x2 && me.x < sub_x2 + sub_w2 &&
                            me.y >= sub_top2 && me.y < sub_top2 + sub_h2) {
                            int rel_yh = me.y - sub_top2 - 2;
                            if (need_scroll_hv) rel_yh -= item_h2; /* skip arrow row */
                            int vi_hv = rel_yh / item_h2;
                            if (vi_hv < 0) vi_hv = -1;
                            else if (vi_hv >= max_vis_hv) vi_hv = -1;
                            g_programs_hover = vi_hv;
                        } else {
                            g_programs_hover = -1;
                        }
                        if (g_programs_hover != old_ph) g_force_full_redraw = 1;
                    }
                    if (me.x >= 0 && me.x < menu_w2 && me.y >= menu_top2 && me.y < menu_top2 + 5 * item_h2 + 4) {
                        g_start_hover = (me.y - menu_top2 - 2) / item_h2;
                        if (g_start_hover < 0) g_start_hover = 0;
                        if (g_start_hover > 4) g_start_hover = 4;
                        /* Hovering over "Programs" (index 0) opens its submenu */
                        if (g_start_hover == 0 && !g_programs_active) {
                            g_programs_active = 1;
                            if (!g_programs_scanned) scan_programs();
                            g_force_full_redraw = 1;
                        } else if (g_start_hover != 0) {
                            g_programs_active = 0;
                            g_programs_hover  = -1;
                        }
                    } else if (!(g_programs_active && g_program_count > 0 &&
                                 me.x >= menu_w2 && me.x < menu_w2 + 160 &&
                                 me.y >= taskbar_h2)) {
                        g_start_hover = -1;
                        g_programs_active = 0;
                        g_programs_hover  = -1;
                    }
                    if (g_start_hover != old_hover) g_force_full_redraw = 1;
                }
                if (g_dropdown_active && g_tb_overflow_count > 0) {
                    int menu_w3 = 180;
                    int menu_x3 = g_tb_overflow_x;
                    if (menu_x3 + menu_w3 > screen_w) menu_x3 = screen_w - menu_w3;
                    int menu_top3 = taskbar_h2;
                    int menu_h3 = g_tb_overflow_count * item_h2 + 4;
                    int old_hover2 = g_dropdown_hover;
                    if (me.x >= menu_x3 && me.x < menu_x3 + menu_w3 && me.y >= menu_top3 && me.y < menu_top3 + menu_h3) {
                        g_dropdown_hover = (me.y - menu_top3 - 2) / item_h2;
                        if (g_dropdown_hover < 0) g_dropdown_hover = 0;
                        if (g_dropdown_hover >= g_tb_overflow_count) g_dropdown_hover = g_tb_overflow_count - 1;
                    } else {
                        g_dropdown_hover = -1;
                    }
                    if (g_dropdown_hover != old_hover2) g_force_full_redraw = 1;
                }
            }
        }

after_mouse_handling:

    int key = tui_read_key();
    if (key) { watchdog_kick("wm-key"); }

        if (g_win_focused >= 0 && g_windows[g_win_focused].used && g_windows[g_win_focused].minimized) {
            g_win_focused = -1;
        }

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
            continue;
        }

        /* ── Start menu keyboard navigation ── */
        if (g_start_active && key) {
            int base_nav = key & 0x1FFF;
            int handled_menu = 0;
            if (base_nav == 0x1001) { /* Up */
                if (g_programs_active && g_programs_hover >= 0) {
                    if (g_programs_hover > 0) g_programs_hover--;
                    else if (g_programs_scroll > 0) g_programs_scroll--;
                } else {
                    if (g_start_hover <= 0) g_start_hover = 5;
                    else g_start_hover--;
                }
                handled_menu = 1;
            } else if (base_nav == 0x1002) { /* Down */
                if (g_programs_active && g_programs_hover >= 0) {
                    int item_h_nav = vga_text_cell_h() + 6;
                    int avail_h_nav = screen_h - (vga_text_cell_h() + 6);
                    int max_vis_nav = (avail_h_nav - 4) / item_h_nav;
                    if (max_vis_nav < 1) max_vis_nav = 1;
                    if (max_vis_nav > g_program_count) max_vis_nav = g_program_count;
                    if (g_programs_hover < max_vis_nav - 1) g_programs_hover++;
                    else {
                        int max_scroll_nav = g_program_count - max_vis_nav;
                        if (max_scroll_nav < 0) max_scroll_nav = 0;
                        if (g_programs_scroll < max_scroll_nav) g_programs_scroll++;
                    }
                } else {
                    if (g_start_hover >= 5) g_start_hover = 0;
                    else g_start_hover++;
                }
                handled_menu = 1;
            } else if (base_nav == 0x1004) { /* Right: enter Programs submenu */
                if (g_start_hover == 0) {
                    g_programs_active = 1;
                    if (!g_programs_scanned) scan_programs();
                    g_programs_hover = 0;
                }
                handled_menu = 1;
            } else if (base_nav == 0x1003) { /* Left: leave Programs submenu */
                if (g_programs_active) {
                    g_programs_active = 0;
                    g_programs_hover = -1;
                }
                handled_menu = 1;
            } else if ((key & 0xFF) == '\n' || (key & 0xFF) == '\r') { /* Enter */
                if (g_programs_active && g_programs_hover >= 0) {
                    int pidx_nav = g_programs_hover + g_programs_scroll;
                    if (pidx_nav >= 0 && pidx_nav < g_program_count)
                        launch_program(g_program_names[pidx_nav]);
                    g_start_active = 0; g_programs_active = 0;
                } else if (g_start_hover >= 0 && g_start_hover < 6) {
                    if (g_start_hover == 0) {
                        g_programs_active = 1;
                        if (!g_programs_scanned) scan_programs();
                        g_programs_hover = 0;
                    } else if (g_start_hover == 1) {
                        g_start_active = 0; g_programs_active = 0;
                        launch_shell_tile();
                    } else if (g_start_hover == 2) {
                        g_start_active = 0; g_programs_active = 0;
                        launch_program("files");
                    } else if (g_start_hover == 3) {
                        g_start_active = 0;
                    } else if (g_start_hover == 4) {
                        g_start_active = 0;
                        outportb(0x64, 0xFE);
                    } else if (g_start_hover == 5) {
                        g_start_active = 0;
                        Shutdown();
                    }
                }
                handled_menu = 1;
            } else if ((key & 0xFF) == 27) { /* Escape */
                if (g_programs_active) {
                    g_programs_active = 0;
                    g_programs_hover = -1;
                } else {
                    g_start_active = 0;
                }
                handled_menu = 1;
            }
            if (handled_menu) {
                g_force_full_redraw = 1;
                continue;
            }
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
                // If a window is focused, close it; otherwise close the focused tile.
                if (g_win_focused >= 0 && g_windows[g_win_focused].used) {
                    // Allow closing focused floating window regardless of tile count
                    wm_close_window(g_win_focused);
                    g_win_focused = -1;
                    g_force_full_redraw = 1;
                } else {
                    if (tile_count > 0 && focused >= 0 && focused < tile_count) {
                        tile_close(focused);
                        g_force_full_redraw = 1;
                        layout_tiles();
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
                if (wfocus->key_cb) { wfocus->key_cb(g_win_focused, key & 0xFFFF, wfocus->userdata); wfocus->needs_redraw = 1; }
                continue;
            }
            if (tile_count <= 0 || focused < 0 || focused >= tile_count) {
                continue;
            }
            int term = tiles[focused].term_idx;
            if (term >= 0 && term < MAX_TILES && gui_key_cb[term]) {
                gui_key_cb[term](focused, key & 0xFFFF, gui_userdata[term]);
                gui_needs_redraw[term] = 1;
                g_any_tile_content_redrew = 1;
                continue;
            }
            // Desktop safety: ignore Esc when no GUI/window wants it.
            continue;
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
                if (tile_count <= 0 || focused < 0 || focused >= tile_count) {
                    continue;
                }
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
            if (wfocus->key_cb) { wfocus->key_cb(g_win_focused, key & 0xFFFF, wfocus->userdata); wfocus->needs_redraw = 1; }
        } else {
            if (tile_count <= 0 || focused < 0 || focused >= tile_count) {
                continue;
            }
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
            if (gui_needs_redraw[_ti] || gui_continuous_redraw[_ti]) { idle_ok = 0; break; }
        }
        // Any windows needing redraw?
        if (idle_ok && g_window_count > 0) {
            for (int _wi = 0; _wi < MAX_WINDOWS; ++_wi) {
                if (g_windows[_wi].used && (g_windows[_wi].needs_redraw || g_windows[_wi].continuous_redraw)) { idle_ok = 0; break; }
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

int tiler_set_display_profile(int scale_pct, int aspect_mode, int persist) {
    ui_prefs_set_workspace_scale_pct(scale_pct);
    ui_prefs_set_workspace_aspect_mode(aspect_mode);

    if (persist) {
        (void)ui_prefs_save((uint8)g_current_drive);
    }

    if (g_tm_initialized) {
        tm_apply_workspace_profile(1);
    }

    tm_refresh_status_text();

    return 0;
}

int tiler_set_display_mode(int mode_w, int mode_h, int bpp, int persist) {
    if (mode_w < 320 || mode_h < 200) return -1;
    if (bpp != 16 && bpp != 24 && bpp != 32) return -1;
    if (vga_set_mode(mode_w, mode_h, bpp) != 0) return -1;

    if (g_mbi && g_mbi->framebuffer_width > 0 && g_mbi->framebuffer_height > 0) {
        g_fb_w = (int)g_mbi->framebuffer_width;
        g_fb_h = (int)g_mbi->framebuffer_height;
    } else {
        g_fb_w = mode_w;
        g_fb_h = mode_h;
    }

    screen_w = g_fb_w;
    screen_h = g_fb_h;

    ui_prefs_set_display_width(g_fb_w);
    ui_prefs_set_display_height(g_fb_h);
    ui_prefs_set_display_bpp((g_mbi && g_mbi->framebuffer_bpp) ? (int)g_mbi->framebuffer_bpp : bpp);

    tm_apply_workspace_profile(1);
    tm_refresh_status_text();

    if (persist) {
        (void)ui_prefs_save((uint8)g_current_drive);
    }

    return 0;
}

void tiler_get_display_profile(tiler_display_profile_t* out) {
    if (!out) return;
    out->fb_w = g_fb_w;
    out->fb_h = g_fb_h;
    out->workspace_w = screen_w;
    out->workspace_h = screen_h;
    out->scale_pct = ui_prefs_get_workspace_scale_pct();
    out->aspect_mode = ui_prefs_get_workspace_aspect_mode();
}

void tiler_get_display_mode(tiler_display_mode_t* out) {
    if (!out) return;
    out->width = g_fb_w;
    out->height = g_fb_h;
    out->bpp = (g_mbi && g_mbi->framebuffer_bpp > 0) ? (int)g_mbi->framebuffer_bpp : 32;
    out->can_switch = vga_can_set_mode();
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
