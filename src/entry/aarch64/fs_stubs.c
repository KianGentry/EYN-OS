#include <misc/types.h>
#include <stddef.h>

#include <utilities/shell/fs_commands.h>
#include <kb.h>

// Minimal filesystem stubs for AArch64 bring-up builds.
// The legacy shell pipeline implementation supports < and > using the
// EYNFS layer and POSIX-like open/read/close. Until the VFS/EYNFS stack
// is ported to AArch64, return failures so pipelines can still be used
// for in-memory piping.

char shell_current_path[128] = "/";

void resolve_path(const char* input, const char* cwd, char* out, size_t outsz) {
	(void)cwd;

	if (!out || outsz == 0) {
		return;
	}
	if (!input) {
		out[0] = 0;
		return;
	}

	size_t i = 0;
	for (; i + 1 < outsz && input[i]; ++i) {
		out[i] = input[i];
	}
	out[i] = 0;
}

int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
	(void)buf;
	(void)len;
	(void)filename;
	(void)disk;
	return -1;
}

int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
	(void)buf;
	(void)len;
	(void)filename;
	(void)disk;
	return -1;
}

void poll_keyboard_for_ctrl_c(void) {
	// No-op for bring-up.
}

int open(const char* path, int flags) {
	(void)path;
	(void)flags;
	return -1;
}

int read(int fd, void* buf, int count) {
	(void)fd;
	(void)buf;
	(void)count;
	return -1;
}

int close(int fd) {
	(void)fd;
	return 0;
}
