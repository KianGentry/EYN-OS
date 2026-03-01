#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Legacy kernel stats GUI (migrated default is userland 'stats').", "kstats");

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: kstats");
        return 0;
    }

    int h = gui_attach("Legacy Stats", "q/esc quits");
    if (h < 0) {
        puts("kstats: failed to attach GUI");
        return 1;
    }

    int running = 1;
    while (running) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY && (ev.a == 27 || ev.a == 'q' || ev.a == 'Q')) running = 0;
        }

        (void)gui_begin(h);
        gui_rgb_t bg = { .r = 14, .g = 16, .b = 22, ._pad = 0 };
        (void)gui_clear(h, &bg);

        gui_text_t t1 = { .x = 10, .y = 12, .r = 255, .g = 220, .b = 140, ._pad = 0, .text = "Legacy stats command" };
        gui_text_t t2 = { .x = 10, .y = 30, .r = 210, .g = 220, .b = 240, ._pad = 0, .text = "Use 'stats' for the full standalone dashboard." };
        (void)gui_draw_text(h, &t1);
        (void)gui_draw_text(h, &t2);

        (void)gui_present(h);
        usleep(16000);
    }

    return 0;
}
