#ifndef OTF_FONT_H
#define OTF_FONT_H

#include <misc/types.h>
#include <stdint.h>

/*
 * Rasterize a scalable OTF/TTF font file from the VFS into fixed-width bit rows
 * plus per-glyph advances for variable-width text layout.
 *
 * Inputs:
 *   drive/path  - font file location in VFS
 *   glyph_h     - target cell height in pixels (currently 6..64)
 *   out_rows    - caller-provided buffer of (glyph_count * glyph_h) uint64 rows
 *                 where bit 63 is the left-most pixel and bit 0 is right-most.
 *   glyph_count - number of sequential codepoints to rasterize (typically 256)
 *   out_advances- caller-provided array of glyph_count advances (pixels)
 *   out_line_h  - optional output for recommended line step (pixels)
 *
 * Returns 0 on success, -1 on failure.
 */
int otf_font_rasterize_mono_from_vfs(
    uint8 drive,
    const char* path,
    int glyph_h,
    uint64_t* out_rows,
    int glyph_count,
    uint8* out_advances,
    int advances_count,
    int* out_line_h
);

#endif
