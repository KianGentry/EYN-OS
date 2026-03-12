#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Draw a rectangle.", "rect 10 20 100 50 255 0 0");

static void usage(void) {
    puts("Usage: rect <x> <y> <width> <height> <r> <g> <b>");
}

static int parse_i(const char* s, int* out) {
    if (!s || !s[0] || !out) return -1;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    if (argc < 8) {
        usage();
        return 1;
    }

    int x, y, w, h, r, g, b;
    if (parse_i(argv[1], &x) != 0 || parse_i(argv[2], &y) != 0 ||
        parse_i(argv[3], &w) != 0 || parse_i(argv[4], &h) != 0 ||
        parse_i(argv[5], &r) != 0 || parse_i(argv[6], &g) != 0 ||
        parse_i(argv[7], &b) != 0) {
        usage();
        return 1;
    }

    if (w <= 0 || h <= 0) {
        puts("rect: width/height must be > 0");
        return 1;
    }
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    int handle = gui_attach("Rectangle", "q/esc quits");
    if (handle < 0) {
        puts("rect: failed to attach GUI");
        return 1;
    }

    gui_rgb_t bg = { .r = GUI_PAL_BG_R, .g = GUI_PAL_BG_G, .b = GUI_PAL_BG_B, ._pad = 0 };
    gui_rect_t rect = { .x = x, .y = y, .w = w, .h = h, .r = (uint8_t)r, .g = (uint8_t)g, .b = (uint8_t)b, ._pad = 0 };

    int running = 1;
    while (running) {
        gui_event_t ev;
        while (gui_poll_event(handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY && (ev.a == 27 || ev.a == 'q' || ev.a == 'Q')) running = 0;
        }

        (void)gui_begin(handle);
        (void)gui_clear(handle, &bg);
        (void)gui_fill_rect(handle, &rect);
        (void)gui_present(handle);
        usleep(16000);
    }

    return 0;
}
