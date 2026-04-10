#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Compute SHA-256 digests for files or strings.", "sha256 /test.txt");

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    size_t buffer_len;
} sha256_ctx_t;

static const uint32_t k_table[64] = {
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

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t big_sigma0(uint32_t x) {
    return rotr32(x, 2u) ^ rotr32(x, 13u) ^ rotr32(x, 22u);
}

static uint32_t big_sigma1(uint32_t x) {
    return rotr32(x, 6u) ^ rotr32(x, 11u) ^ rotr32(x, 25u);
}

static uint32_t small_sigma0(uint32_t x) {
    return rotr32(x, 7u) ^ rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t small_sigma1(uint32_t x) {
    return rotr32(x, 17u) ^ rotr32(x, 19u) ^ (x >> 10u);
}

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | ((uint32_t)p[3]);
}

static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void sha256_transform(sha256_ctx_t* ctx, const uint8_t block[SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = read_be32(block + (size_t)i * 4u);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
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
        uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + k_table[i] + w[i];
        uint32_t t2 = big_sigma0(a) + maj(a, b, c);
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

static void sha256_init(sha256_ctx_t* ctx) {
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

static void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    ctx->total_len += (uint64_t)len;

    size_t off = 0;
    while (off < len) {
        size_t space = SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t take = len - off;
        if (take > space) take = space;

        memcpy(ctx->buffer + ctx->buffer_len, data + off, take);
        ctx->buffer_len += take;
        off += take;

        if (ctx->buffer_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8_t out_digest[SHA256_DIGEST_SIZE]) {
    uint64_t bits = ctx->total_len * 8u;

    ctx->buffer[ctx->buffer_len++] = 0x80;

    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < SHA256_BLOCK_SIZE) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    while (ctx->buffer_len < 56) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }

    for (int i = 7; i >= 0; --i) {
        ctx->buffer[ctx->buffer_len++] = (uint8_t)(bits >> (i * 8));
    }

    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        write_be32(out_digest + (size_t)i * 4u, ctx->state[i]);
    }
}

static void print_digest(const uint8_t digest[SHA256_DIGEST_SIZE]) {
    for (int i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", (unsigned)digest[i]);
    }
}

static int hash_file(const char* path, uint8_t digest[SHA256_DIGEST_SIZE]) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    sha256_ctx_t ctx;
    sha256_init(&ctx);

    uint8_t buf[1024];
    for (;;) {
        ssize_t rc = read(fd, buf, sizeof(buf));
        if (rc < 0) {
            close(fd);
            return -1;
        }
        if (rc == 0) break;
        sha256_update(&ctx, buf, (size_t)rc);
    }

    close(fd);
    sha256_final(&ctx, digest);
    return 0;
}

static void usage(void) {
    puts("Usage: sha256 <file> [file ...]\n"
         "       sha256 -s <text>\n"
         "Examples:\n"
         "  sha256 /test.txt\n"
         "  sha256 /bin/a /bin/b\n"
         "  sha256 -s hello");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "-s") == 0) {
        if (argc < 3 || !argv[2]) {
            puts("sha256: missing string after -s");
            return 1;
        }

        sha256_ctx_t ctx;
        uint8_t digest[SHA256_DIGEST_SIZE];
        const char* text = argv[2];

        sha256_init(&ctx);
        sha256_update(&ctx, (const uint8_t*)text, strlen(text));
        sha256_final(&ctx, digest);

        print_digest(digest);
        printf("  \"%s\"\n", text);
        return 0;
    }

    int had_error = 0;
    for (int i = 1; i < argc; ++i) {
        const char* path = argv[i];
        if (!path || !path[0]) {
            had_error = 1;
            continue;
        }

        uint8_t digest[SHA256_DIGEST_SIZE];
        if (hash_file(path, digest) != 0) {
            printf("sha256: failed to read %s\n", path);
            had_error = 1;
            continue;
        }

        print_digest(digest);
        printf("  %s\n", path);
    }

    return had_error ? 1 : 0;
}
