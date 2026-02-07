#include <multiboot.h>

#include <drivers/aarch64/fb_simple.h>
#include <misc/types.h>
#include <string.h>

// AArch64-full does not boot via Multiboot, but a large part of the shared UI
// stack (VGA/REI/tiler) expects multiboot_info_t *g_mbi to be present.
// Provide a minimal structure that advertises framebuffer parameters.

multiboot_info_t *g_mbi = 0;
static multiboot_info_t g_mbi_storage;

void aarch64_multiboot_compat_init_from_fb(void) {
    memset(&g_mbi_storage, 0, sizeof(g_mbi_storage));

    uint64 fb_base = 0;
    uint32 fb_w = 0, fb_h = 0, fb_stride = 0, fb_bpp = 0;
    const char* fb_fmt = 0;
    if (fb_simple_get_info(&fb_base, &fb_w, &fb_h, &fb_stride, &fb_bpp, &fb_fmt) != 0) {
        // Leave g_mbi = NULL; callers usually guard against it.
        g_mbi = 0;
        return;
    }

    g_mbi_storage.flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
    g_mbi_storage.framebuffer_addr = (multiboot_uint64_t)fb_base;
    g_mbi_storage.framebuffer_pitch = (multiboot_uint32_t)fb_stride;
    g_mbi_storage.framebuffer_width = (multiboot_uint32_t)fb_w;
    g_mbi_storage.framebuffer_height = (multiboot_uint32_t)fb_h;
    g_mbi_storage.framebuffer_bpp = (multiboot_uint8_t)fb_bpp;
    g_mbi_storage.framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_RGB;

    // Best-effort RGB field info based on fb_simple format string.
    // Default: XRGB/ARGB => 0x00RRGGBB in 32-bit word.
    uint8 rpos = 16, gpos = 8, bpos = 0;
    if (fb_fmt) {
        // XBGR implies 0x00BBGGRR packing.
        if (strstr(fb_fmt, "XBGR") || strstr(fb_fmt, "xbgr")) {
            rpos = 0; gpos = 8; bpos = 16;
        }
    }
    g_mbi_storage.framebuffer_red_field_position = rpos;
    g_mbi_storage.framebuffer_red_mask_size = 8;
    g_mbi_storage.framebuffer_green_field_position = gpos;
    g_mbi_storage.framebuffer_green_mask_size = 8;
    g_mbi_storage.framebuffer_blue_field_position = bpos;
    g_mbi_storage.framebuffer_blue_mask_size = 8;

    g_mbi = &g_mbi_storage;
}
