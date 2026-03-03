#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>
#include <gui.h>

EYN_CMDMETA_V1("Open the stats GUI.", "stats");

typedef struct {
    int prev_total_packets;
    int cpu_pct;
    int mem_pct;
    int disk_pct;
    int jobs;
    int history;
    int drives;
    int logs;
    int crashes;
} stats_state_t;

static const int C24_X[24] = {
    1000, 966, 866, 707, 500, 259, 0, -259, -500, -707, -866, -966,
    -1000, -966, -866, -707, -500, -259, 0, 259, 500, 707, 866, 966
};
static const int C24_Y[24] = {
    0, 259, 500, 707, 866, 966, 1000, 966, 866, 707, 500, 259,
    0, -259, -500, -707, -866, -966, -1000, -966, -866, -707, -500, -259
};

static int clamp_pct(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static void draw_text_line(int h, int x, int y, uint8_t r, uint8_t g, uint8_t b, const char* text) {
    gui_text_t t = { .x = x, .y = y, .r = r, .g = g, .b = b, ._pad = 0, .text = text };
    (void)gui_draw_text(h, &t);
}

static void draw_ring_meter(int h, int cx, int cy, int radius, int pct, uint8_t r, uint8_t g, uint8_t b, const char* label) {
    int filled = (pct * 24 + 99) / 100;
    if (filled < 0) filled = 0;
    if (filled > 24) filled = 24;

    for (int i = 0; i < 24; ++i) {
        int x = cx + (radius * C24_X[i]) / 1000;
        int y = cy + (radius * C24_Y[i]) / 1000;

        gui_rect_t seg = {
            .x = x - 3,
            .y = y - 3,
            .w = 7,
            .h = 7,
            .r = (uint8_t)(i < filled ? r : 38),
            .g = (uint8_t)(i < filled ? g : 38),
            .b = (uint8_t)(i < filled ? b : 44),
            ._pad = 0
        };
        (void)gui_fill_rect(h, &seg);
    }

    char pct_line[24];
    snprintf(pct_line, sizeof(pct_line), "%d%%", pct);
    draw_text_line(h, cx - 14, cy - 4, 235, 240, 250, pct_line);
    draw_text_line(h, cx - 26, cy + radius + 8, 170, 185, 215, label);
}

static void update_stats(stats_state_t* st) {
    if (!st) return;

    st->jobs = eyn_sys_bg_job_count();
    if (st->jobs < 0) st->jobs = 0;

    st->history = eyn_sys_history_count();
    if (st->history < 0) st->history = 0;

    st->drives = eyn_sys_drive_get_count();
    if (st->drives < 0) st->drives = 0;

    st->logs = eyn_sys_shell_log_get();
    if (st->logs < 0) st->logs = 0;

    st->crashes = eyn_sys_crashlog_count();
    if (st->crashes < 0) st->crashes = 0;

    eyn_net_udp_stats_t udp;
    memset(&udp, 0, sizeof(udp));
    (void)eyn_sys_net_get_udp_stats(&udp);

    int total_packets = (int)udp.udp_rx_enqueued + (int)udp.udp_tx_checksums;
    int delta_packets = total_packets - st->prev_total_packets;
    if (delta_packets < 0) delta_packets = 0;
    st->prev_total_packets = total_packets;

    st->cpu_pct = clamp_pct(delta_packets * 4 + st->jobs * 8);
    st->mem_pct = clamp_pct(st->history + st->jobs * 5);
    st->disk_pct = clamp_pct((st->logs / 2) + (st->drives * 8) + (st->crashes * 12));
}

static void draw_stats_window(int h, const stats_state_t* st) {
    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 720;
    if (sz.h <= 0) sz.h = 420;

    (void)gui_begin(h);

    gui_rgb_t bg = { .r = 11, .g = 12, .b = 17, ._pad = 0 };
    (void)gui_clear(h, &bg);

    gui_rect_t head = { .x = 0, .y = 0, .w = sz.w, .h = 30, .r = 44, .g = 50, .b = 78, ._pad = 0 };
    (void)gui_fill_rect(h, &head);
    draw_text_line(h, 10, 9, 255, 255, 255, "EYN-OS Stats Dashboard");

    int chart_y = 108;
    int c0x = sz.w / 6;
    int c1x = sz.w / 2;
    int c2x = (sz.w * 5) / 6;
    if (c0x < 72) c0x = 72;
    if (c2x > sz.w - 72) c2x = sz.w - 72;

    draw_ring_meter(h, c0x, chart_y, 42, st->cpu_pct, 255, 120, 80, "CPU");
    draw_ring_meter(h, c1x, chart_y, 42, st->mem_pct, 105, 210, 255, "MEM");
    draw_ring_meter(h, c2x, chart_y, 42, st->disk_pct, 150, 235, 130, "DISK");

    int list_top = 198;
    if (list_top + 130 > sz.h) list_top = sz.h - 130;
    if (list_top < 136) list_top = 136;

    gui_rect_t list_box = { .x = 10, .y = list_top, .w = sz.w - 20, .h = sz.h - list_top - 26, .r = 24, .g = 27, .b = 38, ._pad = 0 };
    (void)gui_fill_rect(h, &list_box);

    draw_text_line(h, 16, list_top + 8, 255, 228, 150, "Active programs:");

    int row_y = list_top + 24;
    int max_rows = (sz.h - row_y - 28) / 12;
    if (max_rows < 1) max_rows = 1;

    if (st->jobs <= 0) {
        draw_text_line(h, 16, row_y, 180, 190, 210, "(none)");
    } else {
        for (int i = 0; i < st->jobs && i < max_rows; ++i) {
            eyn_bg_job_info_t info;
            memset(&info, 0, sizeof(info));
            if (eyn_sys_bg_job_info(i, &info) != 0) continue;

            char line[152];
            snprintf(line, sizeof(line), "[%d] %-7s %s", info.pid, info.active ? "Running" : "Done", info.command);
            draw_text_line(h, 16, row_y + i * 12, 210, 220, 240, line);
        }
    }

    char footer[180];
    snprintf(footer, sizeof(footer), "jobs=%d  history=%d  drives=%d  logs=%d  crashes=%d   (q/esc quits)",
             st->jobs, st->history, st->drives, st->logs, st->crashes);
    draw_text_line(h, 10, sz.h - 16, 160, 176, 205, footer);

    (void)gui_present(h);
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: stats");
        return 0;
    }

    int h = gui_attach("System Stats", "q/esc quits");
    if (h < 0) {
        puts("stats: failed to attach to GUI");
        return 1;
    }
    (void)gui_set_continuous_redraw(h, 1);

    stats_state_t st;
    memset(&st, 0, sizeof(st));

    int running = 1;
    int frame = 0;
    while (running) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == GUI_EVENT_KEY && (ev.a == 27 || ev.a == 'q' || ev.a == 'Q')) running = 0;
        }

        if ((frame++ % 4) == 0) update_stats(&st);
        draw_stats_window(h, &st);
        usleep(33000);
    }

    if (gui_set_continuous_redraw(h, 0) != 0) {
        puts("stats: warning: failed to disable redraw");
    }

    return 0;
}
