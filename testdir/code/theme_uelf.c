#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>
#include <gui.h>

EYN_CMDMETA_V1("Open the theme editor.", "theme /fonts/unscii-16.hex");

typedef struct {
    char path[128];
    char status[96];
    int cursor;
    int handle;
    int running;
} theme_state_t;

static int str_len(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    if (!src) src = "";
    int i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void set_status(theme_state_t* st, const char* msg) {
    str_copy(st->status, (int)sizeof(st->status), msg ? msg : "");
}

static int apply_font(theme_state_t* st, const char* path) {
    if (!path || !path[0]) {
        set_status(st, "empty path");
        return -1;
    }
    if (eyn_sys_setfont_path(path) != 0) {
        set_status(st, "apply failed");
        return -1;
    }
    (void)gui_set_font(st->handle, path);
    set_status(st, "applied");
    return 0;
}

static void handle_key(theme_state_t* st, int key) {
    unsigned ch = (unsigned)key & 0xFFu;
    if (key == 27 || ch == 'q' || ch == 'Q') {
        st->running = 0;
        return;
    }
    if (ch == 'r' || ch == 'R') {
        str_copy(st->path, (int)sizeof(st->path), "builtin");
        st->cursor = str_len(st->path);
        (void)apply_font(st, st->path);
        return;
    }
    if (key == '\n' || key == 13) {
        (void)apply_font(st, st->path);
        return;
    }
    if (key == 8 || key == 127) {
        if (st->cursor > 0) {
            st->cursor--;
            st->path[st->cursor] = '\0';
        }
        return;
    }
    if (ch >= 32 && ch <= 126) {
        if (st->cursor + 1 < (int)sizeof(st->path)) {
            st->path[st->cursor++] = (char)ch;
            st->path[st->cursor] = '\0';
        }
    }
}

static void render(theme_state_t* st) {
    (void)gui_begin(st->handle);
    gui_rgb_t bg = { GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0 };
    (void)gui_clear(st->handle, &bg);

    gui_text_t t1 = { .x = 8, .y = 8, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = "Theme (ring3): set font path" };
    gui_text_t t2 = { .x = 8, .y = 24, .r = GUI_PAL_DIM_R, .g = GUI_PAL_DIM_G, .b = GUI_PAL_DIM_B, ._pad = 0, .text = "Type path, Enter=Apply, R=Reset(builtin), Q=Quit" };
    gui_text_t t3 = { .x = 8, .y = 46, .r = GUI_PAL_TEXT_R, .g = GUI_PAL_TEXT_G, .b = GUI_PAL_TEXT_B, ._pad = 0, .text = st->path };
    gui_text_t t4 = { .x = 8, .y = 64, .r = GUI_PAL_ACCENT_R, .g = GUI_PAL_ACCENT_G, .b = GUI_PAL_ACCENT_B, ._pad = 0, .text = st->status };
    (void)gui_draw_text(st->handle, &t1);
    (void)gui_draw_text(st->handle, &t2);
    (void)gui_draw_text(st->handle, &t3);
    (void)gui_draw_text(st->handle, &t4);
    (void)gui_present(st->handle);
}

static void usage(void) {
    puts("Usage: theme [font-path|builtin]");
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    if (argc >= 2 && argv[1] && argv[1][0]) {
        if (eyn_sys_setfont_path(argv[1]) != 0) {
            printf("theme: failed to apply font: %s\n", argv[1]);
            return 1;
        }
        return 0;
    }

    theme_state_t st;
    memset(&st, 0, sizeof(st));
    str_copy(st.path, (int)sizeof(st.path), "/fonts/unscii-16.hex");
    st.cursor = str_len(st.path);
    st.running = 1;
    set_status(&st, "ready");

    st.handle = gui_attach("Theme", "Enter apply | R reset | Q quit");
    if (st.handle < 0) {
        puts("theme: gui_attach failed");
        return 1;
    }
    (void)gui_set_continuous_redraw(st.handle, 1);

    while (st.running) {
        gui_event_t ev;
        while (gui_poll_event(st.handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                handle_key(&st, ev.a);
            }
        }
        render(&st);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(st.handle, 0);
    return 0;
}
