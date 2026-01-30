#include <misc/types.h>
#include <utilities/shell/shell_command_info.h>
#include <utilities/shell/pipeline.h>

static const char* skip_spaces(const char* s) {
    while (s && *s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    return s;
}

static int streq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 0;
    }
    return b[n] == '\0';
}

/*
 * AArch64 shell dispatcher
 *
 * Takes a full command line and dispatches to an EYN-OS shell command handler
 * registered via REGISTER_SHELL_COMMAND (linker section .shellcmds).
 *
 * This intentionally avoids the i386 keyboard/history loop; the AArch64 bring-up
 * console provides the line editor and calls into this dispatcher.
 */
int aarch64_shell_dispatch_line(string line) {
    const char* s = skip_spaces(line);
    if (!s || *s == '\0') return 0;

    static int pipeline_inited = 0;
    if (!pipeline_inited) {
        init_pipeline_system();
        pipeline_inited = 1;
    }

    /* If the line contains pipeline/redirection operators, use the legacy
     * pipeline implementation which captures command output via shell redirect.
     */
    if (is_pipeline_command(s)) {
        pipeline_t* p = parse_pipeline(s);
        if (!p) {
            return -3;
        }
        int rc = execute_pipeline(p);
        free_pipeline(p);
        return rc;
    }

    int name_len = 0;
    while (s[name_len] && s[name_len] != ' ' && s[name_len] != '\t' && s[name_len] != '\r' && s[name_len] != '\n') {
        name_len++;
        if (name_len > 63) break;
    }

    const shell_command_info_t* cmd = __start_shellcmds;
    while (cmd < __stop_shellcmds) {
        if (cmd->name && streq_n(s, cmd->name, name_len)) {
            if (cmd->handler) {
                cmd->handler((string)line);
                return 0;
            }
            return -2;
        }
        cmd++;
    }

    return -1;
}
