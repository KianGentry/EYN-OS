#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <rei.h>
#include <eynfs.h>
#include <fs_commands.h>
#include <string.h>
#include <stdlib.h>
#include <mouse.h>
#ifndef EYNFS_SUPERBLOCK_LBA
#define EYNFS_SUPERBLOCK_LBA 2048
#endif

extern uint8_t get_current_logical_drive(void);
extern char shell_current_path[128];

typedef struct {
    rei_image_t img;
    int content_x, content_y, content_w, content_h;
    int zoom; // 1..8
    int off_x, off_y; // pan offset in screen pixels
    int dragging; int drag_start_x, drag_start_y; int drag_off_x, drag_off_y;
    char filepath[128];
    char filename_base[128];
    int tile_idx;
    // Window mode support
    int is_window; // 0=tile, 1=window
    int window_id; // valid when is_window
    char status_left[128];
} viewer_t;

static viewer_t g_view;

static const char* get_basename_local(const char* path) {
    const char* last = path; for (const char* p = path; *p; ++p) if (*p=='/') last = p+1; return last;
}

static void viewer_draw_image() {
    // Clear content area
    drawRect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h, 0,0,0);
    if (!g_view.img.data) return;
    // Draw image with zoom and pan
    int ox = g_view.content_x + g_view.off_x;
    int oy = g_view.content_y + g_view.off_y;
    // Mark content area dirty once; we'll draw many pixels without per-pixel dirty marks
    vga_mark_dirty_rect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h);
    // Compute visible image range in source pixels
    int start_x = (g_view.content_x - ox) / g_view.zoom; if (start_x < 0) start_x = 0;
    int start_y = (g_view.content_y - oy) / g_view.zoom; if (start_y < 0) start_y = 0;
    int end_x = (g_view.content_x + g_view.content_w - 1 - ox) / g_view.zoom + 1; if (end_x > g_view.img.header.width) end_x = g_view.img.header.width;
    int end_y = (g_view.content_y + g_view.content_h - 1 - oy) / g_view.zoom + 1; if (end_y > g_view.img.header.height) end_y = g_view.img.header.height;
    for (int y = start_y; y < end_y; ++y) {
        for (int x = start_x; x < end_x; ++x) {
            int off = rei_get_pixel_offset(&g_view.img.header, x, y);
            if (off < 0) continue;
            uint8 r = 0, g = 0, b = 0;
            if (g_view.img.header.depth == REI_DEPTH_MONO) {
                if (off >= (int)g_view.img.data_size) continue;
                uint8 gray = g_view.img.data[off];
                r = g = b = gray;
            } else if (g_view.img.header.depth == REI_DEPTH_RGB || g_view.img.header.depth == REI_DEPTH_RGBA) {
                if (off + 2 >= (int)g_view.img.data_size) continue;
                r = g_view.img.data[off+0];
                g = g_view.img.data[off+1];
                b = g_view.img.data[off+2];
            } else {
                // Unsupported depth
                continue;
            }
            for (int zy=0; zy<g_view.zoom; ++zy) for (int zx=0; zx<g_view.zoom; ++zx) {
                int px = ox + x*g_view.zoom + zx;
                int py = oy + y*g_view.zoom + zy;
                if (px>=g_view.content_x && py>=g_view.content_y && px<g_view.content_x+g_view.content_w && py<g_view.content_y+g_view.content_h)
                    vga_drawPixel_bb(px, py, r,g,b);
            }
        }
    }
}

static void viewer_gui_draw(int tile_idx, int cx, int cy, int cw, int ch, void* ud) {
    (void)ud; g_view.tile_idx = tile_idx; g_view.content_x=cx; g_view.content_y=cy; g_view.content_w=cw; g_view.content_h=ch;
    viewer_draw_image();
}

static void viewer_gui_key(int tile_idx, int key, void* ud) {
    (void)ud; (void)tile_idx; int changed=0;
    if (key==0x2102) { if (g_view.zoom<8) { g_view.zoom++; changed=1; } } // Ctrl+Plus
    else if (key==0x2103) { if (g_view.zoom>1) { g_view.zoom--; changed=1; } } // Ctrl+Minus
    else if (key==0x2002) { // Ctrl+X closes
        if (g_view.is_window) {
            wm_close_window(g_view.window_id);
        } else if (g_view.tile_idx>=0) {
            tile_unregister_gui_client(g_view.tile_idx);
        }
        return;
    }
    if (changed) {
        if (g_view.is_window) wm_invalidate_window(g_view.window_id);
        else tile_invalidate_gui(g_view.tile_idx);
    }
}

static void viewer_gui_mouse(int tile_idx, const mouse_event_t* me, void* ud) {
    (void)ud; (void)tile_idx;
    int left = (me->buttons & MOUSE_BUTTON_LEFT)!=0;
    if (left && !g_view.dragging) { g_view.dragging=1; g_view.drag_start_x=me->x; g_view.drag_start_y=me->y; g_view.drag_off_x=g_view.off_x; g_view.drag_off_y=g_view.off_y; }
    else if (!left && g_view.dragging) { g_view.dragging=0; }
    if (g_view.dragging) {
        g_view.off_x = g_view.drag_off_x + (me->x - g_view.drag_start_x);
        g_view.off_y = g_view.drag_off_y + (me->y - g_view.drag_start_y);
        if (g_view.is_window) wm_invalidate_window(g_view.window_id);
        else tile_invalidate_gui(g_view.tile_idx);
    }
}

static void open_viewer_gui(const char* path) {
    memset(&g_view, 0, sizeof(g_view)); g_view.zoom=1; g_view.off_x=4; g_view.off_y=4;
    strncpy(g_view.filepath, path, sizeof(g_view.filepath)-1);
    const char* b = get_basename_local(path); strncpy(g_view.filename_base, b, sizeof(g_view.filename_base)-1);
    g_view.is_window = 0; g_view.window_id = -1;
    snprintf(g_view.status_left, sizeof(g_view.status_left), "Ctrl+Plus/Minus: Zoom | Ctrl+X: Close");
    // Load image file from EYNFS
    uint8 disk = get_current_logical_drive();
    eynfs_superblock_t sb; eynfs_dir_entry_t entry; uint32_t pb, ei;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        if (eynfs_traverse_path(disk, &sb, path, &entry, &pb, &ei) == 0 && entry.type == EYNFS_TYPE_FILE) {
            uint32 sz = entry.size; if (sz > 65536) sz = 65536;
            uint8* buf = (uint8*)malloc(sz);
            if (buf) {
                int br = eynfs_read_file(disk, &sb, &entry, buf, sz, 0);
                if (br > 0 && rei_parse_image(buf, br, &g_view.img) == 0) {
                    // Ready
                } else {
                    // Failed parse; leave img.data NULL
                }
                free(buf);
            }
        }
    }
    static char title_buf[128]; snprintf(title_buf, sizeof(title_buf), "%s - Viewer", g_view.filename_base);
    int t = tile_create_gui_tile(title_buf, g_view.status_left);
    if (t >= 0) {
        tile_register_gui_client2(t, viewer_gui_draw, viewer_gui_key, viewer_gui_mouse, NULL);
    }
}

static void open_viewer_window(const char* path) {
    memset(&g_view, 0, sizeof(g_view)); g_view.zoom=1; g_view.off_x=4; g_view.off_y=4;
    strncpy(g_view.filepath, path, sizeof(g_view.filepath)-1);
    const char* b = get_basename_local(path); strncpy(g_view.filename_base, b, sizeof(g_view.filename_base)-1);
    g_view.is_window = 1; g_view.window_id = -1;
    snprintf(g_view.status_left, sizeof(g_view.status_left), "Ctrl+Plus/Minus: Zoom | Ctrl+X: Close");
    // Load image file from EYNFS
    uint8 disk = get_current_logical_drive();
    eynfs_superblock_t sb; eynfs_dir_entry_t entry; uint32_t pb, ei;
    if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        if (eynfs_traverse_path(disk, &sb, path, &entry, &pb, &ei) == 0 && entry.type == EYNFS_TYPE_FILE) {
            uint32 sz = entry.size; if (sz > 1<<20) sz = 1<<20; // cap 1MB
            uint8* buf = (uint8*)malloc(sz);
            if (buf) {
                int br = eynfs_read_file(disk, &sb, &entry, buf, sz, 0);
                if (br > 0 && rei_parse_image(buf, br, &g_view.img) == 0) {
                    // parsed
                }
                free(buf);
            }
        }
    }
    static char title_buf[128]; snprintf(title_buf, sizeof(title_buf), "%s - Viewer", g_view.filename_base);
    // Create a reasonable default window size and position
    int win_w = 360, win_h = 280; int win_x = 40, win_y = 40;
    int wid = wm_create_window(title_buf, win_x, win_y, win_w, win_h, g_view.status_left);
    if (wid >= 0) {
        g_view.window_id = wid;
        wm_register_gui_client2(wid, viewer_gui_draw, viewer_gui_key, viewer_gui_mouse, NULL);
        wm_set_title_status(wid, title_buf, g_view.status_left, NULL);
        wm_invalidate_window(wid);
    }
}

// Command entry points
#include <shell_command_info.h>
void view_cmd(string ch) {
    uint8 i=0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i]==' ') i++;
    if (!ch[i]) { printf("%cUsage: view <file.rei>\n", 255,255,255); return; }
    char arg[128]={0}; uint8 j=0; while (ch[i] && ch[i] != ' ' && j<127) arg[j++]=ch[i++]; arg[j]='\0';
    char abspath[128]; resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    open_viewer_gui(abspath);
}
REGISTER_SHELL_COMMAND(view_cmd_info, "view", view_cmd, CMD_STREAMING, "Open a REI image in a GUI viewer.\nUsage: view <file.rei>", "view eynos.rei");

// Window variant: open viewer in a floating window
void vieww_cmd(string ch) {
    uint8 i=0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i]==' ') i++;
    if (!ch[i]) { printf("%cUsage: vieww <file.rei>\n", 255,255,255); return; }
    char arg[128]={0}; uint8 j=0; while (ch[i] && ch[i] != ' ' && j<127) arg[j++]=ch[i++]; arg[j]='\0';
    char abspath[128]; resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    open_viewer_window(abspath);
}
REGISTER_SHELL_COMMAND(vieww_cmd_info, "vieww", vieww_cmd, CMD_STREAMING, "Open a REI image in a floating window.\nUsage: vieww <file.rei>", "vieww eynos.rei");
