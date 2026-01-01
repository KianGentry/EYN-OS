#ifndef UI_PREFS_H
#define UI_PREFS_H

#include <types.h>

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

#endif // UI_PREFS_H
