#include <mouse.h>
#include <system.h>
#include <vga.h>
#include <isr.h>
#include <irq.h>
#include <string.h>
#include <misc/types.h>
#include <context.h>
#include <misc/sched.h>

// PS/2 Mouse ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PIC data ports (for unmasking IRQ12 on the slave PIC)
#define PIC1_DATA_PORT   0x21
#define PIC2_DATA_PORT   0xA1

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

// Mouse packet sizes
#define MOUSE_PACKET_SIZE 4 /* default to 4 if wheel is enabled; will use 3 if not */

// Global mouse state
mouse_state_t g_mouse_state = {0};
static const rei_image_t* g_cursor_image_ptr = NULL;

// Mouse bounds
static int mouse_min_x = 0, mouse_min_y = 0;
static int mouse_max_x = 79, mouse_max_y = 24; // VGA text mode bounds

// Mouse packet buffer
static uint8 mouse_packet_buffer[MOUSE_PACKET_SIZE];
static int mouse_packet_index = 0;
static int mouse_packet_len = 3; // 3 by default; may upgrade to 4 if wheel detected

// Device capabilities
static int mouse_has_wheel = 0;
static int mouse_has_5btn = 0;

// Helpers for packet synchronization and sign extension
static inline int mouse_packet_is_sync(uint8 b0) { return (b0 & 0x08) != 0; }
static inline int8 sign_extend_8(uint8 v) { return (int8)v; }

static void mouse_enable_irq12(void) {
    // Unmask cascade IRQ2 on the master PIC so slave PIC IRQs can be delivered
    uint8 m1 = inportb(PIC1_DATA_PORT);
    m1 &= (uint8)~(1u << 2);
    outportb(PIC1_DATA_PORT, m1);

    // Unmask IRQ12 (bit 4) on the slave PIC
    uint8 m2 = inportb(PIC2_DATA_PORT);
    m2 &= (uint8)~(1u << 4);
    outportb(PIC2_DATA_PORT, m2);
}

static int mouse_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

static void mouse_ctx_account(uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (!ctx) return;
    scheduler_account(ctx->wo, cost);
    scheduler_yield_if_needed(ctx->wo);
    if (sched_det_is_enabled()) ctx->det_seq++;
}

// Wait for mouse to be ready
static int mouse_wait_for_ready(void) {
    int timeout = 100000;
    while (timeout--) {
        if ((timeout & 0x3FF) == 0) mouse_ctx_account(SCHED_COST_CONSOLE);
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
        if ((timeout & 0x3FF) == 0) mouse_ctx_account(SCHED_COST_CONSOLE);
        if (inportb(PS2_STATUS_PORT) & 0x01) {
            return 0; // Data available
        }
    }
    return -1; // Timeout
}

// Send command to mouse
static int mouse_send_command(uint8 command) {
    if (!mouse_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) return -1;
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
    if (!mouse_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) return -1;
    if (mouse_wait_for_data() != 0) return -1;
    *byte = inportb(PS2_DATA_PORT);
    return 0;
}

// Initialize mouse
int mouse_init(void) {
    if (!mouse_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) return -1;
    memset(&g_mouse_state, 0, sizeof(mouse_state_t));
    
    // Enable mouse in PS/2 controller
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0xA8); // Enable mouse
    
    // Enable mouse interrupts
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0x20); // Read command byte
    if (mouse_wait_for_data() != 0) return -1;
    uint8 status = inportb(PS2_DATA_PORT);

    // Command byte (typical i8042):
    // bit0: enable IRQ1 (keyboard), bit1: enable IRQ12 (mouse)
    // bit4: disable keyboard clock, bit5: disable mouse clock (1=disabled)
    // Some emulators/firmware leave clock-disable bits set; clear them explicitly.
    status |= 0x03;        // enable IRQ1 + IRQ12
    status &= (uint8)~0x30; // ensure both ports are enabled (clear clock-disable bits)
    
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_COMMAND_PORT, 0x60); // Write command byte
    if (mouse_wait_for_ready() != 0) return -1;
    outportb(PS2_DATA_PORT, status);
    
    // Reset mouse (best-effort, avoid noisy logs)
    if (mouse_send_command(MOUSE_CMD_RESET) != 0) {
        return -1;
    }
    
    // Wait for reset response
    if (mouse_wait_for_data() != 0) return -1;
    uint8 response = inportb(PS2_DATA_PORT);
    if (response != 0xAA) {
        return -1;
    }
    
    // Wait for device ID
    if (mouse_wait_for_data() != 0) return -1;
    response = inportb(PS2_DATA_PORT);
    
    // Set default settings
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) != 0) {
        return -1;
    }

    // Try to enable IntelliMouse wheel (rate sequence 200,100,80 then read device ID)
    // Best-effort: ignore failures
    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE) == 0) (void)mouse_send_command(200);
    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE) == 0) (void)mouse_send_command(100);
    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE) == 0) (void)mouse_send_command(80);
    // Read device ID
    if (mouse_send_command(MOUSE_CMD_GET_DEVICE_ID) == 0) {
        if (mouse_wait_for_data() == 0) {
            uint8 id = inportb(PS2_DATA_PORT);
            if (id == 3) { mouse_has_wheel = 1; mouse_packet_len = 4; }
            else if (id == 4) { mouse_has_wheel = 1; mouse_has_5btn = 1; mouse_packet_len = 4; }
        }
    }

    // Enable mouse streaming
    if (mouse_send_command(MOUSE_CMD_ENABLE) != 0) {
        return -1;
    }

    // Set sample rate to 200 for better responsiveness (many emulators/hardware support this)
    if (mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE) == 0) {
        (void)mouse_send_command(200);
    }
    
    // Initialize mouse position to center of screen
    g_mouse_state.x = mouse_max_x / 2;
    g_mouse_state.y = mouse_max_y / 2;
    g_mouse_state.initialized = 1;
    
    // Register mouse interrupt handler
    register_interrupt_handler(12, mouse_interrupt_handler); // IRQ 12 is mouse

    // Ensure IRQ12 can actually fire (irq_init only unmasks IRQ0 by default).
    // Without this, QEMU can queue mouse bytes faster than our frame-loop polling drains them,
    // making cursor movement appear to "stop" after a small initial delta.
    mouse_enable_irq12();
    return 0;
}

// Cleanup mouse
void mouse_cleanup(void) {
    if (!mouse_ctx_allow(CAP_DEV_INPUT, SCHED_COST_CONSOLE)) return;
    if (g_mouse_state.initialized) {
        mouse_send_command(MOUSE_CMD_DISABLE);
        g_mouse_state.initialized = 0;
    }
}

// Mouse interrupt handler
void mouse_interrupt_handler(void) {
    if (!g_mouse_state.initialized) {
        return;
    }

    // Drain a bounded number of AUX bytes. QEMU can enqueue multiple bytes between interrupts;
    // draining here avoids output-buffer buildup that makes the cursor appear to "freeze".
    for (int i = 0; i < 256; ++i) {
        uint8 st = inportb(PS2_STATUS_PORT);
        if (!(st & 0x01)) break;      // no data
        if (!(st & 0x20)) break;      // not AUX data

        uint8 data = inportb(PS2_DATA_PORT);

        // Packet synchronization: the first byte must have bit 3 set.
        if (mouse_packet_index == 0) {
            if (!mouse_packet_is_sync(data)) {
                continue;
            }
        }

        mouse_packet_buffer[mouse_packet_index] = data;
        mouse_packet_index++;

        if (mouse_packet_index == mouse_packet_len) {
            uint8 status = mouse_packet_buffer[0];
            int8 delta_x = sign_extend_8(mouse_packet_buffer[1]);
            int8 delta_y = sign_extend_8(mouse_packet_buffer[2]);
            int wheel = 0;
            if (mouse_has_wheel && mouse_packet_len >= 4) {
                int8 z = sign_extend_8(mouse_packet_buffer[3]);
                z = (z & 0x0F);
                if (z & 0x08) z |= 0xF0;
                wheel = (int)z;
            }

            g_mouse_state.prev_buttons = g_mouse_state.buttons;
            g_mouse_state.buttons = status & 0x07;
            if (mouse_has_5btn && mouse_packet_len >= 4) {
                uint8 b4 = mouse_packet_buffer[3];
                if (b4 & 0x10) g_mouse_state.buttons |= MOUSE_BUTTON_4;
                if (b4 & 0x20) g_mouse_state.buttons |= MOUSE_BUTTON_5;
            }

            g_mouse_state.delta_x += delta_x;
            g_mouse_state.delta_y += delta_y;
            g_mouse_state.wheel_delta += wheel;

            g_mouse_state.x += delta_x;
            g_mouse_state.y -= delta_y;

            if (g_mouse_state.x < mouse_min_x) g_mouse_state.x = mouse_min_x;
            if (g_mouse_state.x > mouse_max_x) g_mouse_state.x = mouse_max_x;
            if (g_mouse_state.y < mouse_min_y) g_mouse_state.y = mouse_min_y;
            if (g_mouse_state.y > mouse_max_y) g_mouse_state.y = mouse_max_y;

            mouse_packet_index = 0;
        }
    }
}

// Read mouse event
int mouse_read_event(mouse_event_t* event) {
    if (!g_mouse_state.initialized || !event) return -1;
    
    event->x = g_mouse_state.x;
    event->y = g_mouse_state.y;
    event->delta_x = g_mouse_state.delta_x;
    event->delta_y = g_mouse_state.delta_y;
    event->wheel_delta = g_mouse_state.wheel_delta;
    event->buttons = g_mouse_state.buttons;
    event->button_changes = g_mouse_state.buttons ^ g_mouse_state.prev_buttons;
    
    // Clear deltas after reading
    g_mouse_state.delta_x = 0;
    g_mouse_state.delta_y = 0;
    g_mouse_state.wheel_delta = 0;
    
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

void mouse_set_cursor_image(const rei_image_t* image) {
    g_cursor_image_ptr = image; // currently unused by driver; UI layer uses its own copy
}

// Non-blocking poll for AUX (mouse) bytes; safe to call from main loop
int mouse_poll(void) {
    int progressed = 0;
    // Drain a few AUX bytes per call to avoid starving the UI
    for (int i = 0; i < 256; ++i) {
        uint8 status = inportb(PS2_STATUS_PORT);
        if (!(status & 0x01)) break; // no data waiting
        if (!(status & 0x20)) break;  // data is for keyboard; don't consume here
        uint8 data = inportb(PS2_DATA_PORT);
        // AUX data: assemble packet with synchronization
        if (mouse_packet_index == 0 && !mouse_packet_is_sync(data)) {
            // skip until a valid header
            continue;
        }
        mouse_packet_buffer[mouse_packet_index] = data;
        mouse_packet_index++;
        progressed = 1;
        if (mouse_packet_index == mouse_packet_len) {
            uint8 st = mouse_packet_buffer[0];
            int8 dx = sign_extend_8(mouse_packet_buffer[1]);
            int8 dy = sign_extend_8(mouse_packet_buffer[2]);
            int wheel = 0;
            if (mouse_has_wheel && mouse_packet_len >= 4) {
                int8 z = sign_extend_8(mouse_packet_buffer[3]);
                z = (z & 0x0F); if (z & 0x08) z |= 0xF0; // sign-extend nibble
                wheel = (int)z;
            }
            if (!g_mouse_state.initialized) g_mouse_state.initialized = 1;
            g_mouse_state.prev_buttons = g_mouse_state.buttons;
            g_mouse_state.buttons = st & 0x07;
            if (mouse_has_5btn && mouse_packet_len >= 4) {
                uint8 b4 = mouse_packet_buffer[3];
                if (b4 & 0x10) g_mouse_state.buttons |= MOUSE_BUTTON_4;
                if (b4 & 0x20) g_mouse_state.buttons |= MOUSE_BUTTON_5;
            }
            g_mouse_state.delta_x += dx;
            g_mouse_state.delta_y += dy;
            g_mouse_state.wheel_delta += wheel;
            g_mouse_state.x += dx;
            g_mouse_state.y -= dy;
            if (g_mouse_state.x < mouse_min_x) g_mouse_state.x = mouse_min_x;
            if (g_mouse_state.x > mouse_max_x) g_mouse_state.x = mouse_max_x;
            if (g_mouse_state.y < mouse_min_y) g_mouse_state.y = mouse_min_y;
            if (g_mouse_state.y > mouse_max_y) g_mouse_state.y = mouse_max_y;
            mouse_packet_index = 0; // ready for next packet
        }
    }
    return progressed ? 0 : 1; // 0=updated, 1=nothing to read
}
