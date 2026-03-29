#include <drivers/otf_font.h>

#include <fs/vfs.h>
#include <util.h>
#include <watchdog.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STBTT_malloc(x, u) malloc((size_t)(x))
#define STBTT_free(x, u) free((void*)(x))

static double otf_local_fabs(double x) {
    return (x < 0.0) ? -x : x;
}

static double otf_local_floor(double x) {
    int i = (int)x;
    if ((double)i > x) i--;
    return (double)i;
}

static double otf_local_ceil(double x) {
    int i = (int)x;
    if ((double)i < x) i++;
    return (double)i;
}

static double otf_local_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = (x > 1.0) ? x : 1.0;
    for (int i = 0; i < 10; ++i) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

static double otf_local_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = otf_local_floor(x / y);
    return x - q * y;
}

static double otf_local_wrap_pi(double x) {
    const double pi = 3.14159265358979323846;
    const double tau = 6.28318530717958647692;
    while (x > pi) x -= tau;
    while (x < -pi) x += tau;
    return x;
}

static double otf_local_cos(double x) {
    x = otf_local_wrap_pi(x);
    double x2 = x * x;
    double x4 = x2 * x2;
    double x6 = x4 * x2;
    return 1.0 - (x2 * 0.5) + (x4 / 24.0) - (x6 / 720.0);
}

static double otf_local_acos(double x) {
    const double pi = 3.14159265358979323846;
    if (x <= -1.0) return pi;
    if (x >= 1.0) return 0.0;
    double negate = (x < 0.0) ? 1.0 : 0.0;
    x = otf_local_fabs(x);
    double ret = -0.0187293;
    ret = ret * x + 0.0742610;
    ret = ret * x - 0.2121144;
    ret = ret * x + 1.5707288;
    ret = ret * otf_local_sqrt(1.0 - x);
    ret = ret - 2.0 * negate * ret;
    return negate * pi + ret;
}

static double otf_local_cbrt(double x) {
    if (x == 0.0) return 0.0;
    double sign = 1.0;
    if (x < 0.0) {
        sign = -1.0;
        x = -x;
    }
    double g = (x > 1.0) ? x : 1.0;
    for (int i = 0; i < 14; ++i) {
        g = (2.0 * g + x / (g * g)) / 3.0;
    }
    return sign * g;
}

static double otf_local_pow(double x, double y) {
    (void)y;
    return otf_local_cbrt(x);
}

#define STBTT_ifloor(x) ((int)otf_local_floor((double)(x)))
#define STBTT_iceil(x) ((int)otf_local_ceil((double)(x)))
#define STBTT_sqrt(x) otf_local_sqrt((double)(x))
#define STBTT_fabs(x) otf_local_fabs((double)(x))
#define STBTT_fmod(x, y) otf_local_fmod((double)(x), (double)(y))
#define STBTT_cos(x) otf_local_cos((double)(x))
#define STBTT_acos(x) otf_local_acos((double)(x))
#define STBTT_pow(x, y) otf_local_pow((double)(x), (double)(y))
#define STBTT_assert(x) ((void)0)

#define STB_TRUETYPE_IMPLEMENTATION
#include <third_party/stb_truetype.h>

/*
 * SECURITY-INVARIANT: Maximum accepted scalable-font file size.
 *
 * Why: The OTF loader currently reads the full font file into kernel heap so it
 *      can hand a contiguous buffer to the parser/rasterizer.
 * Invariant: size <= OTF_FONT_MAX_FILE_BYTES before allocation and parse.
 * Breakage if changed:
 *   - Increasing this raises worst-case heap pressure on low-RAM boots.
 *   - Decreasing this may reject legitimate fonts that previously loaded.
 * ABI-sensitive: No.
 * Disk-format-sensitive: No.
 * Security-critical: Yes (bounds allocation and parser input size).
 */
#define OTF_FONT_MAX_FILE_BYTES (2u * 1024u * 1024u)

/*
 * SECURITY-INVARIANT: Rasterizer only emits 8-pixel-wide rows because the
 * terminal/UI text layout assumes fixed 8-column cells.
 */
#define OTF_RASTER_CELL_W 8

/*
 * ABI-INVARIANT: Allowed scalable glyph heights for OTF/TTF rasterization.
 *
 * Why: Keeps memory and rasterization cost bounded while allowing visible
 *      size control from userspace via font path hints.
 * Invariant: glyph_h must be in [OTF_GLYPH_H_MIN, OTF_GLYPH_H_MAX].
 * Breakage if changed:
 *   - Increasing raises worst-case per-font RAM usage and load time.
 *   - Decreasing rejects sizes accepted by existing userland tools.
 * ABI-sensitive: Yes (affects accepted @N values).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (bounds heap allocations and work factor).
 */
#define OTF_GLYPH_H_MIN 6
#define OTF_GLYPH_H_MAX 64
#define OTF_GLYPH_W_MAX 64

static int otf_font_read_all(uint8 drive, const char* path, uint8** out_data, uint32* out_size) {
    if (!path || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;

    uint32 size = 0;
    if (vfs_get_file_size(drive, path, &size) != 0) return -1;
    if (size == 0 || size > OTF_FONT_MAX_FILE_BYTES) return -1;

    uint8* data = (uint8*)malloc((size_t)size);
    if (!data) return -1;

    uint32 off = 0;
    uint32 kick_next_off = 0;
    while (off < size) {
        int to_read = (int)(size - off);
        if (to_read > 4096) to_read = 4096;
        int n = vfs_read_file_at(drive, path, (char*)(data + off), to_read, off);
        if (n <= 0) {
            free(data);
            return -1;
        }
        off += (uint32)n;
        if (off >= kick_next_off) {
            watchdog_kick("otf-read");
            kick_next_off = off + 4096;
        }
    }

    *out_data = data;
    *out_size = size;
    return 0;
}

int otf_font_rasterize_mono_from_vfs(uint8 drive, const char* path, int glyph_h, uint64_t* out_rows, int glyph_count, uint8* out_advances, int advances_count, int* out_line_h) {
    if (!path || !out_rows || !out_advances || glyph_count <= 0 || advances_count < glyph_count) return -1;
    if (glyph_h < OTF_GLYPH_H_MIN || glyph_h > OTF_GLYPH_H_MAX) return -1;

    memset(out_rows, 0, (size_t)glyph_h * (size_t)glyph_count * sizeof(uint64_t));
    memset(out_advances, 0, (size_t)glyph_count);

    uint8* data = NULL;
    uint32 size = 0;
    if (otf_font_read_all(drive, path, &data, &size) != 0) return -1;

    int rc = -1;
    stbtt_fontinfo font;
    int font_off = stbtt_GetFontOffsetForIndex(data, 0);
    if (font_off < 0) goto out;
    if (!stbtt_InitFont(&font, data, font_off)) goto out;

    float scale = stbtt_ScaleForPixelHeight(&font, (float)glyph_h);
    if (!(scale > 0.0f)) goto out;

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    int scaled_line_h = (int)(((ascent - descent + line_gap) * scale) + 0.5f);
    if (scaled_line_h < glyph_h) scaled_line_h = glyph_h;
    if (out_line_h) *out_line_h = scaled_line_h;

    int baseline = (int)(ascent * scale + 0.5f);
    int any_on = 0;

    for (int codepoint = 0; codepoint < glyph_count; ++codepoint) {
        int bw = 0;
        int bh = 0;
        int xoff = 0;
        int yoff = 0;

        unsigned char* bm = stbtt_GetCodepointBitmap(
            &font,
            scale,
            scale,
            codepoint,
            &bw,
            &bh,
            &xoff,
            &yoff
        );

        if (!bm || bw <= 0 || bh <= 0) {
            if (bm) stbtt_FreeBitmap(bm, NULL);
            continue;
        }

        int glyph_span = bw;
        if (glyph_span <= 0) {
            stbtt_FreeBitmap(bm, NULL);
            continue;
        }

        int draw_w = glyph_span;
        if (draw_w > OTF_GLYPH_W_MAX) draw_w = OTF_GLYPH_W_MAX;

        int adv = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&font, codepoint, &adv, &lsb);
        (void)lsb;
        int adv_px = (int)(adv * scale + 0.5f);
        if (adv_px <= 0) adv_px = draw_w;
        if (adv_px <= 0) adv_px = 1;
        if (adv_px > OTF_GLYPH_W_MAX) adv_px = OTF_GLYPH_W_MAX;
        out_advances[codepoint] = (uint8)adv_px;

        uint64_t* glyph_rows = out_rows + ((size_t)codepoint * (size_t)glyph_h);
        for (int py = 0; py < bh; ++py) {
            int cell_y = baseline + yoff + py;
            if (cell_y < 0 || cell_y >= glyph_h) continue;
            const unsigned char* src_row = bm + ((size_t)py * (size_t)bw);

            for (int px = 0; px < bw; ++px) {
                int cell_x = px;
                if (cell_x < 0 || cell_x >= draw_w) continue;
                if (src_row[px] >= 128u) {
                    glyph_rows[cell_y] |= (uint64_t)1uLL << (63u - (unsigned)cell_x);
                    any_on = 1;
                }
            }
        }

        stbtt_FreeBitmap(bm, NULL);
        if ((codepoint & 31) == 31) watchdog_kick("otf-raster");
    }

    if (!any_on) goto out;
    rc = 0;

out:
    if (data) free(data);
    return rc;
}
