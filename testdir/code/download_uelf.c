#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Download a file over HTTP/1.1 (GET only) with DNS support.",
               "download http://example.com/index.html");

#define DOWNLOAD_MAX_URL 256
#define DOWNLOAD_MAX_HOST 128
#define DOWNLOAD_MAX_PATH 256
#define DOWNLOAD_MAX_HEADER 2048
#define DOWNLOAD_MAX_REDIRECTS 5
#define DOWNLOAD_TCP_RECV_BUF 1536

static int parse_ipv4_str(const char* s, uint8_t out[4]) {
    if (!s || !out) return -1;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = (v * 10) + (*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part != 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    return (*s == '\0') ? 0 : -1;
}

typedef struct {
    char host[DOWNLOAD_MAX_HOST];
    char path[DOWNLOAD_MAX_PATH];
    uint16_t port;
} http_url_t;

static int parse_http_url(const char* url, http_url_t* out) {
    if (!url || !out) return -1;
    const char* prefix = "http://";
    size_t prefix_len = strlen(prefix);
    if (strncmp(url, prefix, prefix_len) != 0) return -2;

    const char* p = url + prefix_len;
    const char* host_start = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -3;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    out->port = 80;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = (port * 10) + (*p - '0');
            if (port > 65535) return -4;
            p++;
        }
        if (port == 0) return -4;
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        strncpy(out->path, "/", sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
        return 0;
    }

    if (*p != '/') return -5;
    size_t path_len = strlen(p);
    if (path_len >= sizeof(out->path)) return -6;
    memcpy(out->path, p, path_len + 1);
    return 0;
}

static int is_url_absolute(const char* s) {
    return (s && strncmp(s, "http://", 7) == 0);
}

static int make_redirect_url(const http_url_t* base, const char* location,
                             char* out, size_t out_cap) {
    if (!base || !location || !out || out_cap == 0) return -1;
    if (is_url_absolute(location)) {
        if (strlen(location) >= out_cap) return -2;
        strncpy(out, location, out_cap - 1);
        out[out_cap - 1] = '\0';
        return 0;
    }

    char path[DOWNLOAD_MAX_PATH];
    if (location[0] == '/') {
        strncpy(path, location, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strncpy(path, base->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        char* last = strrchr(path, '/');
        if (!last) {
            strncpy(path, "/", sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            last[1] = '\0';
        }
        if (strlen(path) + strlen(location) + 1 >= sizeof(path)) return -3;
        strcat(path, location);
    }

    char port_suffix[16] = {0};
    if (base->port != 80) {
        snprintf(port_suffix, sizeof(port_suffix), ":%u", (unsigned)base->port);
    }

    int needed = snprintf(out, out_cap, "http://%s%s%s", base->host, port_suffix, path);
    return (needed < 0 || (size_t)needed >= out_cap) ? -4 : 0;
}

static int ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int header_key_match(const char* line, const char* key) {
    while (*line && *key) {
        if (ascii_lower(*line) != ascii_lower(*key)) return 0;
        line++;
        key++;
    }
    return (*key == '\0');
}

static int header_get_value(const char* headers, const char* key, char* out, size_t out_cap) {
    if (!headers || !key || !out || out_cap == 0) return -1;
    const char* line = headers;
    while (*line) {
        const char* line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (line_end == line) break;

        if (header_key_match(line, key)) {
            const char* p = line + strlen(key);
            if (*p != ':') { line = line_end + 2; continue; }
            p++;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = (size_t)(line_end - p);
            if (len >= out_cap) len = out_cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 0;
        }
        line = line_end + 2;
    }
    return -1;
}

static int parse_status_code(const char* headers) {
    if (!headers) return -1;
    const char* sp = strchr(headers, ' ');
    if (!sp) return -1;
    sp++;
    int code = 0;
    while (*sp >= '0' && *sp <= '9') {
        code = (code * 10) + (*sp - '0');
        sp++;
    }
    return code;
}

static int parse_hex_size(const char* s, size_t len, size_t* out) {
    if (!s || !out) return -1;
    size_t v = 0;
    int any = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else if (c == ';' || c == ' ' || c == '\t') break;
        else return -1;
        any = 1;
        v = (v << 4) | (size_t)d;
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

static int derive_output_path(const char* url_path, const char* content_type,
                              char* out, size_t out_cap) {
    if (!url_path || !out || out_cap == 0) return -1;
    const char* name = strrchr(url_path, '/');
    name = name ? name + 1 : url_path;
    if (*name == '\0') name = "index";

    char base[128];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    const char* dot = strrchr(base, '.');
    if (!dot && content_type) {
        if (strncmp(content_type, "text/html", 9) == 0) {
            if (strlen(base) + 5 < sizeof(base)) strcat(base, ".html");
        } else if (strncmp(content_type, "text/plain", 10) == 0) {
            if (strlen(base) + 4 < sizeof(base)) strcat(base, ".txt");
        }
    }

    if (strlen(base) >= out_cap) return -2;
    strncpy(out, base, out_cap - 1);
    out[out_cap - 1] = '\0';
    return 0;
}

static int tcp_send_all(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 512) chunk = 512;
        int rc = eyn_sys_net_tcp_send(p + sent, (uint32_t)chunk);
        if (rc < 0) return rc;
        sent += chunk;
    }
    return 0;
}

static int read_http_response(const char* url, const http_url_t* parts,
                              const char* out_path_hint, char* out_path,
                              size_t out_path_cap, char* out_url,
                              size_t out_url_cap) {
    (void)url;
    uint8_t dst_ip[4];
    if (parse_ipv4_str(parts->host, dst_ip) != 0) {
        if (eyn_sys_net_dns_resolve(parts->host, dst_ip) != 0) {
            printf("download: DNS failed for %s\n", parts->host);
            return -1;
        }
    }

    if (eyn_sys_net_tcp_connect(dst_ip, parts->port, 0) != 0) {
        puts("download: TCP connect failed");
        return -1;
    }

    char req[512];
    int req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EYN-OS/download\r\n"
                           "Connection: close\r\n\r\n",
                           parts->path, parts->host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        eyn_sys_net_tcp_close();
        puts("download: request too large");
        return -1;
    }
    if (tcp_send_all(req, (size_t)req_len) != 0) {
        eyn_sys_net_tcp_close();
        puts("download: request send failed");
        return -1;
    }

    char header[DOWNLOAD_MAX_HEADER];
    size_t header_len = 0;
    int header_done = 0;
    char content_type[64];
    char transfer_encoding[64];
    char location[DOWNLOAD_MAX_URL];
    long content_length = -1;
    int chunked = 0;

    int stream_handle = -1;
    long body_written = 0;

    // Chunked decoder state
    uint8_t chunk_buf[4096];
    size_t chunk_len = 0;
    size_t chunk_need = 0;
    int chunk_have_size = 0;
    int chunk_done = 0;

    while (1) {
        uint8_t rx_buf[DOWNLOAD_TCP_RECV_BUF];
        int rc = eyn_sys_net_tcp_recv(rx_buf, sizeof(rx_buf));
        if (rc == -2) break;
        if (rc < 0) {
            puts("download: recv error");
            break;
        }
        if (rc == 0) {
            usleep(10000);
            continue;
        }

        if (!header_done) {
            if (header_len + (size_t)rc >= sizeof(header)) {
                puts("download: headers too large");
                break;
            }
            memcpy(header + header_len, rx_buf, (size_t)rc);
            header_len += (size_t)rc;
            header[header_len] = '\0';

            char* marker = strstr(header, "\r\n\r\n");
            if (!marker) continue;

            size_t header_end = (size_t)(marker - header) + 4;
            header[header_end] = '\0';
            header_done = 1;

            int status = parse_status_code(header);
            if (header_get_value(header, "Content-Type", content_type, sizeof(content_type)) != 0) {
                content_type[0] = '\0';
            }
            if (header_get_value(header, "Transfer-Encoding", transfer_encoding, sizeof(transfer_encoding)) != 0) {
                transfer_encoding[0] = '\0';
            }
            if (strstr(transfer_encoding, "chunked") != NULL || strstr(transfer_encoding, "Chunked") != NULL) {
                chunked = 1;
            }
            char content_len_str[32];
            if (header_get_value(header, "Content-Length", content_len_str, sizeof(content_len_str)) == 0) {
                content_length = strtol(content_len_str, NULL, 10);
                if (content_length < 0) content_length = -1;
            }

            if ((status == 301 || status == 302) &&
                header_get_value(header, "Location", location, sizeof(location)) == 0) {
                eyn_sys_net_tcp_close();
                if (make_redirect_url(parts, location, out_url, out_url_cap) != 0) {
                    puts("download: redirect URL too long");
                    return -1;
                }
                return 1;
            }

            if (status != 200 && status != 206) {
                printf("download: HTTP status %d\n", status);
                break;
            }

            if (out_path_hint && out_path_hint[0]) {
                strncpy(out_path, out_path_hint, out_path_cap - 1);
                out_path[out_path_cap - 1] = '\0';
            } else {
                if (derive_output_path(parts->path, content_type, out_path, out_path_cap) != 0) {
                    puts("download: unable to derive output path");
                    break;
                }
            }

            stream_handle = eynfs_stream_begin(out_path);
            if (stream_handle < 0) {
                puts("download: failed to create output file");
                break;
            }

            size_t body_len = header_len - header_end;
            if (body_len > 0) {
                if (!chunked) {
                    if (eynfs_stream_write(stream_handle, header + header_end, body_len) != (ssize_t)body_len) {
                        puts("download: write failed");
                        break;
                    }
                    body_written += (long)body_len;
                } else {
                    if (body_len > sizeof(chunk_buf)) {
                        puts("download: chunk buffer overflow");
                        break;
                    }
                    memcpy(chunk_buf, header + header_end, body_len);
                    chunk_len = body_len;
                }
            }
        } else {
            if (stream_handle >= 0) {
                if (!chunked) {
                    if (eynfs_stream_write(stream_handle, rx_buf, (size_t)rc) != (ssize_t)rc) {
                        puts("download: write failed");
                        break;
                    }
                    body_written += rc;
                } else {
                    if (chunk_len + (size_t)rc > sizeof(chunk_buf)) {
                        puts("download: chunk buffer overflow");
                        break;
                    }
                    memcpy(chunk_buf + chunk_len, rx_buf, (size_t)rc);
                    chunk_len += (size_t)rc;
                }
            }
        }

        if (chunked && stream_handle >= 0) {
            while (!chunk_done) {
                if (!chunk_have_size) {
                    size_t line_end = 0;
                    while (line_end + 1 < chunk_len) {
                        if (chunk_buf[line_end] == '\r' && chunk_buf[line_end + 1] == '\n') break;
                        line_end++;
                    }
                    if (line_end + 1 >= chunk_len) break;

                    if (parse_hex_size((const char*)chunk_buf, line_end, &chunk_need) != 0) {
                        puts("download: invalid chunk size");
                        chunk_done = 1;
                        break;
                    }

                    size_t consume = line_end + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 1;

                    if (chunk_need == 0) {
                        chunk_done = 1;
                        break;
                    }
                }

                if (chunk_have_size) {
                    if (chunk_len < chunk_need + 2) break;
                    if (eynfs_stream_write(stream_handle, chunk_buf, chunk_need) != (ssize_t)chunk_need) {
                        puts("download: write failed");
                        chunk_done = 1;
                        break;
                    }
                    body_written += (long)chunk_need;

                    size_t consume = chunk_need + 2; // data + CRLF
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 0;
                    chunk_need = 0;
                }
            }
        }

        if (!chunked && content_length >= 0 && body_written >= content_length) {
            break;
        }
        if (chunked && chunk_done) break;
    }

    if (stream_handle >= 0) {
        (void)eynfs_stream_end(stream_handle);
    }
    (void)eyn_sys_net_tcp_close();

    if (out_path[0] == '\0') return -1;

    printf("download: saved %ld bytes to %s", body_written, out_path);
    if (content_type[0]) printf(" (type=%s)", content_type);
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1]) {
        puts("Usage: download <http://url> [out_path]");
        return 1;
    }

    char url[DOWNLOAD_MAX_URL];
    strncpy(url, argv[1], sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    const char* out_hint = (argc >= 3) ? argv[2] : NULL;
    char out_path[DOWNLOAD_MAX_PATH] = {0};

    for (int redirects = 0; redirects <= DOWNLOAD_MAX_REDIRECTS; redirects++) {
        http_url_t parts;
        if (parse_http_url(url, &parts) != 0) {
            puts("download: invalid URL (use http://host[:port]/path)");
            return 1;
        }

        char next_url[DOWNLOAD_MAX_URL] = {0};
        int rc = read_http_response(url, &parts, out_hint, out_path, sizeof(out_path),
                                    next_url, sizeof(next_url));
        if (rc == 0) return 0;
        if (rc < 0) return 1;

        strncpy(url, next_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    }

    puts("download: too many redirects");
    return 1;
}
