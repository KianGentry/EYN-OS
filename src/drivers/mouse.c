#include <mouse.h>
#include <system.h>
#include <vga.h>
#include <isr.h>
#include <string.h>

// PS/2 Mouse ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 Mouse commands
#define MOUSE_CMD_RESET          0xFF
#define MOUSE_CMD_RESEND         0xFE
#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_DISABLE        0xF5
#define MOUSE_CMD_ENABLE         0xF4
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_GET_DEVICE_ID  0xF2
#define MOUSE_CMD_SET_REMOTE_MODE 0xF0
#define MOUSE_CMD_SET_WRAP_MODE  0xEE
#define MOUSE_CMD_RESET_WRAP_MODE 0xEC
#define MOUSE_CMD_READ_DATA      0xEB
#define MOUSE_CMD_SET_STREAM_MODE 0xEA
#define MOUSE_CMD_STATUS_REQUEST 0xE9
#define MOUSE_CMD_SET_RESOLUTION 0xE8
#define MOUSE_CMD_SET_SCALING_2_1 0xE7
#define MOUSE_CMD_SET_SCALING_1_1 0xE6

// PS/2 Mouse responses
#define MOUSE_ACK        0xFA
#define MOUSE_NACK       0xFE
#define MOUSE_ERROR      0xFC

// Mouse packet structure (3-byte packets)
#define MOUSE_PACKET_SIZE 3

// Global mouse state
mouse_state_t g_mouse_state = {0};

// Mouse bounds
static int mouse_min_x = 0, mouse_min_y = 0;
static int mouse_max_x = 79, mouse_max_y = 24; // VGA text mode bounds

// Mouse packet buffer
static uint8 mouse_packet_buffer[MOUSE_PACKET_SIZE];
static int mouse_packet_index = 0;

// Wait for mouse to be ready
static int mouse_wait_for_ready(void) {
    int timeout = 100000;
    while (timeout--) {
        if (!(inportb(PS2_STATUS_PORT) & 0x02)) {
            return 0; // Ready
        }
    }
    return -1; // Timeout
}

// Wait for mouse data
static int mouse_wait_for_data(void) {
    int timeout = 100000;
    while (timeout--) {
        if (inportb(PS2_STATUS_PORT) & 0x01) {
            return 0; // Data available
        }
    }
    return -1; // Timeout
}

// Send command to mouse
static int mouse_send_command(uint8 command) {
    if (mouse_wait_for_ready() != 0) return -1;
    
    outportb(PS2_COMMAND_PORT, 0xD4); // Tell controller we're sending to mouse
    if (mouse_wait_for_ready() != 0) return -1;
    
    outportb(PS2_DATA_PORT, command);
    
    // Wait for ACK
    if (mouse_wait_for_data() != 0) return -1;
    uint8 response = inportb(PS2_DATA_PORT);
    
    return (response == MOUSE_ACK) ? 0 : -1;
}

// Read byte from mouse
static int mouse_read_byte(uint8* byte) {
    if (mouse_wait_for_data() != 0) return -1;
    *byte = inportb(PS2_DATA_PORT);
    return 0;
}

// Initialize mouse
int mouse_init(void) {
    memset(&g_mouse_state, 0, sizeof(mouse_state_t));
    
    // Enable mouse in PS/2 controller
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0xA8); // Enable mouse
    
    // Enable mouse interrupts
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0x20); // Read command byte
    if (mouse_wait_for_data() != 0) return -1;
    uint8 status = inportb(PS2_DATA_PORT);
    
    status |= 0x02; // Enable mouse interrupt
    status |= 0x01; // Enable keyboard interrupt
    
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0x60); // Write command byte
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_DATA_PORT, status);
    
    // Reset mouse
    if (mouse_send_command(MOUSE_CMD_RESET) != 0) {
        printf("Mouse reset failed\n");
        return -1;
    }
    
    // Wait for reset response
    if (mouse_wait_for_data() != 0) return -1;
    uint8 response = inportb(PS2_DATA_PORT);
    if (response != 0xAA) {
        printf("Mouse reset response error: 0x%02X\n", response);
        return -1;
    }
    
    // Wait for device ID
    if (mouse_wait_for_data() != 0) return -1;
    response = inportb(PS2_DATA_PORT);
    printf("Mouse device ID: 0x%02X\n", response);
    
    // Set default settings
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) != 0) {
        printf("Mouse set defaults failed\n");
        return -1;
    }
    
    // Enable mouse
    if (mouse_send_command(MOUSE_CMD_ENABLE) != 0) {
        printf("Mouse enable failed\n");
        return -1;
    }
    
    // Set sample rate to 100 (for better responsiveness)
    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE) != 0) {
        printf("Mouse set sample rate failed\n");
        return -1;
    }
    if (mouse_send_command(100) != 0) {
        printf("Mouse sample rate value failed\n");
        return -1;
    }
    
    // Initialize mouse position to center of screen
    g_mouse_state.x = mouse_max_x / 2;
    g_mouse_state.y = mouse_max_y / 2;
    g_mouse_state.initialized = 1;
    
    // Register mouse interrupt handler
    register_interrupt_handler(12, mouse_interrupt_handler); // IRQ 12 is mouse
    
    printf("Mouse initialized successfully\n");
    return 0;
}

// Cleanup mouse
void mouse_cleanup(void) {
    if (g_mouse_state.initialized) {
        mouse_send_command(MOUSE_CMD_DISABLE);
        g_mouse_state.initialized = 0;
    }
}

// Mouse interrupt handler
void mouse_interrupt_handler(void) {
    if (!g_mouse_state.initialized) return;
    
    // Read mouse data
    uint8 data;
    if (mouse_read_byte(&data) != 0) return;
    
    // Store in packet buffer
    mouse_packet_buffer[mouse_packet_index] = data;
    mouse_packet_index++;
    
    // Process complete packet
    if (mouse_packet_index >= MOUSE_PACKET_SIZE) {
        mouse_packet_index = 0;
        
        // Parse mouse packet
        uint8 status = mouse_packet_buffer[0];
        int8 delta_x = (int8)mouse_packet_buffer[1];
        int8 delta_y = (int8)mouse_packet_buffer[2];
        
        // Update button state
        g_mouse_state.prev_buttons = g_mouse_state.buttons;
        g_mouse_state.buttons = status & 0x07;
        
        // Update position
        g_mouse_state.delta_x = delta_x;
        g_mouse_state.delta_y = delta_y;
        
        g_mouse_state.x += delta_x;
        g_mouse_state.y -= delta_y; // Invert Y axis
        
        // Clamp to bounds
        if (g_mouse_state.x < mouse_min_x) g_mouse_state.x = mouse_min_x;
        if (g_mouse_state.x > mouse_max_x) g_mouse_state.x = mouse_max_x;
        if (g_mouse_state.y < mouse_min_y) g_mouse_state.y = mouse_min_y;
        if (g_mouse_state.y > mouse_max_y) g_mouse_state.y = mouse_max_y;
    }
}

// Read mouse event
int mouse_read_event(mouse_event_t* event) {
    if (!g_mouse_state.initialized || !event) return -1;
    
    event->x = g_mouse_state.x;
    event->y = g_mouse_state.y;
    event->delta_x = g_mouse_state.delta_x;
    event->delta_y = g_mouse_state.delta_y;
    event->buttons = g_mouse_state.buttons;
    event->button_changes = g_mouse_state.buttons ^ g_mouse_state.prev_buttons;
    
    // Clear deltas after reading
    g_mouse_state.delta_x = 0;
    g_mouse_state.delta_y = 0;
    
    return 0;
}

// Get mouse position
int mouse_get_position(int* x, int* y) {
    if (!g_mouse_state.initialized || !x || !y) return -1;
    
    *x = g_mouse_state.x;
    *y = g_mouse_state.y;
    return 0;
}

// Get mouse button state
int mouse_get_buttons(uint8* buttons) {
    if (!g_mouse_state.initialized || !buttons) return -1;
    
    *buttons = g_mouse_state.buttons;
    return 0;
}

// Set mouse position
void mouse_set_position(int x, int y) {
    if (!g_mouse_state.initialized) return;
    
    g_mouse_state.x = x;
    g_mouse_state.y = y;
    
    // Clamp to bounds
    if (g_mouse_state.x < mouse_min_x) g_mouse_state.x = mouse_min_x;
    if (g_mouse_state.x > mouse_max_x) g_mouse_state.x = mouse_max_x;
    if (g_mouse_state.y < mouse_min_y) g_mouse_state.y = mouse_min_y;
    if (g_mouse_state.y > mouse_max_y) g_mouse_state.y = mouse_max_y;
}

// Set mouse bounds
void mouse_set_bounds(int min_x, int min_y, int max_x, int max_y) {
    mouse_min_x = min_x;
    mouse_min_y = min_y;
    mouse_max_x = max_x;
    mouse_max_y = max_y;
    
    // Clamp current position to new bounds
    if (g_mouse_state.initialized) {
        mouse_set_position(g_mouse_state.x, g_mouse_state.y);
    }
}
