#ifndef MOUSE_H
#define MOUSE_H

#include <types.h>
#include <rei.h>

// Mouse button definitions
#define MOUSE_BUTTON_LEFT    0x01
#define MOUSE_BUTTON_RIGHT   0x02
#define MOUSE_BUTTON_MIDDLE  0x04
// Extended buttons for IntelliMouse Explorer (if available)
#define MOUSE_BUTTON_4       0x08
#define MOUSE_BUTTON_5       0x10

// Mouse state structure
typedef struct {
    int x, y;           // Mouse position
    int delta_x, delta_y; // Movement delta
    int wheel_delta;    // Accumulated wheel delta since last read (positive=up, negative=down)
    uint8 buttons;      // Button state
    uint8 prev_buttons; // Previous button state
    int initialized;    // Initialization status
} mouse_state_t;

// Mouse event structure
typedef struct {
    int x, y;
    int delta_x, delta_y;
    int wheel_delta;    // Wheel movement since last read
    uint8 buttons;
    uint8 button_changes; // Which buttons changed
} mouse_event_t;

// Function prototypes
int mouse_init(void);
void mouse_cleanup(void);
int mouse_read_event(mouse_event_t* event);
int mouse_get_position(int* x, int* y);
int mouse_get_buttons(uint8* buttons);
void mouse_set_position(int x, int y);
void mouse_set_bounds(int min_x, int min_y, int max_x, int max_y);
// Poll the controller for pending AUX bytes and update state (fallback when IRQ12 isn't firing)
int mouse_poll(void);
// Provide a cursor image to be used for the hardware cursor overlay; pass NULL to use fallback
void mouse_set_cursor_image(const rei_image_t* image);

// Mouse interrupt handler
void mouse_interrupt_handler(void);

// Global mouse state
extern mouse_state_t g_mouse_state;

#endif // MOUSE_H
