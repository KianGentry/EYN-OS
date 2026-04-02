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

#define SEARCH_COLOUR_DEFAULT_R 200
#define SEARCH_COLOUR_DEFAULT_G 200
#define SEARCH_COLOUR_DEFAULT_B 200
#define SEARCH_COLOUR_MATCH_R 255
#define SEARCH_COLOUR_MATCH_G 120
#define SEARCH_COLOUR_MATCH_B 120

typedef struct {
    int filename;
    int content;
    int colour_enabled;
    const char* needle;
} search_opts_t;

static void usage(void) {
    puts("Usage: search <pattern> [-f|-c|-a] [path]");
    puts("  -f  search in names only");
    puts("  -c  search in file contents only");
    puts("  -a  search in both (default)");
    puts("  --stdin  filter piped stdin lines for <pattern>");
    puts("  --no-colour  disable highlighted matches");
}

static int streq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

static int line_contains(const char* line, const char* needle) {
    if (!line || !needle || needle[0] == '\0') return 0;
    return strstr(line, needle) != NULL;
}

static void write_all(const char* data, size_t len) {
    while (len > 0) {
        ssize_t rc = write(1, data, len);
        if (rc <= 0) return;
        data += (size_t)rc;
        len -= (size_t)rc;
    }
}

static void emit_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char seq[4];
    seq[0] = (char)0xFF;
    seq[1] = (char)r;
    seq[2] = (char)g;
    seq[3] = (char)b;
    write_all(seq, sizeof(seq));
}

static void print_highlighted_text(const char* text, const char* needle, int colour_enabled) {
    if (!text) {
        if (colour_enabled) {
            emit_rgb(SEARCH_COLOUR_DEFAULT_R, SEARCH_COLOUR_DEFAULT_G, SEARCH_COLOUR_DEFAULT_B);
        }
        return;
    }
    if (!colour_enabled || !needle || needle[0] == '\0') {
        write_all(text, strlen(text));
        if (colour_enabled) {
            emit_rgb(SEARCH_COLOUR_DEFAULT_R, SEARCH_COLOUR_DEFAULT_G, SEARCH_COLOUR_DEFAULT_B);
        }
        return;
    }

    size_t needle_len = strlen(needle);
    const char* cursor = text;

    while (*cursor) {
        const char* hit = strstr(cursor, needle);
        if (!hit) {
            write_all(cursor, strlen(cursor));
            break;
        }

        if (hit > cursor) {
            write_all(cursor, (size_t)(hit - cursor));
        }

        emit_rgb(SEARCH_COLOUR_MATCH_R, SEARCH_COLOUR_MATCH_G, SEARCH_COLOUR_MATCH_B);
        write_all(hit, needle_len);
        emit_rgb(SEARCH_COLOUR_DEFAULT_R, SEARCH_COLOUR_DEFAULT_G, SEARCH_COLOUR_DEFAULT_B);

        cursor = hit + needle_len;
    }

    emit_rgb(SEARCH_COLOUR_DEFAULT_R, SEARCH_COLOUR_DEFAULT_G, SEARCH_COLOUR_DEFAULT_B);
}

static void print_highlighted_line(const char* line, const char* needle, int colour_enabled) {
    print_highlighted_text(line, needle, colour_enabled);
    write_all("\n", 1);
}

static void print_highlighted_labeled_path(const char* label, const char* path, int is_dir, const char* needle, int colour_enabled) {
    if (label) write_all(label, strlen(label));
    print_highlighted_text(path, needle, colour_enabled);
    if (is_dir) write_all("/", 1);
    write_all("\n", 1);
}

static void stdin_filter_matches(const char* needle, int colour_enabled) {
    char chunk[SEARCH_READ_CHUNK];
    char line[SEARCH_PATH_MAX];
    int line_len = 0;
    int skip_rgb = 0;
    int skip_icon = 0;

    for (;;) {
        ssize_t rc = read(0, chunk, sizeof(chunk));
        if (rc <= 0) break;

        for (int i = 0; i < (int)rc; ++i) {
            unsigned char ch = (unsigned char)chunk[i];

            if (skip_rgb > 0) { skip_rgb--; continue; }
            if (skip_icon > 0) { skip_icon--; continue; }
            if (ch == 0xFFu) { skip_rgb = 3; continue; }
            if (ch == 0xFEu) { skip_icon = 16; continue; }

            if (ch == '\r') continue;
            if (ch == '\n') {
                line[line_len] = '\0';
                if (line_contains(line, needle)) {
                    print_highlighted_line(line, needle, colour_enabled);
                }
                line_len = 0;
                continue;
            }

            if ((ch < 32u || ch > 126u) && ch != '\t') continue;
            if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = (char)ch;
            }
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (line_contains(line, needle)) {
            print_highlighted_line(line, needle, colour_enabled);
        }
    }
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
    print_highlighted_labeled_path("name: ", path, is_dir, opts->needle, opts->colour_enabled);
}

static void maybe_report_content_match(const char* path, const search_opts_t* opts) {
    if (!opts->content) return;
    if (!file_contains_needle(path, opts->needle)) return;
    print_highlighted_labeled_path("text: ", path, 0, opts->needle, opts->colour_enabled);
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
    opts.colour_enabled = 1;
    opts.needle = argv[1];
    int use_stdin = 0;

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
        } else if (streq(a, "--stdin")) {
            use_stdin = 1;
        } else if (streq(a, "--no-colour")) {
            opts.colour_enabled = 0;
        } else {
            path = a;
        }
    }

    if (use_stdin) {
        stdin_filter_matches(opts.needle, opts.colour_enabled);
        return 0;
    }

    walk_path(path, 0, &opts);
    return 0;
}
