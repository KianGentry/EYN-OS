#include <utilities/shell/shell_args.h>

#include <string.h>

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int shell_args_parse(shell_args_t* out, const char* line) {
    if (!out) return -1;

    memset(out, 0, sizeof(*out));
    out->raw = line ? line : "";
    out->raw_len = (uint32)strlen(out->raw);

    if (!line || !line[0]) {
        out->argc = 0;
        return 0;
    }

    uint32 argc = 0;
    uint32 i = 0;
    uint32 w = 0;

    while (line[i]) {
        while (line[i] && is_ws(line[i])) i++;
        if (!line[i]) break;

        if (argc >= SHELL_ARGS_MAX) {
            return -2; // too many args
        }

        out->raw_offs[argc] = (uint16)i;
        out->argv[argc] = &out->storage[w];

        while (line[i] && !is_ws(line[i])) {
            if (w + 1 >= SHELL_ARGS_STORAGE_BYTES) {
                return -3; // storage overflow
            }
            out->storage[w++] = line[i++];
        }

        if (w >= SHELL_ARGS_STORAGE_BYTES) {
            return -3;
        }
        out->storage[w++] = '\0';
        argc++;
    }

    out->argc = argc;
    return 0;
}

const char* shell_args_rest_raw(const shell_args_t* args, uint32 start_index) {
    if (!args || !args->raw) return "";
    if (start_index >= args->argc) return "";

    uint32 off = (uint32)args->raw_offs[start_index];
    if (off >= args->raw_len) return "";

    return args->raw + off;
}
