#include <eynos_syscall.h>
#include <gui.h>

int gui_create(const char* title, const char* status_left) {
    return eyn_syscall3_pii(EYN_SYSCALL_GUI_CREATE, title, (int)status_left, 0);
}

int gui_set_title(int handle, const char* title) {
    return eyn_syscall3(EYN_SYSCALL_GUI_SET_TITLE, handle, title, 0);
}

int gui_attach(const char* title, const char* status_left) {
    return eyn_syscall3_pii(EYN_SYSCALL_GUI_ATTACH, title, (int)status_left, 0);
}

int gui_begin(int handle) {
    return eyn_syscall1(EYN_SYSCALL_GUI_BEGIN, handle);
}

int gui_clear(int handle, const gui_rgb_t* rgb) {
    return eyn_syscall3(EYN_SYSCALL_GUI_CLEAR, handle, rgb, 0);
}

int gui_fill_rect(int handle, const gui_rect_t* rect) {
    return eyn_syscall3(EYN_SYSCALL_GUI_FILL_RECT, handle, rect, 0);
}

int gui_draw_text(int handle, const gui_text_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_TEXT, handle, cmd, 0);
}

int gui_draw_line(int handle, const gui_line_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_LINE, handle, cmd, 0);
}

int gui_present(int handle) {
    return eyn_syscall1(EYN_SYSCALL_GUI_PRESENT, handle);
}

int gui_get_content_size(int handle, gui_size_t* out_size) {
    return eyn_syscall3(EYN_SYSCALL_GUI_GET_CONTENT_SIZE, handle, out_size, 0);
}

int gui_set_font(int handle, const char* hex_path) {
    return eyn_syscall3(EYN_SYSCALL_GUI_SET_FONT, handle, hex_path, 0);
}

int gui_load_font(int handle, const char* font_path) {
    return eyn_syscall3(EYN_SYSCALL_GUI_LOAD_FONT, handle, font_path, 0);
}

int gui_draw_text_font(int handle, const gui_text_font_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_TEXT_FONT, handle, cmd, 0);
}

int gui_draw_char_font(int handle, const gui_char_font_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_CHAR_FONT, handle, cmd, 0);
}

int gui_set_continuous_redraw(int handle, int enabled) {
    return eyn_syscall3(EYN_SYSCALL_GUI_SET_CONTINUOUS_REDRAW, handle, (const void*)enabled, 0);
}

int gui_blit_rgb565(int handle, const gui_blit_rgb565_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_BLIT_RGB565, handle, cmd, 0);
}

int gui_draw_icon(int handle, const gui_icon_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_ICON, handle, cmd, 0);
}

int gui_outline_rect(int handle, const gui_rect_t* rect) {
    return eyn_syscall3(EYN_SYSCALL_GUI_OUTLINE_RECT, handle, rect, 0);
}

int gui_draw_char(int handle, const gui_char_t* cmd) {
    return eyn_syscall3(EYN_SYSCALL_GUI_DRAW_CHAR, handle, cmd, 0);
}

int gui_get_font_metrics(int handle, gui_font_metrics_t* out) {
    return eyn_syscall3(EYN_SYSCALL_GUI_GET_FONT_METRICS, handle, out, 0);
}

int gui_poll_event(int handle, gui_event_t* out_event) {
    return eyn_syscall3(EYN_SYSCALL_GUI_POLL_EVENT, handle, out_event, (int)sizeof(*out_event));
}

int gui_wait_event(int handle, gui_event_t* out_event) {
    return eyn_syscall3(EYN_SYSCALL_GUI_WAIT_EVENT, handle, out_event, (int)sizeof(*out_event));
}

int gui_warp_mouse(int handle, int x, int y) {
    return eyn_syscall3(EYN_SYSCALL_GUI_WARP_MOUSE, handle, (const void*)x, y);
}

int gui_set_cursor_visible(int handle, int visible) {
    return eyn_syscall3(EYN_SYSCALL_GUI_SET_CURSOR_VISIBLE, handle, (const void*)visible, 0);
}
