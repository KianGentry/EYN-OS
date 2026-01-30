#include <utilities/shell/alias.h>

// Alias expansion is not wired up on the AArch64 bring-up shell yet.
// Returning 0 indicates "not an alias".
int shell_alias_expand_line(const char* input, char* out, int out_size) {
	(void)input;
	(void)out;
	(void)out_size;
	return 0;
}
