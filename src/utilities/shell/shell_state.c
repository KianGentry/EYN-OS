#include <utilities/shell/shell_state.h>

uint8 g_current_drive = 0;

// Current working directory for the shell (default root)
char shell_current_path[128] = "/";
