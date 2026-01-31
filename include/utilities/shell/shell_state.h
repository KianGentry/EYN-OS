#ifndef SHELL_STATE_H
#define SHELL_STATE_H

#include <misc/types.h>

// Current drive selected by the shell (physical drive index).
extern uint8 g_current_drive;

// Current working directory for the shell (absolute path, no trailing slash except root).
extern char shell_current_path[128];

#endif
