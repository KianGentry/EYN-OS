#include <gui.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_BUILD_ID "demo-2026-01-19a"

#define FB_W 320
#define FB_H 200
static uint16_t g_fb[FB_W * FB_H];
// Z buffer stores invZ in Q16.16 (larger = closer). Using 32-bit avoids truncation jitter.
static int32_t g_zb[FB_W * FB_H];

#define TEX_W 64
#define TEX_H 64
static uint16_t g_tex[TEX_W * TEX_H];

#define STAR_COUNT 64
static int16_t g_star_x[STAR_COUNT];
static int16_t g_star_y[STAR_COUNT];
static uint16_t g_star_z[STAR_COUNT];
static uint32_t g_rng = 0x12345678u;

// Fixed-point sine lookup table (Q16.16) for 256 steps across 0..2pi.
static const int32_t sin_table[256] = {
    0, 1608, 3216, 4821, 6424, 8022, 9616, 11204, 12785, 14359, 15924, 17479, 19024, 20557, 22078, 23586,
    25080, 26558, 28020, 29466, 30893, 32303, 33692, 35062, 36410, 37736, 39040, 40320, 41576, 42806, 44011, 45190,
    46341, 47464, 48559, 49624, 50660, 51665, 52639, 53581, 54491, 55368, 56212, 57022, 57798, 58538, 59244, 59914,
    60547, 61145, 61705, 62228, 62714, 63162, 63572, 63944, 64277, 64571, 64827, 65043, 65220, 65358, 65457, 65516,
    65536, 65516, 65457, 65358, 65220, 65043, 64827, 64571, 64277, 63944, 63572, 63162, 62714, 62228, 61705, 61145,
    60547, 59914, 59244, 58538, 57798, 57022, 56212, 55368, 54491, 53581, 52639, 51665, 50660, 49624, 48559, 47464,
    46341, 45190, 44011, 42806, 41576, 40320, 39040, 37736, 36410, 35062, 33692, 32303, 30893, 29466, 28020, 26558,
    25080, 23586, 22078, 20557, 19024, 17479, 15924, 14359, 12785, 11204, 9616, 8022, 6424, 4821, 3216, 1608,
    0, -1608, -3216, -4821, -6424, -8022, -9616, -11204, -12785, -14359, -15924, -17479, -19024, -20557, -22078, -23586,
    -25080, -26558, -28020, -29466, -30893, -32303, -33692, -35062, -36410, -37736, -39040, -40320, -41576, -42806, -44011, -45190,
    -46341, -47464, -48559, -49624, -50660, -51665, -52639, -53581, -54491, -55368, -56212, -57022, -57798, -58538, -59244, -59914,
    -60547, -61145, -61705, -62228, -62714, -63162, -63572, -63944, -64277, -64571, -64827, -65043, -65220, -65358, -65457, -65516,
    -65536, -65516, -65457, -65358, -65220, -65043, -64827, -64571, -64277, -63944, -63572, -63162, -62714, -62228, -61705, -61145,
    -60547, -59914, -59244, -58538, -57798, -57022, -56212, -55368, -54491, -53581, -52639, -51665, -50660, -49624, -48559, -47464,
    -46341, -45190, -44011, -42806, -41576, -40320, -39040, -37736, -36410, -35062, -33692, -32303, -30893, -29466, -28020, -26558,
    -25080, -23586, -22078, -20557, -19024, -17479, -15924, -14359, -12785, -11204, -9616, -8022, -6424, -4821, -3216, -1608
};

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline int32_t mul_q16(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 16);
}

static inline int32_t sin_q16_interp(int32_t ang_q8) {
    int idx = (ang_q8 >> 8) & 255;
    int frac = ang_q8 & 0xFF;
    int32_t s0 = sin_table[idx];
    int32_t s1 = sin_table[(idx + 1) & 255];
    return s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 8);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline uint32_t rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static void init_stars(void) {
    for (int i = 0; i < STAR_COUNT; ++i) {
        g_star_x[i] = (int16_t)((int)(rng_next() & 0x1FFu) - 0x100); // -256..255
        g_star_y[i] = (int16_t)((int)(rng_next() & 0xFFu) - 0x80);   // -128..127
        g_star_z[i] = (uint16_t)((rng_next() & 0xFFu) + 32u);        // 32..287
    }
}

static void init_texture(void) {
    for (int y = 0; y < TEX_H; ++y) {
        for (int x = 0; x < TEX_W; ++x) {
            int checker = ((x >> 3) ^ (y >> 3)) & 1;
            uint8_t r = checker ? 220 : 60;
            uint8_t g = checker ? 60 : 220;
            uint8_t b = (uint8_t)(80 + (x * 175) / (TEX_W - 1));
            g_tex[y * TEX_W + x] = rgb565(r, g, b);
        }
    }
}

// Minimal REI loader for texture use.
#define REI_MAGIC 0x52454900u
#define REI_DEPTH_MONO 1
#define REI_DEPTH_RGB 3
#define REI_DEPTH_RGBA 4
#define REI_COMP_NONE 0x0
#define REI_COMP_RLE  0x1
#define REI_COMP_MASK 0x0F

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t reserved1;
    uint16_t reserved2;
} rei_header_t;

static int rei_decompress_rle(const uint8_t* in, size_t in_size,
                              uint8_t* out, size_t out_size,
                              uint8_t pixel_size) {
    if (!in || !out || pixel_size == 0) return -1;
    size_t ip = 0;
    size_t op = 0;
    while (ip < in_size && op < out_size) {
        int8_t n = (int8_t)in[ip++];
        if (n >= 0) {
            size_t count = (size_t)n + 1;
            size_t bytes = count * pixel_size;
            if (ip + bytes > in_size) return -1;
            if (op + bytes > out_size) return -1;
            memcpy(out + op, in + ip, bytes);
            ip += bytes;
            op += bytes;
        } else if (n != -128) {
            size_t count = (size_t)(1 - n);
            if (ip + pixel_size > in_size) return -1;
            if (op + count * (size_t)pixel_size > out_size) return -1;
            const uint8_t* px = in + ip;
            ip += pixel_size;
            for (size_t i = 0; i < count; ++i) {
                memcpy(out + op, px, pixel_size);
                op += pixel_size;
            }
        }
    }
    return (op == out_size) ? 0 : -1;
}

static int load_rei_texture(const char* path) {
    if (!path) return -1;
    int fd = open(path, 0, 0);
    if (fd < 0) return -1;

    const size_t cap = 300 * 1024;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) { close(fd); return -1; }

    size_t total = 0;
    for (;;) {
        if (total >= cap) break;
        int n = (int)read(fd, buf + total, cap - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);

    if (total < sizeof(rei_header_t)) { free(buf); return -1; }
    rei_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != REI_MAGIC) { free(buf); return -1; }
    if (hdr.width == 0 || hdr.height == 0) { free(buf); return -1; }
    if (hdr.depth != REI_DEPTH_MONO && hdr.depth != REI_DEPTH_RGB && hdr.depth != REI_DEPTH_RGBA) {
        free(buf); return -1;
    }

    size_t expected = (size_t)hdr.width * (size_t)hdr.height * (size_t)hdr.depth;
    if (expected == 0 || expected > 320u * 200u * 4u) { free(buf); return -1; }

    uint8_t* pixels = (uint8_t*)malloc(expected);
    if (!pixels) { free(buf); return -1; }

    uint8_t comp = hdr.reserved1 & REI_COMP_MASK;
    const uint8_t* in_ptr = buf + sizeof(rei_header_t);
    size_t in_size = total - sizeof(rei_header_t);

    int rc = 0;
    if (comp == REI_COMP_NONE) {
        if (in_size < expected) rc = -1;
        else memcpy(pixels, in_ptr, expected);
    } else if (comp == REI_COMP_RLE) {
        rc = rei_decompress_rle(in_ptr, in_size, pixels, expected, hdr.depth);
    } else {
        rc = -1;
    }

    if (rc != 0) { free(pixels); free(buf); return -1; }

    for (int y = 0; y < TEX_H; ++y) {
        int sy = (int)((int64_t)y * hdr.height / TEX_H);
        if (sy >= hdr.height) sy = hdr.height - 1;
        for (int x = 0; x < TEX_W; ++x) {
            int sx = (int)((int64_t)x * hdr.width / TEX_W);
            if (sx >= hdr.width) sx = hdr.width - 1;
            size_t off = ((size_t)sy * hdr.width + (size_t)sx) * hdr.depth;
            uint8_t r = 0, g = 0, b = 0;
            if (hdr.depth == REI_DEPTH_MONO) {
                r = g = b = pixels[off];
            } else if (hdr.depth == REI_DEPTH_RGB) {
                r = pixels[off + 0];
                g = pixels[off + 1];
                b = pixels[off + 2];
            } else if (hdr.depth == REI_DEPTH_RGBA) {
                r = pixels[off + 0];
                g = pixels[off + 1];
                b = pixels[off + 2];
            }
            g_tex[y * TEX_W + x] = rgb565(r, g, b);
        }
    }

    free(pixels);
    free(buf);
    return 0;
}

static void update_stars(int speed) {
    for (int i = 0; i < STAR_COUNT; ++i) {
        uint16_t z = g_star_z[i];
        if (z > (uint16_t)speed) {
            z -= (uint16_t)speed;
        } else {
            z = (uint16_t)((rng_next() & 0xFFu) + 32u);
            g_star_x[i] = (int16_t)((int)(rng_next() & 0x1FFu) - 0x100);
            g_star_y[i] = (int16_t)((int)(rng_next() & 0xFFu) - 0x80);
        }
        g_star_z[i] = z;
    }
}

static void draw_stars(void) {
    for (int i = 0; i < STAR_COUNT; ++i) {
        int32_t z = (int32_t)g_star_z[i];
        int32_t sx = (FB_W / 2) + ((int32_t)g_star_x[i] * 256) / z;
        int32_t sy = (FB_H / 2) + ((int32_t)g_star_y[i] * 256) / z;
        if (sx < 0 || sx >= FB_W || sy < 0 || sy >= FB_H) continue;
        int bright = 255 - (z * 255) / 288;
        if (bright < 64) bright = 64;
        g_fb[sy * FB_W + sx] = rgb565((uint8_t)bright, (uint8_t)bright, (uint8_t)bright);
    }
}

static void buf_append_char(char* out, int cap, int* io_len, char ch) {
    if (!out || !io_len || cap <= 0) return;
    int len = *io_len;
    if (len < 0) len = 0;
    if (len + 1 >= cap) return;
    out[len++] = ch;
    out[len] = '\0';
    *io_len = len;
}

static void buf_append_str(char* out, int cap, int* io_len, const char* s) {
    if (!s) s = "";
    for (int i = 0; s[i]; ++i) buf_append_char(out, cap, io_len, s[i]);
}

static void buf_append_int(char* out, int cap, int* io_len, int v) {
    if (v == 0) {
        buf_append_char(out, cap, io_len, '0');
        return;
    }
    if (v < 0) {
        buf_append_char(out, cap, io_len, '-');
        unsigned uv = (unsigned)(-(v + 1)) + 1u;
        char tmp[16];
        int n = 0;
        while (uv && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (uv % 10u)); uv /= 10u; }
        for (int i = n - 1; i >= 0; --i) buf_append_char(out, cap, io_len, tmp[i]);
        return;
    }
    unsigned uv = (unsigned)v;
    char tmp[16];
    int n = 0;
    while (uv && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (uv % 10u)); uv /= 10u; }
    for (int i = n - 1; i >= 0; --i) buf_append_char(out, cap, io_len, tmp[i]);
}

static void fb_clear(uint16_t col) {
    for (int y = 0; y < FB_H; ++y) {
        uint16_t* row = &g_fb[y * FB_W];
        for (int x = 0; x < FB_W; ++x) row[x] = col;
    }
}

static void zb_clear(int32_t depth) {
    for (int y = 0; y < FB_H; ++y) {
        int32_t* row = &g_zb[y * FB_W];
        for (int x = 0; x < FB_W; ++x) row[x] = depth;
    }
}

static void draw_raster_bars_fb(int phase, int palette) {
    for (int y = 0; y < FB_H; y += 6) {
        int idx = (phase + y * 3) & 255;
        int32_t v = sin_table[idx];
        int32_t scaled = (int32_t)(((int64_t)(v + 65536) * 255) / 131072);
        int c = (int)scaled;
        uint8_t r = 0, g = 0, b = 0;
        switch (palette & 3) {
            case 0:
                r = clamp_u8(c); g = clamp_u8(80 + (c / 2)); b = clamp_u8(255 - c);
                break;
            case 1:
                r = clamp_u8(255 - c); g = clamp_u8(c); b = clamp_u8(80 + (c / 2));
                break;
            case 2:
                r = clamp_u8(80 + (c / 2)); g = clamp_u8(255 - c); b = clamp_u8(c);
                break;
            default:
                r = clamp_u8(c / 2); g = clamp_u8(120 + (c / 3)); b = clamp_u8(200 + (c / 4));
                break;
        }
        uint16_t col = rgb565(r, g, b);
        int y2 = y + 3;
        if (y2 > FB_H) y2 = FB_H;
        for (int yy = y; yy < y2; ++yy) {
            uint16_t* row = &g_fb[yy * FB_W];
            for (int xx = 0; xx < FB_W; ++xx) row[xx] = col;
        }
    }
}

static void fb_span(int y, int x0, int x1, uint16_t col) {
    if (y < 0 || y >= FB_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= FB_W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= FB_W) x1 = FB_W - 1;
    uint16_t* row = &g_fb[y * FB_W];
    for (int x = x0; x <= x1; ++x) row[x] = col;
}

typedef struct {
    int x;
    int y;
    int32_t uoz;
    int32_t voz;
    int32_t invz;
} tex_vert_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t u;
    int32_t v;
} clip_vert_t;

static int clip_poly_near_z(const clip_vert_t* in, int in_count, clip_vert_t* out, int32_t near_z) {
    if (!in || !out || in_count <= 0) return 0;
    int out_count = 0;
    for (int i = 0; i < in_count; ++i) {
        const clip_vert_t s = in[i];
        const clip_vert_t e = in[(i + 1) % in_count];
        int s_in = (s.z >= near_z);
        int e_in = (e.z >= near_z);

        if (e_in) {
            if (!s_in) {
                int32_t dz = e.z - s.z;
                if (dz != 0) {
                    int32_t t_q16 = (int32_t)(((int64_t)(near_z - s.z) << 16) / dz);
                    clip_vert_t ix;
                    ix.z = near_z;
                    ix.x = s.x + (int32_t)(((int64_t)(e.x - s.x) * t_q16) >> 16);
                    ix.y = s.y + (int32_t)(((int64_t)(e.y - s.y) * t_q16) >> 16);
                    ix.u = s.u + (int32_t)(((int64_t)(e.u - s.u) * t_q16) >> 16);
                    ix.v = s.v + (int32_t)(((int64_t)(e.v - s.v) * t_q16) >> 16);
                    out[out_count++] = ix;
                }
            }
            out[out_count++] = e;
        } else {
            if (s_in) {
                int32_t dz = e.z - s.z;
                if (dz != 0) {
                    int32_t t_q16 = (int32_t)(((int64_t)(near_z - s.z) << 16) / dz);
                    clip_vert_t ix;
                    ix.z = near_z;
                    ix.x = s.x + (int32_t)(((int64_t)(e.x - s.x) * t_q16) >> 16);
                    ix.y = s.y + (int32_t)(((int64_t)(e.y - s.y) * t_q16) >> 16);
                    ix.u = s.u + (int32_t)(((int64_t)(e.u - s.u) * t_q16) >> 16);
                    ix.v = s.v + (int32_t)(((int64_t)(e.v - s.v) * t_q16) >> 16);
                    out[out_count++] = ix;
                }
            }
        }

        if (out_count >= 8) break;
    }
    return out_count;
}

static inline uint16_t tex_sample_bilinear_q16(int32_t uu_q16, int32_t vv_q16) {
    int tu = uu_q16 >> 16;
    int tv = vv_q16 >> 16;
    int fu = (uu_q16 >> 8) & 0xFF;
    int fv = (vv_q16 >> 8) & 0xFF;
    if (tu < 0) { tu = 0; fu = 0; }
    if (tv < 0) { tv = 0; fv = 0; }
    if (tu >= TEX_W - 1) { tu = TEX_W - 2; fu = 255; }
    if (tv >= TEX_H - 1) { tv = TEX_H - 2; fv = 255; }
    uint16_t c00 = g_tex[tv * TEX_W + tu];
    uint16_t c10 = g_tex[tv * TEX_W + (tu + 1)];
    uint16_t c01 = g_tex[(tv + 1) * TEX_W + tu];
    uint16_t c11 = g_tex[(tv + 1) * TEX_W + (tu + 1)];

    int r00 = (c00 >> 11) & 31;
    int g00 = (c00 >> 5) & 63;
    int b00 = c00 & 31;
    int r10 = (c10 >> 11) & 31;
    int g10 = (c10 >> 5) & 63;
    int b10 = c10 & 31;
    int r01 = (c01 >> 11) & 31;
    int g01 = (c01 >> 5) & 63;
    int b01 = c01 & 31;
    int r11 = (c11 >> 11) & 31;
    int g11 = (c11 >> 5) & 63;
    int b11 = c11 & 31;

    int r0 = r00 + (((r10 - r00) * fu) >> 8);
    int g0 = g00 + (((g10 - g00) * fu) >> 8);
    int b0 = b00 + (((b10 - b00) * fu) >> 8);
    int r1 = r01 + (((r11 - r01) * fu) >> 8);
    int g1 = g01 + (((g11 - g01) * fu) >> 8);
    int b1 = b01 + (((b11 - b01) * fu) >> 8);

    int r = r0 + (((r1 - r0) * fv) >> 8);
    int g = g0 + (((g1 - g0) * fv) >> 8);
    int b = b0 + (((b1 - b0) * fv) >> 8);

    if (r < 0) r = 0;
    if (r > 31) r = 31;
    if (g < 0) g = 0;
    if (g > 63) g = 63;
    if (b < 0) b = 0;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void tex_span(int y, int x0, int x1,
                     int32_t uoz0, int32_t voz0, int32_t invz0,
                     int32_t uoz1, int32_t voz1, int32_t invz1) {
    if (y < 0 || y >= FB_H) return;
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
        int32_t tu = uoz0; uoz0 = uoz1; uoz1 = tu;
        int32_t tv = voz0; voz0 = voz1; voz1 = tv;
        int32_t tz = invz0; invz0 = invz1; invz1 = tz;
    }
    if (x1 < 0 || x0 >= FB_W) return;
    int dx = x1 - x0;
    if (dx == 0) {
        if (x0 < 0 || x0 >= FB_W) return;
        int32_t iz = invz0;
        if (iz > 0) {
            int32_t* zrow = &g_zb[y * FB_W];
            if (iz > zrow[x0]) {
                int32_t uu = (int32_t)(((int64_t)uoz0 << 16) / iz);
                int32_t vv = (int32_t)(((int64_t)voz0 << 16) / iz);
                g_fb[y * FB_W + x0] = tex_sample_bilinear_q16(uu, vv);
                zrow[x0] = iz;
            }
        }
        return;
    }

    int32_t du = (int32_t)(((int64_t)(uoz1 - uoz0)) / dx);
    int32_t dv = (int32_t)(((int64_t)(voz1 - voz0)) / dx);
    int32_t dz = (int32_t)(((int64_t)(invz1 - invz0)) / dx);

    int clip_left = 0;
    if (x0 < 0) {
        clip_left = -x0;
        x0 = 0;
    }
    if (x1 >= FB_W) x1 = FB_W - 1;

    int32_t u = uoz0 + (du * clip_left);
    int32_t v = voz0 + (dv * clip_left);
    int32_t iz = invz0 + (dz * clip_left);

    uint16_t* row = &g_fb[y * FB_W];
    int32_t* zrow = &g_zb[y * FB_W];
    for (int x = x0; x <= x1; ++x) {
        if (iz > 0) {
            if (iz > zrow[x]) {
                int32_t uu = (int32_t)(((int64_t)u << 16) / iz);
                int32_t vv = (int32_t)(((int64_t)v << 16) / iz);
                row[x] = tex_sample_bilinear_q16(uu, vv);
                zrow[x] = iz;
            }
        }
        u += du;
        v += dv;
        iz += dz;
    }
}

static void draw_rotozoom(int t) {
    int32_t ang = (t << 6) & ((256 << 8) - 1);
    int32_t s = sin_q16_interp(ang);
    int32_t c = sin_q16_interp(ang + (64 << 8));
    int32_t scale = 32768 + ((sin_q16_interp((t << 5) & ((256 << 8) - 1)) + 65536) >> 2); // ~0.5..1.0 in Q16

    int32_t dux = mul_q16(c, scale);
    int32_t dvx = mul_q16(s, scale);
    int32_t duy = -mul_q16(s, scale);
    int32_t dvy = mul_q16(c, scale);

    int32_t cx = FB_W / 2;
    int32_t cy = FB_H / 2;
    int32_t uoff = (t << 12);
    int32_t voff = (t << 11);

    for (int y = 0; y < FB_H; ++y) {
        int32_t dy = (y - cy) << 16;
        int32_t u_row = uoff + mul_q16(duy, dy) - mul_q16(dux, (cx << 16));
        int32_t v_row = voff + mul_q16(dvy, dy) - mul_q16(dvx, (cx << 16));
        uint16_t* row = &g_fb[y * FB_W];
        for (int x = 0; x < FB_W; ++x) {
            int tu = (u_row >> 16) & (TEX_W - 1);
            int tv = (v_row >> 16) & (TEX_H - 1);
            row[x] = g_tex[tv * TEX_W + tu];
            u_row += dux;
            v_row += dvx;
        }
    }
}

static void draw_tex_flat_bottom(tex_vert_t v0, tex_vert_t v1, tex_vert_t v2) {
    int dy = v1.y - v0.y;
    if (dy <= 0) return;
    int32_t dx1 = (int32_t)(((int64_t)(v1.x - v0.x) << 16) / dy);
    int32_t dx2 = (int32_t)(((int64_t)(v2.x - v0.x) << 16) / dy);
    int32_t du1 = (int32_t)(((int64_t)(v1.uoz - v0.uoz)) / dy);
    int32_t dv1 = (int32_t)(((int64_t)(v1.voz - v0.voz)) / dy);
    int32_t dz1 = (int32_t)(((int64_t)(v1.invz - v0.invz)) / dy);
    int32_t du2 = (int32_t)(((int64_t)(v2.uoz - v0.uoz)) / dy);
    int32_t dv2 = (int32_t)(((int64_t)(v2.voz - v0.voz)) / dy);
    int32_t dz2 = (int32_t)(((int64_t)(v2.invz - v0.invz)) / dy);

    int32_t sx1 = v0.x << 16;
    int32_t sx2 = v0.x << 16;
    int32_t u1 = v0.uoz;
    int32_t v1v = v0.voz;
    int32_t z1 = v0.invz;
    int32_t u2 = v0.uoz;
    int32_t v2v = v0.voz;
    int32_t z2 = v0.invz;

    for (int y = v0.y; y <= v1.y; ++y) {
        tex_span(y, sx1 >> 16, sx2 >> 16, u1, v1v, z1, u2, v2v, z2);
        sx1 += dx1; sx2 += dx2;
        u1 += du1; v1v += dv1; z1 += dz1;
        u2 += du2; v2v += dv2; z2 += dz2;
    }
}

static void draw_tex_flat_top(tex_vert_t v0, tex_vert_t v1, tex_vert_t v2) {
    int dy = v2.y - v0.y;
    if (dy <= 0) return;
    int32_t dx1 = (int32_t)(((int64_t)(v2.x - v0.x) << 16) / dy);
    int32_t dx2 = (int32_t)(((int64_t)(v2.x - v1.x) << 16) / dy);
    int32_t du1 = (int32_t)(((int64_t)(v2.uoz - v0.uoz)) / dy);
    int32_t dv1 = (int32_t)(((int64_t)(v2.voz - v0.voz)) / dy);
    int32_t dz1 = (int32_t)(((int64_t)(v2.invz - v0.invz)) / dy);
    int32_t du2 = (int32_t)(((int64_t)(v2.uoz - v1.uoz)) / dy);
    int32_t dv2 = (int32_t)(((int64_t)(v2.voz - v1.voz)) / dy);
    int32_t dz2 = (int32_t)(((int64_t)(v2.invz - v1.invz)) / dy);

    int32_t sx1 = v0.x << 16;
    int32_t sx2 = v1.x << 16;
    int32_t u1 = v0.uoz;
    int32_t v1v = v0.voz;
    int32_t z1 = v0.invz;
    int32_t u2 = v1.uoz;
    int32_t v2v = v1.voz;
    int32_t z2 = v1.invz;

    for (int y = v0.y; y <= v2.y; ++y) {
        tex_span(y, sx1 >> 16, sx2 >> 16, u1, v1v, z1, u2, v2v, z2);
        sx1 += dx1; sx2 += dx2;
        u1 += du1; v1v += dv1; z1 += dz1;
        u2 += du2; v2v += dv2; z2 += dz2;
    }
}

static void draw_tex_triangle(tex_vert_t v0, tex_vert_t v1, tex_vert_t v2) {
    if (v0.y > v1.y) { tex_vert_t t = v0; v0 = v1; v1 = t; }
    if (v1.y > v2.y) { tex_vert_t t = v1; v1 = v2; v2 = t; }
    if (v0.y > v1.y) { tex_vert_t t = v0; v0 = v1; v1 = t; }

    if (v1.y == v2.y) {
        draw_tex_flat_bottom(v0, v1, v2);
        return;
    }
    if (v0.y == v1.y) {
        draw_tex_flat_top(v0, v1, v2);
        return;
    }

    int dy = v2.y - v0.y;
    if (dy == 0) return;
    int32_t t_q16 = (int32_t)(((int64_t)(v1.y - v0.y) << 16) / dy);
    tex_vert_t v3 = v0;
    v3.y = v1.y;
    v3.x = v0.x + (int)(((int64_t)(v2.x - v0.x) * t_q16) >> 16);
    v3.uoz = v0.uoz + (int32_t)(((int64_t)(v2.uoz - v0.uoz) * t_q16) >> 16);
    v3.voz = v0.voz + (int32_t)(((int64_t)(v2.voz - v0.voz) * t_q16) >> 16);
    v3.invz = v0.invz + (int32_t)(((int64_t)(v2.invz - v0.invz) * t_q16) >> 16);

    draw_tex_flat_bottom(v0, v1, v3);
    draw_tex_flat_top(v1, v3, v2);
}

static void fill_flat_bottom(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
    (void)y2;
    int dy = y1 - y0;
    if (dy <= 0) return;
    int32_t dx1 = ((x1 - x0) << 16) / dy;
    int32_t dx2 = ((x2 - x0) << 16) / dy;
    int32_t sx1 = x0 << 16;
    int32_t sx2 = x0 << 16;
    for (int y = y0; y <= y1; ++y) {
        fb_span(y, sx1 >> 16, sx2 >> 16, col);
        sx1 += dx1;
        sx2 += dx2;
    }
}

static void fill_flat_top(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
    (void)y1;
    int dy = y2 - y0;
    if (dy <= 0) return;
    int32_t dx1 = ((x2 - x0) << 16) / dy;
    int32_t dx2 = ((x2 - x1) << 16) / dy;
    int32_t sx1 = x0 << 16;
    int32_t sx2 = x1 << 16;
    for (int y = y0; y <= y2; ++y) {
        fb_span(y, sx1 >> 16, sx2 >> 16, col);
        sx1 += dx1;
        sx2 += dx2;
    }
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

    if (y1 == y2) {
        fill_flat_bottom(x0, y0, x1, y1, x2, y2, col);
        return;
    }
    if (y0 == y1) {
        fill_flat_top(x0, y0, x1, y1, x2, y2, col);
        return;
    }

    int32_t t_q16 = ((y1 - y0) << 16) / (y2 - y0);
    int x3 = x0 + (int)(((int64_t)(x2 - x0) * t_q16) >> 16);
    int y3 = y1;

    fill_flat_bottom(x0, y0, x1, y1, x3, y3, col);
    fill_flat_top(x1, y1, x3, y3, x2, y2, col);
}

typedef struct {
    int32_t x, y, z;
} vec3i_t;

static void rotate_to_camera(int32_t x, int32_t y, int32_t z,
                             int32_t sinx, int32_t cosx,
                             int32_t siny, int32_t cosy,
                             int32_t sinz, int32_t cosz,
                             int32_t zoff,
                             int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    // Full rotation matrix R = Rz * Ry * Rx (Q16.16)
    // m00 = cz*cy
    int32_t m00 = mul_q16(cosz, cosy);
    // m01 = cz*sy*sx - sz*cx
    int32_t m01 = mul_q16(cosz, mul_q16(siny, sinx)) - mul_q16(sinz, cosx);
    // m02 = cz*sy*cx + sz*sx
    int32_t m02 = mul_q16(cosz, mul_q16(siny, cosx)) + mul_q16(sinz, sinx);

    // m10 = sz*cy
    int32_t m10 = mul_q16(sinz, cosy);
    // m11 = sz*sy*sx + cz*cx
    int32_t m11 = mul_q16(sinz, mul_q16(siny, sinx)) + mul_q16(cosz, cosx);
    // m12 = sz*sy*cx - cz*sx
    int32_t m12 = mul_q16(sinz, mul_q16(siny, cosx)) - mul_q16(cosz, sinx);

    // m20 = -sy
    int32_t m20 = -siny;
    // m21 = cy*sx
    int32_t m21 = mul_q16(cosy, sinx);
    // m22 = cy*cx
    int32_t m22 = mul_q16(cosy, cosx);

    int32_t x2 = (int32_t)(((int64_t)m00 * x + (int64_t)m01 * y + (int64_t)m02 * z) >> 16);
    int32_t y2 = (int32_t)(((int64_t)m10 * x + (int64_t)m11 * y + (int64_t)m12 * z) >> 16);
    int32_t z2 = (int32_t)(((int64_t)m20 * x + (int64_t)m21 * y + (int64_t)m22 * z) >> 16);

    *out_x = x2;
    *out_y = y2;
    *out_z = z2 + zoff;
}

static void project_point(int32_t x, int32_t y, int32_t z,
                          int32_t proj, int32_t cx, int32_t cy,
                          int32_t* out_x, int32_t* out_y) {
    if (z < 1) z = 1;
    *out_x = cx + (int32_t)(((int64_t)x * proj) / z);
    *out_y = cy + (int32_t)(((int64_t)y * proj) / z);
}

static void draw_cube_faces(int ang_x_q8, int ang_y_q8) {
    const int32_t sinx = sin_q16_interp(ang_x_q8);
    const int32_t cosx = sin_q16_interp(ang_x_q8 + (64 << 8));
    const int32_t siny = sin_q16_interp(ang_y_q8);
    const int32_t cosy = sin_q16_interp(ang_y_q8 + (64 << 8));
    const int32_t sinz = sin_q16_interp(ang_x_q8 + ang_y_q8);
    const int32_t cosz = sin_q16_interp(ang_x_q8 + ang_y_q8 + (64 << 8));

    const int32_t size = 80;
    const int32_t zoff = 260;
    const int32_t proj = 140;
    const int32_t near_z = 64;

    const vec3i_t v[8] = {
        {-size, -size, -size},
        { size, -size, -size},
        { size,  size, -size},
        {-size,  size, -size},
        {-size, -size,  size},
        { size, -size,  size},
        { size,  size,  size},
        {-size,  size,  size},
    };

    int32_t camx[8];
    int32_t camy[8];
    int32_t camz[8];
    int32_t sx[8];
    int32_t sy[8];
    int32_t cx = FB_W / 2;
    int32_t cy = FB_H / 2;

    for (int i = 0; i < 8; ++i) {
        rotate_to_camera(v[i].x, v[i].y, v[i].z,
                         sinx, cosx, siny, cosy, sinz, cosz,
                         zoff,
                         &camx[i], &camy[i], &camz[i]);
            int32_t zc = camz[i];
            if (zc < near_z) zc = near_z;
            project_point(camx[i], camy[i], zc, proj, cx, cy, &sx[i], &sy[i]);
    }

    const int faces[6][4] = {
        {4,5,6,7}, // +Z
        {0,3,2,1}, // -Z
        {0,4,7,3}, // -X
        {1,2,6,5}, // +X
        {3,7,6,2}, // +Y
        {0,1,5,4}, // -Y
    };

    const uint16_t face_col[6] = {
        0xFFFF, // white
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFE0, // yellow
        0xF81F, // magenta
    };

    int order[6] = {0,1,2,3,4,5};
    int32_t zavg[6];
    for (int i = 0; i < 6; ++i) {
        int a = faces[i][0];
        int b = faces[i][1];
        int c = faces[i][2];
        int d = faces[i][3];
        zavg[i] = (camz[a] + camz[b] + camz[c] + camz[d]) / 4;
    }
    // Simple painter sort (far to near)
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            if (zavg[order[i]] < zavg[order[j]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    for (int oi = 0; oi < 6; ++oi) {
        int fi = order[oi];
        int a = faces[fi][0];
        int b = faces[fi][1];
        int c = faces[fi][2];
        int d = faces[fi][3];

        int32_t ax = camx[a], ay = camy[a], az = camz[a];
        int32_t bx = camx[b], by = camy[b], bz = camz[b];
        int32_t cx = camx[c], cy = camy[c], cz = camz[c];

        int64_t abx = (int64_t)bx - ax;
        int64_t aby = (int64_t)by - ay;
        int64_t abz = (int64_t)bz - az;
        int64_t acx = (int64_t)cx - ax;
        int64_t acy = (int64_t)cy - ay;
        int64_t acz = (int64_t)cz - az;
        int64_t nz = abx * acy - aby * acx;
        (void)abz;
        (void)acz;
            if (nz >= 0) continue; // backface cull (camera looks +Z)

        uint16_t col = face_col[fi];
        fill_triangle(sx[a], sy[a], sx[b], sy[b], sx[c], sy[c], col);
        fill_triangle(sx[a], sy[a], sx[c], sy[c], sx[d], sy[d], col);
    }
}

static void draw_textured_cube(int ang_x_q8, int ang_y_q8) {
    const int32_t sinx = sin_q16_interp(ang_x_q8);
    const int32_t cosx = sin_q16_interp(ang_x_q8 + (64 << 8));
    const int32_t siny = sin_q16_interp(ang_y_q8);
    const int32_t cosy = sin_q16_interp(ang_y_q8 + (64 << 8));
    const int32_t sinz = sin_q16_interp(ang_x_q8 + ang_y_q8);
    const int32_t cosz = sin_q16_interp(ang_x_q8 + ang_y_q8 + (64 << 8));

    const int32_t size = 90;
    const int32_t zoff = 280;
    const int32_t proj = 140;
    const int32_t near_z = 64;

    const vec3i_t v[8] = {
        {-size, -size, -size},
        { size, -size, -size},
        { size,  size, -size},
        {-size,  size, -size},
        {-size, -size,  size},
        { size, -size,  size},
        { size,  size,  size},
        {-size,  size,  size},
    };

    int32_t camx[8];
    int32_t camy[8];
    int32_t camz[8];
    int32_t cx = FB_W / 2;
    int32_t cy = FB_H / 2;

    for (int i = 0; i < 8; ++i) {
        rotate_to_camera(v[i].x, v[i].y, v[i].z,
                         sinx, cosx, siny, cosy, sinz, cosz,
                         zoff,
                         &camx[i], &camy[i], &camz[i]);
    }

    const int faces[6][4] = {
        {4,5,6,7}, // +Z
        {0,3,2,1}, // -Z
        {0,4,7,3}, // -X
        {1,2,6,5}, // +X
        {3,7,6,2}, // +Y
        {0,1,5,4}, // -Y
    };

    int order[6] = {0,1,2,3,4,5};
    int32_t zavg[6];
    for (int i = 0; i < 6; ++i) {
        int a = faces[i][0];
        int b = faces[i][1];
        int c = faces[i][2];
        int d = faces[i][3];
        zavg[i] = (camz[a] + camz[b] + camz[c] + camz[d]) / 4;
    }
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            if (zavg[order[i]] < zavg[order[j]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    const int u0 = 0, v0 = 0, u1 = TEX_W - 1, v1 = TEX_H - 1;

    for (int oi = 0; oi < 6; ++oi) {
        int fi = order[oi];
        int a = faces[fi][0];
        int b = faces[fi][1];
        int c = faces[fi][2];
        int d = faces[fi][3];

        int32_t ax = camx[a], ay = camy[a];
        int32_t bx = camx[b], by = camy[b];
        int32_t cxv = camx[c], cyv = camy[c];

        int64_t abx = (int64_t)bx - ax;
        int64_t aby = (int64_t)by - ay;
        int64_t acx = (int64_t)cxv - ax;
        int64_t acy = (int64_t)cyv - ay;
        int64_t nz = abx * acy - aby * acx;
        if (nz >= 0) continue;

        // Build face quad in camera space with UVs (Q16.16), then clip against near plane.
        clip_vert_t quad[4];
        quad[0].x = camx[a]; quad[0].y = camy[a]; quad[0].z = camz[a]; quad[0].u = (int32_t)u0 << 16; quad[0].v = (int32_t)v0 << 16;
        quad[1].x = camx[b]; quad[1].y = camy[b]; quad[1].z = camz[b]; quad[1].u = (int32_t)u1 << 16; quad[1].v = (int32_t)v0 << 16;
        quad[2].x = camx[c]; quad[2].y = camy[c]; quad[2].z = camz[c]; quad[2].u = (int32_t)u1 << 16; quad[2].v = (int32_t)v1 << 16;
        quad[3].x = camx[d]; quad[3].y = camy[d]; quad[3].z = camz[d]; quad[3].u = (int32_t)u0 << 16; quad[3].v = (int32_t)v1 << 16;

        clip_vert_t clipped[8];
        int n = clip_poly_near_z(quad, 4, clipped, near_z);
        if (n < 3) continue;

        // Triangulate the clipped polygon as a fan.
        for (int i = 1; i + 1 < n; ++i) {
            const clip_vert_t p0 = clipped[0];
            const clip_vert_t p1 = clipped[i];
            const clip_vert_t p2 = clipped[i + 1];

            int32_t sx0, sy0, sx1, sy1, sx2, sy2;
            project_point(p0.x, p0.y, p0.z, proj, cx, cy, &sx0, &sy0);
            project_point(p1.x, p1.y, p1.z, proj, cx, cy, &sx1, &sy1);
            project_point(p2.x, p2.y, p2.z, proj, cx, cy, &sx2, &sy2);

            tex_vert_t t0;
            tex_vert_t t1;
            tex_vert_t t2;
            t0.x = (int)sx0; t0.y = (int)sy0;
            t1.x = (int)sx1; t1.y = (int)sy1;
            t2.x = (int)sx2; t2.y = (int)sy2;

            t0.invz = (int32_t)(((int64_t)1 << 16) / p0.z);
            t1.invz = (int32_t)(((int64_t)1 << 16) / p1.z);
            t2.invz = (int32_t)(((int64_t)1 << 16) / p2.z);

            t0.uoz = (int32_t)(((int64_t)p0.u * t0.invz) >> 16);
            t0.voz = (int32_t)(((int64_t)p0.v * t0.invz) >> 16);
            t1.uoz = (int32_t)(((int64_t)p1.u * t1.invz) >> 16);
            t1.voz = (int32_t)(((int64_t)p1.v * t1.invz) >> 16);
            t2.uoz = (int32_t)(((int64_t)p2.u * t2.invz) >> 16);
            t2.voz = (int32_t)(((int64_t)p2.v * t2.invz) >> 16);

            draw_tex_triangle(t0, t1, t2);
        }
    }
}

static void draw_pyramid_faces(int ang_x_q8, int ang_y_q8) {
    const int32_t sinx = sin_q16_interp(ang_x_q8);
    const int32_t cosx = sin_q16_interp(ang_x_q8 + (64 << 8));
    const int32_t siny = sin_q16_interp(ang_y_q8);
    const int32_t cosy = sin_q16_interp(ang_y_q8 + (64 << 8));
    const int32_t sinz = sin_q16_interp(ang_x_q8 + ang_y_q8);
    const int32_t cosz = sin_q16_interp(ang_x_q8 + ang_y_q8 + (64 << 8));

    const int32_t size = 90;
    const int32_t zoff = 260;
    const int32_t proj = 140;
    const int32_t near_z = 64;

    const vec3i_t v[5] = {
        {-size, -size, -size},
        { size, -size, -size},
        { size, -size,  size},
        {-size, -size,  size},
        { 0,     size,   0},
    };

    int32_t camx[5];
    int32_t camy[5];
    int32_t camz[5];
    int32_t sx[5];
    int32_t sy[5];
    int32_t cx = FB_W / 2;
    int32_t cy = FB_H / 2;

    for (int i = 0; i < 5; ++i) {
        rotate_to_camera(v[i].x, v[i].y, v[i].z,
                         sinx, cosx, siny, cosy, sinz, cosz,
                         zoff,
                         &camx[i], &camy[i], &camz[i]);
        int32_t zc = camz[i];
        if (zc < near_z) zc = near_z;
        project_point(camx[i], camy[i], zc, proj, cx, cy, &sx[i], &sy[i]);
    }

    const int faces[6][3] = {
        {0, 1, 4},
        {1, 2, 4},
        {2, 3, 4},
        {3, 0, 4},
        {0, 3, 2},
        {0, 2, 1},
    };
    const uint16_t face_col[6] = { 0xFFE0, 0x07E0, 0xF800, 0x001F, 0xFFFF, 0xFFFF };

    int order[6] = {0,1,2,3,4,5};
    int32_t zavg[6];
    for (int i = 0; i < 6; ++i) {
        int a = faces[i][0];
        int b = faces[i][1];
        int c = faces[i][2];
        zavg[i] = (camz[a] + camz[b] + camz[c]) / 3;
    }
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            if (zavg[order[i]] < zavg[order[j]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    for (int oi = 0; oi < 6; ++oi) {
        int fi = order[oi];
        int a = faces[fi][0];
        int b = faces[fi][1];
        int c = faces[fi][2];

        // For the pyramid, disable backface culling to avoid missing front faces
        // during sharp rotations with coarse clipping.
        fill_triangle(sx[a], sy[a], sx[b], sy[b], sx[c], sy[c], face_col[fi]);
        }
    }

static int scene_index_from_frame(int frame) {
    if (frame < 600) return 0;
    if (frame < 1200) return 1;
    if (frame < 1800) return 2;
    if (frame < 2400) return 3;
    return 4;
}

static int scene_start_frame(int scene) {
    switch (scene % 5) {
        case 0: return 0;
        case 1: return 600;
        case 2: return 1200;
        case 3: return 1800;
        default: return 2400;
    }
}

int main(void) {
    init_stars();
    init_texture();
    if (load_rei_texture("/icons16/file_rei.rei") != 0) {
        (void)load_rei_texture("/eynos.rei");
    }
    if (gui_attach("Second Reality Demo", "q quits | space pause | arrows scene") < 0) {
        puts("gui_attach failed");
        return 1;
    }

    int handle = 0;
    (void)gui_set_continuous_redraw(handle, 1);
    int phase = 0;
    int32_t ang_x = 0;
    int32_t ang_y = 0;
    int frame = 0;
    int last_w = 320;
    int last_h = 200;
    int paused = 0;
    const int32_t ang_mask = (256 << 8) - 1;

    for (;;) {
        gui_event_t ev;
        while (gui_poll_event(handle, &ev) > 0) {
            if (ev.type == GUI_EVENT_KEY) {
                int key = ev.a & 0xFFFF;
                unsigned ch = (unsigned)key & 0xFFu;
                if (ch == (unsigned)'q' || ch == (unsigned)'Q') {
                    return 0;
                }
                if (ch == (unsigned)' ') {
                    paused = !paused;
                }
                if (key == 0x1003 || key == 0x1004) {
                    int scene = scene_index_from_frame(frame);
                    scene = (scene + (key == 0x1004 ? 1 : -1) + 5) % 5;
                    frame = scene_start_frame(scene);
                    phase = 0;
                    ang_x = 0;
                    ang_y = 0;
                }
            }
        }

        (void)gui_begin(handle);

        gui_size_t sz = {0, 0};
        (void)gui_get_content_size(handle, &sz);
        int w = sz.w > 0 ? sz.w : last_w;
        int h = sz.h > 0 ? sz.h : last_h;
        if (w <= 0) w = 320;
        if (h <= 0) h = 200;
        last_w = w;
        last_h = h;

        int palette = (frame / 60) & 3;
        if (frame < 600) {
            fb_clear(rgb565(0, 0, 0));
            draw_raster_bars_fb(phase + (frame & 255), palette);
            draw_cube_faces(ang_x, ang_y);
        } else if (frame < 1200) {
            fb_clear(rgb565(0, 0, 0));
            update_stars(3);
            draw_stars();
            draw_cube_faces(ang_x + (40 << 8), ang_y + (80 << 8));
        } else if (frame < 1800) {
            fb_clear(rgb565(0, 0, 0));
            update_stars(4);
            draw_stars();
            draw_pyramid_faces(ang_x + (90 << 8), ang_y + (30 << 8));
        } else if (frame < 2400) {
            fb_clear(rgb565(0, 0, 0));
            zb_clear(0);
            update_stars(2);
            draw_stars();
            draw_textured_cube(ang_x + (30 << 8), ang_y + (10 << 8));
        } else {
            fb_clear(rgb565(0, 0, 0));
            draw_rotozoom(frame);
        }

        gui_blit_rgb565_t blit;
        blit.src_w = FB_W;
        blit.src_h = FB_H;
        blit.pixels = g_fb;
        blit.dst_w = 0;
        blit.dst_h = 0;
        (void)gui_blit_rgb565(handle, &blit);

        char line3[96];
        int l3 = 0;
        line3[0] = '\0';
        buf_append_str(line3, (int)sizeof(line3), &l3, "frame ");
        buf_append_int(line3, (int)sizeof(line3), &l3, frame);
        buf_append_str(line3, (int)sizeof(line3), &l3, "  ");
        buf_append_str(line3, (int)sizeof(line3), &l3, DEMO_BUILD_ID);

        gui_text_t t1 = { .x = 8, .y = 4, .r = 255, .g = 255, .b = 255, ._pad = 0, .text = "Second Reality demo (step 5)" };
        const char* scene_text = "";
        int scene_now = scene_index_from_frame(frame);
        if (scene_now == 0) scene_text = "Raster bars + cube";
        else if (scene_now == 1) scene_text = "Starfield + cube";
        else if (scene_now == 2) scene_text = "Starfield + pyramid";
        else if (scene_now == 3) scene_text = "Textured cube (zbuf + bilinear)";
        else scene_text = "Rotozoom";
        gui_text_t t2 = { .x = 8, .y = 18, .r = 220, .g = 220, .b = 0, ._pad = 0, .text = scene_text };
        gui_text_t t3 = { .x = 8, .y = 32, .r = 200, .g = 200, .b = 200, ._pad = 0, .text = line3 };
        (void)gui_draw_text(handle, &t1);
        (void)gui_draw_text(handle, &t2);
        (void)gui_draw_text(handle, &t3);

        (void)gui_present(handle);

        if (!paused) {
            phase = (phase + 2) & 255;
            ang_x = (ang_x + (1 << 8)) & ang_mask;
            ang_y = (ang_y + (2 << 8)) & ang_mask;
            frame++;
        }

        // Yield so the UI/tiler can repaint even without input events.
        usleep(16000);
    }
}
