#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Parse and normalize JSON files.", "jsonparse --pretty /config/settings.json");

/*
 * SECURITY-INVARIANT: Hard cap for input size in this parser utility.
 *
 * Why: Keeps memory usage bounded on low-RAM systems while still allowing
 * practical config/document files.
 * Invariant: The command never allocates more than JSONPARSE_MAX_INPUT + 1
 * bytes for input payload storage.
 * Breakage if changed:
 *   - Increasing may cause avoidable memory pressure in 9 MB test boots.
 *   - Decreasing can reject legitimate configuration files.
 * ABI-sensitive: No.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (resource exhaustion bound).
 */
#define JSONPARSE_MAX_INPUT (512u * 1024u)
#define JSONPARSE_MAX_DEPTH 128

typedef struct {
    const char* data;
    size_t len;
    size_t pos;
    const char* error;
    size_t error_pos;
    int depth;
} json_parser_t;

typedef struct {
    int enabled;
    int pretty;
} json_emitter_t;

static void usage(void) {
    puts("Usage: jsonparse [--pretty|--minify|--check] <file>\n"
         "Examples:\n"
         "  jsonparse /config/settings.json\n"
         "  jsonparse --minify /config/settings.json\n"
         "  jsonparse --check /config/settings.json");
}

static int is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

static int is_hex_char(char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static void parser_fail(json_parser_t* parser, const char* msg) {
    if (!parser->error) {
        parser->error = msg;
        parser->error_pos = parser->pos;
    }
}

static void skip_ws(json_parser_t* parser) {
    while (parser->pos < parser->len && is_space_char(parser->data[parser->pos])) {
        parser->pos++;
    }
}

static int parser_match_char(json_parser_t* parser, char want) {
    if (parser->pos >= parser->len || parser->data[parser->pos] != want) {
        return 0;
    }
    parser->pos++;
    return 1;
}

static int emit_char(const json_emitter_t* emitter, char c) {
    if (!emitter || !emitter->enabled) return 0;
    return putchar((int)c) == EOF ? -1 : 0;
}

static int emit_slice(const json_emitter_t* emitter, const char* s, size_t n) {
    if (!emitter || !emitter->enabled || n == 0) return 0;
    ssize_t wr = write(1, s, n);
    return (wr == (ssize_t)n) ? 0 : -1;
}

static int emit_indent_line(const json_emitter_t* emitter, int depth) {
    if (!emitter || !emitter->enabled || !emitter->pretty) return 0;
    if (emit_char(emitter, '\n') != 0) return -1;
    for (int i = 0; i < depth * 2; ++i) {
        if (emit_char(emitter, ' ') != 0) return -1;
    }
    return 0;
}

static int parse_value(json_parser_t* parser, const json_emitter_t* emitter);

static int parse_literal(json_parser_t* parser,
                         const json_emitter_t* emitter,
                         const char* text,
                         size_t text_len) {
    if (parser->len - parser->pos < text_len) {
        parser_fail(parser, "unexpected end while reading literal");
        return -1;
    }

    if (memcmp(parser->data + parser->pos, text, text_len) != 0) {
        parser_fail(parser, "invalid literal");
        return -1;
    }

    if (emit_slice(emitter, parser->data + parser->pos, text_len) != 0) {
        parser_fail(parser, "failed to write output");
        return -1;
    }

    parser->pos += text_len;
    return 0;
}

static int parse_string(json_parser_t* parser, const json_emitter_t* emitter) {
    size_t start = parser->pos;

    if (!parser_match_char(parser, '"')) {
        parser_fail(parser, "expected string");
        return -1;
    }

    while (parser->pos < parser->len) {
        char c = parser->data[parser->pos++];
        if ((unsigned char)c < 0x20) {
            parser_fail(parser, "control character in string");
            return -1;
        }

        if (c == '"') {
            size_t tok_len = parser->pos - start;
            if (emit_slice(emitter, parser->data + start, tok_len) != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            return 0;
        }

        if (c == '\\') {
            if (parser->pos >= parser->len) {
                parser_fail(parser, "unfinished escape sequence");
                return -1;
            }

            char esc = parser->data[parser->pos++];
            if (esc == '"' || esc == '\\' || esc == '/' || esc == 'b'
                || esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
                continue;
            }

            if (esc == 'u') {
                if (parser->len - parser->pos < 4) {
                    parser_fail(parser, "short \\u escape");
                    return -1;
                }
                for (int i = 0; i < 4; ++i) {
                    char h = parser->data[parser->pos++];
                    if (!is_hex_char(h)) {
                        parser_fail(parser, "invalid hex in \\u escape");
                        return -1;
                    }
                }
                continue;
            }

            parser_fail(parser, "invalid escape sequence");
            return -1;
        }
    }

    parser_fail(parser, "unterminated string");
    return -1;
}

static int parse_number(json_parser_t* parser, const json_emitter_t* emitter) {
    size_t start = parser->pos;

    if (parser_match_char(parser, '-')) {
        if (parser->pos >= parser->len) {
            parser_fail(parser, "incomplete number");
            return -1;
        }
    }

    if (parser->pos >= parser->len) {
        parser_fail(parser, "incomplete number");
        return -1;
    }

    if (parser->data[parser->pos] == '0') {
        parser->pos++;
    } else {
        if (!is_digit_char(parser->data[parser->pos]) || parser->data[parser->pos] == '0') {
            parser_fail(parser, "invalid number");
            return -1;
        }
        while (parser->pos < parser->len && is_digit_char(parser->data[parser->pos])) {
            parser->pos++;
        }
    }

    if (parser->pos < parser->len && parser->data[parser->pos] == '.') {
        parser->pos++;
        if (parser->pos >= parser->len || !is_digit_char(parser->data[parser->pos])) {
            parser_fail(parser, "fraction has no digits");
            return -1;
        }
        while (parser->pos < parser->len && is_digit_char(parser->data[parser->pos])) {
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
        if (parser->pos >= parser->len || !is_digit_char(parser->data[parser->pos])) {
            parser_fail(parser, "exponent has no digits");
            return -1;
        }
        while (parser->pos < parser->len && is_digit_char(parser->data[parser->pos])) {
            parser->pos++;
        }
    }

    if (emit_slice(emitter, parser->data + start, parser->pos - start) != 0) {
        parser_fail(parser, "failed to write output");
        return -1;
    }
    return 0;
}

static int parse_array(json_parser_t* parser, const json_emitter_t* emitter) {
    if (!parser_match_char(parser, '[')) {
        parser_fail(parser, "expected '['");
        return -1;
    }
    if (emit_char(emitter, '[') != 0) {
        parser_fail(parser, "failed to write output");
        return -1;
    }

    skip_ws(parser);
    if (parser_match_char(parser, ']')) {
        if (emit_char(emitter, ']') != 0) {
            parser_fail(parser, "failed to write output");
            return -1;
        }
        return 0;
    }

    parser->depth++;
    if (parser->depth > JSONPARSE_MAX_DEPTH) {
        parser_fail(parser, "max nesting depth exceeded");
        return -1;
    }

    for (;;) {
        if (emit_indent_line(emitter, parser->depth) != 0) {
            parser_fail(parser, "failed to write output");
            return -1;
        }

        if (parse_value(parser, emitter) != 0) return -1;

        skip_ws(parser);
        if (parser_match_char(parser, ',')) {
            if (emit_char(emitter, ',') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            skip_ws(parser);
            continue;
        }

        if (parser_match_char(parser, ']')) {
            if (emit_indent_line(emitter, parser->depth - 1) != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            if (emit_char(emitter, ']') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            parser->depth--;
            return 0;
        }

        parser_fail(parser, "expected ',' or ']' in array");
        return -1;
    }
}

static int parse_object(json_parser_t* parser, const json_emitter_t* emitter) {
    if (!parser_match_char(parser, '{')) {
        parser_fail(parser, "expected '{'");
        return -1;
    }
    if (emit_char(emitter, '{') != 0) {
        parser_fail(parser, "failed to write output");
        return -1;
    }

    skip_ws(parser);
    if (parser_match_char(parser, '}')) {
        if (emit_char(emitter, '}') != 0) {
            parser_fail(parser, "failed to write output");
            return -1;
        }
        return 0;
    }

    parser->depth++;
    if (parser->depth > JSONPARSE_MAX_DEPTH) {
        parser_fail(parser, "max nesting depth exceeded");
        return -1;
    }

    for (;;) {
        if (emit_indent_line(emitter, parser->depth) != 0) {
            parser_fail(parser, "failed to write output");
            return -1;
        }

        if (parse_string(parser, emitter) != 0) return -1;

        skip_ws(parser);
        if (!parser_match_char(parser, ':')) {
            parser_fail(parser, "expected ':' after object key");
            return -1;
        }

        if (emitter && emitter->enabled && emitter->pretty) {
            if (emit_char(emitter, ':') != 0 || emit_char(emitter, ' ') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
        } else {
            if (emit_char(emitter, ':') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
        }

        skip_ws(parser);
        if (parse_value(parser, emitter) != 0) return -1;

        skip_ws(parser);
        if (parser_match_char(parser, ',')) {
            if (emit_char(emitter, ',') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            skip_ws(parser);
            continue;
        }

        if (parser_match_char(parser, '}')) {
            if (emit_indent_line(emitter, parser->depth - 1) != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            if (emit_char(emitter, '}') != 0) {
                parser_fail(parser, "failed to write output");
                return -1;
            }
            parser->depth--;
            return 0;
        }

        parser_fail(parser, "expected ',' or '}' in object");
        return -1;
    }
}

static int parse_value(json_parser_t* parser, const json_emitter_t* emitter) {
    skip_ws(parser);
    if (parser->pos >= parser->len) {
        parser_fail(parser, "unexpected end of JSON input");
        return -1;
    }

    char c = parser->data[parser->pos];
    if (c == '{') return parse_object(parser, emitter);
    if (c == '[') return parse_array(parser, emitter);
    if (c == '"') return parse_string(parser, emitter);
    if (c == 't') return parse_literal(parser, emitter, "true", 4);
    if (c == 'f') return parse_literal(parser, emitter, "false", 5);
    if (c == 'n') return parse_literal(parser, emitter, "null", 4);
    if (c == '-' || is_digit_char(c)) return parse_number(parser, emitter);

    parser_fail(parser, "invalid JSON value");
    return -1;
}

static int parse_document(json_parser_t* parser, const json_emitter_t* emitter) {
    skip_ws(parser);
    if (parse_value(parser, emitter) != 0) return -1;
    skip_ws(parser);
    if (parser->pos != parser->len) {
        parser_fail(parser, "trailing data after JSON value");
        return -1;
    }
    return 0;
}

static int read_file_fully(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }

    for (;;) {
        char chunk[512];
        ssize_t rc = read(fd, chunk, sizeof(chunk));
        if (rc < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (rc == 0) break;

        if (len + (size_t)rc > JSONPARSE_MAX_INPUT) {
            free(buf);
            close(fd);
            return -2;
        }

        while (len + (size_t)rc + 1 > cap) {
            size_t next = cap * 2;
            if (next > JSONPARSE_MAX_INPUT + 1) {
                next = JSONPARSE_MAX_INPUT + 1;
            }
            char* bigger = (char*)realloc(buf, next);
            if (!bigger) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = bigger;
            cap = next;
        }

        memcpy(buf + len, chunk, (size_t)rc);
        len += (size_t)rc;
    }

    close(fd);
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static void calc_line_col(const char* data, size_t len, size_t pos, int* out_line, int* out_col) {
    int line = 1;
    int col = 1;
    if (pos > len) pos = len;
    for (size_t i = 0; i < pos; ++i) {
        if (data[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    *out_line = line;
    *out_col = col;
}

int main(int argc, char** argv) {
    int pretty = 1;
    int check_only = 0;
    const char* path = NULL;

    if (argc < 2) {
        usage();
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg || !arg[0]) continue;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(arg, "--pretty") == 0) {
            pretty = 1;
            continue;
        }
        if (strcmp(arg, "--minify") == 0) {
            pretty = 0;
            continue;
        }
        if (strcmp(arg, "--check") == 0) {
            check_only = 1;
            continue;
        }
        if (arg[0] == '-') {
            printf("jsonparse: unknown option: %s\n", arg);
            return 1;
        }
        if (path) {
            puts("jsonparse: only one input file is supported");
            return 1;
        }
        path = arg;
    }

    if (!path) {
        usage();
        return 1;
    }

    char* input = NULL;
    size_t input_len = 0;
    int rr = read_file_fully(path, &input, &input_len);
    if (rr == -2) {
        printf("jsonparse: input too large (max %u bytes): %s\n", (unsigned)JSONPARSE_MAX_INPUT, path);
        return 1;
    }
    if (rr != 0) {
        printf("jsonparse: failed to read file: %s\n", path);
        return 1;
    }

    json_parser_t validate = {
        .data = input,
        .len = input_len,
        .pos = 0,
        .error = NULL,
        .error_pos = 0,
        .depth = 0,
    };

    if (parse_document(&validate, NULL) != 0) {
        int line = 1;
        int col = 1;
        calc_line_col(input, input_len, validate.error_pos, &line, &col);
        printf("jsonparse: parse error at %d:%d: %s\n", line, col,
               validate.error ? validate.error : "unknown error");
        free(input);
        return 1;
    }

    if (check_only) {
        puts("jsonparse: valid JSON");
        free(input);
        return 0;
    }

    json_parser_t emit_parser = {
        .data = input,
        .len = input_len,
        .pos = 0,
        .error = NULL,
        .error_pos = 0,
        .depth = 0,
    };
    json_emitter_t emitter = {
        .enabled = 1,
        .pretty = pretty,
    };

    if (parse_document(&emit_parser, &emitter) != 0) {
        puts("jsonparse: output failure");
        free(input);
        return 1;
    }

    putchar('\n');
    free(input);
    return 0;
}
