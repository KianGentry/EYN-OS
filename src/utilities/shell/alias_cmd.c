#include <shell_command_info.h>
#include <utilities/shell/alias.h>
#include <string.h>
#include <vga.h>

static void alias_cmd(string arg) {
    if (!arg) return;

    // Skip command name
    int i = 0;
    while (arg[i] && arg[i] != ' ') i++;
    while (arg[i] == ' ') i++;

    if (!arg[i]) {
        printf("%cUsage: alias <name> <command template>\n", 255, 255, 255);
        printf("%c       alias remove <name>\n", 255, 255, 255);
        printf("%cExample: alias compile chibicc [arg1] -o [arg2]\n", 255, 255, 255);
        return;
    }

    // Read next token (name or 'remove')
    char tok[32] = {0};
    int j = 0;
    while (arg[i] && arg[i] != ' ' && j < (int)sizeof(tok) - 1) {
        tok[j++] = arg[i++];
    }
    tok[j] = '\0';
    while (arg[i] == ' ') i++;

    if (strcmp(tok, "remove") == 0) {
        if (!arg[i]) {
            printf("%cUsage: alias remove <name>\n", 255, 255, 255);
            return;
        }

        char name[32] = {0};
        j = 0;
        while (arg[i] && arg[i] != ' ' && j < (int)sizeof(name) - 1) {
            name[j++] = arg[i++];
        }
        name[j] = '\0';

        int rc = shell_alias_remove(name);
        if (rc == 0) {
            printf("%cAlias removed: %s\n", 0, 255, 0, name);
        } else {
            printf("%cError: alias not found or invalid: %s\n", 255, 0, 0, name);
        }
        return;
    }

    // Define alias: tok is name, rest is template
    const char *name = tok;
    const char *tmpl = arg + i;
    while (*tmpl == ' ') tmpl++;

    if (!tmpl[0]) {
        printf("%cUsage: alias <name> <command template>\n", 255, 255, 255);
        return;
    }

    if (shell_alias_exists(name)) {
        printf("%cError: alias already exists: %s\n", 255, 0, 0, name);
        return;
    }

    int rc = shell_alias_define(name, tmpl);
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
