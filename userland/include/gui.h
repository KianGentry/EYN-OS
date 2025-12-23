#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Minimal GUI/Tiling syscalls.
// Handles are small ints. Handle 0 refers to the calling task's existing tile.

int gui_create(const char* title, const char* status_left);
int gui_set_title(int handle, const char* title);

#ifdef __cplusplus
}
#endif
