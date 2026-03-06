/*
 * EYN-OS Shell Script Interpreter
 *
 * Line-oriented interpreter for .shell script files.  Scripts are read
 * from the EYNFS filesystem and executed line-by-line through the normal
 * shell command dispatch path (handle_shell_command).
 *
 * Supported features:
 *   - Comments (#)
 *   - Variables:  set NAME=value  /  $NAME  /  ${NAME}
 *   - Script arguments:  $0 (script path), $1 .. $9
 *   - Command substitution: $(command args)
 *   - Conditionals:  if COND / elif COND / else / endif
 *   - While loops:   while COND / endwhile
 *   - echo built-in
 *   - exit [code]
 *
 * Design constraints (low-RAM friendly):
 *   - Variables stored in a bounded flat table; no dynamic list growth.
 *   - Script file read in one allocation (size from vfs_stat), freed on exit.
 *   - All scratch buffers are stack-local or bounded statics.
 */

#include <misc/types.h>
#include <utilities/shell/shell_script.h>
#include <fs/vfs.h>
#include <vga.h>
#include <string.h>
#include <util.h>
#include <utilities/shell/fs_commands.h>
#include <watchdog.h>

/* Forward declaration – defined in shell.c */
extern void handle_shell_command(string input);
extern uint8 g_current_drive;

/* Shell redirect and current path are declared in vga.h and fs_commands.h */

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

/*
 * Maximum number of user-defined variables per script invocation.
 *
 * Invariant: the variable table is a flat array scanned linearly.
 * Increasing this value increases per-invocation stack usage
 * (MAX_VARS * sizeof(script_var_t) ≈ MAX_VARS * 200 bytes).
 * Decreasing below the number of `set` statements in any script
 * will cause a "too many variables" error at runtime.
 */
#define MAX_VARS       64

/*
 * Maximum length of a single script line after variable expansion.
 * Lines longer than this are truncated.
 */
#define MAX_LINE      512

/*
 * Maximum nesting depth for if/while blocks.
 */
#define MAX_DEPTH      16

/*
 * Maximum script arguments ($0..$9).
 */
#define MAX_SCRIPT_ARGS 10

/*
 * Maximum script file size (bytes).  Scripts are read entirely into
 * a malloc'd buffer; capping this prevents accidental OOM.
 */
#define MAX_SCRIPT_SIZE (64u * 1024u)

/* ------------------------------------------------------------------ */
/*  Variable table                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[64];
    char value[128];
} script_var_t;

typedef struct {
    script_var_t vars[MAX_VARS];
    int count;

    /* Script arguments ($0..$9) */
    const char *args[MAX_SCRIPT_ARGS];
    int argc;
} script_env_t;

static void env_init(script_env_t *env, const char *script_path,
                     int argc, const char *const *argv) {
    memset(env, 0, sizeof(*env));
    env->args[0] = script_path;
    env->argc = 1;
    for (int i = 0; i < argc && env->argc < MAX_SCRIPT_ARGS; i++) {
        env->args[env->argc++] = argv[i];
    }
}

static const char *env_get(const script_env_t *env, const char *name) {
    /* $0..$9 */
    if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0') {
        int idx = name[0] - '0';
        if (idx < env->argc) return env->args[idx];
        return "";
    }
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->vars[i].name, name) == 0)
            return env->vars[i].value;
    }
    return NULL;
}

static int env_set(script_env_t *env, const char *name, const char *value) {
    /* Overwrite if already exists */
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->vars[i].name, name) == 0) {
            strncpy(env->vars[i].value, value, sizeof(env->vars[i].value) - 1);
            env->vars[i].value[sizeof(env->vars[i].value) - 1] = '\0';
            return 0;
        }
    }
    if (env->count >= MAX_VARS) return -1;
    strncpy(env->vars[env->count].name, name, sizeof(env->vars[env->count].name) - 1);
    env->vars[env->count].name[sizeof(env->vars[env->count].name) - 1] = '\0';
    strncpy(env->vars[env->count].value, value, sizeof(env->vars[env->count].value) - 1);
    env->vars[env->count].value[sizeof(env->vars[env->count].value) - 1] = '\0';
    env->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Variable expansion                                                */
/* ------------------------------------------------------------------ */

/*
 * Expand $VAR, ${VAR}, and $(command) in `src` into `dst`.
 * Returns 0 on success, -1 on buffer overflow.
 */
static int expand_variables(const char *src, char *dst, int dst_size,
                            script_env_t *env) {
    int si = 0, di = 0;

    while (src[si] && di < dst_size - 1) {
        if (src[si] == '$') {
            si++; /* skip '$' */

            /* $(command) – command substitution */
            if (src[si] == '(') {
                si++; /* skip '(' */
                char cmd_buf[MAX_LINE];
                int ci = 0;
                int depth = 1;
                while (src[si] && depth > 0 && ci < MAX_LINE - 1) {
                    if (src[si] == '(') depth++;
                    else if (src[si] == ')') { depth--; if (depth == 0) { si++; break; } }
                    cmd_buf[ci++] = src[si++];
                }
                cmd_buf[ci] = '\0';

                /* Expand variables inside the command itself */
                char expanded_cmd[MAX_LINE];
                expand_variables(cmd_buf, expanded_cmd, MAX_LINE, env);

                /* Capture command output via shell redirect */
                start_shell_redirect();
                handle_shell_command(expanded_cmd);
                /* Copy captured output, stripping trailing newlines */
                int len = shell_redirect_pos;
                while (len > 0 && (shell_redirect_buf[len - 1] == '\n' ||
                                   shell_redirect_buf[len - 1] == '\r'))
                    len--;
                for (int k = 0; k < len && di < dst_size - 1; k++)
                    dst[di++] = shell_redirect_buf[k];
                stop_shell_redirect();
                continue;
            }

            /* ${VAR} */
            if (src[si] == '{') {
                si++; /* skip '{' */
                char var_name[64];
                int ni = 0;
                while (src[si] && src[si] != '}' && ni < 63)
                    var_name[ni++] = src[si++];
                var_name[ni] = '\0';
                if (src[si] == '}') si++;

                const char *val = env_get(env, var_name);
                if (val) {
                    while (*val && di < dst_size - 1) dst[di++] = *val++;
                }
                continue;
            }

            /* $VAR (alphanumeric + underscore) */
            {
                char var_name[64];
                int ni = 0;
                while (src[si] && ni < 63 &&
                       ((src[si] >= 'a' && src[si] <= 'z') ||
                        (src[si] >= 'A' && src[si] <= 'Z') ||
                        (src[si] >= '0' && src[si] <= '9') ||
                        src[si] == '_'))
                    var_name[ni++] = src[si++];
                var_name[ni] = '\0';

                if (ni == 0) {
                    /* Lone '$' – just copy it */
                    dst[di++] = '$';
                    continue;
                }
                const char *val = env_get(env, var_name);
                if (val) {
                    while (*val && di < dst_size - 1) dst[di++] = *val++;
                }
                continue;
            }
        }
        dst[di++] = src[si++];
    }
    dst[di] = '\0';
    return (di >= dst_size - 1 && src[si]) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Condition evaluation                                              */
/* ------------------------------------------------------------------ */

/*
 * Evaluate a condition string.
 *
 * Supported forms:
 *   A == B        string equality (after variable expansion)
 *   A != B        string inequality
 *   -e PATH       file or directory exists
 *   -f PATH       file exists (not directory)
 *   WORD          true if non-empty and not "0"
 */
static int eval_condition(const char *cond, script_env_t *env) {
    /* Skip leading whitespace */
    while (*cond == ' ' || *cond == '\t') cond++;

    /* Expand variables in the condition */
    char expanded[MAX_LINE];
    expand_variables(cond, expanded, MAX_LINE, env);
    const char *p = expanded;
    while (*p == ' ' || *p == '\t') p++;

    /* -e / -f tests */
    if (p[0] == '-' && (p[1] == 'e' || p[1] == 'f') && (p[2] == ' ' || p[2] == '\t')) {
        char test_type = p[1];
        p += 2;
        while (*p == ' ' || *p == '\t') p++;

        char path_buf[128];
        int pi = 0;
        while (*p && *p != ' ' && *p != '\t' && pi < 127)
            path_buf[pi++] = *p++;
        path_buf[pi] = '\0';

        /* Resolve path relative to shell cwd */
        char abspath[128];
        resolve_path(path_buf, shell_current_path, abspath, sizeof(abspath));

        vfs_stat_t st;
        if (vfs_stat(g_current_drive, abspath, &st) != 0)
            return 0; /* doesn't exist */
        if (test_type == 'f')
            return st.type == VFS_NODE_FILE;
        return 1; /* -e: exists */
    }

    /* Look for == or != operator */
    const char *op = NULL;
    int op_len = 0;
    int is_eq = 0;

    /* Scan for == or != (not inside leading word) */
    {
        const char *scan = p;
        /* Find first operand end */
        while (*scan && *scan != ' ' && *scan != '\t') scan++;
        while (*scan == ' ' || *scan == '\t') scan++;
        if (scan[0] == '=' && scan[1] == '=') {
            op = scan; op_len = 2; is_eq = 1;
        } else if (scan[0] == '!' && scan[1] == '=') {
            op = scan; op_len = 2; is_eq = 0;
        }
    }

    if (op) {
        /* Extract left operand */
        char left[128];
        int li = 0;
        const char *lp = p;
        while (lp < op && li < 127) {
            left[li++] = *lp++;
        }
        left[li] = '\0';
        /* Trim trailing whitespace */
        while (li > 0 && (left[li - 1] == ' ' || left[li - 1] == '\t'))
            left[--li] = '\0';

        /* Extract right operand */
        const char *rp = op + op_len;
        while (*rp == ' ' || *rp == '\t') rp++;
        char right[128];
        int ri = 0;
        while (*rp && *rp != ' ' && *rp != '\t' && *rp != '\n' && ri < 127)
            right[ri++] = *rp++;
        right[ri] = '\0';

        int cmp = strcmp(left, right);
        return is_eq ? (cmp == 0) : (cmp != 0);
    }

    /* Simple truthiness: non-empty and not "0" */
    if (*p == '\0' || strcmp(p, "0") == 0)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Control-flow stack                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    BLOCK_IF,
    BLOCK_WHILE
} block_type_t;

typedef struct {
    block_type_t type;
    int executing;       /* 1 if the current branch is active */
    int any_taken;       /* (if-blocks) 1 if any branch has been taken */
    int line_start;      /* (while-blocks) line index of the `while` keyword */
} block_entry_t;

typedef struct {
    block_entry_t stack[MAX_DEPTH];
    int depth;
} block_stack_t;

/* Are we inside a block that is currently suppressed (not executing)? */
static int block_is_suppressed(const block_stack_t *bs) {
    for (int i = 0; i < bs->depth; i++) {
        if (!bs->stack[i].executing) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Line parsing helpers                                              */
/* ------------------------------------------------------------------ */

/* Skip leading whitespace; return pointer into the same buffer */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Check if line starts with a keyword (followed by space/tab/end) */
static int starts_with_kw(const char *line, const char *kw) {
    int len = (int)strlen(kw);
    if (strncmp(line, kw, (size_t)len) != 0) return 0;
    return (line[len] == '\0' || line[len] == ' ' || line[len] == '\t');
}

/* ------------------------------------------------------------------ */
/*  Core interpreter                                                  */
/* ------------------------------------------------------------------ */

/*
 * Parse the script buffer (NUL-terminated) into lines and execute them.
 * Returns 0 on normal completion, negative on error.
 */
static int interpret(char *script_buf, int script_len,
                     script_env_t *env) {
    /*
     * Build a line index: scan for newlines and build an array of
     * line-start offsets so we can jump back for while loops.
     * Maximum ~2048 lines per script.
     */
    #define MAX_LINES 2048
    int line_starts[MAX_LINES];
    int line_count = 0;

    /* First line always starts at offset 0 */
    line_starts[0] = 0;
    line_count = 1;
    for (int i = 0; i < script_len && line_count < MAX_LINES; i++) {
        if (script_buf[i] == '\n') {
            script_buf[i] = '\0';  /* terminate the line */
            if (i + 1 < script_len) {
                line_starts[line_count++] = i + 1;
            }
        }
        /* Strip carriage returns */
        if (script_buf[i] == '\r') script_buf[i] = '\0';
    }
    /* Make sure the last line is NUL-terminated */
    if (script_len > 0) script_buf[script_len] = '\0';

    block_stack_t blocks;
    memset(&blocks, 0, sizeof(blocks));

    int exit_requested = 0;

    for (int ln = 0; ln < line_count && !exit_requested; ln++) {
        watchdog_kick("shell-script");

        const char *raw_line = &script_buf[line_starts[ln]];
        const char *trimmed = skip_ws(raw_line);

        /* Skip blank lines and comments */
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        /* ── Control-flow keywords must be recognized even when suppressed ── */

        /* --- if --- */
        if (starts_with_kw(trimmed, "if")) {
            if (blocks.depth >= MAX_DEPTH) {
                printf("%c[script] Error: too many nested blocks (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            block_entry_t *b = &blocks.stack[blocks.depth++];
            b->type = BLOCK_IF;
            if (block_is_suppressed(&blocks)) {
                /* Outer block suppressed → this block is also suppressed */
                b->executing = 0;
                b->any_taken = 1; /* prevent elif/else from activating */
            } else {
                const char *cond = skip_ws(trimmed + 2);
                int result = eval_condition(cond, env);
                b->executing = result;
                b->any_taken = result;
            }
            continue;
        }

        /* --- elif --- */
        if (starts_with_kw(trimmed, "elif")) {
            if (blocks.depth == 0 || blocks.stack[blocks.depth - 1].type != BLOCK_IF) {
                printf("%c[script] Error: 'elif' without matching 'if' (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            block_entry_t *b = &blocks.stack[blocks.depth - 1];
            /* Check if some outer block is suppressed */
            int outer_suppressed = 0;
            for (int i = 0; i < blocks.depth - 1; i++) {
                if (!blocks.stack[i].executing) { outer_suppressed = 1; break; }
            }
            if (outer_suppressed) {
                b->executing = 0;
            } else if (b->any_taken) {
                b->executing = 0;
            } else {
                const char *cond = skip_ws(trimmed + 4);
                int result = eval_condition(cond, env);
                b->executing = result;
                if (result) b->any_taken = 1;
            }
            continue;
        }

        /* --- else --- */
        if (starts_with_kw(trimmed, "else")) {
            if (blocks.depth == 0 || blocks.stack[blocks.depth - 1].type != BLOCK_IF) {
                printf("%c[script] Error: 'else' without matching 'if' (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            block_entry_t *b = &blocks.stack[blocks.depth - 1];
            int outer_suppressed = 0;
            for (int i = 0; i < blocks.depth - 1; i++) {
                if (!blocks.stack[i].executing) { outer_suppressed = 1; break; }
            }
            if (outer_suppressed) {
                b->executing = 0;
            } else {
                b->executing = !b->any_taken;
            }
            continue;
        }

        /* --- endif --- */
        if (starts_with_kw(trimmed, "endif")) {
            if (blocks.depth == 0 || blocks.stack[blocks.depth - 1].type != BLOCK_IF) {
                printf("%c[script] Error: 'endif' without matching 'if' (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            blocks.depth--;
            continue;
        }

        /* --- while --- */
        if (starts_with_kw(trimmed, "while")) {
            if (blocks.depth >= MAX_DEPTH) {
                printf("%c[script] Error: too many nested blocks (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            block_entry_t *b = &blocks.stack[blocks.depth++];
            b->type = BLOCK_WHILE;
            b->line_start = ln; /* remember the `while` line for looping */
            if (block_is_suppressed(&blocks)) {
                b->executing = 0;
            } else {
                const char *cond = skip_ws(trimmed + 5);
                int result = eval_condition(cond, env);
                b->executing = result;
            }
            continue;
        }

        /* --- endwhile --- */
        if (starts_with_kw(trimmed, "endwhile")) {
            if (blocks.depth == 0 || blocks.stack[blocks.depth - 1].type != BLOCK_WHILE) {
                printf("%c[script] Error: 'endwhile' without matching 'while' (line %d)\n",
                       255, 0, 0, ln + 1);
                return -1;
            }
            block_entry_t *b = &blocks.stack[blocks.depth - 1];
            if (b->executing) {
                /* Jump back to the while line to re-evaluate the condition */
                ln = b->line_start - 1; /* -1 because the for loop will increment */
                blocks.depth--;         /* pop; the while keyword will re-push */
            } else {
                blocks.depth--;
            }
            continue;
        }

        /* ── If we are inside a suppressed block, skip the line ── */
        if (block_is_suppressed(&blocks))
            continue;

        /* ── Variable expansion ── */
        char expanded[MAX_LINE];
        if (expand_variables(trimmed, expanded, MAX_LINE, env) < 0) {
            printf("%c[script] Warning: line %d truncated after expansion\n",
                   255, 128, 0, ln + 1);
        }
        const char *line = skip_ws(expanded);
        if (*line == '\0') continue;

        /* ── Built-in statements ── */

        /* set VAR=value */
        if (starts_with_kw(line, "set")) {
            const char *rest = skip_ws(line + 3);
            const char *eq = NULL;
            for (const char *s = rest; *s; s++) {
                if (*s == '=') { eq = s; break; }
            }
            if (!eq || eq == rest) {
                printf("%c[script] Error: invalid 'set' syntax (line %d)\n",
                       255, 0, 0, ln + 1);
                continue;
            }
            char var_name[64];
            int nlen = (int)(eq - rest);
            if (nlen > 63) nlen = 63;
            memcpy(var_name, rest, (size_t)nlen);
            var_name[nlen] = '\0';

            const char *val = eq + 1;
            if (env_set(env, var_name, val) < 0) {
                printf("%c[script] Error: too many variables (line %d)\n",
                       255, 0, 0, ln + 1);
            }
            continue;
        }

        /* echo ... */
        if (starts_with_kw(line, "echo")) {
            const char *msg = skip_ws(line + 4);
            printf("%s\n", msg);
            continue;
        }

        /* exit [code] */
        if (starts_with_kw(line, "exit")) {
            exit_requested = 1;
            continue;
        }

        /* ── Everything else: dispatch as a shell command ── */
        /*
         * We need a mutable copy because handle_shell_command takes `string`
         * (which is char*) and may modify the buffer internally.
         */
        char cmd_copy[MAX_LINE];
        strncpy(cmd_copy, line, MAX_LINE - 1);
        cmd_copy[MAX_LINE - 1] = '\0';
        handle_shell_command(cmd_copy);
    }

    /* Check for unclosed blocks */
    if (blocks.depth > 0) {
        printf("%c[script] Warning: %d unclosed block(s) at end of script\n",
               255, 128, 0, blocks.depth);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int shell_script_run(uint8 drive, const char *path, int argc,
                     const char *const *argv) {
    if (!path || !path[0]) return -1;

    /* Stat the file to get its size */
    vfs_stat_t st;
    if (vfs_stat(drive, path, &st) != 0) {
        printf("%c[script] Error: file not found: %s\n", 255, 0, 0, path);
        return -1;
    }
    if (st.type != VFS_NODE_FILE) {
        printf("%c[script] Error: not a file: %s\n", 255, 0, 0, path);
        return -1;
    }
    if (st.size == 0) {
        return 0; /* empty script, nothing to do */
    }
    if (st.size > MAX_SCRIPT_SIZE) {
        printf("%c[script] Error: script too large (%u bytes, max %u): %s\n",
               255, 0, 0, st.size, MAX_SCRIPT_SIZE, path);
        return -1;
    }

    /* Allocate buffer (+1 for NUL terminator) */
    uint32 alloc_size = st.size + 1;
    char *buf = (char *)malloc(alloc_size);
    if (!buf) {
        printf("%c[script] Error: out of memory (%u bytes): %s\n",
               255, 0, 0, alloc_size, path);
        return -1;
    }

    /* Read the script file */
    int bytes_read = vfs_read_file(drive, path, buf, (int)st.size);
    if (bytes_read < 0) {
        printf("%c[script] Error: failed to read: %s\n", 255, 0, 0, path);
        free(buf);
        return -1;
    }
    buf[bytes_read] = '\0';

    /* Set up environment and interpret */
    script_env_t env;
    env_init(&env, path, argc, argv);

    int result = interpret(buf, bytes_read, &env);

    free(buf);
    return result;
}
