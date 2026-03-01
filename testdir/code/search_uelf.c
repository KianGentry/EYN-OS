#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Search for text in filenames and file contents.", "search hello -a");

/*
 * SECURITY-INVARIANT: Bounded path depth for recursive traversal.
 *
 * Why: Prevents unbounded stack growth in low-RAM configurations while still
 * allowing practical subtree scanning.
 * Invariant: Recursive walker never descends deeper than SEARCH_MAX_DEPTH.
 * Breakage if changed:
 *   - Increasing can overflow small ring3 stacks under deep directory trees.
 *   - Decreasing may skip valid nested paths unexpectedly.
 * ABI-sensitive: No.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (resource exhaustion boundary).
 */
#define SEARCH_MAX_DEPTH 16

/*
 * SECURITY-INVARIANT: Upper bound for composed absolute/relative paths.
 *
 * Why: Prevents overflow while joining parent + child names during recursion.
 * Invariant: All path joins must fit within SEARCH_PATH_MAX, otherwise skipped.
 * Breakage if changed:
 *   - Increasing raises per-frame stack usage.
 *   - Decreasing causes more long-path entries to be skipped.
 * ABI-sensitive: No.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (memory safety boundary).
 */
#define SEARCH_PATH_MAX 512
#define SEARCH_READ_CHUNK 256
#define SEARCH_CARRY_MAX 127

typedef struct {
    int filename;
    int content;
    const char* needle;
} search_opts_t;

static void usage(void) {
    puts("Usage: search <pattern> [-f|-c|-a] [path]");
    puts("  -f  search in names only");
    puts("  -c  search in file contents only");
    puts("  -a  search in both (default)");
}

static int streq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

static int safe_join_path(const char* base, const char* name, char out[SEARCH_PATH_MAX]) {
    size_t b = (base && base[0]) ? strlen(base) : 0;
    size_t n = name ? strlen(name) : 0;
    if (n == 0) return -1;

    if (b == 0) {
        if (n + 1 > SEARCH_PATH_MAX) return -1;
        memcpy(out, name, n + 1);
        return 0;
    }

    int needs_slash = (base[b - 1] != '/');
    size_t total = b + (size_t)(needs_slash ? 1 : 0) + n + 1;
    if (total > SEARCH_PATH_MAX) return -1;

    memcpy(out, base, b);
    size_t p = b;
    if (needs_slash) out[p++] = '/';
    memcpy(out + p, name, n);
    out[p + n] = '\0';
    return 0;
}

static int file_contains_needle(const char* path, const char* needle) {
    if (!needle || needle[0] == '\0') return 0;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;

    size_t needle_len = strlen(needle);
    if (needle_len > SEARCH_CARRY_MAX) needle_len = SEARCH_CARRY_MAX;

    char chunk[SEARCH_READ_CHUNK];
    char carry[SEARCH_CARRY_MAX];
    char window[SEARCH_READ_CHUNK + SEARCH_CARRY_MAX + 1];
    size_t carry_len = 0;

    for (;;) {
        ssize_t rc = read(fd, chunk, sizeof(chunk));
        if (rc <= 0) break;

        size_t csz = (size_t)rc;
        if (carry_len > 0) memcpy(window, carry, carry_len);
        memcpy(window + carry_len, chunk, csz);
        size_t wsz = carry_len + csz;
        window[wsz] = '\0';

        if (strstr(window, needle) != NULL) {
            (void)close(fd);
            return 1;
        }

        size_t keep = 0;
        if (needle_len > 1) keep = needle_len - 1;
        if (keep > wsz) keep = wsz;
        if (keep > 0) memcpy(carry, window + (wsz - keep), keep);
        carry_len = keep;
    }

    (void)close(fd);
    return 0;
}

static void maybe_report_name_match(const char* path, int is_dir, const search_opts_t* opts) {
    if (!opts->filename) return;
    if (strstr(path, opts->needle) == NULL) return;
    if (is_dir) printf("name: %s/\n", path);
    else printf("name: %s\n", path);
}

static void maybe_report_content_match(const char* path, const search_opts_t* opts) {
    if (!opts->content) return;
    if (!file_contains_needle(path, opts->needle)) return;
    printf("text: %s\n", path);
}

static void walk_path(const char* path, int depth, const search_opts_t* opts) {
    if (!path || !opts) return;
    if (depth > SEARCH_MAX_DEPTH) return;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;

    eyn_dirent_t ents[12];
    int probe = getdents(fd, ents, sizeof(ents));
    if (probe < 0) {
        maybe_report_name_match(path, 0, opts);
        maybe_report_content_match(path, opts);
        (void)close(fd);
        return;
    }

    if (probe > 0) {
        int count = probe / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (ents[i].name[0] == '\0') continue;
            if (streq(ents[i].name, ".") || streq(ents[i].name, "..")) continue;

            char child[SEARCH_PATH_MAX];
            if (safe_join_path(path, ents[i].name, child) != 0) continue;

            if (ents[i].is_dir) {
                maybe_report_name_match(child, 1, opts);
                walk_path(child, depth + 1, opts);
            } else {
                maybe_report_name_match(child, 0, opts);
                maybe_report_content_match(child, opts);
            }
        }
    }

    for (;;) {
        int rc = getdents(fd, ents, sizeof(ents));
        if (rc <= 0) break;

        int count = rc / (int)sizeof(eyn_dirent_t);
        for (int i = 0; i < count; ++i) {
            if (ents[i].name[0] == '\0') continue;
            if (streq(ents[i].name, ".") || streq(ents[i].name, "..")) continue;

            char child[SEARCH_PATH_MAX];
            if (safe_join_path(path, ents[i].name, child) != 0) continue;

            if (ents[i].is_dir) {
                maybe_report_name_match(child, 1, opts);
                walk_path(child, depth + 1, opts);
            } else {
                maybe_report_name_match(child, 0, opts);
                maybe_report_content_match(child, opts);
            }
        }
    }

    (void)close(fd);
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        usage();
        return 1;
    }
    if (streq(argv[1], "-h")) {
        usage();
        return 0;
    }

    search_opts_t opts;
    opts.filename = 1;
    opts.content = 1;
    opts.needle = argv[1];

    const char* path = "";

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!a || a[0] == '\0') continue;
        if (streq(a, "-f")) {
            opts.filename = 1;
            opts.content = 0;
        } else if (streq(a, "-c")) {
            opts.filename = 0;
            opts.content = 1;
        } else if (streq(a, "-a")) {
            opts.filename = 1;
            opts.content = 1;
        } else {
            path = a;
        }
    }

    walk_path(path, 0, &opts);
    return 0;
}
