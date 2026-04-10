#include "package.h"
#include "index.h"

#include <eynos_syscall.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PKG_HTTP_MAX_HOST 128
#define PKG_HTTP_MAX_PATH 256
#define PKG_HTTP_MAX_HEADER 4096
#define PKG_HTTP_RECV_BUF 1536
#define PKG_HTTP_CHUNK_BUF 4096
#define PKG_DOWNLOAD_DEFAULT_MAX (8u * 1024u * 1024u)
#define PKG_INSTALL_PATH_CAP 256
#define PKG_TEMP_PATH_CAP 320
#define PKG_IO_CHUNK 1024

#define PKG_SHA256_BLOCK_SIZE 64
#define PKG_SHA256_DIGEST_SIZE 32

typedef struct {
    char host[PKG_HTTP_MAX_HOST];
    char path[PKG_HTTP_MAX_PATH];
    uint16_t port;
} pkg_http_url_t;

typedef int (*pkg_body_writer_fn)(const uint8_t* data, size_t len, void* ctx);

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buffer[PKG_SHA256_BLOCK_SIZE];
    size_t buffer_len;
} pkg_sha256_ctx_t;

static const uint32_t pkg_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static int pkg_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int pkg_parse_ipv4_str(const char* s, uint8_t out[4]) {
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

static int pkg_parse_http_url(const char* url, pkg_http_url_t* out) {
    if (!url || !out) return -1;

    const char* prefix = "http://";
    size_t prefix_len = strlen(prefix);
    if (strncmp(url, prefix, prefix_len) != 0) return -1;

    const char* p = url + prefix_len;
    const char* host_start = p;
    while (*p && *p != '/' && *p != ':') p++;

    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;

    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    out->port = 80;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = (port * 10) + (*p - '0');
            if (port > 65535) return -1;
            p++;
        }
        if (port == 0) return -1;
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        strncpy(out->path, "/", sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
        return 0;
    }

    if (*p != '/') return -1;

    size_t path_len = strlen(p);
    if (path_len >= sizeof(out->path)) return -1;
    memcpy(out->path, p, path_len + 1);
    return 0;
}

static int pkg_header_key_match(const char* line, const char* key) {
    while (*line && *key) {
        if (pkg_ascii_lower(*line) != pkg_ascii_lower(*key)) return 0;
        line++;
        key++;
    }
    return (*key == '\0');
}

static int pkg_header_get_value(const char* headers, const char* key, char* out, size_t out_cap) {
    if (!headers || !key || !out || out_cap == 0) return -1;

    const char* line = headers;
    while (*line) {
        const char* line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (line_end == line) break;

        if (pkg_header_key_match(line, key)) {
            const char* p = line + strlen(key);
            if (*p != ':') {
                line = line_end + 2;
                continue;
            }

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

static int pkg_parse_status_code(const char* headers) {
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

static int pkg_parse_hex_size(const char* s, size_t len, size_t* out) {
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

static int pkg_string_contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return 0;

    size_t needle_len = strlen(needle);
    size_t hay_len = strlen(haystack);
    if (needle_len > hay_len) return 0;

    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        size_t j = 0;
        while (j < needle_len) {
            if (pkg_ascii_lower(haystack[i + j]) != pkg_ascii_lower(needle[j])) break;
            j++;
        }
        if (j == needle_len) return 1;
    }

    return 0;
}

static int pkg_tcp_send_all(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 512) chunk = 512;

        int rc = eyn_sys_net_tcp_send(p + sent, (uint32_t)chunk);
        if (rc < 0) return -1;
        sent += chunk;
    }

    return 0;
}

static int pkg_http_get_stream(const char* url,
                               pkg_body_writer_fn writer,
                               void* writer_ctx,
                               size_t* out_bytes) {
    if (!url || !writer || !out_bytes) return -1;

    pkg_http_url_t parts;
    if (pkg_parse_http_url(url, &parts) != 0) {
        printf("install: invalid URL: %s\n", url);
        return -1;
    }

    uint8_t dst_ip[4];
    if (pkg_parse_ipv4_str(parts.host, dst_ip) != 0) {
        if (eyn_sys_net_dns_resolve(parts.host, dst_ip) != 0) {
            printf("install: DNS failed for %s\n", parts.host);
            return -1;
        }
    }

    if (eyn_sys_net_tcp_connect(dst_ip, parts.port, 0) != 0) {
        printf("install: TCP connect failed for %s:%u\n", parts.host, (unsigned)parts.port);
        return -1;
    }

    char req[512];
    int req_len = snprintf(req,
                           sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EYN-OS/install\r\n"
                           "Connection: close\r\n\r\n",
                           parts.path,
                           parts.host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        (void)eyn_sys_net_tcp_close();
        puts("install: HTTP request too large");
        return -1;
    }

    if (pkg_tcp_send_all(req, (size_t)req_len) != 0) {
        (void)eyn_sys_net_tcp_close();
        puts("install: failed to send request");
        return -1;
    }

    char header[PKG_HTTP_MAX_HEADER];
    size_t header_len = 0;
    int header_done = 0;
    int status = 0;

    int chunked = 0;
    long content_length = -1;

    uint8_t chunk_buf[PKG_HTTP_CHUNK_BUF];
    size_t chunk_len = 0;
    size_t chunk_need = 0;
    int chunk_have_size = 0;
    int chunk_done = 0;

    size_t total_written = 0;

    for (;;) {
        uint8_t rx_buf[PKG_HTTP_RECV_BUF];
        int rc = eyn_sys_net_tcp_recv(rx_buf, sizeof(rx_buf));
        if (rc == -2) break;

        if (rc < 0) {
            puts("install: HTTP receive failed");
            (void)eyn_sys_net_tcp_close();
            return -1;
        }

        if (rc == 0) {
            usleep(10000);
            continue;
        }

        if (!header_done) {
            if (header_len + (size_t)rc >= sizeof(header)) {
                puts("install: response headers too large");
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            memcpy(header + header_len, rx_buf, (size_t)rc);
            header_len += (size_t)rc;
            header[header_len] = '\0';

            char* marker = strstr(header, "\r\n\r\n");
            if (!marker) continue;

            size_t header_end = (size_t)(marker - header) + 4;
            header[header_end] = '\0';
            header_done = 1;

            status = pkg_parse_status_code(header);
            if (status != 200 && status != 206) {
                printf("install: HTTP status %d for %s\n", status, url);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            char transfer_encoding[64];
            transfer_encoding[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Transfer-Encoding",
                                     transfer_encoding,
                                     sizeof(transfer_encoding)) == 0) {
                if (pkg_string_contains_ci(transfer_encoding, "chunked")) {
                    chunked = 1;
                }
            }

            char content_len_str[32];
            content_len_str[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Content-Length",
                                     content_len_str,
                                     sizeof(content_len_str)) == 0) {
                content_length = strtol(content_len_str, NULL, 10);
                if (content_length < 0) content_length = -1;
            }

            size_t body_len = header_len - header_end;
            if (body_len > 0) {
                if (!chunked) {
                    if (writer((const uint8_t*)(header + header_end), body_len, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += body_len;
                } else {
                    if (body_len > sizeof(chunk_buf)) {
                        puts("install: chunk buffer overflow");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    memcpy(chunk_buf, header + header_end, body_len);
                    chunk_len = body_len;
                }
            }
        } else {
            if (!chunked) {
                if (writer(rx_buf, (size_t)rc, writer_ctx) != 0) {
                    puts("install: body write failed");
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                total_written += (size_t)rc;
            } else {
                if (chunk_len + (size_t)rc > sizeof(chunk_buf)) {
                    puts("install: chunk buffer overflow");
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                memcpy(chunk_buf + chunk_len, rx_buf, (size_t)rc);
                chunk_len += (size_t)rc;
            }
        }

        if (chunked) {
            while (!chunk_done) {
                if (!chunk_have_size) {
                    size_t line_end = 0;
                    while (line_end + 1 < chunk_len) {
                        if (chunk_buf[line_end] == '\r' && chunk_buf[line_end + 1] == '\n') break;
                        line_end++;
                    }
                    if (line_end + 1 >= chunk_len) break;

                    if (pkg_parse_hex_size((const char*)chunk_buf, line_end, &chunk_need) != 0) {
                        puts("install: invalid chunk size");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
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

                    if (writer(chunk_buf, chunk_need, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += chunk_need;

                    size_t consume = chunk_need + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 0;
                    chunk_need = 0;
                }
            }
        }

        if (!chunked && content_length >= 0 && total_written >= (size_t)content_length) {
            break;
        }
        if (chunked && chunk_done) break;
    }

    (void)eyn_sys_net_tcp_close();

    if (chunked && !chunk_done) {
        puts("install: incomplete chunked transfer");
        return -1;
    }

    *out_bytes = total_written;
    return 0;
}

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    size_t max_bytes;
} pkg_mem_sink_t;

static int pkg_mem_sink_write(const uint8_t* data, size_t len, void* ctx) {
    pkg_mem_sink_t* sink = (pkg_mem_sink_t*)ctx;
    if (!sink || !data) return -1;

    if (sink->len + len > sink->max_bytes) {
        puts("install: download exceeded max allowed size");
        return -1;
    }

    size_t needed = sink->len + len;
    while (needed > sink->cap) {
        size_t next = sink->cap * 2;
        if (next < needed) next = needed;
        if (next > sink->max_bytes) next = sink->max_bytes;
        if (next < needed) return -1;

        uint8_t* bigger = (uint8_t*)realloc(sink->data, next + 1);
        if (!bigger) return -1;

        sink->data = bigger;
        sink->cap = next;
    }

    memcpy(sink->data + sink->len, data, len);
    sink->len += len;
    return 0;
}

int package_download_url_to_buffer(const char* url,
                                   uint8_t** out_data,
                                   size_t* out_len,
                                   size_t max_bytes) {
    if (!url || !out_data || !out_len) return -1;

    if (max_bytes == 0) {
        max_bytes = PKG_DOWNLOAD_DEFAULT_MAX;
    }

    pkg_mem_sink_t sink;
    sink.data = (uint8_t*)malloc(4097);
    sink.len = 0;
    sink.cap = 4096;
    sink.max_bytes = max_bytes;
    if (!sink.data) return -1;

    size_t downloaded = 0;
    if (pkg_http_get_stream(url, pkg_mem_sink_write, &sink, &downloaded) != 0) {
        free(sink.data);
        return -1;
    }

    sink.data[sink.len] = '\0';
    *out_data = sink.data;
    *out_len = sink.len;
    (void)downloaded;
    return 0;
}

static uint32_t pkg_sha_rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t pkg_sha_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t pkg_sha_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t pkg_sha_big_sigma0(uint32_t x) {
    return pkg_sha_rotr32(x, 2u) ^ pkg_sha_rotr32(x, 13u) ^ pkg_sha_rotr32(x, 22u);
}

static uint32_t pkg_sha_big_sigma1(uint32_t x) {
    return pkg_sha_rotr32(x, 6u) ^ pkg_sha_rotr32(x, 11u) ^ pkg_sha_rotr32(x, 25u);
}

static uint32_t pkg_sha_small_sigma0(uint32_t x) {
    return pkg_sha_rotr32(x, 7u) ^ pkg_sha_rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t pkg_sha_small_sigma1(uint32_t x) {
    return pkg_sha_rotr32(x, 17u) ^ pkg_sha_rotr32(x, 19u) ^ (x >> 10u);
}

static uint32_t pkg_sha_read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | ((uint32_t)p[3]);
}

static void pkg_sha_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void pkg_sha256_transform(pkg_sha256_ctx_t* ctx, const uint8_t block[PKG_SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = pkg_sha_read_be32(block + (size_t)i * 4u);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = pkg_sha_small_sigma1(w[i - 2]) + w[i - 7] + pkg_sha_small_sigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + pkg_sha_big_sigma1(e) + pkg_sha_ch(e, f, g) + pkg_sha256_k[i] + w[i];
        uint32_t t2 = pkg_sha_big_sigma0(a) + pkg_sha_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void pkg_sha256_init(pkg_sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buffer_len = 0;
}

static void pkg_sha256_update(pkg_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    ctx->total_len += (uint64_t)len;

    size_t off = 0;
    while (off < len) {
        size_t space = PKG_SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t take = len - off;
        if (take > space) take = space;

        memcpy(ctx->buffer + ctx->buffer_len, data + off, take);
        ctx->buffer_len += take;
        off += take;

        if (ctx->buffer_len == PKG_SHA256_BLOCK_SIZE) {
            pkg_sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void pkg_sha256_final(pkg_sha256_ctx_t* ctx, uint8_t out_digest[PKG_SHA256_DIGEST_SIZE]) {
    uint64_t bits = ctx->total_len * 8u;

    ctx->buffer[ctx->buffer_len++] = 0x80;

    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < PKG_SHA256_BLOCK_SIZE) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        pkg_sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    while (ctx->buffer_len < 56) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }

    for (int i = 7; i >= 0; --i) {
        ctx->buffer[ctx->buffer_len++] = (uint8_t)(bits >> (i * 8));
    }

    pkg_sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        pkg_sha_write_be32(out_digest + (size_t)i * 4u, ctx->state[i]);
    }
}

static void pkg_sha256_hex(const uint8_t digest[PKG_SHA256_DIGEST_SIZE], char out_hex[MAX_SHA]) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < PKG_SHA256_DIGEST_SIZE; i++) {
        out_hex[(size_t)i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out_hex[(size_t)i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
}

static int pkg_sha256_match_ci(const char* expected, const char* got) {
    if (!expected || !got) return 0;
    if (strlen(expected) != 64 || strlen(got) != 64) return 0;

    for (int i = 0; i < 64; i++) {
        if (pkg_ascii_lower(expected[i]) != pkg_ascii_lower(got[i])) return 0;
    }

    return 1;
}

typedef struct {
    int stream_handle;
    pkg_sha256_ctx_t sha;
    size_t total_written;
} pkg_install_sink_t;

static int pkg_install_sink_write(const uint8_t* data, size_t len, void* ctx) {
    pkg_install_sink_t* sink = (pkg_install_sink_t*)ctx;
    if (!sink || !data) return -1;

    if (eynfs_stream_write(sink->stream_handle, data, len) != (ssize_t)len) {
        return -1;
    }

    pkg_sha256_update(&sink->sha, data, len);
    sink->total_written += len;
    return 0;
}

static int pkg_build_install_path(const char* name, char out_path[PKG_INSTALL_PATH_CAP]) {
    if (!name || !out_path || !name[0]) return -1;

    int needed = snprintf(out_path, PKG_INSTALL_PATH_CAP, "/binaries/%s", name);
    if (needed <= 0 || needed >= PKG_INSTALL_PATH_CAP) return -1;
    return 0;
}

static int pkg_records_equal(const Package* left, const Package* right) {
    if (!left || !right) return 0;
    if (strcmp(left->name, right->name) != 0) return 0;
    if (strcmp(left->version, right->version) != 0) return 0;
    if (strcmp(left->url, right->url) != 0) return 0;
    if (strcmp(left->sha256, right->sha256) != 0) return 0;
    return 1;
}

static int pkg_has_suffix_ci(const char* value, const char* suffix) {
    if (!value || !suffix) return 0;

    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) return 0;

    const char* at = value + value_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        if (pkg_ascii_lower(at[i]) != pkg_ascii_lower(suffix[i])) return 0;
    }
    return 1;
}

static int pkg_url_is_archive(const char* url) {
    if (!url) return 0;
    return pkg_has_suffix_ci(url, ".tar")
        || pkg_has_suffix_ci(url, ".tar.gz")
        || pkg_has_suffix_ci(url, ".tgz");
}

static int pkg_path_exists(const char* path) {
    if (!path || !path[0]) return 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int pkg_ensure_dir_exists(const char* path) {
    if (!path || !path[0]) return -1;

    if (pkg_path_exists(path)) return 0;
    if (mkdir(path, 0) == 0) return 0;
    return pkg_path_exists(path) ? 0 : -1;
}

static int pkg_build_temp_archive_path(const char* name,
                                       char out_path[PKG_TEMP_PATH_CAP]) {
    if (!name || !name[0] || !out_path) return -1;

    int needed = snprintf(out_path,
                          PKG_TEMP_PATH_CAP,
                          "/tmp/install-%s.pkg",
                          name);
    if (needed <= 0 || needed >= PKG_TEMP_PATH_CAP) return -1;
    return 0;
}

static int pkg_measure_file_size(const char* path, size_t* out_size) {
    if (!path || !out_size) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t total = 0;
    uint8_t buf[PKG_IO_CHUNK];
    for (;;) {
        ssize_t rd = read(fd, buf, sizeof(buf));
        if (rd < 0) {
            close(fd);
            return -1;
        }
        if (rd == 0) break;
        total += (size_t)rd;
    }

    close(fd);
    *out_size = total;
    return 0;
}

static int pkg_extract_archive(const char* archive_path, const char* dest_dir) {
    if (!archive_path || !dest_dir) return -1;

    const char* argv_local[2];
    argv_local[0] = archive_path;
    argv_local[1] = dest_dir;

    int pid = spawn("/binaries/extract", argv_local, 2);
    if (pid <= 0) {
        pid = spawn("extract", argv_local, 2);
    }
    if (pid <= 0) {
        puts("install: failed to launch extract command");
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) <= 0) {
        puts("install: failed to wait for extract command");
        return -1;
    }
    if (status != 0) {
        printf("install: extract failed (status=%d)\n", status);
        return -1;
    }

    return 0;
}

static int pkg_download_verified_to_path(const Package* pkg,
                                         const char* out_path,
                                         size_t* out_bytes) {
    if (!pkg || !out_path) return -1;

    int stream_handle = eynfs_stream_begin(out_path);
    if (stream_handle < 0) {
        printf("install: failed to create %s\n", out_path);
        return -1;
    }

    pkg_install_sink_t sink;
    sink.stream_handle = stream_handle;
    sink.total_written = 0;
    pkg_sha256_init(&sink.sha);

    size_t downloaded = 0;
    if (pkg_http_get_stream(pkg->url, pkg_install_sink_write, &sink, &downloaded) != 0) {
        (void)eynfs_stream_end(stream_handle);
        (void)unlink(out_path);
        return -1;
    }

    if (eynfs_stream_end(stream_handle) != 0) {
        (void)unlink(out_path);
        return -1;
    }

    uint8_t digest[PKG_SHA256_DIGEST_SIZE];
    char digest_hex[MAX_SHA];
    pkg_sha256_final(&sink.sha, digest);
    pkg_sha256_hex(digest, digest_hex);

    if (!pkg_sha256_match_ci(pkg->sha256, digest_hex)) {
        printf("install: checksum mismatch for %s\n", pkg->name);
        printf("install: expected %s\n", pkg->sha256);
        printf("install: got      %s\n", digest_hex);
        (void)unlink(out_path);
        return -1;
    }

    if (sink.total_written != downloaded) {
        printf("install: short write for %s\n", pkg->name);
        (void)unlink(out_path);
        return -1;
    }

    if (out_bytes) *out_bytes = sink.total_written;
    return 0;
}

int install_package(const struct PackageIndex* index, const Package* pkg) {
    if (!index || !pkg || !pkg->name[0]) return -1;

    const Package* in_index = index_find_package((const PackageIndex*)index, pkg->name);
    if (!in_index) {
        printf("install: package '%s' not found in index\n", pkg->name);
        return -1;
    }

    if (!pkg_records_equal(in_index, pkg)) {
        printf("install: package metadata mismatch for %s\n", pkg->name);
        return -1;
    }

    if (strlen(pkg->sha256) != 64) {
        printf("install: invalid sha256 for %s\n", pkg->name);
        return -1;
    }

    char out_path[PKG_INSTALL_PATH_CAP];
    if (pkg_build_install_path(pkg->name, out_path) != 0) {
        printf("install: output path too long for %s\n", pkg->name);
        return -1;
    }

    size_t installed_bytes = 0;
    if (pkg_url_is_archive(pkg->url)) {
        char archive_path[PKG_TEMP_PATH_CAP];

        if (pkg_build_temp_archive_path(pkg->name, archive_path) != 0) {
            printf("install: temporary path too long for %s\n", pkg->name);
            return -1;
        }

        if (pkg_ensure_dir_exists("/tmp") != 0 || pkg_ensure_dir_exists("/binaries") != 0) {
            puts("install: failed to prepare temporary install directory");
            return -1;
        }

        if (pkg_download_verified_to_path(pkg, archive_path, NULL) != 0) {
            printf("install: download failed for %s\n", pkg->name);
            return -1;
        }

        if (pkg_extract_archive(archive_path, "/binaries") != 0) {
            (void)unlink(archive_path);
            printf("install: extraction failed for %s\n", pkg->name);
            return -1;
        }

        if (!pkg_path_exists(out_path)) {
            (void)unlink(archive_path);
            printf("install: no extracted payload found for %s\n", pkg->name);
            return -1;
        }

        if (pkg_measure_file_size(out_path, &installed_bytes) != 0) {
            (void)unlink(archive_path);
            printf("install: failed to inspect extracted payload for %s\n", pkg->name);
            return -1;
        }

        (void)unlink(archive_path);
    } else {
        if (pkg_download_verified_to_path(pkg, out_path, &installed_bytes) != 0) {
            printf("install: download failed for %s\n", pkg->name);
            return -1;
        }
    }

    printf("install: installed %s@%s (%lu bytes)\n",
           pkg->name,
           pkg->version,
           (unsigned long)installed_bytes);
    return 0;
}
