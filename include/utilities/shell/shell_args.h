#ifndef UTILITIES_SHELL_SHELL_ARGS_H
#define UTILITIES_SHELL_SHELL_ARGS_H

#include <types.h>

/*
 * ABI-INVARIANT: Shell argument parsing contract.
 *
 * Why: This standardizes how shell commands receive arguments, removing
 *      per-command ad-hoc tokenization.
 * Invariant:
 *   - Tokenization is whitespace-based (spaces/tabs).
 *   - No quoting/escaping semantics are applied.
 *   - Parsed argv[i] are NUL-terminated C strings stored in a bounded buffer.
 *   - args->raw points to the original input line (unmodified).
 * Breakage if changed:
 *   - Any change in splitting rules changes user-visible command behavior.
 *   - Increasing limits increases stack usage per command dispatch.
 *   - Decreasing limits may break existing scripts/commands.
 * ABI-sensitive: Yes (shell command interface).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (bounds parsing prevents overflows).
 */

/* Maximum number of tokens, including argv[0] command name. */
#define SHELL_ARGS_MAX 32

/* Maximum bytes used to store the NUL-terminated argv strings. */
#define SHELL_ARGS_STORAGE_BYTES 512

typedef struct shell_args {
    const char* raw;
    uint32 raw_len;

    uint32 argc;
    const char* argv[SHELL_ARGS_MAX];

    /* Raw offsets into args->raw for each argv[i] start. */
    uint16 raw_offs[SHELL_ARGS_MAX];

    char storage[SHELL_ARGS_STORAGE_BYTES];
} shell_args_t;

/* Parse a raw command line into argc/argv.
 * Returns 0 on success, or a negative error code.
 */
int shell_args_parse(shell_args_t* out, const char* line);

/* Get pointer to a slice of the original raw line starting at argv[start_index].
 * Useful for commands that treat the remainder as a template/message.
 */
const char* shell_args_rest_raw(const shell_args_t* args, uint32 start_index);

#endif
