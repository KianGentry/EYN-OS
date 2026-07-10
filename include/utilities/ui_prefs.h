#ifndef UI_PREFS_H
#define UI_PREFS_H

#include <misc/types.h>

// Load and apply UI preferences (theme + font) from disk.
// Returns 0 if applied successfully, <0 if missing/invalid.
int ui_prefs_load_apply(uint8 drive);

// Save current UI preferences (theme + last-selected font) to disk.
// Returns 0 on success, <0 on error.
int ui_prefs_save(uint8 drive);

// Track the last selected font path for persistence.
// If never set, defaults to "/fonts/unscii-16.hex".
const char* ui_prefs_get_font_path(void);
void ui_prefs_set_font_path(const char* path);

// Status bar behavior:
// 0 = only show while Alt is held
// 1 = pinned (always show)
int ui_prefs_get_status_bar_mode(void);
void ui_prefs_set_status_bar_mode(int mode);

// Workspace video profile:
// scale is clamped to [50, 100] percent of framebuffer dimensions.
// aspect mode: 0=native, 1=4:3, 2=16:10, 3=16:9, 4=21:9, 5=1:1.
int ui_prefs_get_workspace_scale_pct(void);
void ui_prefs_set_workspace_scale_pct(int pct);
int ui_prefs_get_workspace_aspect_mode(void);
void ui_prefs_set_workspace_aspect_mode(int mode);

// Preferred hardware display mode for runtime switching.
int ui_prefs_get_display_width(void);
void ui_prefs_set_display_width(int width);
int ui_prefs_get_display_height(void);
void ui_prefs_set_display_height(int height);
int ui_prefs_get_display_bpp(void);
void ui_prefs_set_display_bpp(int bpp);

#endif // UI_PREFS_H
