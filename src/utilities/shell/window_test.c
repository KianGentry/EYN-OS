#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <string.h>
#include <shell_command_info.h>
#include <stdlib.h>

static void test_draw(int tile_idx, int cx, int cy, int cw, int ch, void* ud) {
    (void)tile_idx; (void)ud;
    // simple pattern: checkerboard
    vga_mark_dirty_rect(cx, cy, cw, ch);
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            int v = (((x>>4) ^ (y>>4)) & 1) ? 60 : 30;
            vga_drawPixel_bb(cx + x, cy + y, v, v, v);
        }
    }
}

static void test_key(int tile_idx, int key, void* ud) {
    (void)tile_idx; (void)ud;
    if (key == 0x2101 || key == 0x2002) { // Ctrl+Q or Ctrl+X closes the window
        int* pwid = (int*)ud;
        if (pwid && *pwid >= 0) wm_close_window(*pwid);

        if (pwid) {
            free(pwid);
        }
    }
}

static void test_mouse(int tile_idx, const mouse_event_t* me, void* ud) {
    (void)tile_idx; (void)ud; (void)me;
}

void win_test_cmd(string ch) {
    (void)ch;
    int w = 300, h = 200, x = 80, y = 60;
    static char status[] = "Ctrl+X: Close";
    int wid = wm_create_window("Window Test", x, y, w, h, status);
    if (wid >= 0) {
        int* pwid = (int*)malloc(sizeof(int));
        if (pwid) {
            *pwid = wid;
            wm_register_gui_client2(wid, test_draw, test_key, test_mouse, (void*)pwid);
        } else {
            wm_register_gui_client2(wid, test_draw, test_key, test_mouse, NULL);
        }
        wm_invalidate_window(wid);
    }
}

REGISTER_SHELL_COMMAND(win_test_cmd_info, "win_test", win_test_cmd, CMD_STREAMING, "Open a sample floating window to test compositor performance.", "win_test");
