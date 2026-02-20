#include <shell_command_info.h>
#include <utilities/shell/shell_args.h>
#include <utilities/shell/alias.h>
#include <string.h>
#include <vga.h>

static void alias_cmd(const shell_args_t* args) {
    if (!args || args->argc < 2) {
        printf("%cUsage: alias <name> <command template>\n", 255, 255, 255);
        printf("%c       alias remove <name>\n", 255, 255, 255);
        printf("%cExample: alias compile chibicc [arg1] -o [arg2]\n", 255, 255, 255);
        return;
    }

    const char* tok = args->argv[1];
    if (strcmp(tok, "remove") == 0) {
        if (args->argc < 3) {
            printf("%cUsage: alias remove <name>\n", 255, 255, 255);
            return;
        }

        const char* name = args->argv[2];
        int rc = shell_alias_remove((string)name);
        if (rc == 0) {
            printf("%cAlias removed: %s\n", 0, 255, 0, name);
        } else {
            printf("%cError: alias not found or invalid: %s\n", 255, 0, 0, name);
        }
        return;
    }

    // Define alias: tok is name, rest is template (argv[2..])
    if (args->argc < 3) {
        printf("%cUsage: alias <name> <command template>\n", 255, 255, 255);
        return;
    }

    const char* name = tok;
    const char* tmpl = shell_args_rest_raw(args, 2);
    if (!tmpl || !tmpl[0]) {
        printf("%cUsage: alias <name> <command template>\n", 255, 255, 255);
        return;
    }

    if (shell_alias_exists(name)) {
        printf("%cError: alias already exists: %s\n", 255, 0, 0, name);
        return;
    }

    int rc = shell_alias_define((string)name, (string)tmpl);
    if (rc == 0) {
        printf("%cAlias set: %s\n", 0, 255, 0, name);
    } else if (rc == -2) {
        printf("%cError: cannot override existing command: %s\n", 255, 0, 0, name);
    } else {
        printf("%cError: invalid alias or template\n", 255, 0, 0);
    }
}

REGISTER_SHELL_COMMAND(alias_cmd_info, "alias", alias_cmd, CMD_STREAMING,
  "Create persistent command aliases (stored in /config/aliases.cfg).\n"
  "Usage: alias <name> <command template>\n"
  "       alias remove <name>\n"
  "Template placeholders: [arg1], [arg2], ...\n"
  "Example: alias compile chibicc [arg1] -o [arg2]\n"
  "         compile test.c test.uelf",
  "alias compile chibicc [arg1] -o [arg2]");
