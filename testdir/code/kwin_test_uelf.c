#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Legacy kernel window-test command (migrated default is userland 'win_test').", "kwin_test");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: kwin_test");
        return 0;
    }

    int h = gui_create("Legacy Window Test", "q/esc quits");
    if (h < 0) h = gui_attach("Legacy Window Test", "q/esc quits");
    if (h < 0) {
        puts("kwin_test: failed to initialize GUI");
        return 1;
    }

    int x = 80;
    int dx = 2;
    int running = 1;
    while (running) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY && (ev.a == 27 || ev.a == 'q' || ev.a == 'Q')) running = 0;
        }

        if (x < 20) dx = 2;
        if (x > 420) dx = -2;
        x += dx;

        (void)gui_begin(h);
        gui_rgb_t bg = { .r = GUI_PAL_BG_R, .g = GUI_PAL_BG_G, .b = GUI_PAL_BG_B, ._pad = 0 };
        (void)gui_clear(h, &bg);

        gui_rect_t r = { .x = x, .y = 90, .w = 140, .h = 90, .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0 };
        (void)gui_fill_rect(h, &r);

        gui_text_t t = { .x = 10, .y = 10, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = "Legacy window test command" };
        (void)gui_draw_text(h, &t);

        (void)gui_present(h);
        usleep(16000);
    }

    return 0;
}
