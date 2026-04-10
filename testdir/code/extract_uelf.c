#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Extract a TAR or TAR.GZ archive into a directory.", "extract /archive.tar.gz /out");

#define TAR_BLOCK_SIZE 512
#define TAR_IO_CHUNK 1024
#define EXTRACT_PATH_MAX 384
#define PAX_HEADER_MAX 16384

#define TAR_TYPE_REG '\0'
#define TAR_TYPE_REG_ALT '0'
#define TAR_TYPE_DIR '5'
#define TAR_TYPE_GNU_LONGNAME 'L'
#define TAR_TYPE_PAX_LOCAL 'x'
#define TAR_TYPE_PAX_GLOBAL 'g'

#define GZIP_INBUF_SIZE 1024
#define GZIP_WINDOW_SIZE 32768

#define HUFF_MAX_BITS 15
#define HUFF_MAX_NODES 8192
#define HUFF_CLEN_MAX_NODES 512

typedef struct {
    int16_t left;
    int16_t right;
    int16_t sym;
} huff_node_t;

typedef struct {
    int fd;

    uint8_t inbuf[GZIP_INBUF_SIZE];
    size_t in_pos;
    size_t in_len;

    uint32_t bitbuf;
    int bitcount;

    uint8_t window[GZIP_WINDOW_SIZE];
    uint32_t win_pos;

    int block_active;
    int block_kind;       /* 0: stored, 1: huffman */
    int block_is_final;
    int deflate_done;
    int trailer_done;

    uint32_t stored_remaining;
    uint16_t pending_copy_len;
    uint16_t pending_copy_dist;

    int dist_tree_present;
    huff_node_t lit_nodes[HUFF_MAX_NODES];
    huff_node_t dist_nodes[HUFF_MAX_NODES];

    uint32_t crc32;
    uint32_t out_size;
} gzip_state_t;

typedef enum {
    ARCHIVE_RAW = 0,
    ARCHIVE_GZIP = 1,
} archive_kind_t;

typedef struct {
    archive_kind_t kind;
    int fd;
    gzip_state_t gz;
} archive_reader_t;

static void usage(void) {
    puts("Usage: extract <archive.tar|archive.tar.gz> [destination]\n"
         "Examples:\n"
         "  extract /archive.tar\n"
         "  extract /archive.tar.gz /tmp/out");
}

static int fd_read_exact_plain(int fd, void* out, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t rc = read(fd, (uint8_t*)out + off, len - off);
        if (rc <= 0) return -1;
        off += (size_t)rc;
    }
    return 0;
}

static int fd_discard_plain(int fd, uint32_t count) {
    uint8_t scratch[256];
    while (count > 0) {
        size_t want = count > (uint32_t)sizeof(scratch) ? sizeof(scratch) : (size_t)count;
        if (fd_read_exact_plain(fd, scratch, want) != 0) return -1;
        count -= (uint32_t)want;
    }
    return 0;
}

static int fd_skip_cstring_plain(int fd) {
    for (;;) {
        uint8_t c = 0;
        ssize_t rc = read(fd, &c, 1);
        if (rc <= 0) return -1;
        if (c == '\0') return 0;
    }
}

static uint32_t crc32_update_byte(uint32_t crc, uint8_t b) {
    crc ^= (uint32_t)b;
    for (int i = 0; i < 8; ++i) {
        uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

static uint16_t reverse_bits(uint16_t v, int nbits) {
    uint16_t r = 0;
    for (int i = 0; i < nbits; ++i) {
        r = (uint16_t)((r << 1) | (v & 1u));
        v = (uint16_t)(v >> 1);
    }
    return r;
}

static void huff_reset(huff_node_t* nodes, int capacity) {
    for (int i = 0; i < capacity; ++i) {
        nodes[i].left = -1;
        nodes[i].right = -1;
        nodes[i].sym = -1;
    }
}

static int huff_build(huff_node_t* nodes,
                      int capacity,
                      const uint8_t* lengths,
                      int num_syms) {
    uint16_t bl_count[HUFF_MAX_BITS + 1];
    uint16_t next_code[HUFF_MAX_BITS + 1];
    int node_count = 1;
    int num_codes = 0;

    for (int i = 0; i <= HUFF_MAX_BITS; ++i) {
        bl_count[i] = 0;
        next_code[i] = 0;
    }

    huff_reset(nodes, capacity);

    for (int sym = 0; sym < num_syms; ++sym) {
        uint8_t len = lengths[sym];
        if (len > HUFF_MAX_BITS) return -1;
        if (len) {
            bl_count[len]++;
            num_codes++;
        }
    }

    if (num_codes == 0) return -1;

    {
        int left = 1;
        for (int bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            left <<= 1;
            left -= (int)bl_count[bits];
            if (left < 0) return -1;
        }
    }

    {
        uint16_t code = 0;
        for (int bits = 1; bits <= HUFF_MAX_BITS; ++bits) {
            code = (uint16_t)((code + bl_count[bits - 1]) << 1);
            next_code[bits] = code;
        }
    }

    for (int sym = 0; sym < num_syms; ++sym) {
        uint8_t len = lengths[sym];
        if (!len) continue;

        uint16_t code = next_code[len]++;
        uint16_t rev = reverse_bits(code, len);

        int node = 0;
        for (int depth = 0; depth < len; ++depth) {
            int bit = (rev >> depth) & 1;
            int16_t* child = bit ? &nodes[node].right : &nodes[node].left;
            if (*child < 0) {
                if (node_count >= capacity) return -1;
                *child = (int16_t)node_count;
                node_count++;
            }
            node = *child;
        }

        if (nodes[node].sym >= 0) return -1;
        nodes[node].sym = (int16_t)sym;
    }

    return 0;
}

static int gzip_pull_input_byte(gzip_state_t* gz, uint8_t* out) {
    if (!out) return -1;

    if (gz->in_pos >= gz->in_len) {
        ssize_t rc = read(gz->fd, gz->inbuf, sizeof(gz->inbuf));
        if (rc <= 0) return -1;
        gz->in_pos = 0;
        gz->in_len = (size_t)rc;
    }

    *out = gz->inbuf[gz->in_pos++];
    return 0;
}

static int gzip_fill_bits(gzip_state_t* gz, int nbits) {
    while (gz->bitcount < nbits) {
        uint8_t b = 0;
        if (gzip_pull_input_byte(gz, &b) != 0) return -1;
        gz->bitbuf |= ((uint32_t)b) << gz->bitcount;
        gz->bitcount += 8;
    }
    return 0;
}

static int gzip_read_bits(gzip_state_t* gz, int nbits, uint32_t* out) {
    if (!out) return -1;
    if (nbits < 0 || nbits > 16) return -1;
    if (nbits == 0) {
        *out = 0;
        return 0;
    }

    if (gzip_fill_bits(gz, nbits) != 0) return -1;

    *out = gz->bitbuf & ((1u << nbits) - 1u);
    gz->bitbuf >>= nbits;
    gz->bitcount -= nbits;
    return 0;
}

static void gzip_align_to_byte(gzip_state_t* gz) {
    int drop = gz->bitcount & 7;
    gz->bitbuf >>= drop;
    gz->bitcount -= drop;
}

static int huff_decode_symbol(gzip_state_t* gz,
                              const huff_node_t* nodes,
                              int* out_sym) {
    int node = 0;

    for (int depth = 0; depth < HUFF_MAX_BITS; ++depth) {
        uint32_t bit = 0;
        if (gzip_read_bits(gz, 1, &bit) != 0) return -1;

        node = bit ? nodes[node].right : nodes[node].left;
        if (node < 0) return -1;

        if (nodes[node].sym >= 0) {
            *out_sym = nodes[node].sym;
            return 0;
        }
    }

    return -1;
}

static void gzip_emit_byte(gzip_state_t* gz, uint8_t b) {
    gz->window[gz->win_pos] = b;
    gz->win_pos = (gz->win_pos + 1u) & (GZIP_WINDOW_SIZE - 1u);
    gz->crc32 = crc32_update_byte(gz->crc32, b);
    gz->out_size++;
}

static int gzip_build_fixed_trees(gzip_state_t* gz) {
    uint8_t lit_lengths[288];
    uint8_t dist_lengths[32];

    for (int i = 0; i <= 143; ++i) lit_lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) lit_lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) lit_lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) lit_lengths[i] = 8;

    for (int i = 0; i < 32; ++i) dist_lengths[i] = 5;

    if (huff_build(gz->lit_nodes, HUFF_MAX_NODES, lit_lengths, 288) != 0) return -1;
    if (huff_build(gz->dist_nodes, HUFF_MAX_NODES, dist_lengths, 32) != 0) return -1;

    gz->dist_tree_present = 1;
    return 0;
}

static int gzip_build_dynamic_trees(gzip_state_t* gz) {
    static const uint8_t cl_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    uint32_t hlit_bits = 0;
    uint32_t hdist_bits = 0;
    uint32_t hclen_bits = 0;

    if (gzip_read_bits(gz, 5, &hlit_bits) != 0) return -1;
    if (gzip_read_bits(gz, 5, &hdist_bits) != 0) return -1;
    if (gzip_read_bits(gz, 4, &hclen_bits) != 0) return -1;

    int hlit = 257 + (int)hlit_bits;
    int hdist = 1 + (int)hdist_bits;
    int hclen = 4 + (int)hclen_bits;

    if (hlit > 286 || hdist > 32) return -1;

    uint8_t clen_lengths[19];
    for (int i = 0; i < 19; ++i) clen_lengths[i] = 0;

    for (int i = 0; i < hclen; ++i) {
        uint32_t v = 0;
        if (gzip_read_bits(gz, 3, &v) != 0) return -1;
        clen_lengths[cl_order[i]] = (uint8_t)v;
    }

    huff_node_t code_nodes[HUFF_CLEN_MAX_NODES];
    if (huff_build(code_nodes, HUFF_CLEN_MAX_NODES, clen_lengths, 19) != 0) return -1;

    uint8_t all_lengths[288 + 32];
    for (int i = 0; i < (int)(sizeof(all_lengths)); ++i) all_lengths[i] = 0;

    int total = hlit + hdist;
    int idx = 0;
    while (idx < total) {
        int sym = 0;
        if (huff_decode_symbol(gz, code_nodes, &sym) != 0) return -1;

        if (sym >= 0 && sym <= 15) {
            all_lengths[idx++] = (uint8_t)sym;
            continue;
        }

        if (sym == 16) {
            if (idx == 0) return -1;
            uint32_t extra = 0;
            if (gzip_read_bits(gz, 2, &extra) != 0) return -1;
            int repeat = 3 + (int)extra;
            uint8_t prev = all_lengths[idx - 1];
            if (idx + repeat > total) return -1;
            for (int j = 0; j < repeat; ++j) all_lengths[idx++] = prev;
            continue;
        }

        if (sym == 17) {
            uint32_t extra = 0;
            if (gzip_read_bits(gz, 3, &extra) != 0) return -1;
            int repeat = 3 + (int)extra;
            if (idx + repeat > total) return -1;
            for (int j = 0; j < repeat; ++j) all_lengths[idx++] = 0;
            continue;
        }

        if (sym == 18) {
            uint32_t extra = 0;
            if (gzip_read_bits(gz, 7, &extra) != 0) return -1;
            int repeat = 11 + (int)extra;
            if (idx + repeat > total) return -1;
            for (int j = 0; j < repeat; ++j) all_lengths[idx++] = 0;
            continue;
        }

        return -1;
    }

    uint8_t lit_lengths[288];
    uint8_t dist_lengths[32];
    for (int i = 0; i < 288; ++i) lit_lengths[i] = 0;
    for (int i = 0; i < 32; ++i) dist_lengths[i] = 0;

    for (int i = 0; i < hlit; ++i) lit_lengths[i] = all_lengths[i];
    for (int i = 0; i < hdist; ++i) dist_lengths[i] = all_lengths[hlit + i];

    if (lit_lengths[256] == 0) return -1;

    if (huff_build(gz->lit_nodes, HUFF_MAX_NODES, lit_lengths, 288) != 0) return -1;

    int dist_nonzero = 0;
    for (int i = 0; i < 32; ++i) {
        if (dist_lengths[i] != 0) {
            dist_nonzero = 1;
            break;
        }
    }

    if (!dist_nonzero) {
        gz->dist_tree_present = 0;
        return 0;
    }

    if (huff_build(gz->dist_nodes, HUFF_MAX_NODES, dist_lengths, 32) != 0) return -1;
    gz->dist_tree_present = 1;
    return 0;
}

static int gzip_start_block(gzip_state_t* gz) {
    uint32_t bfinal = 0;
    uint32_t btype = 0;

    if (gzip_read_bits(gz, 1, &bfinal) != 0) return -1;
    if (gzip_read_bits(gz, 2, &btype) != 0) return -1;

    gz->block_is_final = (int)bfinal;

    if (btype == 0) {
        gzip_align_to_byte(gz);

        uint32_t len = 0;
        uint32_t nlen = 0;
        if (gzip_read_bits(gz, 16, &len) != 0) return -1;
        if (gzip_read_bits(gz, 16, &nlen) != 0) return -1;
        if (((len ^ 0xFFFFu) & 0xFFFFu) != (nlen & 0xFFFFu)) return -1;

        gz->block_kind = 0;
        gz->stored_remaining = len;

        if (len == 0) {
            gz->block_active = 0;
            if (gz->block_is_final) gz->deflate_done = 1;
            return 0;
        }

        gz->block_active = 1;
        return 0;
    }

    if (btype == 1) {
        if (gzip_build_fixed_trees(gz) != 0) return -1;
        gz->block_kind = 1;
        gz->block_active = 1;
        return 0;
    }

    if (btype == 2) {
        if (gzip_build_dynamic_trees(gz) != 0) return -1;
        gz->block_kind = 1;
        gz->block_active = 1;
        return 0;
    }

    return -1;
}

static int gzip_inflate_next_byte(gzip_state_t* gz, uint8_t* out_b) {
    static const uint16_t len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10,
        11, 13, 15, 17,
        19, 23, 27, 31,
        35, 43, 51, 59,
        67, 83, 99, 115,
        131, 163, 195, 227, 258
    };
    static const uint8_t len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1,
        2, 2, 2, 2,
        3, 3, 3, 3,
        4, 4, 4, 4,
        5, 5, 5, 5, 0
    };
    static const uint16_t dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13,
        17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073,
        4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2,
        3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10,
        11, 11, 12, 12, 13, 13
    };

    if (!out_b) return -1;

    for (;;) {
        if (gz->pending_copy_len > 0) {
            if (gz->pending_copy_dist == 0 || gz->pending_copy_dist > GZIP_WINDOW_SIZE) return -1;

            uint32_t src = (gz->win_pos + GZIP_WINDOW_SIZE - gz->pending_copy_dist)
                         & (GZIP_WINDOW_SIZE - 1u);
            uint8_t b = gz->window[src];
            gzip_emit_byte(gz, b);
            gz->pending_copy_len--;
            *out_b = b;
            return 1;
        }

        if (gz->block_active && gz->block_kind == 0) {
            if (gz->stored_remaining == 0) {
                gz->block_active = 0;
                if (gz->block_is_final) gz->deflate_done = 1;
                continue;
            }

            uint32_t v = 0;
            if (gzip_read_bits(gz, 8, &v) != 0) return -1;
            uint8_t b = (uint8_t)v;
            gzip_emit_byte(gz, b);
            gz->stored_remaining--;

            if (gz->stored_remaining == 0) {
                gz->block_active = 0;
                if (gz->block_is_final) gz->deflate_done = 1;
            }

            *out_b = b;
            return 1;
        }

        if (gz->deflate_done) return 0;

        if (!gz->block_active) {
            if (gzip_start_block(gz) != 0) return -1;
            continue;
        }

        {
            int sym = 0;
            if (huff_decode_symbol(gz, gz->lit_nodes, &sym) != 0) return -1;

            if (sym < 256) {
                uint8_t b = (uint8_t)sym;
                gzip_emit_byte(gz, b);
                *out_b = b;
                return 1;
            }

            if (sym == 256) {
                gz->block_active = 0;
                if (gz->block_is_final) gz->deflate_done = 1;
                continue;
            }

            if (sym < 257 || sym > 285) return -1;

            int len_index = sym - 257;
            uint32_t extra_len = 0;
            if (len_extra[len_index] > 0) {
                if (gzip_read_bits(gz, len_extra[len_index], &extra_len) != 0) return -1;
            }
            uint32_t length = (uint32_t)len_base[len_index] + extra_len;

            if (!gz->dist_tree_present) return -1;

            int dist_sym = 0;
            if (huff_decode_symbol(gz, gz->dist_nodes, &dist_sym) != 0) return -1;
            if (dist_sym < 0 || dist_sym > 29) return -1;

            uint32_t extra_dist = 0;
            if (dist_extra[dist_sym] > 0) {
                if (gzip_read_bits(gz, dist_extra[dist_sym], &extra_dist) != 0) return -1;
            }
            uint32_t dist = (uint32_t)dist_base[dist_sym] + extra_dist;

            if (dist == 0 || dist > GZIP_WINDOW_SIZE) return -1;
            if (length == 0 || length > 258) return -1;

            gz->pending_copy_len = (uint16_t)length;
            gz->pending_copy_dist = (uint16_t)dist;
        }
    }
}

static int gzip_finish_trailer(gzip_state_t* gz) {
    if (gz->trailer_done) return 0;

    gzip_align_to_byte(gz);

    uint32_t crc_expect = 0;
    uint32_t isize_expect = 0;

    for (int i = 0; i < 4; ++i) {
        uint32_t b = 0;
        if (gzip_read_bits(gz, 8, &b) != 0) return -1;
        crc_expect |= (b << (i * 8));
    }
    for (int i = 0; i < 4; ++i) {
        uint32_t b = 0;
        if (gzip_read_bits(gz, 8, &b) != 0) return -1;
        isize_expect |= (b << (i * 8));
    }

    uint32_t crc_actual = ~gz->crc32;
    if (crc_actual != crc_expect) return -1;
    if (gz->out_size != isize_expect) return -1;

    gz->trailer_done = 1;
    return 0;
}

static int gzip_init(gzip_state_t* gz, int fd) {
    memset(gz, 0, sizeof(*gz));
    gz->fd = fd;
    gz->crc32 = 0xFFFFFFFFu;
    gz->out_size = 0;
    gz->win_pos = 0;

    uint8_t hdr[10];
    if (fd_read_exact_plain(fd, hdr, sizeof(hdr)) != 0) return -1;

    if (hdr[0] != 0x1f || hdr[1] != 0x8b) return -1;
    if (hdr[2] != 8) return -1;

    uint8_t flg = hdr[3];
    if (flg & 0xE0) return -1;

    if (flg & 0x04) {
        uint8_t xlen_bytes[2];
        if (fd_read_exact_plain(fd, xlen_bytes, 2) != 0) return -1;
        uint32_t xlen = (uint32_t)xlen_bytes[0] | ((uint32_t)xlen_bytes[1] << 8);
        if (fd_discard_plain(fd, xlen) != 0) return -1;
    }

    if (flg & 0x08) {
        if (fd_skip_cstring_plain(fd) != 0) return -1;
    }

    if (flg & 0x10) {
        if (fd_skip_cstring_plain(fd) != 0) return -1;
    }

    if (flg & 0x02) {
        uint8_t crc16[2];
        if (fd_read_exact_plain(fd, crc16, 2) != 0) return -1;
    }

    return 0;
}

static ssize_t gzip_read(gzip_state_t* gz, void* out, size_t len) {
    if (!out) return -1;
    if (len == 0) return 0;

    uint8_t* dst = (uint8_t*)out;
    size_t produced = 0;

    while (produced < len) {
        if (gz->trailer_done) break;

        uint8_t b = 0;
        int rc = gzip_inflate_next_byte(gz, &b);
        if (rc < 0) {
            return produced ? (ssize_t)produced : -1;
        }
        if (rc == 0) {
            if (gzip_finish_trailer(gz) != 0) {
                return produced ? (ssize_t)produced : -1;
            }
            continue;
        }

        dst[produced++] = b;
    }

    return (ssize_t)produced;
}

static ssize_t archive_read_some(archive_reader_t* ar, void* out, size_t len) {
    if (!ar || !out) return -1;
    if (ar->kind == ARCHIVE_RAW) {
        return read(ar->fd, out, len);
    }
    return gzip_read(&ar->gz, out, len);
}

static int archive_read_exact(archive_reader_t* ar, void* out, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t rc = archive_read_some(ar, (uint8_t*)out + off, len - off);
        if (rc <= 0) return -1;
        off += (size_t)rc;
    }
    return 0;
}

static int archive_discard(archive_reader_t* ar, uint32_t count) {
    uint8_t scratch[256];
    while (count > 0) {
        size_t want = count > (uint32_t)sizeof(scratch) ? sizeof(scratch) : (size_t)count;
        if (archive_read_exact(ar, scratch, want) != 0) return -1;
        count -= (uint32_t)want;
    }
    return 0;
}

/*
 * Read one TAR block. Returns:
 *   1 on success,
 *   0 on clean EOF,
 *  -1 on truncated/corrupt stream.
 */
static int archive_read_block(archive_reader_t* ar, uint8_t out[TAR_BLOCK_SIZE]) {
    ssize_t first = archive_read_some(ar, out, TAR_BLOCK_SIZE);
    if (first == 0) return 0;
    if (first < 0) return -1;

    size_t got = (size_t)first;
    while (got < TAR_BLOCK_SIZE) {
        ssize_t rc = archive_read_some(ar, out + got, TAR_BLOCK_SIZE - got);
        if (rc <= 0) return -1;
        got += (size_t)rc;
    }
    return 1;
}

static int is_zero_block(const uint8_t b[TAR_BLOCK_SIZE]) {
    for (int i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (b[i] != 0) return 0;
    }
    return 1;
}

static int parse_octal_u32(const uint8_t* field, size_t len, uint32_t* out) {
    if (!field || !out) return -1;

    uint32_t v = 0;
    int saw_digit = 0;
    size_t i = 0;

    while (i < len && (field[i] == ' ' || field[i] == '\0')) i++;

    for (; i < len; ++i) {
        uint8_t c = field[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') return -1;
        saw_digit = 1;
        v = (v << 3) + (uint32_t)(c - '0');
    }

    if (!saw_digit) {
        *out = 0;
        return 0;
    }

    *out = v;
    return 0;
}

static void copy_tar_string(char* out, size_t out_cap, const uint8_t* field, size_t field_len) {
    if (!out || out_cap == 0) return;
    size_t n = 0;
    while (n < field_len && n + 1 < out_cap) {
        if (field[n] == '\0') break;
        out[n] = (char)field[n];
        n++;
    }
    out[n] = '\0';

    while (n > 0 && out[n - 1] == ' ') {
        out[n - 1] = '\0';
        n--;
    }
}

static int build_entry_name(const uint8_t hdr[TAR_BLOCK_SIZE], char* out, size_t out_cap) {
    char name[101];
    char prefix[156];

    copy_tar_string(name, sizeof(name), hdr + 0, 100);
    copy_tar_string(prefix, sizeof(prefix), hdr + 345, 155);

    size_t pn = strlen(prefix);
    size_t nn = strlen(name);
    size_t total = pn + (pn && nn ? 1u : 0u) + nn;
    if (total + 1 > out_cap) return -1;

    out[0] = '\0';
    if (pn) {
        strcpy(out, prefix);
        if (nn) strcat(out, "/");
    }
    if (nn) strcat(out, name);
    return 0;
}

static int push_component(const char* comp, size_t comp_len,
                          char* out, size_t out_cap, size_t* out_len) {
    if (comp_len == 0) return 0;
    if (comp_len == 1 && comp[0] == '.') return 0;
    if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') return -1;

    for (size_t i = 0; i < comp_len; ++i) {
        if (comp[i] == '\\' || comp[i] == ':') return -1;
    }

    if (*out_len != 0) {
        if (*out_len + 1 >= out_cap) return -1;
        out[(*out_len)++] = '/';
    }

    if (*out_len + comp_len >= out_cap) return -1;
    memcpy(out + *out_len, comp, comp_len);
    *out_len += comp_len;
    out[*out_len] = '\0';
    return 0;
}

/*
 * Convert TAR path to a relative, safe extraction path.
 * Rejects traversal and absolute path escapes.
 */
static int sanitize_relative_path(const char* in, char* out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;

    size_t i = 0;
    while (in[i] == '/') i++;

    size_t out_len = 0;
    out[0] = '\0';

    while (in[i]) {
        size_t start = i;
        while (in[i] && in[i] != '/') i++;
        size_t comp_len = i - start;

        if (push_component(in + start, comp_len, out, out_cap, &out_len) != 0) {
            return -1;
        }

        while (in[i] == '/') i++;
    }

    return 0;
}

static int join_paths(const char* base, const char* rel, char* out, size_t out_cap) {
    if (!base || !rel || !out || out_cap == 0) return -1;

    if (base[0] == '\0' || (base[0] == '.' && base[1] == '\0')) {
        size_t rn = strlen(rel);
        if (rn + 1 > out_cap) return -1;
        strcpy(out, rel);
        return 0;
    }

    size_t bn = strlen(base);
    size_t rn = strlen(rel);
    int need_sep = (bn > 0 && base[bn - 1] != '/');
    size_t total = bn + (need_sep ? 1u : 0u) + rn;
    if (total + 1 > out_cap) return -1;

    strcpy(out, base);
    if (need_sep) strcat(out, "/");
    strcat(out, rel);
    return 0;
}

static int ensure_dir_exists(const char* path) {
    if (!path || !path[0]) return -1;
    if ((path[0] == '.' && path[1] == '\0')
        || (path[0] == '/' && path[1] == '\0')) {
        return 0;
    }

    if (access(path, F_OK) == 0) return 0;
    if (mkdir(path, 0) == 0) return 0;
    if (access(path, F_OK) == 0) return 0;
    return -1;
}

static int ensure_parent_dirs(const char* path) {
    if (!path || !path[0]) return -1;

    char tmp[EXTRACT_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (tmp[0] != '\0' && ensure_dir_exists(tmp) != 0) return -1;
        tmp[i] = '/';
    }
    return 0;
}

static int read_tar_size(const uint8_t hdr[TAR_BLOCK_SIZE], uint32_t* out_size) {
    return parse_octal_u32(hdr + 124, 12, out_size);
}

static int parse_u32_decimal_with_space(const char* s,
                                        size_t len,
                                        uint32_t* out_value,
                                        size_t* out_digits) {
    if (!s || !out_value || !out_digits || len == 0) return -1;

    uint32_t v = 0;
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    if (i == 0 || i >= len || s[i] != ' ') return -1;

    *out_value = v;
    *out_digits = i;
    return 0;
}

/*
 * Parse a PAX payload and return the final "path" key if present.
 * Returns:
 *   0 => path found in out_path
 *   1 => parsed successfully, no path key found
 *  -1 => malformed payload
 *  -2 => path value too large for out_path
 */
static int parse_pax_path(const char* payload,
                          size_t len,
                          char* out_path,
                          size_t out_path_cap) {
    if (!payload || !out_path || out_path_cap == 0) return -1;

    size_t pos = 0;
    int found = 0;

    while (pos < len) {
        uint32_t rec_len = 0;
        size_t digits = 0;
        if (parse_u32_decimal_with_space(payload + pos, len - pos, &rec_len, &digits) != 0) {
            return -1;
        }
        if (rec_len < (uint32_t)(digits + 2)) return -1;
        if ((size_t)rec_len > len - pos) return -1;

        size_t body_start = pos + digits + 1;
        size_t body_len = (size_t)rec_len - digits - 1;
        if (body_len == 0) return -1;

        const char* body = payload + body_start;
        size_t body_effective_len = body_len;
        if (body[body_len - 1] == '\n') body_effective_len--;

        size_t eq = 0;
        while (eq < body_effective_len && body[eq] != '=') eq++;
        if (eq < body_effective_len) {
            const char* key = body;
            size_t key_len = eq;
            const char* value = body + eq + 1;
            size_t value_len = body_effective_len - (eq + 1);

            if (key_len == 4 && memcmp(key, "path", 4) == 0) {
                if (value_len + 1 > out_path_cap) return -2;
                memcpy(out_path, value, value_len);
                out_path[value_len] = '\0';
                found = 1;
            }
        }

        pos += (size_t)rec_len;
    }

    return found ? 0 : 1;
}

static int read_text_payload(archive_reader_t* ar,
                             uint32_t size,
                             char* out,
                             size_t out_cap,
                             int* out_truncated) {
    if (!out || out_cap == 0) return -1;

    uint32_t remaining = size;
    size_t copied = 0;
    uint8_t buf[256];
    int truncated = 0;

    while (remaining > 0) {
        size_t want = remaining > (uint32_t)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        if (archive_read_exact(ar, buf, want) != 0) return -1;

        size_t room = (copied + 1 < out_cap) ? (out_cap - copied - 1) : 0;
        size_t take = want < room ? want : room;
        if (take > 0) {
            memcpy(out + copied, buf, take);
            copied += take;
        }
        if (take < want) truncated = 1;

        remaining -= (uint32_t)want;
    }

    out[copied] = '\0';

    for (size_t i = 0; i < copied; ++i) {
        if (out[i] == '\0') {
            out[i] = '\0';
            break;
        }
    }

    if (out_truncated) *out_truncated = truncated;
    return 0;
}

/*
 * Read and parse a PAX payload for "path".
 * Returns:
 *   0 => path found and written
 *   1 => no path key found
 *  -1 => read/parse error
 *  -2 => pax payload too large
 *  -3 => path too long for destination buffer
 */
static int read_pax_path_payload(archive_reader_t* ar,
                                 uint32_t size,
                                 char* out_path,
                                 size_t out_path_cap) {
    if (!out_path || out_path_cap == 0) return -1;
    out_path[0] = '\0';

    if (size == 0) return 1;
    if (size > PAX_HEADER_MAX) {
        if (archive_discard(ar, size) != 0) return -1;
        return -2;
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        if (archive_discard(ar, size) != 0) return -1;
        return -1;
    }

    if (archive_read_exact(ar, buf, (size_t)size) != 0) {
        free(buf);
        return -1;
    }
    buf[size] = '\0';

    int parsed = parse_pax_path(buf, (size_t)size, out_path, out_path_cap);
    free(buf);

    if (parsed == 0) return 0;
    if (parsed == 1) return 1;
    if (parsed == -2) return -3;
    return -1;
}

static int skip_tar_padding(archive_reader_t* ar, uint32_t size) {
    uint32_t pad = (uint32_t)((TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE);
    if (pad == 0) return 0;
    return archive_discard(ar, pad);
}

static int extract_regular_file(archive_reader_t* ar, const char* out_path, uint32_t size) {
    if (ensure_parent_dirs(out_path) != 0) return -1;

    int stream = eynfs_stream_begin(out_path);
    if (stream < 0) return -1;

    uint32_t remaining = size;
    uint8_t buf[TAR_IO_CHUNK];
    while (remaining > 0) {
        size_t want = remaining > (uint32_t)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        if (archive_read_exact(ar, buf, want) != 0) {
            (void)eynfs_stream_end(stream);
            (void)unlink(out_path);
            return -1;
        }

        ssize_t wr = eynfs_stream_write(stream, buf, want);
        if (wr != (ssize_t)want) {
            (void)eynfs_stream_end(stream);
            (void)unlink(out_path);
            return -1;
        }

        remaining -= (uint32_t)want;
    }

    if (eynfs_stream_end(stream) != 0) {
        (void)unlink(out_path);
        return -1;
    }
    return 0;
}

static int extract_directory_entry(archive_reader_t* ar, const char* out_path, uint32_t size) {
    if (ensure_parent_dirs(out_path) != 0) return -1;
    if (ensure_dir_exists(out_path) != 0) return -1;
    if (size > 0 && archive_discard(ar, size) != 0) return -1;
    return 0;
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

    const char* archive_path = argv[1];
    const char* dest = (argc >= 3 && argv[2] && argv[2][0]) ? argv[2] : ".";

    if (ensure_parent_dirs(dest) != 0 || ensure_dir_exists(dest) != 0) {
        printf("extract: failed to prepare destination: %s\n", dest);
        return 1;
    }

    int fd = open(archive_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("extract: failed to open archive: %s\n", archive_path);
        return 1;
    }

    uint8_t magic[2] = {0, 0};
    ssize_t mr = read(fd, magic, sizeof(magic));
    if (mr < 0) {
        puts("extract: failed to read archive header");
        close(fd);
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        puts("extract: failed to rewind archive");
        close(fd);
        return 1;
    }

    archive_reader_t reader;
    memset(&reader, 0, sizeof(reader));
    reader.fd = fd;

    if (mr == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        reader.kind = ARCHIVE_GZIP;
        if (gzip_init(&reader.gz, fd) != 0) {
            puts("extract: invalid or unsupported gzip stream");
            close(fd);
            return 1;
        }
    } else {
        reader.kind = ARCHIVE_RAW;
    }

    unsigned files = 0;
    unsigned dirs = 0;
    unsigned skipped = 0;

    char pending_long_name[EXTRACT_PATH_MAX];
    int has_pending_long_name = 0;
    pending_long_name[0] = '\0';

    uint8_t header[TAR_BLOCK_SIZE];
    uint8_t pending[TAR_BLOCK_SIZE];
    int have_pending = 0;

    for (;;) {
        int rb;
        if (have_pending) {
            memcpy(header, pending, TAR_BLOCK_SIZE);
            have_pending = 0;
            rb = 1;
        } else {
            rb = archive_read_block(&reader, header);
        }

        if (rb == 0) break;
        if (rb < 0) {
            puts("extract: failed to read tar header");
            close(fd);
            return 1;
        }

        if (is_zero_block(header)) {
            int rb2 = archive_read_block(&reader, pending);
            if (rb2 == 0) break;
            if (rb2 < 0) {
                puts("extract: failed to read tar trailer");
                close(fd);
                return 1;
            }
            if (is_zero_block(pending)) break;
            have_pending = 1;
            continue;
        }

        uint32_t size = 0;
        if (read_tar_size(header, &size) != 0) {
            puts("extract: invalid size field in tar entry");
            close(fd);
            return 1;
        }

        char type = (char)header[156];

        if (type == TAR_TYPE_GNU_LONGNAME) {
            int truncated = 0;
            if (read_text_payload(&reader, size,
                                  pending_long_name,
                                  sizeof(pending_long_name),
                                  &truncated) != 0) {
                puts("extract: failed to read GNU long-name payload");
                close(fd);
                return 1;
            }
            if (skip_tar_padding(&reader, size) != 0) {
                puts("extract: failed to read tar padding");
                close(fd);
                return 1;
            }

            if (truncated) {
                puts("extract: GNU long-name is too long for extractor path limit");
                has_pending_long_name = 0;
                pending_long_name[0] = '\0';
                skipped++;
                continue;
            }

            has_pending_long_name = 1;
            continue;
        }

        if (type == TAR_TYPE_PAX_LOCAL || type == TAR_TYPE_PAX_GLOBAL) {
            if (type == TAR_TYPE_PAX_LOCAL) {
                char pax_path[EXTRACT_PATH_MAX];
                int pr = read_pax_path_payload(&reader, size, pax_path, sizeof(pax_path));
                if (pr == 0) {
                    strcpy(pending_long_name, pax_path);
                    has_pending_long_name = 1;
                } else if (pr == -2) {
                    puts("extract: PAX header too large");
                    close(fd);
                    return 1;
                } else if (pr == -3) {
                    puts("extract: PAX path exceeds extractor path limit");
                    close(fd);
                    return 1;
                } else if (pr < 0) {
                    puts("extract: failed to parse PAX header");
                    close(fd);
                    return 1;
                }
            } else {
                if (archive_discard(&reader, size) != 0) {
                    puts("extract: failed to read PAX global header");
                    close(fd);
                    return 1;
                }
            }

            if (skip_tar_padding(&reader, size) != 0) {
                puts("extract: failed to read tar padding");
                close(fd);
                return 1;
            }
            continue;
        }

        char raw_name[EXTRACT_PATH_MAX];
        if (has_pending_long_name) {
            size_t ln = strlen(pending_long_name);
            if (ln + 1 > sizeof(raw_name)) {
                puts("extract: pending long-name path too long; skipping");
                has_pending_long_name = 0;
                pending_long_name[0] = '\0';
                if (archive_discard(&reader, size) != 0 || skip_tar_padding(&reader, size) != 0) {
                    close(fd);
                    return 1;
                }
                skipped++;
                continue;
            }
            strcpy(raw_name, pending_long_name);
            has_pending_long_name = 0;
            pending_long_name[0] = '\0';
        } else {
            if (build_entry_name(header, raw_name, sizeof(raw_name)) != 0) {
                puts("extract: entry path too long; skipping");
                if (archive_discard(&reader, size) != 0 || skip_tar_padding(&reader, size) != 0) {
                    close(fd);
                    return 1;
                }
                skipped++;
                continue;
            }
        }

        char rel_name[EXTRACT_PATH_MAX];
        if (sanitize_relative_path(raw_name, rel_name, sizeof(rel_name)) != 0) {
            printf("extract: skipped unsafe path: %s\n", raw_name);
            if (archive_discard(&reader, size) != 0 || skip_tar_padding(&reader, size) != 0) {
                close(fd);
                return 1;
            }
            skipped++;
            continue;
        }

        if (rel_name[0] == '\0') {
            if (archive_discard(&reader, size) != 0 || skip_tar_padding(&reader, size) != 0) {
                close(fd);
                return 1;
            }
            skipped++;
            continue;
        }

        char out_path[EXTRACT_PATH_MAX];
        if (join_paths(dest, rel_name, out_path, sizeof(out_path)) != 0) {
            printf("extract: output path too long: %s\n", rel_name);
            close(fd);
            return 1;
        }

        if (type == TAR_TYPE_REG || type == TAR_TYPE_REG_ALT) {
            if (extract_regular_file(&reader, out_path, size) != 0) {
                printf("extract: failed to extract file: %s\n", out_path);
                close(fd);
                return 1;
            }
            files++;
        } else if (type == TAR_TYPE_DIR) {
            if (extract_directory_entry(&reader, out_path, size) != 0) {
                printf("extract: failed to extract directory: %s\n", out_path);
                close(fd);
                return 1;
            }
            dirs++;
        } else {
            if (archive_discard(&reader, size) != 0) {
                close(fd);
                return 1;
            }
            skipped++;
            printf("extract: skipped type '%c' for %s\n", type ? type : '0', rel_name);
        }

        if (skip_tar_padding(&reader, size) != 0) {
            puts("extract: failed to read tar padding");
            close(fd);
            return 1;
        }
    }

    close(fd);
    printf("extract: %u files, %u directories, %u skipped\n", files, dirs, skipped);
    return 0;
}
