#include "index.h"
#include "package.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IDX_JSON_MAX_DEPTH 64
#define IDX_MAX_VERSIONS_PER_PACKAGE 32

typedef struct {
    const char* data;
    size_t len;
    size_t pos;
    const char* error;
    size_t error_pos;
} idx_json_parser_t;

static void idx_parser_fail(idx_json_parser_t* parser, const char* msg) {
    if (!parser->error) {
        parser->error = msg;
        parser->error_pos = parser->pos;
    }
}

static int idx_is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int idx_is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

static int idx_is_hex_char(char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static void idx_skip_ws(idx_json_parser_t* parser) {
    while (parser->pos < parser->len && idx_is_space_char(parser->data[parser->pos])) {
        parser->pos++;
    }
}

static int idx_match_char(idx_json_parser_t* parser, char want) {
    if (parser->pos >= parser->len || parser->data[parser->pos] != want) {
        return 0;
    }
    parser->pos++;
    return 1;
}

static int idx_parse_string(idx_json_parser_t* parser, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    if (!idx_match_char(parser, '"')) {
        idx_parser_fail(parser, "expected string");
        return -1;
    }

    size_t out_len = 0;

    while (parser->pos < parser->len) {
        char c = parser->data[parser->pos++];
        if ((unsigned char)c < 0x20) {
            idx_parser_fail(parser, "control character in string");
            return -1;
        }

        if (c == '"') {
            out[out_len] = '\0';
            return 0;
        }

        if (c == '\\') {
            if (parser->pos >= parser->len) {
                idx_parser_fail(parser, "unfinished escape sequence");
                return -1;
            }

            char esc = parser->data[parser->pos++];
            switch (esc) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (parser->len - parser->pos < 4) {
                    idx_parser_fail(parser, "short \\u escape");
                    return -1;
                }
                for (int i = 0; i < 4; i++) {
                    if (!idx_is_hex_char(parser->data[parser->pos])) {
                        idx_parser_fail(parser, "invalid hex in \\u escape");
                        return -1;
                    }
                    parser->pos++;
                }
                c = '?';
                break;
            }
            default:
                idx_parser_fail(parser, "invalid escape sequence");
                return -1;
            }
        }

        if (out_len + 1 >= out_cap) {
            idx_parser_fail(parser, "string too long");
            return -1;
        }
        out[out_len++] = c;
    }

    idx_parser_fail(parser, "unterminated string");
    return -1;
}

static int idx_skip_string(idx_json_parser_t* parser) {
    if (!idx_match_char(parser, '"')) {
        idx_parser_fail(parser, "expected string");
        return -1;
    }

    while (parser->pos < parser->len) {
        char c = parser->data[parser->pos++];
        if ((unsigned char)c < 0x20) {
            idx_parser_fail(parser, "control character in string");
            return -1;
        }

        if (c == '"') {
            return 0;
        }

        if (c == '\\') {
            if (parser->pos >= parser->len) {
                idx_parser_fail(parser, "unfinished escape sequence");
                return -1;
            }

            char esc = parser->data[parser->pos++];
            if (esc == 'u') {
                if (parser->len - parser->pos < 4) {
                    idx_parser_fail(parser, "short \\u escape");
                    return -1;
                }

                for (int i = 0; i < 4; i++) {
                    if (!idx_is_hex_char(parser->data[parser->pos])) {
                        idx_parser_fail(parser, "invalid hex in \\u escape");
                        return -1;
                    }
                    parser->pos++;
                }
            }
        }
    }

    idx_parser_fail(parser, "unterminated string");
    return -1;
}

static int idx_skip_number(idx_json_parser_t* parser) {
    if (idx_match_char(parser, '-')) {
        if (parser->pos >= parser->len) {
            idx_parser_fail(parser, "incomplete number");
            return -1;
        }
    }

    if (parser->pos >= parser->len) {
        idx_parser_fail(parser, "incomplete number");
        return -1;
    }

    if (parser->data[parser->pos] == '0') {
        parser->pos++;
    } else {
        if (!idx_is_digit_char(parser->data[parser->pos]) || parser->data[parser->pos] == '0') {
            idx_parser_fail(parser, "invalid number");
            return -1;
        }
        while (parser->pos < parser->len && idx_is_digit_char(parser->data[parser->pos])) {
            parser->pos++;
        }
    }

    if (parser->pos < parser->len && parser->data[parser->pos] == '.') {
        parser->pos++;
        if (parser->pos >= parser->len || !idx_is_digit_char(parser->data[parser->pos])) {
            idx_parser_fail(parser, "fraction has no digits");
            return -1;
        }
        while (parser->pos < parser->len && idx_is_digit_char(parser->data[parser->pos])) {
            parser->pos++;
        }
    }

    if (parser->pos < parser->len
        && (parser->data[parser->pos] == 'e' || parser->data[parser->pos] == 'E')) {
        parser->pos++;
        if (parser->pos < parser->len
            && (parser->data[parser->pos] == '+' || parser->data[parser->pos] == '-')) {
            parser->pos++;
        }

        if (parser->pos >= parser->len || !idx_is_digit_char(parser->data[parser->pos])) {
            idx_parser_fail(parser, "exponent has no digits");
            return -1;
        }

        while (parser->pos < parser->len && idx_is_digit_char(parser->data[parser->pos])) {
            parser->pos++;
        }
    }

    return 0;
}

static int idx_skip_literal(idx_json_parser_t* parser, const char* text) {
    size_t text_len = strlen(text);
    if (parser->len - parser->pos < text_len) {
        idx_parser_fail(parser, "unexpected end while reading literal");
        return -1;
    }

    if (memcmp(parser->data + parser->pos, text, text_len) != 0) {
        idx_parser_fail(parser, "invalid literal");
        return -1;
    }

    parser->pos += text_len;
    return 0;
}

static int idx_skip_value(idx_json_parser_t* parser, int depth);

static int idx_skip_array(idx_json_parser_t* parser, int depth) {
    if (depth > IDX_JSON_MAX_DEPTH) {
        idx_parser_fail(parser, "max depth exceeded");
        return -1;
    }

    if (!idx_match_char(parser, '[')) {
        idx_parser_fail(parser, "expected '['");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, ']')) {
        return 0;
    }

    for (;;) {
        if (idx_skip_value(parser, depth + 1) != 0) return -1;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, ']')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or ']' in array");
        return -1;
    }
}

static int idx_skip_object(idx_json_parser_t* parser, int depth) {
    if (depth > IDX_JSON_MAX_DEPTH) {
        idx_parser_fail(parser, "max depth exceeded");
        return -1;
    }

    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected '{'");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        return 0;
    }

    for (;;) {
        if (idx_skip_string(parser) != 0) return -1;

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after key");
            return -1;
        }

        idx_skip_ws(parser);
        if (idx_skip_value(parser, depth + 1) != 0) return -1;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or '}' in object");
        return -1;
    }
}

static int idx_skip_value(idx_json_parser_t* parser, int depth) {
    idx_skip_ws(parser);
    if (parser->pos >= parser->len) {
        idx_parser_fail(parser, "unexpected end of JSON input");
        return -1;
    }

    char c = parser->data[parser->pos];
    if (c == '{') return idx_skip_object(parser, depth);
    if (c == '[') return idx_skip_array(parser, depth);
    if (c == '"') return idx_skip_string(parser);
    if (c == 't') return idx_skip_literal(parser, "true");
    if (c == 'f') return idx_skip_literal(parser, "false");
    if (c == 'n') return idx_skip_literal(parser, "null");
    if (c == '-' || idx_is_digit_char(c)) return idx_skip_number(parser);

    idx_parser_fail(parser, "invalid JSON value");
    return -1;
}

static int idx_parse_deps_array(idx_json_parser_t* parser, Package* pkg) {
    if (!idx_match_char(parser, '[')) {
        idx_parser_fail(parser, "expected '[' for deps");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, ']')) {
        return 0;
    }

    for (;;) {
        if (pkg->dep_count >= MAX_DEPS) {
            idx_parser_fail(parser, "too many dependencies");
            return -1;
        }

        if (idx_parse_string(parser, pkg->deps[pkg->dep_count], MAX_NAME) != 0) {
            return -1;
        }
        pkg->dep_count++;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, ']')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or ']' in deps array");
        return -1;
    }
}

static void idx_package_apply_defaults(Package* pkg) {
    if (!pkg) return;

    if (!pkg->install_dir[0]) {
        strncpy(pkg->install_dir, "/binaries", sizeof(pkg->install_dir) - 1);
        pkg->install_dir[sizeof(pkg->install_dir) - 1] = '\0';
    }

    if (!pkg->install_name[0] && pkg->name[0]) {
        strncpy(pkg->install_name, pkg->name, sizeof(pkg->install_name) - 1);
        pkg->install_name[sizeof(pkg->install_name) - 1] = '\0';
    }
}

static int idx_parse_release_object(idx_json_parser_t* parser,
                                    const char* version_key,
                                    Package* out_pkg) {
    if (!out_pkg) return -1;

    memset(out_pkg, 0, sizeof(*out_pkg));

    int has_version = 0;
    int has_url = 0;
    int has_sha = 0;

    if (version_key && version_key[0]) {
        if (strlen(version_key) >= sizeof(out_pkg->version)) {
            idx_parser_fail(parser, "version key too long");
            return -1;
        }
        strncpy(out_pkg->version, version_key, sizeof(out_pkg->version) - 1);
        out_pkg->version[sizeof(out_pkg->version) - 1] = '\0';
        has_version = 1;
    }

    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected release object");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        idx_parser_fail(parser, "empty release object");
        return -1;
    }

    for (;;) {
        char key[MAX_NAME];
        if (idx_parse_string(parser, key, sizeof(key)) != 0) {
            return -1;
        }

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after release key");
            return -1;
        }

        idx_skip_ws(parser);
        if (strcmp(key, "version") == 0) {
            if (idx_parse_string(parser, out_pkg->version, sizeof(out_pkg->version)) != 0) {
                return -1;
            }
            has_version = 1;
        } else if (strcmp(key, "url") == 0) {
            if (idx_parse_string(parser, out_pkg->url, sizeof(out_pkg->url)) != 0) {
                return -1;
            }
            has_url = 1;
        } else if (strcmp(key, "sha256") == 0) {
            if (idx_parse_string(parser, out_pkg->sha256, sizeof(out_pkg->sha256)) != 0) {
                return -1;
            }
            has_sha = 1;
        } else if (strcmp(key, "install_dir") == 0 || strcmp(key, "target_dir") == 0) {
            if (idx_parse_string(parser, out_pkg->install_dir, sizeof(out_pkg->install_dir)) != 0) {
                return -1;
            }
        } else if (strcmp(key, "install_name") == 0 || strcmp(key, "target_name") == 0) {
            if (idx_parse_string(parser, out_pkg->install_name, sizeof(out_pkg->install_name)) != 0) {
                return -1;
            }
        } else if (strcmp(key, "deps") == 0 || strcmp(key, "dependencies") == 0) {
            if (idx_parse_deps_array(parser, out_pkg) != 0) {
                return -1;
            }
        } else {
            if (idx_skip_value(parser, 0) != 0) {
                return -1;
            }
        }

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            break;
        }

        idx_parser_fail(parser, "expected ',' or '}' in release object");
        return -1;
    }

    if (!has_version || !has_url || !has_sha) {
        idx_parser_fail(parser, "release missing required fields");
        return -1;
    }

    return 0;
}

static int idx_parse_versions_object(idx_json_parser_t* parser,
                                     Package versions[IDX_MAX_VERSIONS_PER_PACKAGE],
                                     int* out_count) {
    if (!out_count) return -1;

    *out_count = 0;
    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected versions object");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        return 0;
    }

    for (;;) {
        if (*out_count >= IDX_MAX_VERSIONS_PER_PACKAGE) {
            idx_parser_fail(parser, "too many versions for package");
            return -1;
        }

        char version_key[MAX_NAME];
        if (idx_parse_string(parser, version_key, sizeof(version_key)) != 0) {
            return -1;
        }

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after version key");
            return -1;
        }

        idx_skip_ws(parser);
        if (idx_parse_release_object(parser, version_key, &versions[*out_count]) != 0) {
            return -1;
        }
        (*out_count)++;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or '}' in versions object");
        return -1;
    }
}

static int idx_parse_package_object(idx_json_parser_t* parser,
                                    Package* out_pkg,
                                    const char* fallback_name) {
    if (!out_pkg) return -1;

    memset(out_pkg, 0, sizeof(*out_pkg));

    int has_name = 0;
    int has_version = 0;
    int has_url = 0;
    int has_sha = 0;
    int has_versions = 0;

    Package version_candidates[IDX_MAX_VERSIONS_PER_PACKAGE];
    int version_candidate_count = 0;
    char latest_version[MAX_NAME];
    latest_version[0] = '\0';

    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected package object");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        idx_parser_fail(parser, "empty package object");
        return -1;
    }

    for (;;) {
        char key[MAX_NAME];
        if (idx_parse_string(parser, key, sizeof(key)) != 0) {
            return -1;
        }

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after package key");
            return -1;
        }

        idx_skip_ws(parser);

        if (strcmp(key, "name") == 0) {
            if (idx_parse_string(parser, out_pkg->name, sizeof(out_pkg->name)) != 0) return -1;
            has_name = 1;
        } else if (strcmp(key, "version") == 0) {
            if (idx_parse_string(parser, out_pkg->version, sizeof(out_pkg->version)) != 0) return -1;
            has_version = 1;
        } else if (strcmp(key, "url") == 0) {
            if (idx_parse_string(parser, out_pkg->url, sizeof(out_pkg->url)) != 0) return -1;
            has_url = 1;
        } else if (strcmp(key, "sha256") == 0) {
            if (idx_parse_string(parser, out_pkg->sha256, sizeof(out_pkg->sha256)) != 0) return -1;
            has_sha = 1;
        } else if (strcmp(key, "install_dir") == 0 || strcmp(key, "target_dir") == 0) {
            if (idx_parse_string(parser, out_pkg->install_dir, sizeof(out_pkg->install_dir)) != 0) return -1;
        } else if (strcmp(key, "install_name") == 0 || strcmp(key, "target_name") == 0) {
            if (idx_parse_string(parser, out_pkg->install_name, sizeof(out_pkg->install_name)) != 0) return -1;
        } else if (strcmp(key, "deps") == 0 || strcmp(key, "dependencies") == 0) {
            if (idx_parse_deps_array(parser, out_pkg) != 0) return -1;
        } else if (strcmp(key, "latest") == 0) {
            if (idx_parse_string(parser, latest_version, sizeof(latest_version)) != 0) return -1;
        } else if (strcmp(key, "versions") == 0) {
            if (idx_parse_versions_object(parser,
                                          version_candidates,
                                          &version_candidate_count) != 0) {
                return -1;
            }
            has_versions = 1;
        } else {
            if (idx_skip_value(parser, 0) != 0) return -1;
        }

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            break;
        }

        idx_parser_fail(parser, "expected ',' or '}' in package object");
        return -1;
    }

    if (!has_name && fallback_name && fallback_name[0]) {
        if (strlen(fallback_name) >= sizeof(out_pkg->name)) {
            idx_parser_fail(parser, "package key name too long");
            return -1;
        }
        strncpy(out_pkg->name, fallback_name, sizeof(out_pkg->name) - 1);
        out_pkg->name[sizeof(out_pkg->name) - 1] = '\0';
        has_name = 1;
    }

    if (has_versions) {
        if (version_candidate_count <= 0) {
            idx_parser_fail(parser, "versions object is empty");
            return -1;
        }

        int selected = 0;
        if (latest_version[0]) {
            selected = -1;
            for (int i = 0; i < version_candidate_count; i++) {
                if (strcmp(version_candidates[i].version, latest_version) == 0) {
                    selected = i;
                    break;
                }
            }

            if (selected < 0) {
                idx_parser_fail(parser, "latest version not found in versions object");
                return -1;
            }
        }

        Package chosen = version_candidates[selected];
        if (!has_name) {
            idx_parser_fail(parser, "package missing required fields");
            return -1;
        }

        strncpy(chosen.name, out_pkg->name, sizeof(chosen.name) - 1);
        chosen.name[sizeof(chosen.name) - 1] = '\0';

        if (!chosen.install_dir[0] && out_pkg->install_dir[0]) {
            strncpy(chosen.install_dir, out_pkg->install_dir, sizeof(chosen.install_dir) - 1);
            chosen.install_dir[sizeof(chosen.install_dir) - 1] = '\0';
        }
        if (!chosen.install_name[0] && out_pkg->install_name[0]) {
            strncpy(chosen.install_name, out_pkg->install_name, sizeof(chosen.install_name) - 1);
            chosen.install_name[sizeof(chosen.install_name) - 1] = '\0';
        }

        *out_pkg = chosen;
        has_version = 1;
        has_url = 1;
        has_sha = 1;
    }

    if (!has_name || !has_version || !has_url || !has_sha) {
        idx_parser_fail(parser, "package missing required fields");
        return -1;
    }

    idx_package_apply_defaults(out_pkg);

    return 0;
}

static int idx_parse_packages_array(idx_json_parser_t* parser, PackageIndex* out_index) {
    if (!idx_match_char(parser, '[')) {
        idx_parser_fail(parser, "expected packages array");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, ']')) {
        return 0;
    }

    for (;;) {
        if (out_index->count >= INDEX_MAX_PACKAGES) {
            idx_parser_fail(parser, "too many packages in index");
            return -1;
        }

        if (idx_parse_package_object(parser,
                                     &out_index->packages[out_index->count],
                                     NULL) != 0) {
            return -1;
        }
        out_index->count++;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, ']')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or ']' in packages array");
        return -1;
    }
}

static int idx_parse_packages_object_map(idx_json_parser_t* parser, PackageIndex* out_index) {
    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected packages object map");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        return 0;
    }

    for (;;) {
        char package_name[MAX_NAME];
        if (idx_parse_string(parser, package_name, sizeof(package_name)) != 0) {
            return -1;
        }

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after package map key");
            return -1;
        }

        idx_skip_ws(parser);
        if (out_index->count >= INDEX_MAX_PACKAGES) {
            idx_parser_fail(parser, "too many packages in index");
            return -1;
        }

        if (idx_parse_package_object(parser,
                                     &out_index->packages[out_index->count],
                                     package_name) != 0) {
            return -1;
        }
        out_index->count++;

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            return 0;
        }

        idx_parser_fail(parser, "expected ',' or '}' in packages object map");
        return -1;
    }
}

static int idx_parse_packages_collection(idx_json_parser_t* parser, PackageIndex* out_index) {
    idx_skip_ws(parser);
    if (parser->pos >= parser->len) {
        idx_parser_fail(parser, "unexpected end while reading packages");
        return -1;
    }

    if (parser->data[parser->pos] == '[') {
        return idx_parse_packages_array(parser, out_index);
    }

    if (parser->data[parser->pos] == '{') {
        return idx_parse_packages_object_map(parser, out_index);
    }

    idx_parser_fail(parser, "expected packages array or object map");
    return -1;
}

static int idx_parse_root(idx_json_parser_t* parser, PackageIndex* out_index) {
    idx_skip_ws(parser);
    if (parser->pos >= parser->len) {
        idx_parser_fail(parser, "empty JSON document");
        return -1;
    }

    if (parser->data[parser->pos] == '[') {
        return idx_parse_packages_array(parser, out_index);
    }

    if (parser->data[parser->pos] != '{') {
        idx_parser_fail(parser, "index root must be object or array");
        return -1;
    }

    if (!idx_match_char(parser, '{')) {
        idx_parser_fail(parser, "expected '{'");
        return -1;
    }

    idx_skip_ws(parser);
    if (idx_match_char(parser, '}')) {
        idx_parser_fail(parser, "empty index object");
        return -1;
    }

    int found_packages = 0;

    for (;;) {
        char key[MAX_NAME];
        if (idx_parse_string(parser, key, sizeof(key)) != 0) return -1;

        idx_skip_ws(parser);
        if (!idx_match_char(parser, ':')) {
            idx_parser_fail(parser, "expected ':' after index key");
            return -1;
        }

        idx_skip_ws(parser);
        if (strcmp(key, "packages") == 0) {
            if (idx_parse_packages_collection(parser, out_index) != 0) return -1;
            found_packages = 1;
        } else {
            if (idx_skip_value(parser, 0) != 0) return -1;
        }

        idx_skip_ws(parser);
        if (idx_match_char(parser, ',')) {
            idx_skip_ws(parser);
            continue;
        }

        if (idx_match_char(parser, '}')) {
            break;
        }

        idx_parser_fail(parser, "expected ',' or '}' in root object");
        return -1;
    }

    if (!found_packages) {
        idx_parser_fail(parser, "root object missing 'packages' entry");
        return -1;
    }

    return 0;
}

static int idx_validate_unique_names(PackageIndex* index) {
    for (int i = 0; i < index->count; i++) {
        if (!index->packages[i].name[0]) {
            puts("install: package with empty name in index");
            return -1;
        }

        for (int j = i + 1; j < index->count; j++) {
            if (strcmp(index->packages[i].name, index->packages[j].name) == 0) {
                printf("install: duplicate package name in index: %s\n", index->packages[i].name);
                return -1;
            }
        }
    }
    return 0;
}

static void idx_trim_leading_bytes(const uint8_t** data, size_t* len) {
    if (!data || !*data || !len) return;

    while (*len > 0 && idx_is_space_char((char)(*data)[0])) {
        (*data)++;
        (*len)--;
    }
}

static void idx_strip_utf8_bom(const uint8_t** data, size_t* len) {
    if (!data || !*data || !len) return;
    if (*len < 3) return;

    if ((*data)[0] == 0xEF && (*data)[1] == 0xBB && (*data)[2] == 0xBF) {
        *data += 3;
        *len -= 3;
    }
}

static int idx_strip_leaked_http_headers(const uint8_t** data, size_t* len) {
    if (!data || !*data || !len) return -1;
    if (*len < 5) return 0;

    if (memcmp(*data, "HTTP/", 5) != 0) {
        return 0;
    }

    for (size_t i = 0; i + 3 < *len; i++) {
        if ((*data)[i] == '\r'
            && (*data)[i + 1] == '\n'
            && (*data)[i + 2] == '\r'
            && (*data)[i + 3] == '\n') {
            *data += i + 4;
            *len -= i + 4;
            return 1;
        }
    }

    for (size_t i = 0; i + 1 < *len; i++) {
        if ((*data)[i] == '\n' && (*data)[i + 1] == '\n') {
            *data += i + 2;
            *len -= i + 2;
            return 1;
        }
    }

    return -1;
}

static void idx_debug_dump_prefix(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        puts("install: index payload prefix: <empty>");
        return;
    }

    size_t n = len;
    if (n > 16) n = 16;

    char hex_buf[16 * 3 + 1];
    char ascii_buf[16 + 1];
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        hex_buf[i * 3] = hex[(b >> 4) & 0x0F];
        hex_buf[i * 3 + 1] = hex[b & 0x0F];
        hex_buf[i * 3 + 2] = (i + 1 < n) ? ' ' : '\0';

        if (b >= 32 && b <= 126) {
            ascii_buf[i] = (char)b;
        } else {
            ascii_buf[i] = '.';
        }
    }

    if (n == 0) {
        hex_buf[0] = '\0';
    }
    ascii_buf[n] = '\0';

    printf("install: index payload prefix (%lu bytes): %s |%s|\n",
           (unsigned long)n,
           hex_buf,
           ascii_buf);
}

static int idx_read_file_to_buffer(const char* path,
                                   uint8_t** out_data,
                                   size_t* out_len,
                                   size_t max_bytes) {
    if (!path || !out_data || !out_len || max_bytes == 0) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t cap = 4096;
    if (cap > max_bytes) cap = max_bytes;

    uint8_t* data = (uint8_t*)malloc(cap + 1);
    if (!data) {
        close(fd);
        return -1;
    }

    size_t len = 0;
    for (;;) {
        uint8_t chunk[1024];
        ssize_t rd = read(fd, chunk, sizeof(chunk));
        if (rd < 0) {
            free(data);
            close(fd);
            return -1;
        }
        if (rd == 0) break;

        size_t got = (size_t)rd;
        if (len + got > max_bytes) {
            free(data);
            close(fd);
            return -1;
        }

        if (len + got > cap) {
            size_t next_cap = cap;
            while (len + got > next_cap) {
                size_t grown = next_cap * 2;
                if (grown < next_cap || grown > max_bytes) {
                    grown = max_bytes;
                }
                if (grown <= next_cap) break;
                next_cap = grown;
            }

            if (len + got > next_cap) {
                free(data);
                close(fd);
                return -1;
            }

            uint8_t* bigger = (uint8_t*)realloc(data, next_cap + 1);
            if (!bigger) {
                free(data);
                close(fd);
                return -1;
            }

            data = bigger;
            cap = next_cap;
        }

        memcpy(data + len, chunk, got);
        len += got;
    }

    close(fd);
    data[len] = '\0';

    *out_data = data;
    *out_len = len;
    return 0;
}

static int idx_parse_index_bytes(PackageIndex* out_index,
                                 const uint8_t* index_json,
                                 size_t index_json_len,
                                 const char* source_label) {
    if (!out_index || !index_json) return -1;

    memset(out_index, 0, sizeof(*out_index));

    const uint8_t* parse_data = index_json;
    size_t parse_len = index_json_len;

    idx_trim_leading_bytes(&parse_data, &parse_len);
    idx_strip_utf8_bom(&parse_data, &parse_len);
    idx_trim_leading_bytes(&parse_data, &parse_len);

    int stripped_http = idx_strip_leaked_http_headers(&parse_data, &parse_len);
    if (stripped_http < 0) {
        if (source_label && source_label[0]) {
            printf("install: malformed HTTP headers in %s\n", source_label);
        } else {
            puts("install: malformed HTTP headers in index response");
        }
        idx_debug_dump_prefix(parse_data, parse_len);
        return -1;
    }
    if (stripped_http > 0) {
        idx_trim_leading_bytes(&parse_data, &parse_len);
        idx_strip_utf8_bom(&parse_data, &parse_len);
        idx_trim_leading_bytes(&parse_data, &parse_len);
    }

    if (parse_len == 0) {
        if (source_label && source_label[0]) {
            printf("install: %s is empty\n", source_label);
        } else {
            puts("install: package index response is empty");
        }
        return -1;
    }

    idx_json_parser_t parser;
    parser.data = (const char*)parse_data;
    parser.len = parse_len;
    parser.pos = 0;
    parser.error = NULL;
    parser.error_pos = 0;

    int rc = idx_parse_root(&parser, out_index);
    if (rc == 0) {
        idx_skip_ws(&parser);
        if (parser.pos != parser.len) {
            idx_parser_fail(&parser, "trailing data after JSON document");
            rc = -1;
        }
    }

    if (rc == 0 && idx_validate_unique_names(out_index) != 0) {
        rc = -1;
    }

    if (rc != 0) {
        size_t at = parser.error_pos;
        if (at > parser.len) at = parser.len;
        if (source_label && source_label[0]) {
            printf("install: %s parse error at offset %lu: %s\n",
                   source_label,
                   (unsigned long)at,
                   parser.error ? parser.error : "unknown error");
        } else {
            printf("install: index parse error at offset %lu: %s\n",
                   (unsigned long)at,
                   parser.error ? parser.error : "unknown error");
        }
        idx_debug_dump_prefix(parse_data, parse_len);
        return -1;
    }

    if (out_index->count == 0) {
        if (source_label && source_label[0]) {
            printf("install: %s is empty\n", source_label);
        } else {
            puts("install: package index is empty");
        }
        return -1;
    }

    return 0;
}

const Package* index_find_package(const PackageIndex* index, const char* name) {
    if (!index || !name || !name[0]) return NULL;

    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->packages[i].name, name) == 0) {
            return &index->packages[i];
        }
    }

    return NULL;
}

int index_has_package(const PackageIndex* index, const char* name) {
    return index_find_package(index, name) != NULL;
}

int index_fetch_and_parse(PackageIndex* out_index) {
    if (!out_index) return -1;

    uint8_t* local_index_json = NULL;
    size_t local_index_json_len = 0;

    if (idx_read_file_to_buffer(INSTALL_LOCAL_INDEX_PATH,
                                &local_index_json,
                                &local_index_json_len,
                                INDEX_MAX_JSON_BYTES) == 0) {
        if (idx_parse_index_bytes(out_index,
                                  local_index_json,
                                  local_index_json_len,
                                  "local index") == 0) {
            free(local_index_json);
            return 0;
        }

        puts("install: falling back to network index");
        free(local_index_json);
    }

    uint8_t* index_json = NULL;
    size_t index_json_len = 0;

    if (package_download_url_to_buffer(INSTALL_INDEX_URL,
                                       &index_json,
                                       &index_json_len,
                                       INDEX_MAX_JSON_BYTES) != 0) {
        printf("install: failed to fetch package index from %s\n", INSTALL_INDEX_URL);
        return -1;
    }

    int rc = idx_parse_index_bytes(out_index,
                                   index_json,
                                   index_json_len,
                                   "network index");

    free(index_json);
    return rc;
}
