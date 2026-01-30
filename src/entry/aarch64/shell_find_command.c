#include <stddef.h>
#include <string.h>

#include <utilities/shell/shell.h>
#include <utilities/shell/shell_command_info.h>

shell_cmd_handler_t find_command(const char* command_name) {
	if (!command_name || !*command_name) {
		return NULL;
	}

	const shell_command_info_t* it = __start_shellcmds;
	for (; it < __stop_shellcmds; ++it) {
		if (it->name && strcmp(it->name, command_name) == 0) {
			return it->handler;
		}
	}

	return NULL;
}
