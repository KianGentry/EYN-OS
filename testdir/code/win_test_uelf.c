#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Open compositor window test.", "win_test");

typedef struct {
    int x;
    int y;
    int w;
    int h;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int z;
} demo_box_t;

static int point_in_box(int x, int y, const demo_box_t* box) {
    if (!box) return 0;
    return (x >= box->x && y >= box->y && x < (box->x + box->w) && y < (box->y + box->h));
}

static void draw_grid(int h, int w, int hgt, int tick) {
    for (int y = 0; y < hgt; y += 24) {
        for (int x = 0; x < w; x += 24) {
            int v = ((x / 24) + (y / 24) + (tick / 10)) & 1;
            gui_rect_t cell = {
                .x = x,
                .y = y,
                .w = 24,
                .h = 24,
                .r = (uint8_t)(v ? GUI_PAL_HEADER_R : GUI_PAL_BG_R),
                .g = (uint8_t)(v ? GUI_PAL_HEADER_G : GUI_PAL_BG_G),
                .b = (uint8_t)(v ? GUI_PAL_HEADER_B : GUI_PAL_BG_B),
                ._pad = 0
            };
            (void)gui_fill_rect(h, &cell);
        }
    }
}

static void draw_box(int h, const demo_box_t* box, int selected) {
    if (!box) return;
    gui_rect_t body = {
        .x = box->x,
        .y = box->y,
        .w = box->w,
        .h = box->h,
        .r = box->r,
        .g = box->g,
        .b = box->b,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &body);

    gui_rect_t title = {
        .x = box->x,
        .y = box->y,
        .w = box->w,
        .h = 14,
        .r = (uint8_t)(selected ? 255 : 220),
        .g = (uint8_t)(selected ? 236 : 210),
        .b = (uint8_t)(selected ? 145 : 180),
        ._pad = 0
    };
    (void)gui_fill_rect(h, &title);
}

static void draw_scene(int h, demo_box_t boxes[3], int active_idx, int mx, int my, int tick) {
    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 640;
    if (sz.h <= 0) sz.h = 360;

    (void)gui_begin(h);

    gui_rgb_t bg = { .r = GUI_PAL_BG_R, .g = GUI_PAL_BG_G, .b = GUI_PAL_BG_B, ._pad = 0 };
    (void)gui_clear(h, &bg);
    draw_grid(h, sz.w, sz.h, tick);

    int idx[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (boxes[idx[i]].z > boxes[idx[j]].z) {
                int t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        int bi = idx[i];
        draw_box(h, &boxes[bi], bi == active_idx);
    }

    gui_text_t title = { .x = 8, .y = 6, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = "Window Test: drag coloured windows, click to raise" };
    (void)gui_draw_text(h, &title);

    char info[120];
    snprintf(info, sizeof(info), "mouse=%d,%d  active=%d  q/esc quits", mx, my, active_idx + 1);
    gui_text_t bottom = { .x = 8, .y = sz.h - 14, .r = 210, .g = 225, .b = 255, ._pad = 0, .text = info };
    (void)gui_draw_text(h, &bottom);

    (void)gui_present(h);
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: win_test");
        return 0;
    }

    int h = gui_create("Window Test", "drag/click to stack");
    if (h < 0) {
        h = gui_attach("Window Test", "drag/click to stack");
    }
    if (h < 0) {
        puts("win_test: failed to initialize GUI");
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    demo_box_t boxes[3];
    boxes[0].x = 80;  boxes[0].y = 80;  boxes[0].w = 190; boxes[0].h = 118; boxes[0].r = 58;  boxes[0].g = 88;  boxes[0].b = 145; boxes[0].z = 1;
    boxes[1].x = 180; boxes[1].y = 120; boxes[1].w = 220; boxes[1].h = 132; boxes[1].r = 112; boxes[1].g = 76;  boxes[1].b = 150; boxes[1].z = 2;
    boxes[2].x = 320; boxes[2].y = 92;  boxes[2].w = 170; boxes[2].h = 110; boxes[2].r = 72;  boxes[2].g = 122; boxes[2].b = 86;  boxes[2].z = 3;

    int running = 1;
    int tick = 0;
    int mx = 0;
    int my = 0;
    int left_down = 0;
    int prev_left_down = 0;
    int drag_idx = -1;
    int drag_dx = 0;
    int drag_dy = 0;
    int z_counter = 3;

    while (running) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY && (ev.a == 27 || ev.a == 'q' || ev.a == 'Q')) {
                running = 0;
            }
            if (ev.type == GUI_EVENT_MOUSE) {
                mx = ev.a;
                my = ev.b;
                left_down = (ev.c & 0x1) ? 1 : 0;
            }
        }

        int press_edge = (left_down && !prev_left_down);
        int release_edge = (!left_down && prev_left_down);
        prev_left_down = left_down;

        if (press_edge) {
            int best = -1;
            int best_z = -2147483647;
            for (int i = 0; i < 3; ++i) {
                if (point_in_box(mx, my, &boxes[i]) && boxes[i].z > best_z) {
                    best = i;
                    best_z = boxes[i].z;
                }
            }
            if (best >= 0) {
                drag_idx = best;
                drag_dx = mx - boxes[best].x;
                drag_dy = my - boxes[best].y;
                boxes[best].z = ++z_counter;
            }
        }

        if (left_down && drag_idx >= 0) {
            boxes[drag_idx].x = mx - drag_dx;
            boxes[drag_idx].y = my - drag_dy;

            if (boxes[drag_idx].x < 0) boxes[drag_idx].x = 0;
            if (boxes[drag_idx].y < 16) boxes[drag_idx].y = 16;
        }

        if (release_edge) {
            drag_idx = -1;
        }

        int active = -1;
        int active_z = -2147483647;
        for (int i = 0; i < 3; ++i) {
            if (boxes[i].z > active_z) {
                active_z = boxes[i].z;
                active = i;
            }
        }

        draw_scene(h, boxes, active, mx, my, tick++);
        usleep(16666);
    }

    if (gui_set_continuous_redraw(h, 0) != 0) {
        puts("win_test: warning: failed to disable redraw");
    }

    return 0;
}
