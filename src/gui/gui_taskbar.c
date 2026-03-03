/* 
 * Taskbar and start menu: desktop task-bar rendering, start-menu draw,
 * programs-list scan and display, clock/RTC, overflow dropdown,
 * and the background-selection modal key/mouse handlers.
*/

static int g_start_active = 0;
static int g_dropdown_active = 0;
static int g_start_hover = -1;     // hovered start menu item (-1 = none)
static int g_dropdown_hover = -1;  // hovered overflow item (-1 = none)
static int g_programs_active = 0;  // Programs submenu open
static int g_programs_hover = -1;  // hovered program index

/* (Context menu state declared near top of file with other globals.) */

/*
 * Cached list of program names from /binaries.
 * Scanned once on first open; refreshed on each start-menu toggle.
 */
#define PROGRAMS_MAX 96
static char  g_program_names[PROGRAMS_MAX][PROG_NAME_MAX];
static int   g_program_count = 0;
static int   g_programs_scanned = 0;
static int   g_programs_scroll = 0; /* scroll offset for the programs submenu */

static void scan_programs(void) {
    g_program_count = 0;
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) return;
    eynfs_dir_entry_t bin_entry;
    /* Try /binaries first, then /testdir/binaries */
    const char* dirs[] = { "/binaries", "/testdir/binaries", NULL };
    uint32_t dir_block = 0;
    int found = 0;
    for (int di = 0; dirs[di]; ++di) {
        if (eynfs_traverse_path(0, &sb, dirs[di], &bin_entry, NULL, NULL) == 0 &&
            bin_entry.type == EYNFS_TYPE_DIR) {
            dir_block = bin_entry.first_block;
            found = 1;
            break;
        }
    }
    if (!found) return;
    /* Read directory entries (cap stack usage: 64 * ~52 = ~3.3KB) */
    eynfs_dir_entry_t entries[64];
    int count = eynfs_read_dir_table(0, dir_block, entries, 64);
    if (count <= 0) return;
    for (int i = 0; i < count && g_program_count < PROGRAMS_MAX; ++i) {
        if (entries[i].type != EYNFS_TYPE_FILE) continue;
        if (entries[i].name[0] == '\0' || entries[i].name[0] == '.') continue;
        /* Skip .uelf suffix variants if an extensionless version exists */
        int nlen = (int)strlen(entries[i].name);
        if (nlen >= PROG_NAME_MAX) nlen = PROG_NAME_MAX - 1;
        for (int c = 0; c < nlen; ++c) g_program_names[g_program_count][c] = entries[i].name[c];
        g_program_names[g_program_count][nlen] = '\0';
        g_program_count++;
    }

    for (int i = 1; i < g_program_count; ++i) {
        char key[PROG_NAME_MAX];
        for (int c = 0; c < PROG_NAME_MAX; ++c) key[c] = g_program_names[i][c];
        int j = i - 1;
        while (j >= 0 && strcmp(g_program_names[j], key) > 0) {
            for (int c = 0; c < PROG_NAME_MAX; ++c) g_program_names[j + 1][c] = g_program_names[j][c];
            --j;
        }
        for (int c = 0; c < PROG_NAME_MAX; ++c) g_program_names[j + 1][c] = key[c];
    }

    g_programs_scanned = 1;
}

/* (Desktop switcher state declared near top of file with other globals.) */

/* Per-taskbar-button hit rectangles (rebuilt every frame by draw_taskbar). */
#define TB_MAX_BUTTONS 16
typedef struct {
    int x, w;         // pixel rect (y/h = 0..taskbar_height implicitly)
    int kind;         // 0=tile, 1=window
    int index;        // tile index or window index
} tb_button_t;
static tb_button_t g_tb_buttons[TB_MAX_BUTTONS];
static int         g_tb_button_count = 0;
static int         g_tb_overflow_x   = 0;  // x of the overflow icon (0 = hidden)
static int         g_tb_overflow_w   = 0;

/* Overflow list: items that did not fit on the main taskbar row. */
#define TB_OVERFLOW_MAX 12
typedef struct {
    int kind;   // 0=tile, 1=window
    int index;
} tb_overflow_t;
static tb_overflow_t g_tb_overflow[TB_OVERFLOW_MAX];
static int           g_tb_overflow_count = 0;

static int get_rtc_reg(int reg) {
    outportb(0x70, reg);
    return inportb(0x71);
}

/* Day-of-week names (Mon..Sun).  RTC register 6 returns 1-7 (Sun=1 in
 * most BIOSes, but some return Mon=1).  We handle both by just indexing
 * modulo 7 and accepting minor mismatch on exotic hardware. */
static const char* g_dow_names[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void draw_taskbar(void) {
    int cw   = vga_text_cell_w();
    int ch   = vga_text_cell_h();
    int th   = ch + 6;                    // slightly taller for padding
    int text_y = (th - ch) / 2;

    /* ---- Materia palette ---- */
    int bar_r = 48, bar_g = 48, bar_b = 48;   // taskbar background
    int fg    = 230;                           // default icon/text
    int accent_r = 72, accent_g = 72, accent_b = 72; // focused button bg
    int hover_r  = 58, hover_g  = 58, hover_b  = 58; // hovered button bg
    (void)hover_r; (void)hover_g; (void)hover_b;

    /* Background fill */
    drawRect(0, 0, screen_w, th, bar_r, bar_g, bar_b);

    /* --- Left: Start button "!" --- */
    int start_w = cw + 10;
    drawRect(2, 1, start_w, th - 2, accent_r, accent_g, accent_b);
    drawCharAt(2 + (start_w - cw) / 2, text_y, '!', fg, fg, fg);
    int tx = start_w + 6;  // running x cursor for app buttons

    // App buttons (tiles + windows)
    
    /* We reserve the right portion for clock + desktop switcher.
     * Available width = screen_w - right_zone - tx
     */
    int desk_zone_w = g_desktop_count * (cw + 8) + 8;  // desktop pills
    int clock_chars = 14;   // "Tue 12:46" max ~14 chars
    int right_reserved = desk_zone_w + clock_chars * cw + 24;
    int avail_w = screen_w - right_reserved - tx;
    int btn_pad = 6;     // horizontal padding inside each button
    int btn_gap = 2;     // gap between buttons

    g_tb_button_count   = 0;
    g_tb_overflow_count = 0;
    g_tb_overflow_x     = 0;
    g_tb_overflow_w     = 0;

    /* Collect all open apps: tiles first, then windows.
     * Only include entries belonging to the current desktop. */
    typedef struct { int kind; int idx; const char* title; } app_entry_t;
    app_entry_t apps[MAX_TILES + MAX_WINDOWS];
    int app_count = 0;
    for (int i = 0; i < tile_count; ++i) {
        if (tiles[i].type == TILE_EMPTY) continue;
        if (tiles[i].desktop != g_current_desktop) continue;
        apps[app_count].kind  = 0;
        apps[app_count].idx   = i;
        apps[app_count].title = tiles[i].title ? tiles[i].title : "Shell";
        app_count++;
    }
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) continue;
        if (g_windows[i].desktop != g_current_desktop) continue;
        apps[app_count].kind  = 1;
        apps[app_count].idx   = i;
        apps[app_count].title = g_windows[i].title ? g_windows[i].title : "Window";
        app_count++;
    }

    /* Measure how many fit; leave room for an overflow arrow if needed */
    int overflow_icon_w = cw + btn_pad * 2;  // the downward-arrow button
    int used_w = 0;
    int fits   = 0;
    for (int i = 0; i < app_count; ++i) {
        int title_len = (int)strlen(apps[i].title);
        if (title_len > 12) title_len = 12; // truncate long names
        int bw = title_len * cw + btn_pad * 2;
        if (bw < cw + btn_pad * 2) bw = cw + btn_pad * 2; // minimum 1-char
        int future = used_w + bw + btn_gap;
        int space  = avail_w;
        if (i < app_count - 1) space -= overflow_icon_w + btn_gap; // need overflow
        if (future > space && i > 0) break;
        used_w = future;
        fits++;
    }
    int need_overflow = (fits < app_count);

    /* Draw visible app buttons */
    for (int i = 0; i < fits; ++i) {
        int title_len = (int)strlen(apps[i].title);
        if (title_len > 12) title_len = 12;
        int bw = title_len * cw + btn_pad * 2;
        if (bw < cw + btn_pad * 2) bw = cw + btn_pad * 2;

        /* Determine if this app is focused */
        int is_active = 0;
        if (apps[i].kind == 0 && apps[i].idx == focused && g_win_focused < 0) is_active = 1;
        if (apps[i].kind == 1 && apps[i].idx == g_win_focused) is_active = 1;

        /* Button background */
        if (is_active)
            drawRect(tx, 1, bw, th - 2, accent_r, accent_g, accent_b);

        /* Icon: first letter of title */
        char icon_ch = apps[i].title[0];
        drawCharAt(tx + 3, text_y, (unsigned char)icon_ch, fg, fg, fg);

        /* Title text (after icon) */
        int text_start = tx + 3 + cw + 2;
        for (int c = 1; c < title_len; ++c) {
            int px = text_start + (c - 1) * cw;
            if (px + cw > tx + bw - 2) break;
            drawCharAt(px, text_y, (unsigned char)apps[i].title[c], fg, fg, fg);
        }

        /* Active indicator: thin line at bottom */
        if (is_active)
            drawRect(tx, th - 2, bw, 2, 180, 180, 180);

        /* Record hit rect */
        if (g_tb_button_count < TB_MAX_BUTTONS) {
            g_tb_buttons[g_tb_button_count].x     = tx;
            g_tb_buttons[g_tb_button_count].w     = bw;
            g_tb_buttons[g_tb_button_count].kind  = apps[i].kind;
            g_tb_buttons[g_tb_button_count].index = apps[i].idx;
            g_tb_button_count++;
        }
        tx += bw + btn_gap;
    }

    /* Overflow arrow button (downward arrow) */
    if (need_overflow) {
        drawRect(tx, 1, overflow_icon_w, th - 2, accent_r, accent_g, accent_b);
        /* draw a small downward arrow glyph */
        int ax = tx + overflow_icon_w / 2;
        int ay = text_y + ch / 2 - 2;
        for (int row = 0; row < 4; ++row) {
            drawRect(ax - row, ay + row, row * 2 + 1, 1, fg, fg, fg);
        }
        g_tb_overflow_x = tx;
        g_tb_overflow_w = overflow_icon_w;

        /* Build overflow list */
        for (int i = fits; i < app_count && g_tb_overflow_count < TB_OVERFLOW_MAX; ++i) {
            g_tb_overflow[g_tb_overflow_count].kind  = apps[i].kind;
            g_tb_overflow[g_tb_overflow_count].index = apps[i].idx;
            g_tb_overflow_count++;
        }
    }

    /* --- Centre: Date and Time --- */
    int rtc_h = get_rtc_reg(4);
    int rtc_m = get_rtc_reg(2);
    int rtc_dow = get_rtc_reg(6);  // day of week (1-7)
    int rtc_day = get_rtc_reg(7);  // day of month
    int rtc_mon = get_rtc_reg(8);  // month
    rtc_h   = (rtc_h   & 0x0F) + ((rtc_h   >> 4) * 10);
    rtc_m   = (rtc_m   & 0x0F) + ((rtc_m   >> 4) * 10);
    rtc_dow = (rtc_dow  & 0x0F) + ((rtc_dow >> 4) * 10);
    rtc_day = (rtc_day  & 0x0F) + ((rtc_day >> 4) * 10);
    rtc_mon = (rtc_mon  & 0x0F) + ((rtc_mon >> 4) * 10);
    if (rtc_dow < 1) rtc_dow = 1;
    if (rtc_dow > 7) rtc_dow = 7;
    const char* dow_str = g_dow_names[(rtc_dow - 1) % 7];

    /* Build date/time string manually (kernel snprintf lacks %02d). */
    char ts[32];
    {
        int p = 0;
        for (int i = 0; dow_str[i]; ++i) ts[p++] = dow_str[i];
        ts[p++] = ' ';
        ts[p++] = '0' + (rtc_day / 10);
        ts[p++] = '0' + (rtc_day % 10);
        ts[p++] = '/';
        ts[p++] = '0' + (rtc_mon / 10);
        ts[p++] = '0' + (rtc_mon % 10);
        ts[p++] = ' '; ts[p++] = ' ';
        ts[p++] = '0' + (rtc_h / 10);
        ts[p++] = '0' + (rtc_h % 10);
        ts[p++] = ':';
        ts[p++] = '0' + (rtc_m / 10);
        ts[p++] = '0' + (rtc_m % 10);
        ts[p] = '\0';
    }
    int ts_len = (int)strlen(ts);
    int time_x = (screen_w - ts_len * cw) / 2;
    for (int i = 0; i < ts_len; ++i)
        drawCharAt(time_x + i * cw, text_y, (unsigned char)ts[i], fg, fg, fg);

    /* --- Right: Desktop switcher pills --- */
    int pill_w = cw + 6;
    int pill_gap = 3;
    int desk_total_w = g_desktop_count * (pill_w + pill_gap) - pill_gap;
    int ds_x = screen_w - desk_total_w - 8;
    for (int d = 0; d < g_desktop_count; ++d) {
        int px = ds_x + d * (pill_w + pill_gap);
        int is_cur = (d == g_current_desktop);
        /* Pill background */
        drawRect(px, 2, pill_w, th - 4,
                 is_cur ? 90 : 56,
                 is_cur ? 90 : 56,
                 is_cur ? 90 : 56);
        char digit = '1' + (char)d;
        int dx = px + (pill_w - cw) / 2;
        drawCharAt(dx, text_y, digit,
                   is_cur ? 255 : 160,
                   is_cur ? 255 : 160,
                   is_cur ? 255 : 160);
    }

    /* --- Dropdown: Start menu --- */
    if (g_start_active) {
        int menu_w = 150;
        int item_h = ch + 6;
        const char* items[] = {"Programs", "Shell", "Files", "Settings", "Reboot", "Shutdown"};
        int item_count = 6;
        int menu_h = item_count * item_h + 4;
        drawRect(0, th, menu_w, menu_h, 42, 42, 42);
        /* thin top accent line */
        drawRect(0, th, menu_w, 1, 90, 90, 90);
        int dy = th + 2;
        for (int i = 0; i < item_count; ++i) {
            if (i == g_start_hover)
                drawRect(0, dy, menu_w, item_h, 60, 60, 60);
            int len = (int)strlen(items[i]);
            for (int j = 0; j < len; ++j)
                drawCharAt(12 + j * cw, dy + (item_h - ch) / 2,
                           (unsigned char)items[i][j], fg, fg, fg);
            /* Right-arrow indicator for Programs submenu */
            if (i == 0)
                drawCharAt(menu_w - cw - 4, dy + (item_h - ch) / 2, (unsigned char)'>', fg, fg, fg);
            dy += item_h;
        }
        vga_mark_dirty_rect(0, th, menu_w, menu_h);

        /* --- Programs submenu (shown when Programs item is hovered) --- */
        if (g_programs_active || g_start_hover == 0) {
            g_programs_active = 1;
            if (!g_programs_scanned) scan_programs();
            if (g_program_count > 0) {
                int sub_w = 160;
                int sub_x = menu_w;
                int sub_top = th;
                int avail_h = screen_h - sub_top;
                int max_vis = (avail_h - 4) / item_h;
                if (max_vis < 1) max_vis = 1;
                if (max_vis > g_program_count) max_vis = g_program_count;
                /* Clamp scroll offset */
                int max_scroll = g_program_count - max_vis;
                if (max_scroll < 0) max_scroll = 0;
                if (g_programs_scroll > max_scroll) g_programs_scroll = max_scroll;
                if (g_programs_scroll < 0) g_programs_scroll = 0;
                int sub_h = max_vis * item_h + 4;
                int need_scroll = (g_program_count > max_vis);
                /* Up scroll arrow row */
                int arrow_h = need_scroll ? item_h : 0;
                if (need_scroll) sub_h += arrow_h * 2; /* top + bottom arrow rows */
                drawRect(sub_x, sub_top, sub_w, sub_h, 42, 42, 42);
                drawRect(sub_x, sub_top, sub_w, 1, 90, 90, 90);
                int sy = sub_top + 2;
                /* Draw scroll-up indicator */
                if (need_scroll) {
                    int arrow_fg = (g_programs_scroll > 0) ? fg : 80;
                    int ax_center = sub_x + sub_w / 2;
                    /* Upward triangle */
                    for (int row = 0; row < 3; ++row)
                        drawRect(ax_center - row, sy + (arrow_h - ch) / 2 + (2 - row), row * 2 + 1, 1, arrow_fg, arrow_fg, arrow_fg);
                    sy += arrow_h;
                }
                /* Draw visible program items */
                for (int vi = 0; vi < max_vis; ++vi) {
                    int pi = vi + g_programs_scroll;
                    if (pi >= g_program_count) break;
                    if (vi == g_programs_hover)
                        drawRect(sub_x, sy, sub_w, item_h, 60, 60, 60);
                    int nlen = (int)strlen(g_program_names[pi]);
                    int max_ch_sub = (sub_w - 12) / cw;
                    if (nlen > max_ch_sub) nlen = max_ch_sub;
                    for (int j = 0; j < nlen; ++j)
                        drawCharAt(sub_x + 8 + j * cw, sy + (item_h - ch) / 2,
                                   (unsigned char)g_program_names[pi][j], fg, fg, fg);
                    sy += item_h;
                }
                /* Draw scroll-down indicator */
                if (need_scroll) {
                    int arrow_fg = (g_programs_scroll < max_scroll) ? fg : 80;
                    int ax_center = sub_x + sub_w / 2;
                    for (int row = 0; row < 3; ++row)
                        drawRect(ax_center - row, sy + (arrow_h - ch) / 2 + row, row * 2 + 1, 1, arrow_fg, arrow_fg, arrow_fg);
                    sy += arrow_h;
                }
                vga_mark_dirty_rect(sub_x, sub_top, sub_w, sub_h);
            }
        }
    } else {
        g_programs_active = 0;
        g_programs_hover = -1;
    }

    /* --- Dropdown: Overflow list --- */
    if (g_dropdown_active && g_tb_overflow_count > 0) {
        int menu_w = 180;
        int item_h = ch + 6;
        int menu_h = g_tb_overflow_count * item_h + 4;
        int menu_x = g_tb_overflow_x;
        if (menu_x + menu_w > screen_w) menu_x = screen_w - menu_w;
        drawRect(menu_x, th, menu_w, menu_h, 42, 42, 42);
        drawRect(menu_x, th, menu_w, 1, 90, 90, 90);
        int dy = th + 2;
        for (int i = 0; i < g_tb_overflow_count; ++i) {
            const char* name = NULL;
            if (g_tb_overflow[i].kind == 0) {
                int ti = g_tb_overflow[i].index;
                name = (ti >= 0 && ti < tile_count && tiles[ti].title) ? tiles[ti].title : "Shell";
            } else {
                int wi = g_tb_overflow[i].index;
                name = (wi >= 0 && wi < MAX_WINDOWS && g_windows[wi].title) ? g_windows[wi].title : "Window";
            }
            if (i == g_dropdown_hover)
                drawRect(menu_x, dy, menu_w, item_h, 60, 60, 60);
            /* icon: first letter */
            char ic = (name && name[0]) ? name[0] : '?';
            drawCharAt(menu_x + 6, dy + (item_h - ch) / 2, (unsigned char)ic, fg, fg, fg);
            /* title */
            int name_len = name ? (int)strlen(name) : 0;
            if (name_len > 18) name_len = 18;
            for (int j = 0; j < name_len; ++j)
                drawCharAt(menu_x + 6 + cw + 4 + j * cw, dy + (item_h - ch) / 2,
                           (unsigned char)name[j], fg, fg, fg);
            dy += item_h;
        }
        vga_mark_dirty_rect(menu_x, th, menu_w, menu_h);
    }

    vga_mark_dirty_rect(0, 0, screen_w, th);
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
