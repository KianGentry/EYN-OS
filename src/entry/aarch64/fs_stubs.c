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

/* ---- Minimal FD table for open/read/close ---- */

typedef struct {
	int used;
	uint8 drive;
	char path[128];
	uint32 size;
	uint32 off;
} a64_fd_t;

#define A64_MAX_FDS 8
static a64_fd_t g_fds[A64_MAX_FDS];

static int alloc_fd(void) {
	for (int i = 0; i < A64_MAX_FDS; i++) {
		if (!g_fds[i].used) {
			memset(&g_fds[i], 0, sizeof(g_fds[i]));
			g_fds[i].used = 1;
			return i;
		}
	}
	return -1;
}

int open(const char* path, int flags) {
	if (!path) return -1;

	/* Only read is needed for pipeline input redirection today. */
	if (flags != EYNFS_READ) {
		return -1;
	}

	char abspath[128];
	resolve_path(path, shell_current_path, abspath, sizeof(abspath));

	vfs_stat_t st;
	if (vfs_stat(g_current_drive, abspath, &st) != 0 || st.type != VFS_NODE_FILE) {
		return -1;
	}

	uint32 size = 0;
	if (vfs_get_file_size(g_current_drive, abspath, &size) != 0) {
		return -1;
	}

	int fd = alloc_fd();
	if (fd < 0) return -1;

	g_fds[fd].drive = g_current_drive;
	strncpy(g_fds[fd].path, abspath, sizeof(g_fds[fd].path) - 1);
	g_fds[fd].path[sizeof(g_fds[fd].path) - 1] = '\0';
	g_fds[fd].size = size;
	g_fds[fd].off = 0;
	return fd;
}

int read(int fd, void* buf, int count) {
	if (fd < 0 || fd >= A64_MAX_FDS) return -1;
	if (!g_fds[fd].used) return -1;
	if (!buf || count <= 0) return 0;

	a64_fd_t* f = &g_fds[fd];
	if (f->off >= f->size) return 0;

	int to_read = count;
	uint32 remaining = f->size - f->off;
	if ((uint32)to_read > remaining) to_read = (int)remaining;

	int br = vfs_read_file_at(f->drive, f->path, buf, to_read, f->off);
	if (br > 0) {
		f->off += (uint32)br;
	}
	return br;
}

int close(int fd) {
	if (fd < 0 || fd >= A64_MAX_FDS) return -1;
	g_fds[fd].used = 0;
	return 0;
}

int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
	if (!filename) return -1;
	if (!buf) buf = "";
	if (len < 0) len = 0;

	char abspath[128];
	resolve_path(filename, shell_current_path, abspath, sizeof(abspath));

	int wr = vfs_write_file(disk, abspath, buf, (uint32)len);
	return (wr >= 0) ? 0 : -1;
}

int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk) {
	if (!filename) return -1;
	if (!buf) buf = "";
	if (len < 0) len = 0;

	char abspath[128];
	resolve_path(filename, shell_current_path, abspath, sizeof(abspath));

	/* Keep memory bounded: only support appending to reasonably small files. */
	uint32 old_size = 0;
	vfs_stat_t st;
	int has_old = (vfs_stat(disk, abspath, &st) == 0 && st.type == VFS_NODE_FILE);
	if (has_old) {
		if (vfs_get_file_size(disk, abspath, &old_size) != 0) return -1;
		if (old_size > 4096u) return -1;
	}

	/* 1024 matches the current redirect buffer size; keep this bounded. */
	static char tmp[4096 + 1024];
	uint32 pos = 0;
	if (has_old && old_size > 0) {
		int br = vfs_read_file(disk, abspath, tmp, (int)old_size);
		if (br < 0) return -1;
		pos = (uint32)br;
	}

	if ((uint32)len > (uint32)sizeof(tmp) - pos) return -1;
	memcpy(tmp + pos, buf, (size_t)len);
	pos += (uint32)len;

	int wr = vfs_write_file(disk, abspath, tmp, pos);
	return (wr >= 0) ? 0 : -1;
}
