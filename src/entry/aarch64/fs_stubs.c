#include <misc/types.h>
#include <stddef.h>
#include <string.h>

#include <drivers/eynfs.h> /* for EYNFS_READ/EYNFS_WRITE/EYNFS_APPEND constants used by existing code */
#include <fs/vfs.h>
#include <hal/keyboard.h>
#include <utilities/shell/shell_state.h>

/*
 * AArch64 compatibility layer for legacy shell/pipeline helpers.
 *
 * The i386 kernel historically used EYNFS' POSIX-like open/read/close wrappers
 * for input redirection and helper functions write_output_to_file()/append_*.
 *
 * On AArch64 we back these operations via the VFS facade so the same shell code
 * can compile and work regardless of filesystem type.
 */

__attribute__((weak)) void resolve_path(const char* input, const char* cwd, char* out, size_t outsz) {
	if (!out || outsz == 0) return;
	if (!cwd) cwd = "/";

	if (!input || !input[0]) {
		strncpy(out, cwd, outsz - 1);
		out[outsz - 1] = '\0';
		return;
	}

	if (input[0] == '/') {
		strncpy(out, input, outsz - 1);
		out[outsz - 1] = '\0';
		return;
	}

	/* Join cwd and input into a temporary buffer, then normalize . and .. */
	char tmp[256];
	size_t cwdlen = strlen(cwd);
	if (cwdlen > 1 && cwd[cwdlen - 1] == '/') cwdlen--;
	if (cwdlen >= sizeof(tmp)) cwdlen = sizeof(tmp) - 1;
	memcpy(tmp, cwd, cwdlen);
	tmp[cwdlen] = '\0';

	if (cwdlen > 0 && tmp[cwdlen - 1] != '/') {
		strncat(tmp, "/", sizeof(tmp) - strlen(tmp) - 1);
	}
	strncat(tmp, input, sizeof(tmp) - strlen(tmp) - 1);

	char* parts[64];
	int nparts = 0;
	char* save = NULL;
	for (char* tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (strcmp(tok, ".") == 0) continue;
		if (strcmp(tok, "..") == 0) {
			if (nparts > 0) nparts--;
			continue;
		}
		if (nparts < (int)(sizeof(parts) / sizeof(parts[0]))) {
			parts[nparts++] = tok;
		}
	}

	out[0] = '/';
	out[1] = '\0';
	for (int i = 0; i < nparts; i++) {
		strncat(out, parts[i], outsz - strlen(out) - 1);
		if (i < nparts - 1) {
			strncat(out, "/", outsz - strlen(out) - 1);
		}
	}
}

void poll_keyboard_for_ctrl_c(void) {
	extern volatile int g_user_interrupt;

	uint32 key = hal_kbd_read_key_nonblock();
	if (key == HAL_KEY_CTRL_C || key == 0x03u) {
		g_user_interrupt = 1;
	}
}


