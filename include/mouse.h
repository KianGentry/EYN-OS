#ifndef MOUSE_H
#define MOUSE_H

#include <types.h>

// Mouse button definitions
#define MOUSE_BUTTON_LEFT    0x01
#define MOUSE_BUTTON_RIGHT   0x02
#define MOUSE_BUTTON_MIDDLE  0x04

// Mouse state structure
typedef struct {
    int x, y;           // Mouse position
    int delta_x, delta_y; // Movement delta
    uint8 buttons;      // Button state
    uint8 prev_buttons; // Previous button state
    int initialized;    // Initialization status
} mouse_state_t;

// Mouse event structure
typedef struct {
    int x, y;
    int delta_x, delta_y;
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

// Mouse interrupt handler
void mouse_interrupt_handler(void);

// Global mouse state
extern mouse_state_t g_mouse_state;

#endif // MOUSE_H
