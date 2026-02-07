#include <mouse.h>

#include <drivers/aarch64/fb_simple.h>
#include <string.h>

mouse_state_t g_mouse_state = {0};

static int g_min_x = 0;
static int g_min_y = 0;
static int g_max_x = 0;
static int g_max_y = 0;
static const rei_image_t* g_cursor_img = 0;

static void clamp_pos(void) {
    if (g_mouse_state.x < g_min_x) g_mouse_state.x = g_min_x;
    if (g_mouse_state.y < g_min_y) g_mouse_state.y = g_min_y;
    if (g_mouse_state.x > g_max_x) g_mouse_state.x = g_max_x;
    if (g_mouse_state.y > g_max_y) g_mouse_state.y = g_max_y;
}

int mouse_init(void) {
    memset(&g_mouse_state, 0, sizeof(g_mouse_state));
    g_mouse_state.initialized = 1;

    // Default bounds to framebuffer dimensions if available.
    uint64 base = 0;
    uint32 w = 0, h = 0, stride = 0, bpp = 0;
    const char* fmt = 0;
    if (fb_simple_get_info(&base, &w, &h, &stride, &bpp, &fmt) == 0 && w && h) {
        g_min_x = 0;
        g_min_y = 0;
        g_max_x = (int)w - 1;
        g_max_y = (int)h - 1;
        g_mouse_state.x = (int)w / 2;
        g_mouse_state.y = (int)h / 2;
    }
    clamp_pos();
    return 0;
}

void mouse_cleanup(void) {
    g_mouse_state.initialized = 0;
}

int mouse_read_event(mouse_event_t* event) {
    if (!event) return -1;

    event->x = g_mouse_state.x;
    event->y = g_mouse_state.y;
    event->delta_x = g_mouse_state.delta_x;
    event->delta_y = g_mouse_state.delta_y;
    event->wheel_delta = g_mouse_state.wheel_delta;
    event->buttons = g_mouse_state.buttons;
    event->button_changes = (uint8)(g_mouse_state.buttons ^ g_mouse_state.prev_buttons);

    g_mouse_state.prev_buttons = g_mouse_state.buttons;
    g_mouse_state.delta_x = 0;
    g_mouse_state.delta_y = 0;
    g_mouse_state.wheel_delta = 0;
    return 0;
}

int mouse_get_position(int* x, int* y) {
    if (x) *x = g_mouse_state.x;
    if (y) *y = g_mouse_state.y;
    return 0;
}

int mouse_get_buttons(uint8* buttons) {
    if (buttons) *buttons = g_mouse_state.buttons;
    return 0;
}

void mouse_set_position(int x, int y) {
    g_mouse_state.delta_x += (x - g_mouse_state.x);
    g_mouse_state.delta_y += (y - g_mouse_state.y);
    g_mouse_state.x = x;
    g_mouse_state.y = y;
    clamp_pos();
}

void mouse_set_bounds(int min_x, int min_y, int max_x, int max_y) {
    g_min_x = min_x;
    g_min_y = min_y;
    g_max_x = max_x;
    g_max_y = max_y;
    clamp_pos();
}

int mouse_poll(void) {
    // No virtio mouse backend wired yet.
    return 0;
}

void mouse_set_cursor_image(const rei_image_t* image) {
    g_cursor_img = image;
    (void)g_cursor_img;
}

void mouse_interrupt_handler(void) {
    // No-op
}
