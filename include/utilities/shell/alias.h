#ifndef SHELL_ALIAS_H
#define SHELL_ALIAS_H

#include <misc/types.h>

// Persistent command aliases stored in /config/aliases.cfg.
//
// Syntax:
//   alias <name> <command template>
//   alias remove <name>
//
// Name may be a multi-word phrase. Config supports:
//   "multi word name" <command template>
//   multi word name = <command template>
//   (legacy) singleword <command template>
//
// Template supports placeholders:
//   [arg1], [arg2], ... replaced by positional args to the alias invocation.
//
// Aliases never override built-in commands.

// Expands `input` if it begins with a known alias. Writes the expanded command
// into `out`.
// Returns: 1 expanded, 0 not an alias, -1 error.
int shell_alias_expand_line(const char *input, char *out, int out_size);

// Define a new alias.
// Returns 0 on success, <0 on error.
int shell_alias_define(const char *name, const char *template_cmd);

// Remove an alias.
// Returns 0 on success, <0 on error.
int shell_alias_remove(const char *name);

// Returns 1 if alias exists, 0 otherwise.
int shell_alias_exists(const char *name);

#endif
