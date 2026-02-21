# EYN-OS Text User Interface System (LEGACY)

> **DEPRECATION NOTICE**: This system has been superseded by the **Tiling Manager** and **GUI Framework**. New applications should use the Tiling API (`include/utilities/tile_manager.h`) instead of this legacy TUI framework.

The EYN-OS Text User Interface (TUI) system provides a comprehensive framework for creating text-based user interfaces within the operating system. Built on top of the VGA graphics driver, the TUI system offers consistent, professional-looking interfaces for applications while maintaining the simplicity and efficiency that EYN-OS is known for.

Be warned that this is no longer supported, and may not be compatible with future EYN-OS updates.

## Overview

The TUI system is designed with the philosophy of "reinventing the wheel" - building everything from scratch to understand and control every aspect of the user interface. It provides a consistent framework for applications while allowing developers to create custom interfaces tailored to their specific needs.

### Modern Replacement: Tiling Manager
The new Tiling Manager provides:
- **Multiple Windows**: Run multiple apps side-by-side.
- **Virtual Terminals**: Standard shell I/O in each tile.
- **GUI Support**: Pixel-perfect graphics mixed with text.
- **Mouse Integration**: Full mouse support for window management.

See `docs/ui/tiling-manager.md` for the new system.

### Design Principles

- **Simplicity**: Clean, straightforward API that's easy to understand and use
- **Consistency**: Uniform look and feel across all TUI applications
- **Efficiency**: Optimized for low-end systems with minimal memory usage
- **Flexibility**: Modular design that allows for custom components and layouts
- **Accessibility**: Clear visual feedback and intuitive navigation

## Architecture

### Core Components

The TUI system consists of several key components:

1. **TUI Core (`tui.c`)**: Main interface functions and window management
2. **VGA Driver**: Underlying graphics system for text and pixel rendering
3. **Keyboard Handler**: Input processing for user interaction
4. **Application Layer**: Specific implementations like the write editor and help system

### System Layers

```
┌─────────────────────────────────────┐
│           Applications              │
│    (write_editor, help_tui)         │
├─────────────────────────────────────┤
│           TUI Framework             │
│    (windows, lists, text areas)     │
├─────────────────────────────────────┤
│           VGA Driver                │
│    (text rendering, colors)         │
├─────────────────────────────────────┤
│           Hardware                  │
│    (VGA controller, keyboard)       │
└─────────────────────────────────────┘
```

## Core TUI Functions

### Initialization and Setup

```c
// Initialize the TUI system with screen dimensions
void tui_init(int screen_width, int screen_height);

// Clear the entire screen
void tui_clear();

// Refresh the display (no buffering needed)
void tui_refresh();
```

### Window Management

```c
// Window structure definition
typedef struct {
    int x, y, width, height;           // Position and size
    const char* title;                  // Window title
    tui_style_t title_style;           // Title styling
    tui_style_t border_style;          // Border styling
    tui_style_t bg_style;              // Background styling
} tui_window_t;

// Draw a complete window with borders and title
void tui_draw_window(const tui_window_t* win);
```

### Text Rendering

```c
// Draw text at specific coordinates with styling
void tui_draw_text(int x, int y, const char* text, tui_style_t style);

// Draw text area with automatic wrapping and scrolling
void tui_draw_text_area(const tui_window_t* win, const char* text, 
                        int scroll_offset, tui_style_t style);
```

### List Components

```c
// Draw a scrollable, selectable list
void tui_draw_list(const tui_window_t* win, const char** items, 
                   int item_count, int selected_index, 
                   int scroll_offset, tui_style_t style, 
                   tui_style_t selected_style);
```

### Status and Information

```c
// Draw a status bar below a window or at screen bottom
void tui_draw_status_bar(const tui_window_t* win, const char* text, 
                         tui_style_t style);
```

## Styling System

### Color Definitions

The TUI system provides a predefined color palette optimized for text display:

```c
#define TUI_COLOR_DEFAULT 0    // System default
#define TUI_COLOR_YELLOW  1    // Highlighting and titles
#define TUI_COLOR_RED     2    // Errors and warnings
#define TUI_COLOR_MAGENTA 3    // Special indicators
#define TUI_COLOR_WHITE   4    // Primary text
#define TUI_COLOR_BLACK   5    // Background
#define TUI_COLOR_GRAY    6    // Secondary text
```

### Style Structure

```c
typedef struct {
    uint8_t fg_color;    // Foreground color
    uint8_t bg_color;    // Background color
    uint8_t bold;        // Bold text flag
} tui_style_t;
```

### Style Examples

```c
// Standard text style
tui_style_t normal_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};

// Highlighted text style
tui_style_t highlight_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};

// Error text style
tui_style_t error_style = {TUI_COLOR_RED, TUI_COLOR_BLACK, 0};

// Secondary text style
tui_style_t secondary_style = {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0};
```

## Input Handling

### Keyboard Input

The TUI system provides comprehensive keyboard input handling:

```c
// Read a single key press (blocking)
int tui_read_key();
```

### Key Codes

The system returns special key codes for navigation and control:

```c
// Arrow keys
0x1001  // Up arrow
0x1002  // Down arrow
0x1003  // Left arrow
0x1004  // Right arrow

// Control keys
0x1005  // Delete
0x1006  // Home
0x1007  // End
0x1008  // Page Up
0x1009  // Page Down

// Control combinations
0x2001  // Ctrl+O (save)
0x2002  // Ctrl+X (exit)

// Standard keys
'\n'    // Enter
'\b'    // Backspace
27      // Escape
' '     // Space
```

### Input Processing Example

```c
while (running) {
    int key = tui_read_key();
    switch (key) {
        case 0x1001:  // Up arrow
            if (selected > 0) selected--;
            break;
        case 0x1002:  // Down arrow
            if (selected < max_items - 1) selected++;
            break;
        case 0x2002:  // Ctrl+X
            running = 0;
            break;
        case '\n':    // Enter
            // Handle selection
            break;
    }
}
```

## Application Examples

### Write Editor

The write editor demonstrates a comprehensive TUI application:

```c
void write_editor_draw(const char* filename) {
    tui_clear();
    
    // Create main editor window
    tui_window_t editor_win = {
        0, 0,                    // Position
        78, EDITOR_HEIGHT + 3,   // Size
        "",                      // Title (custom drawn)
        {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1},  // Title style
        {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0},    // Border style
        {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}    // Background style
    };
    
    tui_draw_window(&editor_win);
    
    // Custom title bar with filename and status
    tui_style_t file_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 1};
    tui_draw_text(editor_win.x + 2, editor_win.y, filename, file_style);
    
    // Draw text content with cursor
    tui_style_t text_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
    tui_style_t cursor_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
    
    // Render text lines with cursor positioning
    for (int i = 0; i < max_visible; i++) {
        int line_idx = i + scroll_y;
        if (line_idx < num_lines) {
            // Draw line content
            tui_draw_text(editor_win.x + 1, editor_win.y + 2 + i, 
                         buffer[line_idx], text_style);
            
            // Draw cursor if this is the current line
            if (line_idx == cursor_y) {
                tui_draw_text(editor_win.x + 1 + cursor_x, 
                             editor_win.y + 2 + i, "!", cursor_style);
            }
        }
    }
    
    // Status bar
    char status[160];
    snprintf(status, sizeof(status), 
             "Line %d/%d, Col %d | Ctrl+O: Save | Ctrl+X: Exit",
             cursor_y + 1, num_lines, cursor_x + 1);
    tui_draw_status_bar(&editor_win, status, text_style);
}
```

### Help System

The help system demonstrates dual-pane layout and hierarchical navigation:

```c
void help_tui() {
    // Create dual-pane layout
    tui_window_t left_win = {
        0, 0, CMD_LIST_WIDTH, win_height,
        "Commands",
        {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1},
        {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0},
        {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}
    };
    
    tui_window_t right_win = {
        CMD_LIST_WIDTH + 2, 0, DESC_WIDTH, win_height,
        "Description",
        {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1},
        {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0},
        {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}
    };
    
    while (running) {
        tui_clear();
        tui_draw_window(&left_win);
        tui_draw_window(&right_win);
        
        // Draw command list with selection
        tui_style_t norm_style = {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0};
        tui_style_t sel_style = {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1};
        
        for (int i = 0; i < visible_commands; i++) {
            int y_pos = left_win.y + 2 + i;
            if (i == selected) {
                tui_draw_text(left_win.x + 1, y_pos, "!", sel_style);
                tui_draw_text(left_win.x + 2, y_pos, commands[i], sel_style);
            } else {
                tui_draw_text(left_win.x + 1, y_pos, commands[i], norm_style);
            }
        }
        
        // Draw description in right pane
        tui_draw_text_area(&right_win, description, 0, norm_style);
        
        // Status bar
        tui_draw_status_bar(NULL, "^/v: Move | Enter: Select | Ctrl+X: Exit", norm_style);
        
        // Handle input
        int key = tui_read_key();
        // Process navigation keys...
    }
}
```

## Best Practices

### Window Design

1. **Consistent Sizing**: Use standard window sizes for common elements
2. **Clear Hierarchy**: Use borders and spacing to organize information
3. **Responsive Layout**: Design for different screen sizes and content lengths

### Color Usage

1. **Semantic Colors**: Use colors consistently for their intended purpose
2. **Contrast**: Ensure text is readable against backgrounds
3. **Moderation**: Don't overuse colors - they should enhance, not distract

### User Experience

1. **Clear Navigation**: Provide obvious ways to move between elements
2. **Status Information**: Always show current state and available actions
3. **Error Handling**: Provide clear feedback for user actions
4. **Keyboard Shortcuts**: Support both mouse and keyboard navigation

## Performance Considerations

### Memory Usage

- The TUI system is designed for low-memory systems
- Text buffers are sized appropriately for target hardware
- No unnecessary memory allocation during rendering

### Rendering Efficiency

- Direct VGA access for fast text rendering
- Minimal redraw operations
- Efficient window clipping and border drawing

### Input Responsiveness

- Non-blocking key reading where possible
- Immediate visual feedback for user actions
- Smooth scrolling and navigation

## Extending the TUI System

### Custom Components

Developers can extend the TUI system by creating custom drawing functions:

```c
// Custom progress bar component
void tui_draw_progress_bar(const tui_window_t* win, int progress, int max) {
    int bar_width = win->width - 2;
    int filled_width = (progress * bar_width) / max;
    
    // Draw background
    for (int i = 0; i < bar_width; i++) {
        tui_draw_text(win->x + 1 + i, win->y + 2, "░", 
                     {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0});
    }
    
    // Draw filled portion
    for (int i = 0; i < filled_width; i++) {
        tui_draw_text(win->x + 1 + i, win->y + 2, "█", 
                     {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 0});
    }
}
```

### Integration with Applications

The TUI system integrates seamlessly with EYN-OS applications:

```c
// In your application
#include <tui.h>

void my_app() {
    tui_init(80, 25);  // Standard terminal size
    
    tui_window_t main_win = {
        5, 3, 70, 20,
        "My Application",
        {TUI_COLOR_YELLOW, TUI_COLOR_BLACK, 1},
        {TUI_COLOR_GRAY, TUI_COLOR_BLACK, 0},
        {TUI_COLOR_BLACK, TUI_COLOR_BLACK, 0}
    };
    
    while (running) {
        tui_clear();
        tui_draw_window(&main_win);
        
        // Your application logic here
        
        tui_draw_status_bar(&main_win, "Status: Ready", 
                           {TUI_COLOR_WHITE, TUI_COLOR_BLACK, 0});
        
        int key = tui_read_key();
        // Handle input...
    }
}
```

## Troubleshooting

### Common Issues

1. **Screen Not Clearing**: Ensure `tui_clear()` is called before drawing
2. **Text Not Visible**: Check color combinations for adequate contrast
3. **Windows Not Drawing**: Verify window coordinates are within screen bounds
4. **Input Not Responding**: Check key handling in your main loop

### Debugging Tips

1. **Use Status Bars**: Display debug information in status bars
2. **Color Coding**: Use different colors to identify different components
3. **Incremental Testing**: Test each TUI component individually
4. **Screen Coordinates**: Verify all drawing coordinates are valid

## Conclusion

The EYN-OS TUI system provides a robust foundation for creating professional text-based user interfaces. Its simple yet powerful API makes it easy to build consistent applications while maintaining the educational value and performance characteristics that define EYN-OS.

By following the design principles and best practices outlined in this documentation, developers can create intuitive, efficient user interfaces that enhance the overall EYN-OS experience while maintaining the system's philosophy of simplicity and control.
