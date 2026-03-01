#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Create or remove command aliases.", "alias ll ls -l");

#define ALIAS_CFG_PATH "/config/aliases.cfg"
#define ALIAS_MAX_COUNT 32
#define ALIAS_MAX_NAME 32
#define ALIAS_MAX_TMPL 200
#define ALIAS_FILE_MAX 4096

typedef struct {
    char name[ALIAS_MAX_NAME];
    char tmpl[ALIAS_MAX_TMPL];
} alias_entry_t;

static int is_name_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_';
}

static int alias_name_valid(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; name[i]; ++i) {
        if (i >= (ALIAS_MAX_NAME - 1)) return 0;
        if (!is_name_char(name[i])) return 0;
    }
    return 1;
}

static int contains_meta_chars(const char* s) {
    if (!s) return 1;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '|' || s[i] == '<' || s[i] == '>' || s[i] == '&') return 1;
    }
    return 0;
}

static int trim_bounds(const char* s, int start, int end, int* out_s, int* out_e) {
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
    *out_s = start;
    *out_e = end;
    return (start < end) ? 0 : -1;
}

static int alias_builtin_exists(const char* name) {
    char path[128];
    path[0] = '\0';
    strcpy(path, "/binaries/");
    if ((strlen(path) + strlen(name)) >= sizeof(path)) return 0;
    strcat(path, name);
    int fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

static int load_aliases(alias_entry_t entries[ALIAS_MAX_COUNT], int* out_count) {
    char buf[ALIAS_FILE_MAX];
    int fd = open(ALIAS_CFG_PATH, O_RDONLY, 0);
    *out_count = 0;
    if (fd < 0) return 0;

    int len = 0;
    while (len < (ALIAS_FILE_MAX - 1)) {
        int rc = (int)read(fd, buf + len, (size_t)(ALIAS_FILE_MAX - 1 - len));
        if (rc <= 0) break;
        len += rc;
    }
    close(fd);
    buf[len] = '\0';

    int line_start = 0;
    for (int i = 0; i <= len; ++i) {
        if (i < len && buf[i] != '\n') continue;

        int s = line_start;
        int e = i;
        line_start = i + 1;
        if (trim_bounds(buf, s, e, &s, &e) != 0) continue;
        if (buf[s] == '#') continue;

        int p = s;
        while (p < e && buf[p] != ' ' && buf[p] != '\t') p++;
        if (p >= e) continue;

        int ns = s;
        int ne = p;
        int ts = p;
        int te = e;
        if (trim_bounds(buf, ns, ne, &ns, &ne) != 0) continue;
        if (trim_bounds(buf, ts, te, &ts, &te) != 0) continue;

        int name_len = ne - ns;
        int tmpl_len = te - ts;
        if (name_len <= 0 || name_len >= ALIAS_MAX_NAME) continue;
        if (tmpl_len <= 0 || tmpl_len >= ALIAS_MAX_TMPL) continue;

        char name[ALIAS_MAX_NAME];
        char tmpl[ALIAS_MAX_TMPL];
        memcpy(name, buf + ns, (size_t)name_len);
        name[name_len] = '\0';
        memcpy(tmpl, buf + ts, (size_t)tmpl_len);
        tmpl[tmpl_len] = '\0';

        if (!alias_name_valid(name) || contains_meta_chars(tmpl) || alias_builtin_exists(name)) continue;
        if (*out_count >= ALIAS_MAX_COUNT) continue;

        strncpy(entries[*out_count].name, name, ALIAS_MAX_NAME - 1);
        entries[*out_count].name[ALIAS_MAX_NAME - 1] = '\0';
        strncpy(entries[*out_count].tmpl, tmpl, ALIAS_MAX_TMPL - 1);
        entries[*out_count].tmpl[ALIAS_MAX_TMPL - 1] = '\0';
        (*out_count)++;
    }

    return 0;
}

static int save_aliases(const alias_entry_t entries[ALIAS_MAX_COUNT], int count) {
    char out[ALIAS_FILE_MAX];
    int pos = 0;
    for (int i = 0; i < count; ++i) {
        int need = (int)strlen(entries[i].name) + 1 + (int)strlen(entries[i].tmpl) + 1;
        if (pos + need >= (ALIAS_FILE_MAX - 1)) break;
        strcpy(out + pos, entries[i].name);
        pos += (int)strlen(entries[i].name);
        out[pos++] = ' ';
        strcpy(out + pos, entries[i].tmpl);
        pos += (int)strlen(entries[i].tmpl);
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return (writefile(ALIAS_CFG_PATH, out, (size_t)pos) >= 0) ? 0 : -1;
}

static int find_alias(const alias_entry_t entries[ALIAS_MAX_COUNT], int count, const char* name) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

int main(int argc, char** argv) {
    alias_entry_t entries[ALIAS_MAX_COUNT];
    int count = 0;

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: alias <name> <template>");
        puts("       alias remove <name>");
        return 0;
    }

    if (load_aliases(entries, &count) != 0) {
        puts("alias: failed to load aliases");
        return 1;
    }

    if (argc < 2 || !argv[1] || !argv[1][0]) {
        puts("Usage: alias <name> <template>");
        puts("       alias remove <name>");
        return 1;
    }

    if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3 || !argv[2] || !argv[2][0]) {
            puts("Usage: alias remove <name>");
            return 1;
        }
        int idx = find_alias(entries, count, argv[2]);
        if (idx < 0) {
            printf("alias: not found: %s\n", argv[2]);
            return 1;
        }
        for (int i = idx; i < count - 1; ++i) entries[i] = entries[i + 1];
        count--;
        if (save_aliases(entries, count) != 0) {
            puts("alias: failed to save aliases");
            return 1;
        }
        printf("Alias removed: %s\n", argv[2]);
        return 0;
    }

    if (argc < 3) {
        puts("Usage: alias <name> <template>");
        return 1;
    }

    const char* name = argv[1];
    if (!alias_name_valid(name)) {
        puts("alias: invalid alias name");
        return 1;
    }
    if (alias_builtin_exists(name)) {
        puts("alias: cannot override existing command");
        return 1;
    }
    if (find_alias(entries, count, name) >= 0) {
        puts("alias: alias already exists");
        return 1;
    }

    char tmpl[ALIAS_MAX_TMPL];
    int pos = 0;
    tmpl[0] = '\0';
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i] ? argv[i] : "";
        int need = (int)strlen(a) + ((i > 2) ? 1 : 0);
        if (pos + need >= (ALIAS_MAX_TMPL - 1)) {
            puts("alias: template too long");
            return 1;
        }
        if (i > 2) tmpl[pos++] = ' ';
        strcpy(tmpl + pos, a);
        pos += (int)strlen(a);
    }
    tmpl[pos] = '\0';
    if (!tmpl[0] || contains_meta_chars(tmpl)) {
        puts("alias: invalid template");
        return 1;
    }

    if (count >= ALIAS_MAX_COUNT) {
        puts("alias: alias table full");
        return 1;
    }

    strncpy(entries[count].name, name, ALIAS_MAX_NAME - 1);
    entries[count].name[ALIAS_MAX_NAME - 1] = '\0';
    strncpy(entries[count].tmpl, tmpl, ALIAS_MAX_TMPL - 1);
    entries[count].tmpl[ALIAS_MAX_TMPL - 1] = '\0';
    count++;

    if (save_aliases(entries, count) != 0) {
        puts("alias: failed to save aliases");
        return 1;
    }

    printf("Alias set: %s\n", name);
    return 0;
}
