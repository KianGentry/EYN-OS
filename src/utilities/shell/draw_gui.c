#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <mouse.h>
#include <rei.h>
#include <eynfs.h>
#include <util.h>
#include <string.h>
#include <fs_commands.h> // resolve_path
// current drive helper from shell.c
extern uint8_t get_current_logical_drive(void);
extern char shell_current_path[128];

static const char* get_basename_local(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

typedef struct {
    int img_w, img_h; // image size in pixels
    int depth;        // REI depth (3 = RGB)
    rei_image_t img;  // pixel buffer
    int content_x, content_y, content_w, content_h;
    int offset_x, offset_y; // top-left of canvas in content
    int zoom; // integer zoom: 1,2,3
    int brush; // 1..8 px
    uint8 r,g,b; // current color
    int mouse_down;
    int modified;
    // file
    char filepath[128];
    char filename_base[128];
    uint8 disk;
    // UI state
    enum { UI_PROMPT_SIZE = 0, UI_EDITING = 1 } ui_state;
    int prompt_w, prompt_h; // pending new size before allocation
    int prompt_focus; // 0=width,1=height,2=create,3=cancel
    // Stroke smoothing
    int last_draw_x, last_draw_y; int has_last;
} draw_state_t;

static draw_state_t g_draw;
static int g_draw_tile = -1;

static void draw_put_pixel(int cx, int cy) {
    // Convert content coords to image coords via offset/zoom
    int x = (cx - g_draw.offset_x) / g_draw.zoom;
    int y = (cy - g_draw.offset_y) / g_draw.zoom;
    for (int dy = 0; dy < g_draw.brush; ++dy) {
        for (int dx = 0; dx < g_draw.brush; ++dx) {
            int px = x + dx; int py = y + dy;
            if (px >= 0 && py >= 0 && px < g_draw.img.header.width && py < g_draw.img.header.height) {
                uint32 col = (g_draw.r << 16) | (g_draw.g << 8) | g_draw.b;
                rei_set_pixel_color(&g_draw.img, px, py, col);
                g_draw.modified = 1;
            }
        }
    }
}

static void draw_render_canvas(int x, int y, int w, int h) {
    // Clear content
    drawRect(x, y, w, h, 0, 0, 0);
    // Palette row at top inside content
    const uint8 pal[][3] = {
        {0,0,0},{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{0,255,255}
    };
    int pal_count = sizeof(pal)/sizeof(pal[0]);
    int pal_y = y + 2;
    int pal_x = x + 4;
    int sw = 12, sh = 12, sp = 4;
    for (int i = 0; i < pal_count; ++i) {
        int px = pal_x + i * (sw + sp);
        drawRect(px, pal_y, sw, sh, pal[i][0], pal[i][1], pal[i][2]);
        // highlight current
        if (g_draw.r == pal[i][0] && g_draw.g == pal[i][1] && g_draw.b == pal[i][2]) {
            // simple border
            drawRect(px-1, pal_y-1, sw+2, 1, 200,200,200);
            drawRect(px-1, pal_y+sh, sw+2, 1, 200,200,200);
            drawRect(px-1, pal_y-1, 1, sh+2, 200,200,200);
            drawRect(px+sw, pal_y-1, 1, sh+2, 200,200,200);
        }
    }
    // Tips on the right
    const char* tips = "[S] Save  [Q] Quit  [+/-] Zoom  [[]] Brush";
    int tips_x = x + w - (int)strlen(tips)*8 - 4;
    if (tips_x < x + pal_x) tips_x = pal_x;
    for (int i = 0; tips[i]; ++i) drawCharAt(tips_x + i*8, pal_y+2, (unsigned char)tips[i], 180, 180, 180);

    // Compute placement for canvas below palette
    g_draw.offset_x = x + 4;
    g_draw.offset_y = pal_y + sh + 6;

    // Draw image scaled at offset (use backbuffered drawPixel to avoid flicker)
    for (int py = 0; py < g_draw.img.header.height; ++py) {
        for (int px = 0; px < g_draw.img.header.width; ++px) {
            uint8 rr, gg, bb;
            // sample directly from data
            int off = rei_get_pixel_offset(&g_draw.img.header, px, py);
            if (off >= 0 && off + 2 < (int)g_draw.img.data_size) {
                rr = g_draw.img.data[off + 0];
                gg = g_draw.img.data[off + 1];
                bb = g_draw.img.data[off + 2];
            } else { rr = gg = bb = 0; }
            // Scale
            for (int zy = 0; zy < g_draw.zoom; ++zy) {
                for (int zx = 0; zx < g_draw.zoom; ++zx) {
                    drawPixel(g_draw.offset_x + px * g_draw.zoom + zx,
                              g_draw.offset_y + py * g_draw.zoom + zy,
                              rr, gg, bb);
                }
            }
        }
    }
    // Border around canvas
    int cw = g_draw.img.header.width * g_draw.zoom;
    int ch = g_draw.img.header.height * g_draw.zoom;
    drawRect(g_draw.offset_x - 1, g_draw.offset_y - 1, cw + 2, 1, 80, 80, 80);
    drawRect(g_draw.offset_x - 1, g_draw.offset_y + ch, cw + 2, 1, 80, 80, 80);
    drawRect(g_draw.offset_x - 1, g_draw.offset_y - 1, 1, ch + 2, 80, 80, 80);
    drawRect(g_draw.offset_x + cw, g_draw.offset_y - 1, 1, ch + 2, 80, 80, 80);
}

static void draw_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    (void)userdata;
    g_draw.content_x = content_x; g_draw.content_y = content_y;
    g_draw.content_w = content_w; g_draw.content_h = content_h;
    if (g_draw.ui_state == UI_PROMPT_SIZE) {
        // Dim background
        drawRect(content_x, content_y, content_w, content_h, 0, 0, 0);
        int bw = 200, bh = 80;
        int bx = content_x + (content_w - bw)/2;
        int by = content_y + (content_h - bh)/2;
        drawRect(bx, by, bw, bh, 32, 32, 32);
        const char* title = "New image";
        for (int i = 0; title[i]; ++i) drawCharAt(bx + 8 + i*8, by + 6, (unsigned char)title[i], 255, 255, 255);
        // Width
    const char* wl = "W:";
    for (int i = 0; wl[i]; ++i) drawCharAt(bx + 8 + i*8, by + 24, (unsigned char)wl[i], 220, 220, 220);
    char wbuf[8]; snprintf(wbuf, sizeof(wbuf), "%d", g_draw.prompt_w);
    int wbx = bx + 48; int wby = by + 24; int wbl = 8; for (int t = 0; wbuf[t]; ++t) wbl += 8;
    uint8 fr = (g_draw.prompt_focus==0)?80:48, fg = (g_draw.prompt_focus==0)?80:48, fb = (g_draw.prompt_focus==0)?80:48;
    drawRect(wbx-2, wby-2, wbl+4, 12, fr, fg, fb);
    for (int i = 0; wbuf[i]; ++i) drawCharAt(wbx + i*8, wby, (unsigned char)wbuf[i], 255, 255, 0);
        // Height
        const char* hl = "H:";
        for (int i = 0; hl[i]; ++i) drawCharAt(bx + 8 + i*8, by + 40, (unsigned char)hl[i], 220, 220, 220);
    char hbuf[8]; snprintf(hbuf, sizeof(hbuf), "%d", g_draw.prompt_h);
    int hbx = bx + 48; int hby = by + 40; int hbl = 8; for (int t = 0; hbuf[t]; ++t) hbl += 8;
    fr = (g_draw.prompt_focus==1)?80:48; fg = (g_draw.prompt_focus==1)?80:48; fb = (g_draw.prompt_focus==1)?80:48;
    drawRect(hbx-2, hby-2, hbl+4, 12, fr, fg, fb);
    for (int i = 0; hbuf[i]; ++i) drawCharAt(hbx + i*8, hby, (unsigned char)hbuf[i], 255, 255, 0);
        // Buttons
    drawRect(bx + 30, by + 60, 60, 14, (g_draw.prompt_focus==2)?96:64, (g_draw.prompt_focus==2)?96:64, (g_draw.prompt_focus==2)?96:64);
        const char* create = "Create";
        for (int i = 0; create[i]; ++i) drawCharAt(bx + 40 + i*8, by + 62, (unsigned char)create[i], 255, 255, 255);
    drawRect(bx + 110, by + 60, 60, 14, (g_draw.prompt_focus==3)?96:64, (g_draw.prompt_focus==3)?96:64, (g_draw.prompt_focus==3)?96:64);
        const char* cancel = "Cancel";
        for (int i = 0; cancel[i]; ++i) drawCharAt(bx + 120 + i*8, by + 62, (unsigned char)cancel[i], 255, 255, 255);
    } else {
        draw_render_canvas(content_x, content_y, content_w, content_h);
    }
}

static void draw_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata) {
    (void)userdata;
    if (g_draw.ui_state == UI_PROMPT_SIZE) {
        // Handle click on Create/Cancel
        int bw = 200, bh = 80;
        int bx = g_draw.content_x + (g_draw.content_w - bw)/2;
        int by = g_draw.content_y + (g_draw.content_h - bh)/2;
        int left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
        if (left_down) {
            // Click-to-focus for width/height fields
            char wbuf[8]; snprintf(wbuf, sizeof(wbuf), "%d", g_draw.prompt_w);
            int wbx = bx + 48, wby = by + 24; int wbl = 8; for (int t=0; wbuf[t]; ++t) wbl += 8;
            if (me->x >= wbx-2 && me->x < wbx-2 + wbl + 4 && me->y >= wby-2 && me->y < wby-2 + 12) {
                g_draw.prompt_focus = 0; tile_invalidate_gui(tile_idx); return;
            }
            char hbuf[8]; snprintf(hbuf, sizeof(hbuf), "%d", g_draw.prompt_h);
            int hbx = bx + 48, hby = by + 40; int hbl = 8; for (int t=0; hbuf[t]; ++t) hbl += 8;
            if (me->x >= hbx-2 && me->x < hbx-2 + hbl + 4 && me->y >= hby-2 && me->y < hby-2 + 12) {
                g_draw.prompt_focus = 1; tile_invalidate_gui(tile_idx); return;
            }
            if (me->x >= bx + 30 && me->x < bx + 90 && me->y >= by + 60 && me->y < by + 74) {
                // Create
                // Allocate image
                int size = g_draw.prompt_w * g_draw.prompt_h * REI_DEPTH_RGB;
                if (g_draw.img.data) { free(g_draw.img.data); g_draw.img.data = NULL; }
                g_draw.img.header.magic = REI_MAGIC;
                g_draw.img.header.width = (uint16)g_draw.prompt_w;
                g_draw.img.header.height = (uint16)g_draw.prompt_h;
                g_draw.img.header.depth = REI_DEPTH_RGB;
                g_draw.img.data = (uint8*)malloc(size);
                g_draw.img.data_size = size;
                memset(g_draw.img.data, 0, size);
                g_draw.ui_state = UI_EDITING;
                tile_invalidate_gui(tile_idx);
            } else if (me->x >= bx + 110 && me->x < bx + 170 && me->y >= by + 60 && me->y < by + 74) {
                // Cancel
                if (g_draw_tile >= 0) tile_unregister_gui_client(g_draw_tile);
            }
        }
        return;
    }

    // Palette click detection
    const uint8 pal[][3] = {
        {0,0,0},{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{0,255,255}
    };
    int pal_count = sizeof(pal)/sizeof(pal[0]);
    int pal_y = g_draw.content_y + 2;
    int pal_x = g_draw.content_x + 4;
    int sw = 12, sh = 12, sp = 4;
    int left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    if (left_down) {
        for (int i = 0; i < pal_count; ++i) {
            int px = pal_x + i * (sw + sp);
            if (me->x >= px && me->x < px + sw && me->y >= pal_y && me->y < pal_y + sh) {
                g_draw.r = pal[i][0]; g_draw.g = pal[i][1]; g_draw.b = pal[i][2];
                tile_invalidate_gui(tile_idx);
                return;
            }
        }
    }

    // Check if inside canvas
    int cw = g_draw.img.header.width * g_draw.zoom;
    int ch = g_draw.img.header.height * g_draw.zoom;
    int inside = (me->x >= g_draw.offset_x && me->y >= g_draw.offset_y && me->x < g_draw.offset_x + cw && me->y < g_draw.offset_y + ch);
    // Left draw, right erase
    int right_down = (me->buttons & MOUSE_BUTTON_RIGHT) != 0;
    if (inside && (left_down || right_down)) {
        int was_modified = g_draw.modified;
        uint8 old_r = g_draw.r, old_g = g_draw.g, old_b = g_draw.b;
        if (right_down) { g_draw.r = g_draw.g = g_draw.b = 0; }
        // Smooth strokes by interpolating between last and current mouse positions
        if (!g_draw.has_last) {
            draw_put_pixel(me->x, me->y);
            g_draw.last_draw_x = me->x; g_draw.last_draw_y = me->y; g_draw.has_last = 1;
        } else {
            int x0 = g_draw.last_draw_x, y0 = g_draw.last_draw_y;
            int x1 = me->x, y1 = me->y;
            int dx = x1 - x0, dy = y1 - y0;
            int adx = dx >= 0 ? dx : -dx;
            int ady = dy >= 0 ? dy : -dy;
            int steps = (adx > ady) ? adx : ady;
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; ++s) {
                int ix = x0 + (dx * s) / steps;
                int iy = y0 + (dy * s) / steps;
                draw_put_pixel(ix, iy);
            }
            g_draw.last_draw_x = x1; g_draw.last_draw_y = y1;
        }
        // Set [Modified] in subtitle when first change happens
        if (!was_modified && g_draw.modified) {
            static char title_buf2[128];
            snprintf(title_buf2, sizeof(title_buf2), "%s - Draw", g_draw.filename_base);
            tile_set_title_status(g_draw_tile, title_buf2, g_draw.filename_base, "[Modified] ");
        }
        // restore current color if we temporarily set erase
        g_draw.r = old_r; g_draw.g = old_g; g_draw.b = old_b;
        tile_invalidate_gui(tile_idx);
    } else if (!left_down && !right_down) {
        g_draw.has_last = 0;
    }
}

static void draw_gui_key(int tile_idx, int key, void* userdata) {
    (void)userdata; (void)tile_idx;
    // If in prompt, accept digits/backspace/enter
    if (g_draw.ui_state == UI_PROMPT_SIZE) {
        if (key == '\t') { g_draw.prompt_focus = (g_draw.prompt_focus==0)?1:0; tile_invalidate_gui(tile_idx); return; }
        if (key >= '0' && key <= '9') {
            int d = key - '0';
            if (g_draw.prompt_focus == 0) {
                g_draw.prompt_w = g_draw.prompt_w * 10 + d;
                if (g_draw.prompt_w > REI_MAX_WIDTH) g_draw.prompt_w = REI_MAX_WIDTH;
            } else {
                g_draw.prompt_h = g_draw.prompt_h * 10 + d;
                if (g_draw.prompt_h > REI_MAX_HEIGHT) g_draw.prompt_h = REI_MAX_HEIGHT;
            }
            tile_invalidate_gui(tile_idx);
            return;
        }
        if (key == '\b' || key == 8) {
            if (g_draw.prompt_focus == 0) { g_draw.prompt_w /= 10; if (g_draw.prompt_w <= 0) g_draw.prompt_w = 1; }
            else { g_draw.prompt_h /= 10; if (g_draw.prompt_h <= 0) g_draw.prompt_h = 1; }
            tile_invalidate_gui(tile_idx);
            return;
        }
        if (key == '\n' || key == '\r') {
            // move to height entry if width is set small; here we accept both at once using +/- UI, so Enter creates
            int size = g_draw.prompt_w * g_draw.prompt_h * REI_DEPTH_RGB;
            if (g_draw.img.data) { free(g_draw.img.data); g_draw.img.data = NULL; }
            g_draw.img.header.magic = REI_MAGIC;
            g_draw.img.header.width = (uint16)g_draw.prompt_w;
            g_draw.img.header.height = (uint16)g_draw.prompt_h;
            g_draw.img.header.depth = REI_DEPTH_RGB;
            g_draw.img.data = (uint8*)malloc(size);
            g_draw.img.data_size = size;
            memset(g_draw.img.data, 0, size);
            g_draw.ui_state = UI_EDITING;
            tile_invalidate_gui(tile_idx);
            return;
        }
    }

    // +/- zoom, [ ] brush, S save, Q quit
    int changed = 0;
    if (key == '+' || key == '=') { if (g_draw.zoom < 4) { g_draw.zoom++; changed = 1; } }
    else if (key == '-' || key == '_') { if (g_draw.zoom > 1) { g_draw.zoom--; changed = 1; } }
    else if (key == '[') { if (g_draw.brush > 1) { g_draw.brush--; changed = 1; } }
    else if (key == ']') { if (g_draw.brush < 8) { g_draw.brush++; changed = 1; } }
    else if ((key & 0x20FF) == 's' || (key & 0x20FF) == 'S') {
        // Save to chosen path on current drive via VFS
        uint8 disk = g_draw.disk;
        // Build a contiguous buffer: header + pixel data
        uint32 total = sizeof(rei_header_t) + (uint32)g_draw.img.data_size;
        uint8* tmp = (uint8*)malloc(total);
        if (tmp) {
            memcpy(tmp, &g_draw.img.header, sizeof(rei_header_t));
            memcpy(tmp + sizeof(rei_header_t), g_draw.img.data, g_draw.img.data_size);
            extern int vfs_write_file(uint8 drive, const char* path, const void* buf, uint32 size);
            int w = vfs_write_file(disk, g_draw.filepath, tmp, total);
            free(tmp);
            if (w == (int)total) {
                g_draw.modified = 0;
                static char title_buf[128];
                snprintf(title_buf, sizeof(title_buf), "%s - Draw", g_draw.filename_base);
                tile_set_title_status(g_draw_tile, title_buf, g_draw.filename_base, NULL);
            } else {
                // optional: failure indicator
            }
        }
        changed = 1;
    } else if ((key & 0x20FF) == 'q') {
        // Exit GUI
        if (g_draw_tile >= 0) tile_unregister_gui_client(g_draw_tile);
        return;
    }
    if (changed) tile_invalidate_gui(tile_idx);
}

void draw_cmd(string arg) {
    // Initialize state
    memset(&g_draw, 0, sizeof(g_draw));
    g_draw.zoom = 2; g_draw.brush = 2; g_draw.r = 255; g_draw.g = 255; g_draw.b = 255;
    g_draw.modified = 0;

    // Parse filename from arg; accept either "test.rei" or "draw test.rei"
    char tok1[128]={0}, tok2[128]={0};
    int i = 0; while (arg[i] == ' ') i++;
    int j = 0; while (arg[i] && arg[i] != ' ' && j < (int)sizeof(tok1)-1) tok1[j++] = arg[i++]; tok1[j] = '\0';
    while (arg[i] == ' ') i++;
    j = 0; while (arg[i] && arg[i] != ' ' && j < (int)sizeof(tok2)-1) tok2[j++] = arg[i++]; tok2[j] = '\0';
    const char* filename_arg = NULL;
    if (tok2[0] && (strcmp(tok1, "draw") == 0)) filename_arg = tok2; else if (tok1[0]) filename_arg = tok1;
    if (!filename_arg || !filename_arg[0]) {
        printf("%cUsage: draw <filename.rei>\n", 255,255,255);
        return;
    }
    // Resolve absolute path and disk
    char abspath[128];
    resolve_path(filename_arg, shell_current_path, abspath, sizeof(abspath));
    strncpy(g_draw.filepath, abspath, sizeof(g_draw.filepath)-1);
    g_draw.filepath[sizeof(g_draw.filepath)-1] = '\0';
    // Basename for UI
    const char* base = get_basename_local(g_draw.filepath);
    strncpy(g_draw.filename_base, base, sizeof(g_draw.filename_base)-1);
    g_draw.filename_base[sizeof(g_draw.filename_base)-1] = '\0';
    g_draw.disk = get_current_logical_drive();

    // Prompt for size first
    g_draw.ui_state = UI_PROMPT_SIZE;
    g_draw.prompt_w = 160;
    g_draw.prompt_h = 120;

    // Register GUI on focused tile
    g_draw_tile = tile_get_focused();
    // Title and subtitle bar like Write: left filename, right [Modified]
    static char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s - Draw", g_draw.filename_base);
    tile_set_title_status(g_draw_tile, title_buf, g_draw.filename_base, NULL);
    tile_register_gui_client2(g_draw_tile, draw_gui_draw, draw_gui_key, draw_gui_mouse, NULL);
}

// Register shell command
#include <shell_command_info.h>
REGISTER_SHELL_COMMAND(draw_cmd_info, "draw", draw_cmd, CMD_STREAMING, "Create or edit a REI image with a GUI. Usage: draw <filename.rei>", "draw test.rei");
