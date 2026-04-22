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

#define TASKBAR_TOAST_MAX 6
#define TASKBAR_TOAST_TITLE_MAX 64
#define TASKBAR_TOAST_MESSAGE_MAX 176
#define TASKBAR_TOAST_DEFAULT_TIMEOUT_MS 7000u
#define TASKBAR_TOAST_MIN_TIMEOUT_MS 1000u
#define TASKBAR_TOAST_MAX_TIMEOUT_MS 60000u

typedef struct {
    int used;
    int level;
    uint32 seq;
    uint32 born_tick;
    uint32 timeout_ticks;
    char title[TASKBAR_TOAST_TITLE_MAX];
    char message[TASKBAR_TOAST_MESSAGE_MAX];
} taskbar_toast_t;

static taskbar_toast_t g_taskbar_toasts[TASKBAR_TOAST_MAX];
static uint32 g_taskbar_toast_seq = 0;

static void taskbar_toast_sanitize_copy(char* dst, int cap, const char* src, const char* fallback) {
    if (!dst || cap <= 0) return;

    const char* use = (src && src[0]) ? src : fallback;
    if (!use) use = "";

    int di = 0;
    for (int si = 0; use[si] && di + 1 < cap; ++si) {
        char c = use[si];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        dst[di++] = c;
    }
    dst[di] = '\0';

    if (di == 0 && fallback && fallback[0] && use != fallback) {
        di = 0;
        for (int si = 0; fallback[si] && di + 1 < cap; ++si) {
            char c = fallback[si];
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            dst[di++] = c;
        }
        dst[di] = '\0';
    }
}

static uint32 taskbar_toast_timeout_ticks(uint32 timeout_ms) {
    uint32 hz = sched_get_tick_hz();
    if (!hz) hz = 100;

    uint32 ticks = (timeout_ms * hz + 999u) / 1000u;
    if (ticks == 0) ticks = 1;
    return ticks;
}

static int taskbar_toast_prune(void) {
    uint32 now = sched_get_tick_count();
    int removed = 0;
    for (int i = 0; i < TASKBAR_TOAST_MAX; ++i) {
        taskbar_toast_t* t = &g_taskbar_toasts[i];
        if (!t->used) continue;
        if (t->timeout_ticks == 0) continue;
        if ((uint32)(now - t->born_tick) >= t->timeout_ticks) {
            t->used = 0;
            removed++;
        }
    }
    return removed;
}

static int taskbar_toast_find_slot(void) {
    int free_slot = -1;
    int oldest_slot = 0;
    uint32 oldest_seq = 0xFFFFFFFFu;

    for (int i = 0; i < TASKBAR_TOAST_MAX; ++i) {
        if (!g_taskbar_toasts[i].used) {
            free_slot = i;
            break;
        }
        if (g_taskbar_toasts[i].seq < oldest_seq) {
            oldest_seq = g_taskbar_toasts[i].seq;
            oldest_slot = i;
        }
    }

    if (free_slot >= 0) return free_slot;
    return oldest_slot;
}

static void taskbar_toast_draw_clamped_text(int x,
                                            int y,
                                            const char* text,
                                            int max_chars,
                                            int r,
                                            int g,
                                            int b) {
    if (!text || max_chars <= 0) return;

    int len = (int)strlen(text);
    int clipped = (len > max_chars) ? 1 : 0;
    int draw_len = clipped ? max_chars : len;
    if (clipped && max_chars >= 3) {
        draw_len = max_chars - 3;
    }

    for (int i = 0; i < draw_len; ++i) {
        drawCharAt(x + i * vga_text_cell_w(), y, (unsigned char)text[i], r, g, b);
    }

    if (clipped && max_chars >= 3) {
        drawCharAt(x + draw_len * vga_text_cell_w(), y, (unsigned char)'.', r, g, b);
        drawCharAt(x + (draw_len + 1) * vga_text_cell_w(), y, (unsigned char)'.', r, g, b);
        drawCharAt(x + (draw_len + 2) * vga_text_cell_w(), y, (unsigned char)'.', r, g, b);
    }
}

static void taskbar_draw_toasts(int taskbar_h, int cw, int ch) {
    int removed = taskbar_toast_prune();
    if (removed > 0) {
        // Toast expiration reveals underlying content. Trigger one compositor
        // refresh so stale toast pixels are actively cleaned up.
        g_force_full_redraw = 1;
        g_tiles_full_content_redraw = 1;
    }

    int order[TASKBAR_TOAST_MAX];
    int order_count = 0;

    for (int i = 0; i < TASKBAR_TOAST_MAX; ++i) {
        if (g_taskbar_toasts[i].used) {
            order[order_count++] = i;
        }
    }

    if (order_count == 0) return;

    for (int i = 1; i < order_count; ++i) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && g_taskbar_toasts[order[j]].seq < g_taskbar_toasts[key].seq) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    int card_w = cw * 44;
    int max_w = screen_w - 16;
    if (card_w > max_w) card_w = max_w;
    if (card_w < cw * 20) card_w = cw * 20;

    int card_h = ch * 2 + 14;
    if (card_h < 28) card_h = 28;

    int x = screen_w - card_w - 6;
    int y0 = taskbar_h + 6;
    int max_visible = 3;
    uint32 now = sched_get_tick_count();

    for (int n = 0; n < order_count && n < max_visible; ++n) {
        taskbar_toast_t* t = &g_taskbar_toasts[order[n]];
        int y = y0 + n * (card_h + 5);
        if (y + card_h > screen_h) break;

        int accent_r = 186;
        int accent_g = 186;
        int accent_b = 186;
        if (t->level == 1) {
            accent_r = 156;
            accent_g = 156;
            accent_b = 156;
        } else if (t->level == 2) {
            accent_r = 228;
            accent_g = 228;
            accent_b = 228;
        }

        draw_raised_box(x, y, card_w, card_h, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
        drawRect(x + 2, y + 2, card_w - 4, 2, accent_r, accent_g, accent_b);

        int max_chars = (card_w - 12) / cw;
        if (max_chars < 1) max_chars = 1;

        taskbar_toast_draw_clamped_text(x + 6, y + 6, t->title, max_chars, UI_TEXT_R, UI_TEXT_G, UI_TEXT_B);
        taskbar_toast_draw_clamped_text(x + 6, y + 8 + ch, t->message, max_chars, UI_TEXT_DIM_R, UI_TEXT_DIM_G, UI_TEXT_DIM_B);

        if (t->timeout_ticks > 0) {
            uint32 elapsed = (uint32)(now - t->born_tick);
            if (elapsed > t->timeout_ticks) elapsed = t->timeout_ticks;
            int track_w = card_w - 6;
            uint32 remain = t->timeout_ticks - elapsed;
            int fill_w = (int)((remain * (uint32)track_w) / t->timeout_ticks);
            if (fill_w > 0) {
                drawRect(x + 3, y + card_h - 4, fill_w, 2, accent_r, accent_g, accent_b);
            }
        }

        vga_mark_dirty_rect(x, y, card_w, card_h);
    }
}

int tile_notify_post(const char* title, const char* message, int level, uint32 timeout_ms) {
    if (level < 0 || level > 2) level = 0;
    if (timeout_ms == 0) timeout_ms = TASKBAR_TOAST_DEFAULT_TIMEOUT_MS;
    if (timeout_ms < TASKBAR_TOAST_MIN_TIMEOUT_MS) timeout_ms = TASKBAR_TOAST_MIN_TIMEOUT_MS;
    if (timeout_ms > TASKBAR_TOAST_MAX_TIMEOUT_MS) timeout_ms = TASKBAR_TOAST_MAX_TIMEOUT_MS;

    (void)taskbar_toast_prune();

    int slot = taskbar_toast_find_slot();
    if (slot < 0 || slot >= TASKBAR_TOAST_MAX) return -1;

    taskbar_toast_t* t = &g_taskbar_toasts[slot];
    memset(t, 0, sizeof(*t));

    t->used = 1;
    t->level = level;
    t->seq = ++g_taskbar_toast_seq;
    t->born_tick = sched_get_tick_count();
    t->timeout_ticks = taskbar_toast_timeout_ticks(timeout_ms);
    taskbar_toast_sanitize_copy(t->title, sizeof(t->title), title, "Notification");
    taskbar_toast_sanitize_copy(t->message, sizeof(t->message), message, "(empty)");

    g_force_full_redraw = 1;
    return 0;
}

int tile_notify_dismiss_all(void) {
    int cleared = 0;
    for (int i = 0; i < TASKBAR_TOAST_MAX; ++i) {
        if (g_taskbar_toasts[i].used) {
            g_taskbar_toasts[i].used = 0;
            cleared++;
        }
    }

    if (cleared > 0) {
        g_force_full_redraw = 1;
        g_tiles_full_content_redraw = 1;
    }
    return cleared;
}

/* (Context menu state declared near top of file with other globals.) */

/*
 * Cached list of program names from /binaries.
 * Scanned once on first open; refreshed on each start-menu toggle.
 */
#define PROGRAMS_MAX 128
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

    int fg = UI_TEXT_R;
    int fg_dim = UI_TEXT_DIM_R;

    /* Background panel */
    draw_raised_box(0, 0, screen_w, th, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);

    /* --- Left: Start button "!" --- */
    int start_w = cw + 10;
    if (g_start_active)
        draw_sunken_box(2, 1, start_w, th - 2, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
    else
        draw_raised_box(2, 1, start_w, th - 2, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
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
            draw_sunken_box(tx, 1, bw, th - 2, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
        else
            draw_raised_box(tx, 1, bw, th - 2, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);

        /* Icon: first letter of title */
        char icon_ch = apps[i].title[0];
        int app_fg = is_active ? fg : fg_dim;
        drawCharAt(tx + 3, text_y, (unsigned char)icon_ch, app_fg, app_fg, app_fg);

        /* Title text (after icon) */
        int text_start = tx + 3 + cw + 2;
        for (int c = 1; c < title_len; ++c) {
            int px = text_start + (c - 1) * cw;
            if (px + cw > tx + bw - 2) break;
            drawCharAt(px, text_y, (unsigned char)apps[i].title[c], app_fg, app_fg, app_fg);
        }

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
        if (g_dropdown_active)
            draw_sunken_box(tx, 1, overflow_icon_w, th - 2, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
        else
            draw_raised_box(tx, 1, overflow_icon_w, th - 2, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
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
        if (is_cur)
            draw_sunken_box(px, 2, pill_w, th - 4, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
        else
            draw_raised_box(px, 2, pill_w, th - 4, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
        char digit = '1' + (char)d;
        int dx = px + (pill_w - cw) / 2;
        drawCharAt(dx, text_y, digit,
                   is_cur ? UI_TEXT_R : UI_TEXT_DIM_R,
                   is_cur ? UI_TEXT_G : UI_TEXT_DIM_G,
                   is_cur ? UI_TEXT_B : UI_TEXT_DIM_B);
    }

    /* --- Dropdown: Start menu --- */
    if (g_start_active) {
        int menu_w = 150;
        int item_h = ch + 6;
        const char* items[] = {"Programs", "Shell", "Files", "Settings", "Reboot", "Shutdown"};
        int item_count = 6;
        int menu_h = item_count * item_h + 4;
        draw_raised_box(0, th, menu_w, menu_h, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
        int dy = th + 2;
        for (int i = 0; i < item_count; ++i) {
            if (i == g_start_hover)
                draw_sunken_box(2, dy, menu_w - 4, item_h, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
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
                draw_raised_box(sub_x, sub_top, sub_w, sub_h, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
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
                        draw_sunken_box(sub_x + 2, sy, sub_w - 4, item_h, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
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
        draw_raised_box(menu_x, th, menu_w, menu_h, UI_SURFACE_DARK_R, UI_SURFACE_DARK_G, UI_SURFACE_DARK_B);
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
                draw_sunken_box(menu_x + 2, dy, menu_w - 4, item_h, UI_SURFACE_R, UI_SURFACE_G, UI_SURFACE_B);
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

    taskbar_draw_toasts(th, cw, ch);

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
            (void)tile_set_background_from_image(ti, g_bg_modal.img, mode);
            g_bg_modal.img = NULL;
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
