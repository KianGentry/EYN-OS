#include <utilities/shell/alias.h>

#include <fs/vfs.h>
#include <shell.h>
#include <string.h>
#include <util.h>

// Config path (absolute)
#define ALIAS_CFG_PATH "/config/aliases.cfg"

#define ALIAS_MAX_COUNT 32
#define ALIAS_MAX_NAME  32
#define ALIAS_MAX_TMPL  200
#define ALIAS_FILE_MAX  4096

typedef struct {
    char name[ALIAS_MAX_NAME];
    char tmpl[ALIAS_MAX_TMPL];
} alias_entry_t;

static alias_entry_t g_aliases[ALIAS_MAX_COUNT];
static int g_alias_count = 0;
static int g_alias_loaded = 0;

// System config is expected on primary drive.
extern uint8 g_current_drive;

static uint8 alias_cfg_drive(void) {
    // Keep this simple: store aliases on the current drive.
    // (If you want global-only, change to 0.)
    return g_current_drive;
}

static int is_name_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '_') return 1;
    return 0;
}

static int alias_name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; name[i]; i++) {
        if (i >= (ALIAS_MAX_NAME - 1)) return 0;
        if (!is_name_char(name[i])) return 0;
    }
    return 1;
}

static alias_entry_t *alias_find_mut(const char *name) {
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0)
            return &g_aliases[i];
    }
    return NULL;
}

int shell_alias_exists(const char *name) {
    if (!g_alias_loaded) {
        // lazy load
        (void)shell_alias_define("__lazy_load_guard__", "");
        // define() above will return error; but triggers load via helper.
    }
    return alias_find_mut(name) != NULL;
}

static void alias_clear_all(void) {
    memset(g_aliases, 0, sizeof(g_aliases));
    g_alias_count = 0;
}

static void trim_right(char *s) {
    int n = (int)strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[n - 1] = '\0';
            n--;
            continue;
        }
        break;
    }
}

static char *trim_left(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

static int contains_meta_chars(const char *s) {
    // Keep aliases simple and safe: no pipeline/redirection/control operators.
    for (int i = 0; s[i]; i++) {
        if (s[i] == '|' || s[i] == '<' || s[i] == '>' || s[i] == '&')
            return 1;
    }
    return 0;
}

static void alias_load_if_needed(void) {
    if (g_alias_loaded)
        return;
    g_alias_loaded = 1;

    alias_clear_all();

    vfs_stat_t st;
    if (vfs_stat(alias_cfg_drive(), ALIAS_CFG_PATH, &st) != 0 || st.type != VFS_NODE_FILE)
        return;

    uint32 size = 0;
    if (vfs_get_file_size(alias_cfg_drive(), ALIAS_CFG_PATH, &size) != 0)
        return;
    if (size == 0)
        return;
    if (size > (ALIAS_FILE_MAX - 1))
        size = (ALIAS_FILE_MAX - 1);

    static char buf[ALIAS_FILE_MAX];
    int br = vfs_read_file(alias_cfg_drive(), ALIAS_CFG_PATH, buf, (int)size);
    if (br <= 0)
        return;
    buf[br] = '\0';

    char *p = buf;
    while (*p) {
        // Extract one line
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = '\0'; p++; }

        line = trim_left(line);
        trim_right(line);
        if (!line[0])
            continue;
        if (line[0] == '#')
            continue;

        // name is first token, rest is template
        char *sp = line;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp)
            continue;
        *sp = '\0';
        char *name = line;
        char *tmpl = trim_left(sp + 1);
        trim_right(tmpl);

        if (!alias_name_valid(name))
            continue;
        if (!tmpl[0])
            continue;
        if (contains_meta_chars(tmpl))
            continue;
        if (find_command(name) != NULL)
            continue;
        if (alias_find_mut(name) != NULL)
            continue;
        if (g_alias_count >= ALIAS_MAX_COUNT)
            continue;

        strncpy(g_aliases[g_alias_count].name, name, ALIAS_MAX_NAME - 1);
        strncpy(g_aliases[g_alias_count].tmpl, tmpl, ALIAS_MAX_TMPL - 1);
        g_alias_count++;
    }
}

static int alias_save(void) {
    // Ensure /config exists
    vfs_stat_t st;
    if (vfs_stat(alias_cfg_drive(), "/config", &st) != 0 || st.type != VFS_NODE_DIR) {
        (void)vfs_mkdir(alias_cfg_drive(), "/config");
    }

    static char out[ALIAS_FILE_MAX];
    int pos = 0;
    for (int i = 0; i < g_alias_count; i++) {
        const char *name = g_aliases[i].name;
        const char *tmpl = g_aliases[i].tmpl;
        int need = (int)strlen(name) + 1 + (int)strlen(tmpl) + 1;
        if (pos + need >= (ALIAS_FILE_MAX - 1))
            break;
        strcpy(out + pos, name);
        pos += (int)strlen(name);
        out[pos++] = ' ';
        strcpy(out + pos, tmpl);
        pos += (int)strlen(tmpl);
        out[pos++] = '\n';
    }
    out[pos] = '\0';

    int wr = vfs_write_file(alias_cfg_drive(), ALIAS_CFG_PATH, out, (uint32)pos);
    return (wr >= 0) ? 0 : -1;
}

int shell_alias_define(const char *name, const char *template_cmd) {
    alias_load_if_needed();

    // internal lazy-load guard call
    if (name && strcmp(name, "__lazy_load_guard__") == 0)
        return -1;

    if (!alias_name_valid(name))
        return -1;
    if (!template_cmd || !template_cmd[0])
        return -1;
    if ((int)strlen(template_cmd) >= ALIAS_MAX_TMPL)
        return -1;

    if (contains_meta_chars(template_cmd))
        return -1;

    // Disallow overriding built-ins or existing aliases.
    if (find_command(name) != NULL)
        return -2;
    if (alias_find_mut(name) != NULL)
        return -3;

    if (g_alias_count >= ALIAS_MAX_COUNT)
        return -4;

    strncpy(g_aliases[g_alias_count].name, name, ALIAS_MAX_NAME - 1);
    strncpy(g_aliases[g_alias_count].tmpl, template_cmd, ALIAS_MAX_TMPL - 1);
    g_alias_count++;

    return alias_save();
}

int shell_alias_remove(const char *name) {
    alias_load_if_needed();

    if (!alias_name_valid(name))
        return -1;

    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) {
            // compact
            for (int j = i; j < g_alias_count - 1; j++)
                g_aliases[j] = g_aliases[j + 1];
            memset(&g_aliases[g_alias_count - 1], 0, sizeof(g_aliases[0]));
            g_alias_count--;
            return alias_save();
        }
    }
    return -2;
}

static int parse_arg_placeholder(const char *tok) {
    // Matches "[argN]" where N is 1..99
    if (!tok) return 0;
    if (tok[0] != '[') return 0;
    if (tok[1] != 'a' || tok[2] != 'r' || tok[3] != 'g') return 0;

    int i = 4;
    int n = 0;
    if (tok[i] < '0' || tok[i] > '9') return 0;
    while (tok[i] >= '0' && tok[i] <= '9') {
        n = n * 10 + (tok[i] - '0');
        i++;
        if (n > 99) return 0;
    }
    if (tok[i] != ']') return 0;
    if (tok[i + 1] != '\0') return 0;
    return n;
}

int shell_alias_expand_line(const char *input, char *out, int out_size) {
    alias_load_if_needed();

    if (!input || !out || out_size <= 0)
        return -1;

    // Extract command name
    char name[ALIAS_MAX_NAME] = {0};
    int i = 0;
    while (input[i] && input[i] != ' ' && input[i] != '\t' && i < (ALIAS_MAX_NAME - 1)) {
        name[i] = input[i];
        i++;
    }
    name[i] = '\0';

    if (!name[0])
        return 0;

    // Never override built-ins
    if (find_command(name) != NULL)
        return 0;

    alias_entry_t *ent = alias_find_mut(name);
    if (!ent)
        return 0;

    // Collect invocation args (tokens after name)
    const char *p = input + i;
    while (*p == ' ' || *p == '\t') p++;

    const char *args[16] = {0};
    int argc = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - start);
        if (len <= 0) break;
        if (argc < 16) {
            // store pointer into input; safe for duration
            args[argc++] = start;
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    // Expand template token-by-token
    int pos = 0;
    const char *t = ent->tmpl;
    while (*t) {
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) break;

        char tok[64] = {0};
        int tl = 0;
        while (t[tl] && t[tl] != ' ' && t[tl] != '\t' && tl < (int)sizeof(tok) - 1) {
            tok[tl] = t[tl];
            tl++;
        }
        tok[tl] = '\0';
        t += tl;

        const char *emit = tok;
        int emit_len = tl;

        int n = parse_arg_placeholder(tok);
        if (n > 0) {
            if (n > argc)
                return -1;
            // Arg token is a substring in input; compute length
            const char *a = args[n - 1];
            int al = 0;
            while (a[al] && a[al] != ' ' && a[al] != '\t') al++;
            emit = a;
            emit_len = al;
        }

        if (emit_len <= 0)
            continue;

        if (pos != 0) {
            if (pos + 1 >= out_size)
                return -1;
            out[pos++] = ' ';
        }

        if (pos + emit_len >= out_size)
            return -1;
        for (int k = 0; k < emit_len; k++)
            out[pos++] = emit[k];
    }

    out[pos] = '\0';

    if (!out[0])
        return -1;

    if (contains_meta_chars(out))
        return -1;

    return 1;
}
