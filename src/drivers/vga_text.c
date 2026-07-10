#include <vga_text.h>
#include <util.h>
#include <system.h>
#include <stdint.h>
#include <string.h>

/*
 * VGA Text Mode (80x25) Driver for TTY-only builds
 * 
 * Provides basic text-mode output to VGA hardware at 0xB8000.
 * Supports scrolling, cursor positioning, and basic color attributes.
 */

#define VGA_TEXT_BUFFER     ((uint16 *)(0xB8000))
#define VGA_TEXT_WIDTH      80
#define VGA_TEXT_HEIGHT     25
#define VGA_CURSOR_PORT_LO  0x3D4
#define VGA_CURSOR_PORT_HI  0x3D5
#define VGA_CURSOR_INDEX_LO 0x0F
#define VGA_CURSOR_INDEX_HI 0x0E

// Attribute byte: [bright:1][bg_color:3][bright:1][fg_color:3]
#define VGA_ATTR_DEFAULT    0x0F  // White on black
#define VGA_ATTR_ERROR      0x0C  // Red on black
#define VGA_ATTR_SUCCESS    0x0A  // Green on black

static int g_cursor_x = 0;
static int g_cursor_y = 0;
static uint8 g_current_attr = VGA_ATTR_DEFAULT;

static inline void vga_text_set_cursor_hardware(int pos) {
    outportb(VGA_CURSOR_PORT_LO, VGA_CURSOR_INDEX_LO);
    outportb(VGA_CURSOR_PORT_HI, (uint8)(pos & 0xFF));
    outportb(VGA_CURSOR_PORT_LO, VGA_CURSOR_INDEX_HI);
    outportb(VGA_CURSOR_PORT_HI, (uint8)((pos >> 8) & 0xFF));
}

static void vga_text_scroll_up(void) {
    // Copy lines 1..24 to lines 0..23
    for (int i = 0; i < VGA_TEXT_HEIGHT - 1; i++) {
        memcpy(
            &VGA_TEXT_BUFFER[i * VGA_TEXT_WIDTH],
            &VGA_TEXT_BUFFER[(i + 1) * VGA_TEXT_WIDTH],
            VGA_TEXT_WIDTH * sizeof(uint16)
        );
    }
    
    // Clear last line
    uint16 blank = (uint16)' ' | ((uint16)VGA_ATTR_DEFAULT << 8);
    for (int i = 0; i < VGA_TEXT_WIDTH; i++) {
        VGA_TEXT_BUFFER[(VGA_TEXT_HEIGHT - 1) * VGA_TEXT_WIDTH + i] = blank;
    }
}

void vga_text_init(void) {
    // Clear screen
    uint16 blank = (uint16)' ' | ((uint16)VGA_ATTR_DEFAULT << 8);
    for (int i = 0; i < VGA_TEXT_WIDTH * VGA_TEXT_HEIGHT; i++) {
        VGA_TEXT_BUFFER[i] = blank;
    }
    
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_current_attr = VGA_ATTR_DEFAULT;
    
    vga_text_set_cursor_hardware(0);
}

void vga_text_clear(void) {
    uint16 blank = (uint16)' ' | ((uint16)VGA_ATTR_DEFAULT << 8);
    for (int i = 0; i < VGA_TEXT_WIDTH * VGA_TEXT_HEIGHT; i++) {
        VGA_TEXT_BUFFER[i] = blank;
    }
    g_cursor_x = 0;
    g_cursor_y = 0;
    vga_text_set_cursor_hardware(0);
}

void vga_text_putchar(char c) {
    uint16 attr_byte = g_current_attr;
    
    if (c == '\n') {
        g_cursor_x = 0;
        g_cursor_y++;
    } else if (c == '\r') {
        g_cursor_x = 0;
    } else if (c == '\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
            int pos = g_cursor_y * VGA_TEXT_WIDTH + g_cursor_x;
            VGA_TEXT_BUFFER[pos] = (uint16)' ' | ((uint16)attr_byte << 8);
        }
    } else {
        // Printable character
        int pos = g_cursor_y * VGA_TEXT_WIDTH + g_cursor_x;
        VGA_TEXT_BUFFER[pos] = (uint16)(unsigned char)c | ((uint16)attr_byte << 8);
        g_cursor_x++;
    }
    
    // Handle line wrapping
    if (g_cursor_x >= VGA_TEXT_WIDTH) {
        g_cursor_x = 0;
        g_cursor_y++;
    }
    
    // Handle vertical scrolling
    if (g_cursor_y >= VGA_TEXT_HEIGHT) {
        vga_text_scroll_up();
        g_cursor_y = VGA_TEXT_HEIGHT - 1;
    }
    
    // Update hardware cursor
    int cursor_pos = g_cursor_y * VGA_TEXT_WIDTH + g_cursor_x;
    vga_text_set_cursor_hardware(cursor_pos);
}

void vga_text_puts(const char *str) {
    if (!str) return;
    while (*str) {
        vga_text_putchar(*str++);
    }
}

void vga_text_set_color(uint8 fg, uint8 bg) {
    // fg and bg are 0-15 (standard VGA colors)
    // Attribute byte format: [bright_bg:1][bg:3][bright_fg:1][fg:3]
    uint8 fg_attr = (fg & 0x0F);
    uint8 bg_attr = ((bg & 0x07) << 4);
    g_current_attr = bg_attr | fg_attr;
}

void vga_text_reset_color(void) {
    g_current_attr = VGA_ATTR_DEFAULT;
}
