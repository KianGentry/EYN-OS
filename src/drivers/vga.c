#include <vga.h>
#include <vga_text.h>
#include <multiboot.h>
#include <eynfs.h>
#include <util.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <misc/types.h>
#include <shell.h>
#include <system.h>
#include <serial.h>
#include <watchdog.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <context.h>
#include <misc/sched.h>
#include <drivers/otf_font.h>

extern multiboot_info_t *g_mbi;

#if defined(EYNOS_ARCH_I386)
extern uint32 vbe_bios_int10_set_mode_i386(uint16 mode);
extern uint32 vbe_bios_int10_get_mode_info_i386(uint16 mode, void* out_block);
#endif

int width, height;
int vga_default_r = 255, vga_default_g = 255, vga_default_b = 255; // Default to white
// When non-zero, drawText operates in a minimal glyph-draw mode used by drawCharAt.
static int g_drawCharAt_mode = 0;
// Software cursor control for framebuffer-backed console
static int g_console_cursor_visible = 1;
static const int g_console_cursor_w = 1; // pixels

// Forward declarations for static helpers defined later in this file.
static uint8 vga_font_line_h_or_builtin(int handle);
static uint8 vga_font_advance_or_builtin(int handle, int charnum);
static int vga_get_system_font_handle_raw(void);

// Erase software cursor (if framebuffer-backed console active)
void vga_console_erase_cursor(void) {
	if (!g_console_cursor_visible || !g_mbi || !g_mbi->framebuffer_addr) return;
	int font_handle = vga_get_system_font_handle_raw();
	int line_h = (int)vga_font_line_h_or_builtin(font_handle);
	drawRect(width, height, g_console_cursor_w, line_h, 0, 0, 0);
}

// Draw software cursor at current position
void vga_console_draw_cursor(void) {
	if (!g_console_cursor_visible || !g_mbi || !g_mbi->framebuffer_addr) return;
	int font_handle = vga_get_system_font_handle_raw();
	int line_h = (int)vga_font_line_h_or_builtin(font_handle);
	drawRect(width, height, g_console_cursor_w, line_h, vga_default_r, vga_default_g, vga_default_b);
}

// Move cursor helpers
void vga_console_move_left(void) {
	if (g_mbi && g_mbi->framebuffer_addr) {
		vga_console_erase_cursor();
		int font_handle = vga_get_system_font_handle_raw();
		int adv = (int)vga_font_advance_or_builtin(font_handle, 'W');
		if (adv <= 0) adv = 8;
		if (width >= adv) width -= adv; else width = 0;
		vga_console_draw_cursor();
	} else {
		vga_text_putchar('\b');
	}
}

void vga_console_move_right(void) {
	if (g_mbi && g_mbi->framebuffer_addr) {
		vga_console_erase_cursor();
		int font_handle = vga_get_system_font_handle_raw();
		int adv = (int)vga_font_advance_or_builtin(font_handle, 'W');
		if (adv <= 0) adv = 8;
		int fbw = g_mbi ? (int)g_mbi->framebuffer_width : 640;
		if (width + adv < fbw) width += adv;
		vga_console_draw_cursor();
	} else {
		// No direct right-move for VGA text; emulate by printing a space then backspacing
		vga_text_putchar(' ');
		vga_text_putchar('\b');
	}
}

void vga_console_move_up(void) {
	if (g_mbi && g_mbi->framebuffer_addr) {
		vga_console_erase_cursor();
		int font_handle = vga_get_system_font_handle_raw();
		int line_h = (int)vga_font_line_h_or_builtin(font_handle);
		if (height - line_h >= 0) height -= line_h; else height = 0;
		vga_console_draw_cursor();
	} else {
		// Not supported in legacy VGA text
	}
}

void vga_console_move_down(void) {
	if (g_mbi && g_mbi->framebuffer_addr) {
		vga_console_erase_cursor();
		int font_handle = vga_get_system_font_handle_raw();
		int line_h = (int)vga_font_line_h_or_builtin(font_handle);
		int fbh = g_mbi ? (int)g_mbi->framebuffer_height : 480;
		if (height + line_h < fbh) height += line_h;
		vga_console_draw_cursor();
	} else {
		// Not supported in legacy VGA text
	}
}

/*
 * ABI-INVARIANT: Bochs/QEMU VBE register interface (ports 0x01CE/0x01CF).
 *
 * Why: Enables runtime hardware mode switching without BIOS callbacks.
 * Invariant: Register indices and IDs follow the Bochs VBE extension ABI.
 * Breakage if changed: Mode switches stop working or program wrong registers.
 * ABI-sensitive: Yes (hardware I/O contract).
 */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF

#define VBE_DISPI_INDEX_ID 0x0
#define VBE_DISPI_INDEX_XRES 0x1
#define VBE_DISPI_INDEX_YRES 0x2
#define VBE_DISPI_INDEX_BPP 0x3
#define VBE_DISPI_INDEX_ENABLE 0x4
#define VBE_DISPI_INDEX_X_OFFSET 0x8
#define VBE_DISPI_INDEX_Y_OFFSET 0x9

#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_LFB_ENABLED 0x40

#define VBE_DISPI_ID0 0xB0C0
#define VBE_DISPI_ID5 0xB0C5

static void vbe_dispi_write(uint16 index, uint16 value) {
	outw(VBE_DISPI_IOPORT_INDEX, index);
	outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16 vbe_dispi_read(uint16 index) {
	outw(VBE_DISPI_IOPORT_INDEX, index);
	return inw(VBE_DISPI_IOPORT_DATA);
}

static vga_capabilities_t g_vga_caps;

typedef struct {
	uint16 mode;
	uint16 width;
	uint16 height;
	uint8 bpp;
} vbe_mode_map_t;

typedef struct __attribute__((packed)) {
	uint16 mode_attributes;
	uint8 win_a_attributes;
	uint8 win_b_attributes;
	uint16 win_granularity;
	uint16 win_size;
	uint16 win_a_segment;
	uint16 win_b_segment;
	uint32 win_func_ptr;
	uint16 bytes_per_scan_line;
	uint16 x_resolution;
	uint16 y_resolution;
	uint8 x_char_size;
	uint8 y_char_size;
	uint8 number_of_planes;
	uint8 bits_per_pixel;
	uint8 number_of_banks;
	uint8 memory_model;
	uint8 bank_size;
	uint8 number_of_image_pages;
	uint8 reserved0;
	uint8 red_mask_size;
	uint8 red_field_position;
	uint8 green_mask_size;
	uint8 green_field_position;
	uint8 blue_mask_size;
	uint8 blue_field_position;
	uint8 reserved_mask_size;
	uint8 reserved_field_position;
	uint8 direct_color_mode_info;
	uint32 phys_base_ptr;
	uint32 off_screen_mem_offset;
	uint16 off_screen_mem_size;
	uint8 reserved1[206];
} vbe_mode_info_block_t;

static vbe_mode_info_block_t g_vbe_runtime_mode_info;

/*
 * Common VBE mode IDs used by many 1990s BIOS implementations.
 * This table is intentionally conservative and is used only for selecting
 * candidate BIOS modes in the i386 backend path.
 */
static const vbe_mode_map_t g_vbe_mode_map[] = {
	{0x111, 640,  480, 16},
	{0x114, 800,  600, 16},
	{0x117, 1024, 768, 16},
	{0x11A, 1280, 1024, 16},
	{0x112, 640,  480, 24},
	{0x115, 800,  600, 24},
	{0x118, 1024, 768, 24},
	{0x11B, 1280, 1024, 24},
	/* 32bpp VBE mode IDs are less consistent; include common BIOS IDs. */
	{0x141, 640,  480, 32},
	{0x144, 800,  600, 32},
	{0x147, 1024, 768, 32},
};

static uint16 vga_resolve_standard_vbe_mode(int width, int height, int bpp) {
	for (size_t i = 0; i < sizeof(g_vbe_mode_map) / sizeof(g_vbe_mode_map[0]); ++i) {
		if ((int)g_vbe_mode_map[i].width == width &&
		    (int)g_vbe_mode_map[i].height == height &&
		    (int)g_vbe_mode_map[i].bpp == bpp) {
			return g_vbe_mode_map[i].mode;
		}
	}
	return 0;
}

static int vga_bpp_to_bytes(uint8 bpp) {
	if (bpp == 16) return 2;
	if (bpp == 24) return 3;
	if (bpp == 32) return 4;
	return 0;
}

static int vga_boot_framebuffer_valid(const multiboot_info_t* mbi) {
	if (!mbi) return 0;
	if (!mbi->framebuffer_addr) return 0;
	if (!mbi->framebuffer_width || !mbi->framebuffer_height || !mbi->framebuffer_pitch) return 0;

	int bytes = vga_bpp_to_bytes(mbi->framebuffer_bpp);
	if (bytes <= 0) return 0;

	uint32 min_pitch = mbi->framebuffer_width * (uint32)bytes;
	if (mbi->framebuffer_pitch < min_pitch) return 0;

	return 1;
}

static int vga_try_bios_vbe_set_mode_i386(int mode_w, int mode_h, int bpp, const vga_capabilities_t* caps) {
#if defined(EYNOS_ARCH_I386)
	if (!caps || !caps->bios_runtime_mode_switch_available) {
		printf("[gfx] bios mode switch fail: backend unavailable\n");
		return -1;
	}

	uint16 candidate = vga_resolve_standard_vbe_mode(mode_w, mode_h, bpp);
	if (!candidate) {
		printf("[gfx] bios mode switch fail: no standard VBE mode mapping for %dx%dx%d\n",
		       mode_w,
		       mode_h,
		       bpp);
		return -1;
	}

	uint16 active_mode = (uint16)(caps->bios_active_mode & 0x01FFu);
	uint16 target_mode = (uint16)(candidate & 0x01FFu);
	if (active_mode == target_mode && caps->valid_boot_framebuffer) {
		printf("[gfx] bios mode switch: mode 0x%X already active via boot framebuffer\n",
		       (unsigned)candidate);
		return 0;
	}

	if (!g_mbi || !g_mbi->framebuffer_addr) {
		printf("[gfx] bios mode switch fail: framebuffer address unavailable\n");
		return -1;
	}

	uint32 bios_status = vbe_bios_int10_set_mode_i386(candidate);
	if ((bios_status & 0xFFFFu) != 0x004Fu) {
		printf("[gfx] bios mode switch fail: int10 status=0x%X mode=0x%X\n",
		       (unsigned)(bios_status & 0xFFFFu),
		       (unsigned)candidate);
		return -1;
	}

	vbe_mode_info_block_t mode_info;
	memset(&mode_info, 0, sizeof(mode_info));
	uint32 mode_info_status = vbe_bios_int10_get_mode_info_i386(candidate, &mode_info);

	int resolved_w = mode_w;
	int resolved_h = mode_h;
	int resolved_bpp = bpp;
	int bytes = vga_bpp_to_bytes((uint8)bpp);
	if (bytes <= 0) {
		bytes = 4;
	}
	uint32 resolved_pitch = (uint32)mode_w * (uint32)bytes;
	uint32 resolved_fb_addr = (uint32)g_mbi->framebuffer_addr;
	int mode_info_usable = 0;

	if ((mode_info_status & 0xFFFFu) == 0x004Fu) {
		int info_w = (int)mode_info.x_resolution;
		int info_h = (int)mode_info.y_resolution;
		int info_bpp = (int)mode_info.bits_per_pixel;
		int info_bytes = vga_bpp_to_bytes(mode_info.bits_per_pixel);
		uint32 info_pitch = (uint32)mode_info.bytes_per_scan_line;
		uint32 min_pitch = 0;

		if (info_w > 0 && info_bytes > 0) {
			min_pitch = (uint32)info_w * (uint32)info_bytes;
		}

		if ((mode_info.mode_attributes & 0x0001u) &&
		    info_w > 0 &&
		    info_h > 0 &&
		    info_bytes > 0 &&
		    info_pitch >= min_pitch) {
			resolved_w = info_w;
			resolved_h = info_h;
			resolved_bpp = info_bpp;
			resolved_pitch = info_pitch;
			if (mode_info.phys_base_ptr) {
				resolved_fb_addr = mode_info.phys_base_ptr;
			}
			g_vbe_runtime_mode_info = mode_info;
			mode_info_usable = 1;
		} else {
			printf("[gfx] bios mode info warning: unusable block mode=0x%X attr=0x%X %dx%dx%d pitch=%u lfb=0x%X\n",
			       (unsigned)candidate,
			       (unsigned)mode_info.mode_attributes,
			       info_w,
			       info_h,
			       info_bpp,
			       (unsigned)info_pitch,
			       (unsigned)mode_info.phys_base_ptr);
		}
	} else {
		printf("[gfx] bios mode info warning: int10 status=0x%X mode=0x%X\n",
		       (unsigned)(mode_info_status & 0xFFFFu),
		       (unsigned)candidate);
	}

	g_mbi->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
	g_mbi->flags |= MULTIBOOT_INFO_VBE_INFO;
	g_mbi->vbe_mode = candidate;
	if (mode_info_usable) {
		g_mbi->vbe_mode_info = (uint32)(uintptr_t)&g_vbe_runtime_mode_info;
	}
	g_mbi->framebuffer_addr = (uint64)resolved_fb_addr;
	g_mbi->framebuffer_width = (uint32)resolved_w;
	g_mbi->framebuffer_height = (uint32)resolved_h;
	g_mbi->framebuffer_bpp = (uint8)resolved_bpp;
	g_mbi->framebuffer_pitch = resolved_pitch;

	printf("[gfx] bios mode switch: mode=0x%X set=0x%X info=0x%X %dx%dx%d pitch=%u lfb=0x%X source=%s\n",
	       (unsigned)candidate,
	       (unsigned)(bios_status & 0xFFFFu),
	       (unsigned)(mode_info_status & 0xFFFFu),
	       resolved_w,
	       resolved_h,
	       resolved_bpp,
	       (unsigned)resolved_pitch,
	       (unsigned)resolved_fb_addr,
	       mode_info_usable ? "bios-mode-info" : "fallback-requested");

	return 0;
#else
	(void)mode_w;
	(void)mode_h;
	(void)bpp;
	(void)caps;
	printf("[gfx] bios mode switch fail: backend requires i386\n");
	return -1;
#endif
}

static void vga_refresh_capabilities_internal(int probe_dispi) {
	uint16 dispi_id = g_vga_caps.bochs_dispi_id;

	memset(&g_vga_caps, 0, sizeof(g_vga_caps));
	g_vga_caps.initialized = 1;
	g_vga_caps.fallback_text_eligible = 1;

	if (g_mbi) {
		g_vga_caps.has_multiboot_state = 1;
		g_vga_caps.has_multiboot_fb_info = (g_mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) ? 1 : 0;
		g_vga_caps.has_multiboot_vbe_info = (g_mbi->flags & MULTIBOOT_INFO_VBE_INFO) ? 1 : 0;
		g_vga_caps.has_framebuffer_addr = g_mbi->framebuffer_addr ? 1 : 0;
		g_vga_caps.has_framebuffer_geometry =
			(g_mbi->framebuffer_width && g_mbi->framebuffer_height && g_mbi->framebuffer_pitch) ? 1 : 0;
		g_vga_caps.valid_boot_framebuffer = vga_boot_framebuffer_valid(g_mbi) ? 1 : 0;

		g_vga_caps.boot_fb_addr = (uint32)g_mbi->framebuffer_addr;
		g_vga_caps.boot_fb_pitch = g_mbi->framebuffer_pitch;
		g_vga_caps.boot_fb_width = g_mbi->framebuffer_width;
		g_vga_caps.boot_fb_height = g_mbi->framebuffer_height;
		g_vga_caps.boot_fb_bpp = g_mbi->framebuffer_bpp;
		g_vga_caps.boot_vbe_mode = g_mbi->vbe_mode;
		g_vga_caps.boot_vbe_control_info = g_mbi->vbe_control_info;
		g_vga_caps.boot_vbe_mode_info = g_mbi->vbe_mode_info;
		g_vga_caps.bios_active_mode = g_mbi->vbe_mode;

#if defined(EYNOS_ARCH_I386)
		if (g_vga_caps.has_multiboot_vbe_info && g_mbi->vbe_mode && g_mbi->vbe_mode != 0xFFFFu) {
			g_vga_caps.bios_vbe_backend_available = 1;
			g_vga_caps.bios_runtime_mode_switch_available = 1;
		}
#endif
	}

	if (probe_dispi || dispi_id == 0) {
		dispi_id = vbe_dispi_read(VBE_DISPI_INDEX_ID);
	}

	g_vga_caps.bochs_dispi_id = dispi_id;
	g_vga_caps.bochs_dispi_available = (dispi_id >= VBE_DISPI_ID0 && dispi_id <= VBE_DISPI_ID5) ? 1 : 0;
	g_vga_caps.runtime_mode_switch_available =
		(g_vga_caps.bochs_dispi_available || g_vga_caps.bios_runtime_mode_switch_available) ? 1 : 0;
	g_vga_caps.fallback_grub_fb_eligible = g_vga_caps.valid_boot_framebuffer ? 1 : 0;
}

void vga_get_capabilities(vga_capabilities_t* out) {
	vga_refresh_capabilities_internal(1);
	if (out) {
		*out = g_vga_caps;
	}
}

int vga_can_set_mode(void) {
	vga_refresh_capabilities_internal(1);
	return g_vga_caps.runtime_mode_switch_available ? 1 : 0;
}

void vga_log_boot_capabilities(void) {
	vga_capabilities_t caps;
	vga_get_capabilities(&caps);

	if (!caps.has_multiboot_state) {
		printf("[gfx] boot: multiboot info missing\n");
		return;
	}

	printf("[gfx] boot: flags=0x%X fb_flag=%d vbe_flag=%d\n",
	       (unsigned)g_mbi->flags,
	       (int)caps.has_multiboot_fb_info,
	       (int)caps.has_multiboot_vbe_info);

	if (caps.has_framebuffer_addr && caps.has_framebuffer_geometry) {
		printf("[gfx] boot fb: addr=0x%X %ux%ux%u pitch=%u\n",
		       (unsigned)caps.boot_fb_addr,
		       (unsigned)caps.boot_fb_width,
		       (unsigned)caps.boot_fb_height,
		       (unsigned)caps.boot_fb_bpp,
		       (unsigned)caps.boot_fb_pitch);
	} else if (caps.has_framebuffer_addr) {
		printf("[gfx] boot fb: addr=0x%X (geometry unavailable)\n", (unsigned)caps.boot_fb_addr);
	} else {
		printf("[gfx] boot fb: not provided\n");
	}

	printf("[gfx] boot fb validity: %s\n", caps.valid_boot_framebuffer ? "valid" : "invalid");

	printf("[gfx] multiboot vbe: mode=0x%X ctrl=0x%X mode_info=0x%X\n",
	       (unsigned)caps.boot_vbe_mode,
	       (unsigned)caps.boot_vbe_control_info,
	       (unsigned)caps.boot_vbe_mode_info);

	printf("[gfx] capabilities: dispi_id=0x%X runtime_switch=%d grub_fb_fallback=%d text_fallback=%d\n",
	       (unsigned)caps.bochs_dispi_id,
	       (int)caps.runtime_mode_switch_available,
	       (int)caps.fallback_grub_fb_eligible,
	       (int)caps.fallback_text_eligible);
	printf("[gfx] bios backend: available=%d runtime=%d active_mode=0x%X\n",
	       (int)caps.bios_vbe_backend_available,
	       (int)caps.bios_runtime_mode_switch_available,
	       (unsigned)caps.bios_active_mode);

	const char* runtime_path = "unavailable";
	if (caps.bochs_dispi_available) {
		runtime_path = "bochs/qemu-dispi";
	} else if (caps.bios_runtime_mode_switch_available) {
		runtime_path = "bios-vbe-i386";
	}
	printf("[gfx] runtime mode path: %s\n", runtime_path);
}

int vga_set_mode(int mode_w, int mode_h, int bpp) {
    vga_capabilities_t caps;
    vga_get_capabilities(&caps);

	if (!caps.has_multiboot_state) {
		printf("[gfx] mode switch fail: no multiboot state\n");
		return -1;
	}
	if (mode_w < 320 || mode_h < 200) {
		printf("[gfx] mode switch fail: invalid geometry %dx%d\n", mode_w, mode_h);
		return -1;
	}
	if (bpp != 16 && bpp != 24 && bpp != 32) {
		printf("[gfx] mode switch fail: unsupported bpp=%d\n", bpp);
		return -1;
	}
	if (!caps.runtime_mode_switch_available) {
		printf("[gfx] mode switch fail: runtime switch unavailable (grub_fb_fallback=%d text_fallback=%d)\n",
		       (int)caps.fallback_grub_fb_eligible,
		       (int)caps.fallback_text_eligible);
		return -1;
	}

	if (!caps.bochs_dispi_available) {
		int bios_rc = vga_try_bios_vbe_set_mode_i386(mode_w, mode_h, bpp, &caps);
		if (bios_rc != 0) {
			return -1;
		}

		/* Mode may already be active (no-op case) or updated by backend thunk. */
		vga_refresh_capabilities_internal(1);
		vga_init_double_buffer();
		clearScreen();
		vga_begin_frame();
		vga_mark_dirty_rect(0, 0,
					  (int)(g_mbi ? g_mbi->framebuffer_width : (uint32)mode_w),
					  (int)(g_mbi ? g_mbi->framebuffer_height : (uint32)mode_h));
		vga_swap_buffers();
		printf("[gfx] mode switch: backend=bios-vbe-i386 requested=%dx%dx%d\n",
		       mode_w,
		       mode_h,
		       bpp);
		return 0;
	}

	vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	vbe_dispi_write(VBE_DISPI_INDEX_XRES, (uint16)mode_w);
	vbe_dispi_write(VBE_DISPI_INDEX_YRES, (uint16)mode_h);
	vbe_dispi_write(VBE_DISPI_INDEX_BPP, (uint16)bpp);
	vbe_dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
	vbe_dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
	vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

	int actual_w = (int)vbe_dispi_read(VBE_DISPI_INDEX_XRES);
	int actual_h = (int)vbe_dispi_read(VBE_DISPI_INDEX_YRES);
	int actual_bpp = (int)vbe_dispi_read(VBE_DISPI_INDEX_BPP);
	if (actual_w <= 0 || actual_h <= 0) {
		printf("[gfx] mode switch fail: invalid actual geometry %dx%d\n", actual_w, actual_h);
		return -1;
	}
	if (actual_bpp != 16 && actual_bpp != 24 && actual_bpp != 32) {
		printf("[gfx] mode switch fail: invalid actual bpp=%d\n", actual_bpp);
		return -1;
	}

	g_mbi->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
	g_mbi->framebuffer_width = (uint32)actual_w;
	g_mbi->framebuffer_height = (uint32)actual_h;
	g_mbi->framebuffer_bpp = (uint8)actual_bpp;
	g_mbi->framebuffer_pitch = (uint32)(actual_w * (actual_bpp / 8));
	vga_refresh_capabilities_internal(1);

	vga_init_double_buffer();
	clearScreen();
	vga_begin_frame();
	vga_mark_dirty_rect(0, 0, actual_w, actual_h);
	vga_swap_buffers();

	printf("[gfx] mode switch: requested=%dx%dx%d actual=%dx%dx%d\n",
	       mode_w,
	       mode_h,
	       bpp,
	       actual_w,
	       actual_h,
	       actual_bpp);

	return 0;
}

// Bitmap font registry (up to 64xN, 256 glyphs)
// Supports .hex bitmaps and rasterized scalable fonts.
#define VGA_FONT_DEFAULT_ADVANCE 8
#define VGA_FONT_GLYPH_W_MAX 64
#define VGA_FONT_GLYPH_H_8 8
#define VGA_FONT_GLYPH_H_16 16
#define VGA_FONT_GLYPHS 256
#define VGA_FONT_MAX 12

/*
 * ABI-INVARIANT: Supported scalable font pixel-height bounds.
 *
 * Why: The text pipeline assumes fixed-width 8px glyph rows but permits
 *      variable row counts for vertical scaling. Bounds keep per-font memory
 *      usage predictable on low-RAM boots.
 * Invariant: Scalable glyph_h must be in [VGA_SCALABLE_GLYPH_H_MIN, VGA_SCALABLE_GLYPH_H_MAX].
 * Breakage if changed:
 *   - Increasing raises per-font heap cost (VGA_FONT_GLYPHS * glyph_h bytes).
 *   - Decreasing rejects previously-valid size hints and can change UI layout.
 * ABI-sensitive: Yes (user-visible accepted @N values in font paths).
 * Disk-format-sensitive: No.
 * Security-critical: Yes (bounds memory allocations from user-supplied font paths).
 */
#define VGA_SCALABLE_GLYPH_H_MIN 6
#define VGA_SCALABLE_GLYPH_H_MAX 64

// System default font (used by drawText/drawCharAt when callers don't specify one).
#define VGA_SYSTEM_FONT_DRIVE 0
#define VGA_SYSTEM_FONT_PATH "/fonts/unscii-16.hex"

// Runtime-selected system font (can be changed while the OS is running).
static uint8 g_vga_system_font_drive = (uint8)VGA_SYSTEM_FONT_DRIVE;
static char g_vga_system_font_path[128] = VGA_SYSTEM_FONT_PATH;

static int vga_ctx_allow(uint32 caps, uint32 cost)
{
	command_context_t* ctx = current_command_context;
	if (ctx && !cap_check(ctx->caps, caps)) return 0;
	if (ctx) {
		scheduler_account(ctx->wo, cost);
		scheduler_yield_if_needed(ctx->wo);
		if (sched_det_is_enabled()) ctx->det_seq++;
	}
	return 1;
}

typedef struct {
	int used;
	uint8 drive;
	uint16 refcount;
	char path[128];
	uint8 glyph_h;                  // rendered glyph bitmap height in pixels
	uint8 line_h;                   // recommended line step in pixels
	uint8 max_advance;              // widest per-glyph advance in pixels
	uint8 advance[VGA_FONT_GLYPHS]; // per-glyph horizontal advance
	uint64* rows;                   // VGA_FONT_GLYPHS * glyph_h rows (bit63 = left)
} vga_font_entry_t;

// Handle 0 = built-in font.
// Handles 1..VGA_FONT_MAX map to g_vga_fonts[handle-1].
static vga_font_entry_t g_vga_fonts[VGA_FONT_MAX];

// System default font handle is held with a permanent refcount by the driver.
static int g_vga_system_font_handle = 0;
static int g_vga_system_font_attempted = 0;

static const unsigned char* vga_builtin_font(void);

static vga_font_entry_t* vga_font_entry_from_handle(int handle) {
	if (handle <= 0) return NULL;
	int idx = handle - 1;
	if (idx < 0 || idx >= VGA_FONT_MAX) return NULL;
	if (!g_vga_fonts[idx].used) return NULL;
	return &g_vga_fonts[idx];
}

static uint8 vga_font_glyph_h_or_builtin(int handle) {
	vga_font_entry_t* e = vga_font_entry_from_handle(handle);
	if (!e || !e->rows) return (uint8)VGA_FONT_GLYPH_H_8;
	if (e->glyph_h == 0) return (uint8)VGA_FONT_GLYPH_H_8;
	return e->glyph_h;
}

static uint8 vga_font_line_h_or_builtin(int handle) {
	vga_font_entry_t* e = vga_font_entry_from_handle(handle);
	if (!e || !e->rows) return (uint8)VGA_FONT_GLYPH_H_8;
	if (e->line_h == 0) return vga_font_glyph_h_or_builtin(handle);
	return e->line_h;
}

static uint8 vga_font_advance_or_builtin(int handle, int charnum) {
	vga_font_entry_t* e = vga_font_entry_from_handle(handle);
	if (!e || !e->rows || charnum < 0) return (uint8)VGA_FONT_DEFAULT_ADVANCE;
	unsigned int c = (unsigned int)(unsigned char)charnum;
	uint8 adv = e->advance[c];
	if (adv == 0) adv = (e->max_advance > 0) ? e->max_advance : (uint8)VGA_FONT_DEFAULT_ADVANCE;
	return adv;
}

int vga_font_glyph_height(int font_handle) {
	return (int)vga_font_glyph_h_or_builtin(font_handle);
}

int vga_font_line_height(int font_handle) {
	return (int)vga_font_line_h_or_builtin(font_handle);
}

int vga_font_advance_width(int font_handle) {
	vga_font_entry_t* e = vga_font_entry_from_handle(font_handle);
	if (!e || !e->rows) return VGA_FONT_DEFAULT_ADVANCE;
	uint8 adv = e->advance[(unsigned int)'W'];
	if (adv == 0) adv = e->advance[(unsigned int)'M'];
	if (adv == 0) adv = (e->max_advance > 0) ? e->max_advance : (uint8)VGA_FONT_DEFAULT_ADVANCE;
	return (int)adv;
}

int vga_font_char_advance(int font_handle, int charnum) {
	return (int)vga_font_advance_or_builtin(font_handle, charnum);
}

static void vga_try_init_system_font(void) {
	if (g_vga_system_font_attempted) return;
	if (g_vga_system_font_path[0] == '\0') return;
	// Only attempt after a filesystem is detectable.
	if (vfs_detect(g_vga_system_font_drive) == VFS_FS_NONE) return;
	g_vga_system_font_attempted = 1;
	int h = vga_font_acquire_path(g_vga_system_font_drive, g_vga_system_font_path);
	if (h > 0) g_vga_system_font_handle = h;
}

static int vga_get_system_font_handle_raw(void) {
	vga_try_init_system_font();
	return g_vga_system_font_handle;
}

int vga_system_font_acquire(void) {
	vga_try_init_system_font();
	if (g_vga_system_font_handle > 0) {
		return vga_font_acquire_path(g_vga_system_font_drive, g_vga_system_font_path);
	}
	return 0;
}

// Set the system font at runtime.
// - On success, the previous system font (if any) is released.
// - If path is NULL, empty, or "builtin", the system font is set to built-in fallback.
// Returns 0 on success, -1 on failure (e.g., file not found / OOM).
int vga_system_font_set(uint8 drive, const char* path) {
	if (!path || !path[0] || strcmp(path, "builtin") == 0) {
		int old = g_vga_system_font_handle;
		g_vga_system_font_handle = 0;
		g_vga_system_font_attempted = 1;
		g_vga_system_font_drive = drive;
		g_vga_system_font_path[0] = '\0';
		if (old > 0) vga_font_release(old);
		return 0;
	}

	int new_handle = vga_font_acquire_path(drive, path);
	if (new_handle <= 0) return -1;

	int old = g_vga_system_font_handle;
	g_vga_system_font_handle = new_handle;
	g_vga_system_font_attempted = 1;
	g_vga_system_font_drive = drive;
	strncpy(g_vga_system_font_path, path, sizeof(g_vga_system_font_path) - 1);
	g_vga_system_font_path[sizeof(g_vga_system_font_path) - 1] = '\0';

	if (old > 0) vga_font_release(old);
	return 0;
}

int vga_text_cell_w(void) {
	int h = vga_get_system_font_handle_raw();
	vga_font_entry_t* e = vga_font_entry_from_handle(h);
	if (!e || !e->rows) return VGA_FONT_DEFAULT_ADVANCE;
	uint8 adv = e->advance[(unsigned int)'W'];
	if (adv == 0) adv = e->advance[(unsigned int)'M'];
	if (adv == 0) adv = (e->max_advance > 0) ? e->max_advance : (uint8)VGA_FONT_DEFAULT_ADVANCE;
	return (int)adv;
}

int vga_text_cell_h(void) {
	// Use recommended line height so larger scalable fonts do not overlap lines.
	int h = (int)vga_font_line_h_or_builtin(vga_get_system_font_handle_raw());
	if (h <= 0) return VGA_FONT_GLYPH_H_8;
	return h;
}

int vga_font_glyph_h(int font_handle) {
	int h = (int)vga_font_glyph_h_or_builtin(font_handle);
	if (h <= 0) return VGA_FONT_GLYPH_H_8;
	return h;
}

static int vga_is_scalable_glyph_h_valid(int glyph_h) {
	return (glyph_h >= VGA_SCALABLE_GLYPH_H_MIN && glyph_h <= VGA_SCALABLE_GLYPH_H_MAX) ? 1 : 0;
}

static int vga_hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static char vga_ascii_tolower(char c) {
	if (c >= 'A' && c <= 'Z') return (char)('a' + (c - 'A'));
	return c;
}

static int vga_str_ends_with_icase(const char* s, const char* suffix) {
	if (!s || !suffix) return 0;
	size_t sl = strlen(s);
	size_t su = strlen(suffix);
	if (su == 0 || sl < su) return 0;
	const char* tail = s + (sl - su);
	for (size_t i = 0; i < su; ++i) {
		if (vga_ascii_tolower(tail[i]) != vga_ascii_tolower(suffix[i])) return 0;
	}
	return 1;
}

static uint8 vga_font_pick_glyph_h_for_path(const char* path) {
	if (!path) return (uint8)VGA_FONT_GLYPH_H_16;
	if (strstr(path, "-8.") || strstr(path, "_8.") || strstr(path, "8x8")) return (uint8)VGA_FONT_GLYPH_H_8;
	if (strstr(path, "-16.") || strstr(path, "_16.") || strstr(path, "8x16")) return (uint8)VGA_FONT_GLYPH_H_16;
	return (uint8)VGA_FONT_GLYPH_H_16;
}

// Parse optional scalable-font size suffix from a path.
// Supported suffix at the end of the string:
//   @N where N is a decimal size in [VGA_SCALABLE_GLYPH_H_MIN, VGA_SCALABLE_GLYPH_H_MAX]
// Example: /fonts/unscii-16.otf@14
static int vga_font_prepare_open_path(const char* path, char* out_open_path, size_t out_cap, uint8* out_hint_h, int* out_has_hint) {
	if (!path || !out_open_path || out_cap == 0 || !out_hint_h || !out_has_hint) return -1;
	*out_hint_h = 0;
	*out_has_hint = 0;
	strncpy(out_open_path, path, out_cap - 1);
	out_open_path[out_cap - 1] = '\0';
	if (strncmp(path, out_open_path, out_cap) != 0) return -1;

	char* at = strrchr(out_open_path, '@');
	if (!at) return 0;
	if (*(at + 1) == '\0') return 0;

	int glyph_h = 0;
	for (char* p = at + 1; *p; ++p) {
		if (*p < '0' || *p > '9') return 0;
		glyph_h = glyph_h * 10 + (*p - '0');
		if (glyph_h > VGA_SCALABLE_GLYPH_H_MAX) return 0;
	}
	if (!vga_is_scalable_glyph_h_valid(glyph_h)) return 0;

	*at = '\0';
	*out_hint_h = (uint8)glyph_h;
	*out_has_hint = 1;
	return 0;
}

static int vga_hex_parse_u32(const char* s, int max_chars, uint32* out) {
	if (!s || !out) return -1;
	uint32 v = 0;
	int n = 0;
	for (; n < max_chars && s[n]; ++n) {
		int h = vga_hex_nibble(s[n]);
		if (h < 0) break;
		v = (v << 4) | (uint32)h;
	}
	if (n == 0) return -1;
	*out = v;
	return n;
}

static int vga_font_parse_hex_line_bytes(const char* line, int* out_glyph_index, uint8* out_bytes, int out_cap, int* out_count) {
	if (!line || !out_glyph_index || !out_bytes || out_cap <= 0 || !out_count) return -1;
	*out_glyph_index = -1;
	*out_count = 0;

	// Skip leading whitespace
	const char* p = line;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
	if (*p == '\0') return 0;
	// Comments
	if (*p == '#' || *p == ';') return 0;
	if (p[0] == '/' && p[1] == '/') return 0;

	// out_glyph_index meanings:
	//  >=0 : explicit glyph index (0..255)
	//  -1  : no prefix label found (caller may use sequential indexing)
	//  -2  : prefix label found but codepoint is out of 8-bit range (caller should skip)
	int glyph_index = -1;
	// Optional prefixes: U+00XX: or 00XX: or 0xXX:
	if ((p[0] == 'U' || p[0] == 'u') && p[1] == '+') {
		uint32 cp = 0;
		int n = vga_hex_parse_u32(p + 2, 8, &cp);
		if (n > 0) {
			const char* q = p + 2 + n;
			while (*q == ' ' || *q == '\t') ++q;
			if (*q == ':') {
				glyph_index = (cp <= 255u) ? (int)cp : -2;
				p = q + 1;
			}
		}
	} else {
		const char* q = p;
		if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
		uint32 cp = 0;
		int n = vga_hex_parse_u32(q, 8, &cp);
		if (n > 0) {
			const char* r = q + n;
			while (*r == ' ' || *r == '\t') ++r;
			if (*r == ':') {
				glyph_index = (cp <= 255u) ? (int)cp : -2;
				p = r + 1;
			}
		}
	}

	int byte_count = 0;
	int have_hi = 0;
	int hi = 0;
	for (; *p && *p != '\n' && *p != '\r'; ++p) {
		int h = vga_hex_nibble(*p);
		if (h < 0) continue;
		if (!have_hi) {
			hi = h;
			have_hi = 1;
		} else {
			if (byte_count < out_cap) {
				out_bytes[byte_count++] = (uint8)((hi << 4) | h);
			}
			have_hi = 0;
		}
	}

	*out_glyph_index = glyph_index;
	*out_count = byte_count;
	return (byte_count > 0) ? 1 : 0;
}

static int vga_font_probe_hex_glyph_h(uint8 drive, const char* path, uint8* out_glyph_h) {
	if (!path || !out_glyph_h) return -1;
	*out_glyph_h = 0;

	uint32 size = 0;
	if (vfs_get_file_size(drive, path, &size) != 0) return -1;
	if (size == 0) return -1;
	if (size > (uint32)(256 * 1024)) return -1;

	char chunk[256];
	char line[256];
	int line_len = 0;
	uint32 off = 0;
	uint32 kick_next_off = 0;
	int count8 = 0;
	int count16 = 0;
	int samples = 0;
	while (off < size) {
		int to_read = (int)(size - off);
		if (to_read > (int)sizeof(chunk)) to_read = (int)sizeof(chunk);
		int n = vfs_read_file_at(drive, path, chunk, to_read, off);
		if (n <= 0) break;
		off += (uint32)n;
		if (off >= kick_next_off) { watchdog_kick("font-probe"); kick_next_off = off + 4096; }
		for (int i = 0; i < n; ++i) {
			char c = chunk[i];
			if (c == '\n') {
				line[line_len] = '\0';
				uint8 bytes[VGA_FONT_GLYPH_H_16];
				int gi = -1;
				int bc = 0;
				int prc = vga_font_parse_hex_line_bytes(line, &gi, bytes, (int)sizeof(bytes), &bc);
				if (prc > 0) {
					if (bc == VGA_FONT_GLYPH_H_8) { count8++; samples++; }
					else if (bc == VGA_FONT_GLYPH_H_16) { count16++; samples++; }
					// Sample enough lines to make a robust decision even if the
					// first glyph is atypical (some fonts encode U+0000 with 16 bytes).
					if (samples >= 32) {
						*out_glyph_h = (uint8)((count16 > count8) ? VGA_FONT_GLYPH_H_16 : VGA_FONT_GLYPH_H_8);
						return 0;
					}
				}
				line_len = 0;
				continue;
			}
			if (c == '\r') continue;
			if (line_len < (int)sizeof(line) - 1) line[line_len++] = c;
		}
	}
	// trailing partial line
	if (line_len > 0) {
		line[line_len] = '\0';
		uint8 bytes[VGA_FONT_GLYPH_H_16];
		int gi = -1;
		int bc = 0;
		int prc = vga_font_parse_hex_line_bytes(line, &gi, bytes, (int)sizeof(bytes), &bc);
		if (prc > 0) {
			if (bc == VGA_FONT_GLYPH_H_8) { count8++; samples++; }
			else if (bc == VGA_FONT_GLYPH_H_16) { count16++; samples++; }
		}
	}
	if (samples <= 0) return -1;
	*out_glyph_h = (uint8)((count16 > count8) ? VGA_FONT_GLYPH_H_16 : VGA_FONT_GLYPH_H_8);
	return 0;
}

static int vga_font_load_hex_stream(uint8 drive, const char* path, uint8* out_bitmap, uint8 glyph_h) {
	if (!path || !out_bitmap) return -1;
	if (glyph_h != VGA_FONT_GLYPH_H_8 && glyph_h != VGA_FONT_GLYPH_H_16) return -1;
	// Initialize to blank glyphs.
	memset(out_bitmap, 0, (size_t)VGA_FONT_GLYPHS * (size_t)glyph_h);

	uint32 size = 0;
	if (vfs_get_file_size(drive, path, &size) != 0) return -1;
	if (size == 0) return -1;
	// Safety: refuse absurdly large font text files.
	if (size > (uint32)(256 * 1024)) return -1;

	char chunk[256];
	char line[256];
	int line_len = 0;
	uint32 off = 0;
	int seq_glyph = 0;
	int any = 0;
	uint32 kick_next_off = 0;
	while (off < size) {
		int to_read = (int)(size - off);
		if (to_read > (int)sizeof(chunk)) to_read = (int)sizeof(chunk);
		int n = vfs_read_file_at(drive, path, chunk, to_read, off);
		if (n <= 0) break;
		off += (uint32)n;
		if (off >= kick_next_off) { watchdog_kick("font-load"); kick_next_off = off + 4096; }
		for (int i = 0; i < n; ++i) {
			char c = chunk[i];
			if (c == '\n') {
				line[line_len] = '\0';
				uint8 bytes[VGA_FONT_GLYPH_H_16];
				int gi_pref = -1;
				int bc = 0;
				int prc = vga_font_parse_hex_line_bytes(line, &gi_pref, bytes, (int)sizeof(bytes), &bc);
				if (prc > 0 && bc == (int)glyph_h) {
					if (gi_pref != -2) {
						int gi = gi_pref;
						if (gi < 0) {
							gi = seq_glyph;
							if (gi >= 0 && gi < VGA_FONT_GLYPHS) seq_glyph++;
						}
						if (gi >= 0 && gi < VGA_FONT_GLYPHS) {
							memcpy(out_bitmap + (gi * (int)glyph_h), bytes, (size_t)glyph_h);
							any = 1;
						}
					}
				}
				line_len = 0;
				if (seq_glyph >= VGA_FONT_GLYPHS) return any ? 0 : -1;
				continue;
			}
			if (c == '\r') continue;
			if (line_len < (int)sizeof(line) - 1) {
				line[line_len++] = c;
			}
		}
	}
	// Process trailing partial line
	if (line_len > 0) {
		line[line_len] = '\0';
		uint8 bytes[VGA_FONT_GLYPH_H_16];
		int gi_pref = -1;
		int bc = 0;
		int prc = vga_font_parse_hex_line_bytes(line, &gi_pref, bytes, (int)sizeof(bytes), &bc);
		if (prc > 0 && bc == (int)glyph_h) {
			if (gi_pref != -2) {
				int gi = gi_pref;
				if (gi < 0) {
					gi = seq_glyph;
					if (gi >= 0 && gi < VGA_FONT_GLYPHS) seq_glyph++;
				}
				if (gi >= 0 && gi < VGA_FONT_GLYPHS) {
					memcpy(out_bitmap + (gi * (int)glyph_h), bytes, (size_t)glyph_h);
					any = 1;
				}
			}
		}
	}
	return any ? 0 : -1;
}

int vga_font_acquire_hex(uint8 drive, const char* path) {
	if (!vga_ctx_allow(CAP_READ_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return -1;
	if (!path || !path[0]) return -1;
	char open_path[128];
	uint8 hint_h = 0;
	int has_hint = 0;
	if (vga_font_prepare_open_path(path, open_path, sizeof(open_path), &hint_h, &has_hint) != 0) return -1;
	int is_hex = vga_str_ends_with_icase(open_path, ".hex");
	int is_otf = vga_str_ends_with_icase(open_path, ".otf");
	int is_ttf = vga_str_ends_with_icase(open_path, ".ttf");
	if (!is_hex && !is_otf && !is_ttf) return -1;
	// Reuse already-loaded fonts.
	for (int i = 0; i < VGA_FONT_MAX; ++i) {
		vga_font_entry_t* e = &g_vga_fonts[i];
		if (!e->used) continue;
		if (e->drive == drive && strncmp(e->path, path, sizeof(e->path)) == 0) {
			if (e->refcount < 0xFFFF) e->refcount++;
			return i + 1;
		}
	}
	// Find a free slot.
	int slot = -1;
	for (int i = 0; i < VGA_FONT_MAX; ++i) {
		if (!g_vga_fonts[i].used) { slot = i; break; }
	}
	if (slot < 0) return -1;

	vga_font_entry_t* e = &g_vga_fonts[slot];
	memset(e, 0, sizeof(*e));
	uint8 glyph_h = 0;
	if (is_hex) {
		if (vga_font_probe_hex_glyph_h(drive, open_path, &glyph_h) != 0) {
			memset(e, 0, sizeof(*e));
			return -1;
		}
	} else {
		glyph_h = has_hint ? hint_h : vga_font_pick_glyph_h_for_path(open_path);
	}
	int row_count = VGA_FONT_GLYPHS * (int)glyph_h;
	e->rows = (uint64*)malloc((size_t)row_count * sizeof(uint64));
	if (!e->rows) { memset(e, 0, sizeof(*e)); return -1; }
	memset(e->rows, 0, (size_t)row_count * sizeof(uint64));
	e->line_h = glyph_h;
	e->max_advance = (uint8)VGA_FONT_DEFAULT_ADVANCE;
	for (int i = 0; i < VGA_FONT_GLYPHS; ++i) e->advance[i] = (uint8)VGA_FONT_DEFAULT_ADVANCE;

	if (is_hex) {
		uint8* packed_rows = (uint8*)malloc((size_t)row_count);
		if (!packed_rows) {
			free(e->rows);
			memset(e, 0, sizeof(*e));
			return -1;
		}
		if (vga_font_load_hex_stream(drive, open_path, packed_rows, glyph_h) != 0) {
			free(packed_rows);
			free(e->rows);
			memset(e, 0, sizeof(*e));
			return -1;
		}
		for (int cp = 0; cp < VGA_FONT_GLYPHS; ++cp) {
			e->advance[cp] = (uint8)VGA_FONT_DEFAULT_ADVANCE;
			for (int row = 0; row < glyph_h; ++row) {
				uint8 bits8 = packed_rows[(cp * (int)glyph_h) + row];
				uint64 row_bits = 0;
				for (int x = 0; x < VGA_FONT_DEFAULT_ADVANCE; ++x) {
					if (bits8 & (0x80u >> (unsigned)x)) {
						row_bits |= (uint64)1uLL << (63u - (unsigned)x);
					}
				}
				e->rows[(cp * (int)glyph_h) + row] = row_bits;
			}
		}
		free(packed_rows);
	} else {
		int line_h = (int)glyph_h;
		if (otf_font_rasterize_mono_from_vfs(drive, open_path, (int)glyph_h, (uint64_t*)e->rows, VGA_FONT_GLYPHS, e->advance, VGA_FONT_GLYPHS, &line_h) != 0) {
			free(e->rows);
			memset(e, 0, sizeof(*e));
			return -1;
		}
		if (line_h < (int)glyph_h) line_h = (int)glyph_h;
		if (line_h > 255) line_h = 255;
		e->line_h = (uint8)line_h;
		uint8 max_adv = (uint8)VGA_FONT_DEFAULT_ADVANCE;
		for (int cp = 0; cp < VGA_FONT_GLYPHS; ++cp) {
			if (e->advance[cp] > max_adv) max_adv = e->advance[cp];
		}
		e->max_advance = max_adv;
	}
	e->used = 1;
	e->drive = drive;
	e->refcount = 1;
	e->glyph_h = glyph_h;
	strncpy(e->path, path, sizeof(e->path) - 1);
	e->path[sizeof(e->path) - 1] = '\0';
	return slot + 1;
}

int vga_font_acquire_path(uint8 drive, const char* path) {
	return vga_font_acquire_hex(drive, path);
}

void vga_font_release(int font_handle) {
	vga_font_entry_t* e = vga_font_entry_from_handle(font_handle);
	if (!e) return;
	if (e->refcount > 0) e->refcount--;
	if (e->refcount == 0) {
		if (e->rows) free(e->rows);
		memset(e, 0, sizeof(*e));
	}
}

// Simple software backbuffer (RGBA8)
static unsigned char* g_backbuffer = NULL;
static int g_backbuffer_w = 0;
static int g_backbuffer_h = 0;

typedef enum {
	VGA_BACKBUFFER_ALLOC_NONE = 0,
	VGA_BACKBUFFER_ALLOC_HEAP = 1,
	VGA_BACKBUFFER_ALLOC_CONTIG_FRAMES = 2,
} vga_backbuffer_alloc_t;

static vga_backbuffer_alloc_t g_backbuffer_alloc = VGA_BACKBUFFER_ALLOC_NONE;
static uint32 g_backbuffer_pages = 0;

static uint32 vga_ptr_to_u32(const void* ptr) {
	uintptr raw = (uintptr)ptr;
	uint32 narrowed = (uint32)raw;
	if ((uintptr)narrowed != raw) {
		return 0;
	}
	return narrowed;
}

static inline const void* vga_u32_to_ptr(uint32 address) {
	return (const void*)(uintptr)address;
}

/*
 * RESOURCE-INVARIANT: Backbuffer allocation on low-RAM systems.
 *
 * Why: A full-screen RGBA8 backbuffer is large (w*h*4). On ~5MB targets it can
 *      easily consume essentially all free physical frames and starve paging,
 *      slabs, and user programs. The UI can still function without a backbuffer
 *      (with more flicker), so prefer skipping allocation when frames are tight.
 */
#define VGA_BACKBUFFER_MIN_FREE_FRAMES_AFTER 64u /* 256KB headroom */

static void vga_free_backbuffer(void) {
	if (!g_backbuffer) return;

	if (g_backbuffer_alloc == VGA_BACKBUFFER_ALLOC_CONTIG_FRAMES) {
		uint32 va = vga_ptr_to_u32(g_backbuffer);
		if (va >= KERNEL_BASE && g_backbuffer_pages) {
			uint32 phys = va - KERNEL_BASE;
			for (uint32 i = 0; i < g_backbuffer_pages; ++i) {
				frame_free(phys + i * PAGE_SIZE);
			}
		}
	} else {
		free(g_backbuffer);
	}

	g_backbuffer = NULL;
	g_backbuffer_w = 0;
	g_backbuffer_h = 0;
	g_backbuffer_alloc = VGA_BACKBUFFER_ALLOC_NONE;
	g_backbuffer_pages = 0;
}

static int vga_draw_glyph8xN_at(int font_handle, int x0, int y0, int charnum, int rr, int gg, int bb) {
	if (charnum < 0) return 0;
	vga_font_entry_t* e = vga_font_entry_from_handle(font_handle);
	int glyph_h = (int)vga_font_glyph_h_or_builtin(font_handle);
	if (glyph_h <= 0) glyph_h = VGA_FONT_GLYPH_H_8;
	unsigned int c = (unsigned int)(unsigned char)charnum;
	int draw_w = (int)vga_font_advance_or_builtin(font_handle, charnum);
	if (draw_w <= 0) draw_w = VGA_FONT_DEFAULT_ADVANCE;
	if (draw_w > VGA_FONT_GLYPH_W_MAX) draw_w = VGA_FONT_GLYPH_W_MAX;

	int use_builtin = (!e || !e->rows);
	const unsigned char* font = NULL;
	if (use_builtin) font = vga_builtin_font();

	// draw into backbuffer if present
	if (g_backbuffer) {
		for (int k = 0; k < glyph_h; k++) {
			int yy = y0 + k;
			if (yy < 0 || yy >= g_backbuffer_h) continue;
			for (int i = 0; i < draw_w; i++) {
				int xx = x0 + i;
				if (xx < 0 || xx >= g_backbuffer_w) continue;
				int on = 0;
				if (use_builtin) {
					unsigned char rowbits = font[c * (unsigned)glyph_h + (unsigned)k];
					on = (rowbits & (0x80u >> (unsigned)i)) ? 1 : 0;
				} else {
					uint64 rowbits = e->rows[c * (unsigned)glyph_h + (unsigned)k];
					on = (rowbits & ((uint64)1uLL << (63u - (unsigned)i))) ? 1 : 0;
				}
				if (on) {
					unsigned char *px = g_backbuffer + ((size_t)yy * (size_t)g_backbuffer_w + (size_t)xx) * 4;
					px[0] = (unsigned char)bb;
					px[1] = (unsigned char)gg;
					px[2] = (unsigned char)rr;
					px[3] = 0;
				}
			}
		}
		vga_mark_dirty_rect(x0, y0, draw_w, glyph_h);
		return draw_w;
	}
	// fallback to framebuffer pixel drawing
	for (int k = 0; k < glyph_h; k++) {
		for (int i = 0; i < draw_w; i++) {
			int on = 0;
			if (use_builtin) {
				on = (font[c * (unsigned)glyph_h + (unsigned)k] & (0x80u >> (unsigned)i)) ? 1 : 0;
			} else {
				uint64 rowbits = e->rows[c * (unsigned)glyph_h + (unsigned)k];
				on = (rowbits & ((uint64)1uLL << (63u - (unsigned)i))) ? 1 : 0;
			}
			if (on) {
				drawPixel(x0 + i, k + y0, rr, gg, bb);
			}
		}
	}
	return draw_w;
}

void drawCharAt_font(int font_handle, int x, int y, int charnum, int rr, int gg, int bb) {
	(void)vga_draw_glyph8xN_at(font_handle, x, y, charnum, rr, gg, bb);
}
// Optional exclusion rect for swap (e.g., software cursor overlay)
static int g_exclude_x = -1, g_exclude_y = -1, g_exclude_w = 0, g_exclude_h = 0;
// Dirty rect tracking (multi-rect)
typedef struct { int x, y, w, h; } dirty_rect_t;
#define MAX_DIRTY_RECTS 128
static dirty_rect_t g_dirty_rects[MAX_DIRTY_RECTS];
static int g_dirty_count = 0;
// VSync toggle (1 by default to reduce tearing)
static int g_vsync_enabled = 1;
// Dirty strategy: 0=multi-rect smart merge, 1=collapse to single bounding rect per swap
static int g_dirty_strategy = 0;

// 5-bit and 6-bit to 8-bit expansion lookup tables (initialized on first use)
static int g_565_lut_init = 0;
static uint8_t g_lut5[32];
static uint8_t g_lut6[64];

static void vga_init_565_luts(void) {
	if (g_565_lut_init) return;
	for (int i = 0; i < 32; ++i) g_lut5[i] = (uint8_t)((i * 255 + 15) / 31);
	for (int i = 0; i < 64; ++i) g_lut6[i] = (uint8_t)((i * 255 + 31) / 63);
	g_565_lut_init = 1;
}

// Shell redirection globals
int shell_redirect_active = 0;
// Global capture mode to force capture during command substitution
int g_shell_capture_mode = 0;
char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];
int shell_redirect_pos = 0;
// Colour for redirected output
int shell_redirect_colour_r = 255;
int shell_redirect_colour_g = 255;
int shell_redirect_colour_b = 255;
// Per-char redirect colours parallel to shell_redirect_buf
unsigned char shell_redirect_r[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_g[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_b[SHELL_REDIRECT_BUF_SIZE];

// Icon markers recorded during shell redirect
shell_redirect_icon_t shell_redirect_icons[SHELL_REDIRECT_ICON_MAX];
int shell_redirect_icon_count = 0;

// Dynamic buffer sizing based on available memory
char* shell_log_buf = NULL;
int shell_log_buf_size = 0;
int shell_log_pos = 0;
int shell_log_line_count = 0;  // Track number of lines
int shell_log_line_starts[1001];  // Track start positions of last 1000 lines
int shell_log_current_line_start = 0;  // Start of current line
int shell_log_active = 0;

// Initialize dynamic log buffer based on available memory
void init_dynamic_log_buffer() {
    if (shell_log_buf != NULL) return; // Already initialized
	if (!vga_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) return;
    
    // Detect available memory and set appropriate buffer size
    uint32_t available_memory = 32 * 1024 * 1024; // Default assumption
    
    if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        // Calculate total available memory from memory map
        uint32_t total_ram = 0;
		multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)vga_u32_to_ptr(g_mbi->mmap_addr);
        uint32_t entries = g_mbi->mmap_length / sizeof(multiboot_memory_map_t);
        
        for (uint32_t i = 0; i < entries && i < 50; i++) {
            if (mmap[i].type == MULTIBOOT_MEMORY_AVAILABLE) {
                total_ram += mmap[i].len;
            }
        }
        
        if (total_ram > 0) {
            available_memory = total_ram;
        }
    }
    
    // Set buffer size based on available memory
    if (available_memory < 8 * 1024 * 1024) {        // Less than 8MB
        shell_log_buf_size = 4096;                   // 4KB buffer
    } else if (available_memory < 32 * 1024 * 1024) { // Less than 32MB
        shell_log_buf_size = 16384;                  // 16KB buffer
    } else if (available_memory < 128 * 1024 * 1024) { // Less than 128MB
        shell_log_buf_size = 32768;                  // 32KB buffer
    } else {                                          // 128MB+
        shell_log_buf_size = 65536;                  // 64KB buffer
    }
    
    // Allocate the buffer
    shell_log_buf = (char*)malloc(shell_log_buf_size);
    if (!shell_log_buf) {
        // Fallback to static buffer if allocation fails
        shell_log_buf_size = 4096;
        shell_log_buf = (char*)malloc(shell_log_buf_size);
        if (!shell_log_buf) {
            printf("%cWarning: Failed to allocate log buffer\n", 255, 165, 0);
            shell_log_buf_size = 0;
        }
    }
    
    shell_log_pos = 0;
    shell_log_line_count = 0;
    shell_log_current_line_start = 0;
}

void shell_log_enable() {
	if (!vga_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return;
	shell_log_active = 1;
}
void shell_log_disable() { shell_log_active = 0; }
int shell_log_is_enabled(void) { return shell_log_active ? 1 : 0; }

void shell_log_flush() {
	if (!vga_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) return;
    // Initialize dynamic buffer if not already done
    init_dynamic_log_buffer();
    
    if (shell_log_pos == 0 || !shell_log_buf) return;
    
    // Memory safety: limit log file size to prevent excessive allocation
    if (shell_log_pos > shell_log_buf_size) {
        printf("%cWarning: Log buffer too large (%d bytes), truncating to %d bytes\n", 255, 165, 0, shell_log_pos, shell_log_buf_size);
        shell_log_pos = shell_log_buf_size;
    }
    
    eynfs_superblock_t sb;
    if (eynfs_read_superblock(0, 2048, &sb) != 0 || sb.magic != EYNFS_MAGIC) {
        printf("%cWarning: Failed to read filesystem for logging\n", 255, 165, 0);
        return;
    }
    
    eynfs_dir_entry_t entry;
    uint32_t entry_idx;
    if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, "log", &entry, &entry_idx) != 0) {
        if (eynfs_create_entry(0, &sb, sb.root_dir_block, "log", EYNFS_TYPE_FILE) != 0) {
            printf("%cWarning: Failed to create log file\n", 255, 165, 0);
            return;
        }
        if (eynfs_find_in_dir(0, &sb, sb.root_dir_block, "log", &entry, &entry_idx) != 0) {
            printf("%cWarning: Failed to find created log file\n", 255, 165, 0);
            return;
        }
    }
    
    int old_size = entry.size;
    
    // Memory safety: limit total allocation size
    size_t total_size = old_size + shell_log_pos;
    size_t max_log_size = (shell_log_buf_size > 8192) ? 8192 : shell_log_buf_size;
    if (total_size > max_log_size) { // Dynamic limit based on buffer size
        printf("%cWarning: Log operation too large (%d bytes), limiting to %d bytes\n", 255, 165, 0, total_size, max_log_size);
        if ((size_t)old_size > max_log_size / 2) old_size = (int)(max_log_size / 2);
        if (shell_log_pos > (int)shell_log_buf_size) shell_log_pos = shell_log_buf_size;
        total_size = old_size + shell_log_pos;
    }
    
    char* newbuf = (char*)malloc(total_size);
    if (!newbuf) {
        printf("%cWarning: Out of memory for log operation\n", 255, 165, 0);
        return;
    }
    
    int n = 0;
    if (old_size > 0) n = eynfs_read_file(0, &sb, &entry, newbuf, old_size, 0);
    if (n < 0) n = 0;
    
    // Bounds check for memory copy
    if ((size_t)(n + shell_log_pos) <= total_size) {
        memcpy(newbuf + n, shell_log_buf, shell_log_pos);
    } else {
        printf("%cWarning: Log buffer overflow prevented\n", 255, 165, 0);
        free(newbuf);
        return;
    }
    
    int written = eynfs_write_file(0, &sb, &entry, newbuf, n + shell_log_pos, sb.root_dir_block, entry_idx);
    free(newbuf);
    
    if (written < 0) {
        printf("%cWarning: Failed to write log file\n", 255, 165, 0);
    }
    
    shell_log_pos = 0;
}

void drawRect(int x, int y, int w, int h, int r, int g, int b)
{
	// Clip rectangle to framebuffer/backbuffer bounds
	if (w <= 0 || h <= 0) { return; }
	int sw = g_mbi ? (int)g_mbi->framebuffer_width : 0;
	int sh = g_mbi ? (int)g_mbi->framebuffer_height : 0;
	if (x >= sw || y >= sh) { return; }
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) { w = sw - x; }
	if (y + h > sh) { h = sh - y; }
	if (w <= 0 || h <= 0) { return; }

	// If a backbuffer is available, draw into it; otherwise draw directly to framebuffer
	if (g_backbuffer && g_backbuffer_w >= sw && g_backbuffer_h >= sh) {
		// 32-bit packed colour write for speed
		uint32_t packed = ((uint32_t)0 << 24) | ((uint32_t)(unsigned char)r << 16) | ((uint32_t)(unsigned char)g << 8) | (uint32_t)(unsigned char)b;
		for (int i = 0; i < h; i++) {
			uint32_t* row = (uint32_t*)(g_backbuffer + ((size_t)(y + i) * g_backbuffer_w + x) * 4);
			for (int j = 0; j < w; j++) row[j] = packed;
		}
		// mark region dirty
		vga_mark_dirty_rect(x, y, w, h);
		return;
	}
	// fallback to drawing directly to framebuffer
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (bpp < 3) bpp = 3;
	for (int i = 0; i < h; i++) {
		unsigned char *row = video + (y + i) * pitch + x * bpp;
		for (int j = 0; j < w; j++) {
			row[j*bpp + 0] = (unsigned char)b;
			row[j*bpp + 1] = (unsigned char)g;
			row[j*bpp + 2] = (unsigned char)r;
			if (bpp >= 4) row[j*bpp + 3] = 0xFF;
		}
	}
}

// Multiply-darken a rectangle in the backbuffer (or framebuffer if no backbuffer)
// factor is 0..255 where 255 leaves image unchanged and 128 is roughly 50% darken.
void vga_darkenRect_bb(int x, int y, int w, int h, int factor)
{
	if (w <= 0 || h <= 0) return;
	if (factor < 0) factor = 0;
	if (factor > 255) factor = 255;
	int sw = g_mbi ? (int)g_mbi->framebuffer_width : 0;
	int sh = g_mbi ? (int)g_mbi->framebuffer_height : 0;
	if (x >= sw || y >= sh) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;
	if (w <= 0 || h <= 0) return;

	// Prefer operating on the backbuffer so swap can blit it
	if (g_backbuffer && g_backbuffer_w >= sw && g_backbuffer_h >= sh) {
		// Backbuffer is 32bpp RGBA
		for (int yy = 0; yy < h; ++yy) {
			uint8_t* row = g_backbuffer + ((size_t)(y + yy) * g_backbuffer_w + x) * 4;
			for (int xx = 0; xx < w; ++xx) {
				uint8_t* p = row + xx * 4;
				// p[0]=B p[1]=G p[2]=R p[3]=A (we keep A as-is)
				p[0] = (uint8_t)((p[0] * factor + 127) / 255);
				p[1] = (uint8_t)((p[1] * factor + 127) / 255);
				p[2] = (uint8_t)((p[2] * factor + 127) / 255);
			}
		}
		vga_mark_dirty_rect(x, y, w, h);
		return;
	}

	// Fallback: operate directly on framebuffer
	if (!g_mbi) return;
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8; if (bpp < 3) bpp = 3;
	for (int yy = 0; yy < h; ++yy) {
		unsigned char* row = video + (y + yy) * pitch + x * bpp;
		for (int xx = 0; xx < w; ++xx) {
			unsigned char* p = row + xx * bpp;
			p[0] = (uint8_t)((p[0] * factor + 127) / 255);
			p[1] = (uint8_t)((p[1] * factor + 127) / 255);
			p[2] = (uint8_t)((p[2] * factor + 127) / 255);
			if (bpp >= 4) { /* preserve alpha */ }
		}
	}
}


static const unsigned char* vga_builtin_font(void) {
	// Built-in 8x8 font. Used as a safe fallback if font loading fails.
	static const unsigned char font[2057] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x7E, 0x81, 0xA5, 0x81, 0xBD, 0x99, 0x81, 0x7E,
		0x7E, 0xFF, 0xDB, 0xFF, 0xC3, 0xE7, 0xFF, 0x7E,
		0x6C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0x00,
		0x10, 0x38, 0x7C, 0xFE, 0x7C, 0x38, 0x10, 0x00,
		0x38, 0x7C, 0x38, 0xFE, 0xFE, 0xD6, 0x10, 0x38,
		0x10, 0x10, 0x38, 0x7C, 0xFE, 0x7C, 0x10, 0x38,
		0x00, 0x00, 0x18, 0x3C, 0x3C, 0x18, 0x00, 0x00,
		0xFF, 0xFF, 0xE7, 0xC3, 0xC3, 0xE7, 0xFF, 0xFF,
		0x00, 0x3C, 0x66, 0x42, 0x42, 0x66, 0x3C, 0x00,
		0xFF, 0xC3, 0x99, 0xBD, 0xBD, 0x99, 0xC3, 0xFF,
		0x0F, 0x07, 0x0F, 0x7D, 0xCC, 0xCC, 0xCC, 0x78,
		0x3C, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x7E, 0x18,
		0x3F, 0x33, 0x3F, 0x30, 0x30, 0x70, 0xF0, 0xE0,
		0x7F, 0x63, 0x7F, 0x63, 0x63, 0x67, 0xE6, 0xC0,
		0x18, 0xDB, 0x3C, 0xE7, 0xE7, 0x3C, 0xDB, 0x18,
		0x80, 0xE0, 0xF8, 0xFE, 0xF8, 0xE0, 0x80, 0x00,
		0x02, 0x0E, 0x3E, 0xFE, 0x3E, 0x0E, 0x02, 0x00,
		0x18, 0x3C, 0x7E, 0x18, 0x18, 0x7E, 0x3C, 0x18,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x66, 0x00,
		0x7F, 0xDB, 0xDB, 0x7B, 0x1B, 0x1B, 0x1B, 0x00,
		0x3E, 0x63, 0x38, 0x6C, 0x6C, 0x38, 0xCC, 0x78,
		0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x7E, 0x00,
		0x18, 0x3C, 0x7E, 0x18, 0x7E, 0x3C, 0x18, 0xFF,
		0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x3C, 0x18, 0x00, 0x00, 0x18, 0x0C, 0xFE, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x30, 0x60, 0xFE, 0x60, 0x30, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xFE, 0x00, 0x00, 0x00, 0x24, 0x66, 0xFF, 0x66, 0x24, 0x00, 0x00, 0x00, 0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x78, 0x78, 0x30, 0x30, 0x00, 0x30, 0x00, 0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00, 0x30, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x30, 0x00, 0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00, 0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00, 0x60, 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x30, 0x60, 0x60, 0x60, 0x30, 0x18, 0x00, 0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00, 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00, 0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00, 0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00, 0x78, 0xCC, 0x0C, 0x38, 0x60, 0xCC, 0xFC, 0x00, 0x78, 0xCC, 0x0C, 0x38, 0x0C, 0xCC, 0x78, 0x00, 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00, 0xFC, 0xC0, 0xF8, 0x0C, 0x0C, 0xCC, 0x78, 0x00, 0x38, 0x60, 0xC0, 0xF8, 0xCC, 0xCC, 0x78, 0x00, 0xFC, 0xCC, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00, 0x78, 0xCC, 0xCC, 0x78, 0xCC, 0xCC, 0x78, 0x00, 0x78, 0xCC, 0xCC, 0x7C, 0x0C, 0x18, 0x70, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x60, 0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x78, 0xCC, 0x0C, 0x18, 0x30, 0x00, 0x30, 0x00, 0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00, 0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00, 0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00, 0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00, 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00, 0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00, 0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00, 0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00, 0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00, 0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00, 0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00, 0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00, 0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00, 0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00, 0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00, 0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0xDC, 0x78, 0x1C, 0x00, 0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00, 0x78, 0xCC, 0x60, 0x30, 0x18, 0xCC, 0x78, 0x00, 0xFC, 0xB4, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFC, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00, 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00, 0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x30, 0x78, 0x00, 0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00, 0x78, 0x60, 0x60, 0x60, 0x60, 0x60, 0x78, 0x00, 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x76, 0x00, 0xE0, 0x60, 0x60, 0x7C, 0x66, 0x66, 0xDC, 0x00, 0x00, 0x00, 0x78, 0xCC, 0xC0, 0xCC, 0x78, 0x00, 0x1C, 0x0C, 0x0C, 0x7C, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00, 0x38, 0x6C, 0x60, 0xF0, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8, 0xE0, 0x60, 0x6C, 0x76, 0x66, 0x66, 0xE6, 0x00, 0x30, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00, 0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0xE0, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0xE6, 0x00, 0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, 0x00, 0x00, 0xCC, 0xFE, 0xFE, 0xD6, 0xC6, 0x00, 0x00, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0x00, 0xDC, 0x66, 0x66, 0x7C, 0x60, 0xF0, 0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0x1E, 0x00, 0x00, 0xDC, 0x76, 0x66, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x00, 0x10, 0x30, 0x7C, 0x30, 0x30, 0x34, 0x18, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00, 0x00, 0x00, 0xC6, 0xD6, 0xFE, 0xFE, 0x6C, 0x00, 0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8, 0x00, 0x00, 0xFC, 0x98, 0x30, 0x64, 0xFC, 0x00, 0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00, 0xE0, 0x30, 0x30, 0x1C, 0x30, 0x30, 0xE0, 0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0x00, 0x78, 0xCC, 0xC0, 0xCC, 0x78, 0x18, 0x0C, 0x78, 0x00, 0xCC, 0x00, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x1C, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00, 0x7E, 0xC3, 0x3C, 0x06, 0x3E, 0x66, 0x3F, 0x00, 0xCC, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x7E, 0x00, 0xE0, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x7E, 0x00, 0x30, 0x30, 0x78, 0x0C, 0x7C, 0xCC, 0x7E, 0x00, 0x00, 0x00, 0x78, 0xC0, 0xC0, 0x78, 0x0C, 0x38, 0x7E, 0xC3, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00, 0xCC, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00, 0xE0, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00, 0xCC, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00, 0x7C, 0xC6, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00, 0xE0, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00, 0xC6, 0x38, 0x6C, 0xC6, 0xFE, 0xC6, 0xC6, 0x00, 0x30, 0x30, 0x00, 0x78, 0xCC, 0xFC, 0xCC, 0x00, 0x1C, 0x00, 0xFC, 0x60, 0x78, 0x60, 0xFC, 0x00, 0x00, 0x00, 0x7F, 0x0C, 0x7F, 0xCC, 0x7F, 0x00, 0x3E, 0x6C, 0xCC, 0xFE, 0xCC, 0xCC, 0xCE, 0x00, 0x78, 0xCC, 0x00, 0x78, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0xCC, 0x00, 0x78, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0xE0, 0x00, 0x78, 0xCC, 0xCC, 0x78, 0x00, 0x78, 0xCC, 0x00, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x00, 0xE0, 0x00, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x00, 0xCC, 0x00, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8, 0xC3, 0x18, 0x3C, 0x66, 0x66, 0x3C, 0x18, 0x00, 0xCC, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x00, 0x18, 0x18, 0x7E, 0xC0, 0xC0, 0x7E, 0x18, 0x18, 0x38, 0x6C, 0x64, 0xF0, 0x60, 0xE6, 0xFC, 0x00, 0xCC, 0xCC, 0x78, 0xFC, 0x30, 0xFC, 0x30, 0x30, 0xF8, 0xCC, 0xCC, 0xFA, 0xC6, 0xCF, 0xC6, 0xC7, 0x0E, 0x1B, 0x18, 0x3C, 0x18, 0x18, 0xD8, 0x70, 0x1C, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x7E, 0x00, 0x38, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00, 0x00, 0x1C, 0x00, 0x78, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0x1C, 0x00, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x00, 0xF8, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0x00, 0xFC, 0x00, 0xCC, 0xEC, 0xFC, 0xDC, 0xCC, 0x00, 0x3C, 0x6C, 0x6C, 0x3E, 0x00, 0x7E, 0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x00, 0x7C, 0x00, 0x00, 0x30, 0x00, 0x30, 0x60, 0xC0, 0xCC, 0x78, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xC0, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x0C, 0x0C, 0x00, 0x00, 0xC3, 0xC6, 0xCC, 0xDE, 0x33, 0x66, 0xCC, 0x0F, 0xC3, 0xC6, 0xCC, 0xDB, 0x37, 0x6F, 0xCF, 0x03, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x33, 0x66, 0xCC, 0x66, 0x33, 0x00, 0x00, 0x00, 0xCC, 0x66, 0x33, 0x66, 0xCC, 0x00, 0x00, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0xDB, 0x77, 0xDB, 0xEE, 0xDB, 0x77, 0xDB, 0xEE, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xF8, 0x18, 0x18, 0x18, 0x18, 0x18, 0xF8, 0x18, 0xF8, 0x18, 0x18, 0x18, 0x36, 0x36, 0x36, 0x36, 0xF6, 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x36, 0x36, 0x36, 0x00, 0x00, 0xF8, 0x18, 0xF8, 0x18, 0x18, 0x18, 0x36, 0x36, 0xF6, 0x06, 0xF6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x00, 0x00, 0xFE, 0x06, 0xF6, 0x36, 0x36, 0x36, 0x36, 0x36, 0xF6, 0x06, 0xFE, 0x00, 0x00, 0x00, 0x36, 0x36, 0x36, 0x36, 0xFE, 0x00, 0x00, 0x00, 0x18, 0x18, 0xF8, 0x18, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0xFF, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x18, 0x1F, 0x18, 0x18, 0x18, 0x36, 0x36, 0x36, 0x36, 0x37, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x30, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x30, 0x37, 0x36, 0x36, 0x36, 0x36, 0x36, 0xF7, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xF7, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x30, 0x37, 0x36, 0x36, 0x36, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x36, 0x36, 0xF7, 0x00, 0xF7, 0x36, 0x36, 0x36, 0x18, 0x18, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x36, 0x36, 0x36, 0x36, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x3F, 0x00, 0x00, 0x00, 0x18, 0x18, 0x1F, 0x18, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x18, 0x1F, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xFF, 0x36, 0x36, 0x36, 0x18, 0x18, 0xFF, 0x18, 0xFF, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x18, 0x18, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xDC, 0xC8, 0xDC, 0x76, 0x00, 0x00, 0x78, 0xCC, 0xF8, 0xCC, 0xF8, 0xC0, 0xC0, 0x00, 0xFC, 0xCC, 0xC0, 0xC0, 0xC0, 0xC0, 0x00, 0x00, 0xFE, 0x6C, 0x6C, 0x6C, 0x6C, 0x6C, 0x00, 0xFC, 0xCC, 0x60, 0x30, 0x60, 0xCC, 0xFC, 0x00, 0x00, 0x00, 0x7E, 0xD8, 0xD8, 0xD8, 0x70, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x60, 0xC0, 0x00, 0x76, 0xDC, 0x18, 0x18, 0x18, 0x18, 0x00, 0xFC, 0x30, 0x78, 0xCC, 0xCC, 0x78, 0x30, 0xFC, 0x38, 0x6C, 0xC6, 0xFE, 0xC6, 0x6C, 0x38, 0x00, 0x38, 0x6C, 0xC6, 0xC6, 0x6C, 0x6C, 0xEE, 0x00, 0x1C, 0x30, 0x18, 0x7C, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0x00, 0x7E, 0xDB, 0xDB, 0x7E, 0x00, 0x00, 0x06, 0x0C, 0x7E, 0xDB, 0xDB, 0x7E, 0x60, 0xC0, 0x38, 0x60, 0xC0, 0xF8, 0xC0, 0x60, 0x38, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0x00, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x00, 0xFC, 0x00, 0x60, 0x30, 0x18, 0x30, 0x60, 0x00, 0xFC, 0x00, 0x18, 0x30, 0x60, 0x30, 0x18, 0x00, 0xFC, 0x00, 0x0E, 0x1B, 0x1B, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xD8, 0xD8, 0x70, 0x30, 0x30, 0x00, 0xFC, 0x00, 0x30, 0x30, 0x00, 0x00, 0x76, 0xDC, 0x00, 0x76, 0xDC, 0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0F, 0x0C, 0x0C, 0x0C, 0xEC, 0x6C, 0x3C, 0x1C, 0x78, 0x6C, 0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x70, 0x18, 0x30, 0x60, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x3C, 0x3C, 0x3C, 0x00, 0x00,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	return font;
}

static void vga_console_scroll_one_line(int line_h) {
	if (!g_mbi) return;
	int fb_h = (int)g_mbi->framebuffer_height;
	if (fb_h <= 0) return;
	if (line_h <= 0) line_h = 8;
	if (line_h >= fb_h) {
		clearScreen();
		return;
	}

	int pitch = (int)g_mbi->framebuffer_pitch;
	if (pitch <= 0) return;

	int src_off = line_h * pitch;
	int move_bytes = (fb_h - line_h) * pitch;

	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	if (fb) {
		memmove(fb, fb + src_off, (size_t)move_bytes);
		memset(fb + move_bytes, 0, (size_t)(line_h * pitch));
	}

	if (g_backbuffer && g_backbuffer_w == (int)g_mbi->framebuffer_width && g_backbuffer_h == fb_h) {
		memmove(g_backbuffer, g_backbuffer + src_off, (size_t)move_bytes);
		memset(g_backbuffer + move_bytes, 0, (size_t)(line_h * pitch));
	}

	width = 0;
	height = fb_h - line_h;
	if (height < 0) height = 0;
}

void drawText(int charnum, int r, int g, int b)
{
	int font_handle = vga_get_system_font_handle_raw();
	int glyph_h = (int)vga_font_glyph_h_or_builtin(font_handle);
	int line_h = (int)vga_font_line_h_or_builtin(font_handle);
	int nominal_w = (int)vga_font_advance_or_builtin(font_handle, 'W');
	if (nominal_w <= 0) nominal_w = VGA_FONT_DEFAULT_ADVANCE;

	// Minimal side-effect-free path for drawCharAt to prevent console scrolling/wrapping side effects.
	if (g_drawCharAt_mode) {
		if (charnum >= 0) {
			(void)vga_draw_glyph8xN_at(font_handle, width, height, charnum, r, g, b);
		}
		return;
	}

	// Erase software cursor before making changes (framebuffer console)
	if (g_console_cursor_visible && g_mbi && g_mbi->framebuffer_addr) {
		int cursor_h = (int)vga_font_line_h_or_builtin(font_handle);
		drawRect(width, height, g_console_cursor_w, cursor_h, 0, 0, 0);
	}
	
	// If shell output is being redirected (or forced capture), capture characters into the redirect buffer
	if (shell_redirect_active || g_shell_capture_mode) {
		// Only handle printable ASCII and newline/backspace; ignore graphic-only ops (charnum == -1)
		if (charnum >= 0 && shell_redirect_pos < SHELL_REDIRECT_BUF_SIZE - 1) {
			char ch = (char)charnum;
			shell_redirect_buf[shell_redirect_pos] = ch;
			shell_redirect_r[shell_redirect_pos] = (unsigned char)r;
			shell_redirect_g[shell_redirect_pos] = (unsigned char)g;
			shell_redirect_b[shell_redirect_pos] = (unsigned char)b;
			shell_redirect_pos++;
			shell_redirect_buf[shell_redirect_pos] = '\0';
		} else if (charnum == 10 && shell_redirect_pos < SHELL_REDIRECT_BUF_SIZE - 1) { // newline
			shell_redirect_buf[shell_redirect_pos] = '\n';
			shell_redirect_r[shell_redirect_pos] = (unsigned char)r;
			shell_redirect_g[shell_redirect_pos] = (unsigned char)g;
			shell_redirect_b[shell_redirect_pos] = (unsigned char)b;
			shell_redirect_pos++;
			shell_redirect_buf[shell_redirect_pos] = '\0';
		}
		return;
	}


	// charnum = charnum + 1;
	// Moving the cursor to the next line when we reached the end of the existing line
	if (width > (int)(g_mbi->framebuffer_width - nominal_w - 4))
    {
		width = 0;
		height = height + line_h;
	}
	
	if (width < -2 && height > 34)
    {
		width = g_mbi->framebuffer_width - 30;
		height = height - line_h;
	}
	
	if (charnum == -1)
    {
		drawRect(width, height, nominal_w, line_h, 0, 0, 0);
		width = width + nominal_w;
	}

	else if (charnum==10) // enter
    {
		width = 0;
		height = height + line_h;
	}

	else if (charnum== 8) // backspace
    {
        if (width > 0) {  // Only backspace if we're not at the start of a line
			width = width - nominal_w;
			if (width < 0) width = 0;
            // Draw a black rectangle to erase the previous character
			drawRect(width, height, nominal_w, line_h, 0, 0, 0);
        }
    }

	else // drawing characters in 8x8
    {
		int adv = vga_draw_glyph8xN_at(font_handle, width, height, charnum, r, g, b);
		if (adv <= 0) adv = nominal_w;
		width = width + adv;
	}

	if (height > (int)(g_mbi->framebuffer_height - line_h)) {
		vga_console_scroll_one_line(line_h);
	}

	// Draw software cursor after text operations (framebuffer console)
	if (g_console_cursor_visible && g_mbi && g_mbi->framebuffer_addr) {
		int cursor_h = (int)vga_font_line_h_or_builtin(font_handle);
		drawRect(width, height, g_console_cursor_w, cursor_h, vga_default_r, vga_default_g, vga_default_b);
	}
}

// Kernel printf formatter.
// Supports: %s %c %% %d %i %u %x %X %p and basic length modifiers: l, ll, z.
// Also supports zero-padded widths like %08X.
static int vga_format_to_buffer(char* out, int out_cap, const char* fmt, va_list ap_in) {
	if (!out || out_cap <= 0) return 0;

	int pos = 0;
	va_list ap;
	va_copy(ap, ap_in);

	const char* p = fmt;
	while (p && *p && pos < out_cap - 1) {
		if (*p != '%') {
			out[pos++] = *p++;
			continue;
		}
		++p;
		if (!*p) break;

		int zero_pad = 0;
		int field_width = 0;
		if (*p == '0') {
			zero_pad = 1;
			++p;
		}
		while (*p >= '0' && *p <= '9') {
			field_width = (field_width * 10) + (*p - '0');
			++p;
		}
		if (!*p) break;

		enum { LEN_NONE, LEN_L, LEN_LL, LEN_Z } len = LEN_NONE;
		if (*p == 'l') {
			len = LEN_L;
			++p;
			if (*p == 'l') {
				len = LEN_LL;
				++p;
			}
		} else if (*p == 'z') {
			len = LEN_Z;
			++p;
		}
		if (!*p) break;

		switch (*p) {
			case 's': {
				char* str = va_arg(ap, char*);
				if (!str) str = "(null)";
				while (*str && pos < out_cap - 1) out[pos++] = *str++;
				break;
			}
			case 'c': {
				out[pos++] = (char)va_arg(ap, int);
				break;
			}
			case '%': {
				out[pos++] = '%';
				break;
			}
			case 'd':
			case 'i': {
				int v;
				if (len == LEN_LL) v = (int)va_arg(ap, long long);
				else if (len == LEN_L) v = (int)va_arg(ap, long);
				else v = va_arg(ap, int);

				unsigned int uv;
				int neg = 0;
				if (v < 0) {
					neg = 1;
					uv = (unsigned int)(-v);
				} else {
					uv = (unsigned int)v;
				}

				char tmp[16];
				int nd = 0;
				do {
					tmp[nd++] = (char)('0' + (uv % 10U));
					uv /= 10U;
				} while (uv != 0U && nd < (int)sizeof(tmp));

				int content = nd + (neg ? 1 : 0);
				int pad = field_width - content;
				if (!zero_pad) pad = 0;
				if (neg && pos < out_cap - 1) out[pos++] = '-';
				while (pad-- > 0 && pos < out_cap - 1) out[pos++] = '0';
				while (nd-- > 0 && pos < out_cap - 1) out[pos++] = tmp[nd];
				break;
			}
			case 'u': {
				unsigned int uv;
				if (len == LEN_LL) uv = (unsigned int)va_arg(ap, unsigned long long);
				else if (len == LEN_L) uv = (unsigned int)va_arg(ap, unsigned long);
				else if (len == LEN_Z) uv = (unsigned int)va_arg(ap, size_t);
				else uv = va_arg(ap, unsigned int);

				char tmp[16];
				int nd = 0;
				do {
					tmp[nd++] = (char)('0' + (uv % 10U));
					uv /= 10U;
				} while (uv != 0U && nd < (int)sizeof(tmp));

				int pad = field_width - nd;
				if (!zero_pad) pad = 0;
				while (pad-- > 0 && pos < out_cap - 1) out[pos++] = '0';
				while (nd-- > 0 && pos < out_cap - 1) out[pos++] = tmp[nd];
				break;
			}
			case 'x':
			case 'X': {
				if (len == LEN_LL) {
					unsigned long long uv = va_arg(ap, unsigned long long);
					int started = 0;
					int nd = 0;
					for (int i = 15; i >= 0 && pos < out_cap - 1; --i) {
						unsigned int d = (unsigned int)((uv >> (i * 4)) & 0xFULL);
						if (!started && d == 0 && i != 0) continue;
						started = 1;
						char c;
						if (d < 10) c = (char)('0' + d);
						else c = (char)((*p == 'X' ? 'A' : 'a') + (d - 10));
						out[pos++] = c;
						nd++;
					}
					(void)nd;
				} else {
					unsigned int uv;
					if (len == LEN_L) uv = (unsigned int)va_arg(ap, unsigned long);
					else if (len == LEN_Z) uv = (unsigned int)va_arg(ap, size_t);
					else uv = va_arg(ap, unsigned int);

					char tmp[9];
					int nd = 0;
					do {
						unsigned int d = uv & 0xF;
						char c;
						if (d < 10) c = (char)('0' + d);
						else c = (char)((*p == 'X' ? 'A' : 'a') + (d - 10));
						tmp[nd++] = c;
						uv >>= 4;
					} while (uv != 0 && nd < 8);

					if (field_width > 8) field_width = 8;
					int pad = field_width - nd;
					if (!zero_pad) pad = 0;
					while (pad-- > 0 && pos < out_cap - 1) out[pos++] = '0';
					while (nd-- > 0 && pos < out_cap - 1) out[pos++] = tmp[nd];
				}
				break;
			}
			case 'p': {
				uintptr_t uv = (uintptr_t)va_arg(ap, void*);
				if (pos < out_cap - 1) out[pos++] = '0';
				if (pos < out_cap - 1) out[pos++] = 'x';
				for (int i = 7; i >= 0 && pos < out_cap - 1; --i) {
					unsigned int d = (unsigned int)((uv >> (i * 4)) & 0xF);
					out[pos++] = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
				}
				break;
			}
			default: {
				// Unknown specifier: preserve it literally to aid debugging.
				out[pos++] = '%';
				if (pos < out_cap - 1) out[pos++] = *p;
				break;
			}
		}

		++p;
	}

	out[pos] = '\0';
	va_end(ap);
	return pos;
}

void printf(const char* format, ...) 
{
	command_context_t* ctx = current_command_context;
	if (ctx && !cap_check(ctx->caps, CAP_WRITE_CONSOLE)) {
		return;
	}
	if (ctx) {
		scheduler_account(ctx->wo, SCHED_COST_CONSOLE);
		scheduler_yield_if_needed(ctx->wo);
		if (sched_det_is_enabled()) ctx->det_seq++;
	}

	va_list ap;
	va_start(ap, format);

	uint8 *ptr;
	static int r = 255, g = 255, b = 255; // Default to white, static to maintain state

	// Reset colour to white at the start of each printf call
	r = 255;
	g = 255;
	b = 255;

	// Check if colour parameters are provided
	if (format[0] == '%' && format[1] == 'c') {
		r = va_arg(ap, int);
		g = va_arg(ap, int);
		b = va_arg(ap, int);
		format += 2; // Skip the colour format specifier
	}

	// Redirection/capture logic
	if (shell_redirect_active || g_shell_capture_mode) {
		char temp[512];
		int temp_pos = vga_format_to_buffer(temp, (int)sizeof(temp), format, ap);
		
		// record colour for redirected output so callers can render it with the same colour
		shell_redirect_colour_r = r;
		shell_redirect_colour_g = g;
		shell_redirect_colour_b = b;
		// Append to the global buffer (with bounds checking)
		int to_copy = temp_pos;
		if (shell_redirect_pos + to_copy >= SHELL_REDIRECT_BUF_SIZE - 1) {
			to_copy = SHELL_REDIRECT_BUF_SIZE - shell_redirect_pos - 1;
		}
		// store per-char colour metadata for the temp buffer (for the actual copied count)
		for (int i = 0; i < to_copy; ++i) {
			shell_redirect_r[shell_redirect_pos + i] = (unsigned char)r;
			shell_redirect_g[shell_redirect_pos + i] = (unsigned char)g;
			shell_redirect_b[shell_redirect_pos + i] = (unsigned char)b;
		}
		for (int i = 0; i < to_copy; ++i) {
			shell_redirect_buf[shell_redirect_pos++] = temp[i];
		}
		shell_redirect_buf[shell_redirect_pos] = '\0';
		// Also mirror to serial without colours
		for (int i = 0; i < to_copy; ++i) {
			serial_write_char(SERIAL_COM1, temp[i]);
		}
		va_end(ap);
		return;
	}

	// Logging logic
	if (shell_log_active) {
		if (!vga_ctx_allow(CAP_READ_FS | CAP_WRITE_FS | CAP_ALLOC_MEMORY, SCHED_COST_FS)) {
			shell_log_active = 0;
		} else {
		va_list ap_log;
		va_copy(ap_log, ap);
		char temp[512];
		int temp_pos = vga_format_to_buffer(temp, (int)sizeof(temp), format, ap_log);
		// Append to the log buffer
		for (int i = 0; i < temp_pos; ++i) {
			if (shell_log_pos < shell_log_buf_size - 1) {
				// Check if we need to start a new line
				if (shell_log_pos == 0 || shell_log_buf[shell_log_pos - 1] == '\n') {
					shell_log_current_line_start = shell_log_pos;
				}
				
				shell_log_buf[shell_log_pos++] = temp[i];
				
				// If we just added a newline, record the line start
				if (temp[i] == '\n') {
					shell_log_line_starts[shell_log_line_count] = shell_log_current_line_start;
					shell_log_line_count++;
					
					// Keep only last 1000 lines
					if (shell_log_line_count > 1000) {
						// Move buffer content to start, keeping only last 1000 lines
						int first_line_start = shell_log_line_starts[1];
						int bytes_to_keep = shell_log_pos - first_line_start;
						
						if (bytes_to_keep > 0 && first_line_start < shell_log_pos) {
							                                                        memmove(shell_log_buf, shell_log_buf + first_line_start, bytes_to_keep);
							shell_log_pos = bytes_to_keep;
							
							// Adjust line start positions
							for (int j = 0; j < 1000; j++) {
								shell_log_line_starts[j] = shell_log_line_starts[j + 1] - first_line_start;
							}
							shell_log_line_count = 1000;
						}
					}
				}
			}
			if (temp[i] == '\n') {
				shell_log_buf[shell_log_pos] = '\0';
				shell_log_flush();
			}
		}
		shell_log_buf[shell_log_pos] = '\0';
		va_end(ap_log);
		}
	}

	// Mirror output to serial as we render to VGA (without colour codes)
	{
		va_list ap_serial;
		va_copy(ap_serial, ap);
		char temp[512];
		int temp_pos = vga_format_to_buffer(temp, (int)sizeof(temp), format, ap_serial);
		for (int i = 0; i < temp_pos; ++i) {
			serial_write_char(SERIAL_COM1, temp[i]);
		}
		va_end(ap_serial);
	}

	// Render to VGA
	{
		char temp[512];
		int temp_pos = vga_format_to_buffer(temp, (int)sizeof(temp), format, ap);
		for (int i = 0; i < temp_pos; ++i) {
			unsigned char ch = (unsigned char)temp[i];
#if CONFIG_TTY_ENABLED
			// In TTY mode, prefer the framebuffer/text-rendering console if a
			// framebuffer is available (this is the same backend used for early
			// boot messages). Fall back to the legacy VGA text driver when no
			// framebuffer is present.
			if (g_boot_text_mode) {
				if (g_mbi && g_mbi->framebuffer_addr) {
					if (ch == (unsigned char)'\n') {
						drawText(10, r, g, b);
					} else {
						drawText((int)ch, r, g, b);
					}
				} else {
					vga_text_putchar((char)ch);
				}
			} else {
				// Graphics mode: render via the framebuffer text path
				if (ch == (unsigned char)'\n') {
					drawText(10, r, g, b);
				} else {
					drawText((int)ch, r, g, b);
				}
			}
#else
			// If TTY isn't enabled at build time, always use the graphics path
			if (ch == (unsigned char)'\n') {
				drawText(10, r, g, b);
			} else {
				drawText((int)ch, r, g, b);
			}
#endif
		}
	}

	va_end(ap);
}

void drawPixel(int x, int y, int r, int g, int b) 
{
	// Bounds check
	if (!g_mbi) return;
	if (x < 0 || y < 0 || x >= (int)g_mbi->framebuffer_width || y >= (int)g_mbi->framebuffer_height) return;

    if (g_backbuffer && g_backbuffer_w >= (int)g_mbi->framebuffer_width && g_backbuffer_h >= (int)g_mbi->framebuffer_height) {
		unsigned int offset = (x + y * g_backbuffer_w) * 4;
		g_backbuffer[offset] = (unsigned char)b;
		g_backbuffer[offset + 1] = (unsigned char)g;
		g_backbuffer[offset + 2] = (unsigned char)r;
		g_backbuffer[offset + 3] = 0;
		// mark region dirty
		vga_mark_dirty_rect(x, y, 1, 1);
		return;
	}

	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!video || pitch <= 0 || bpp <= 0) return;

	unsigned char* p = video + y * pitch + x * bpp;
	if (bpp >= 4) {
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
		p[3] = 0xFF;
	} else if (bpp == 3) {
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
	} else if (bpp == 2) {
		// RGB565 fallback for 16bpp framebuffers.
		uint16_t pix565 = (uint16_t)((((unsigned)r & 0xF8u) << 8) |
		                           (((unsigned)g & 0xFCu) << 3) |
		                           (((unsigned)b & 0xF8u) >> 3));
		p[0] = (unsigned char)(pix565 & 0xFFu);
		p[1] = (unsigned char)((pix565 >> 8) & 0xFFu);
	}
	return;
}

// Write a pixel into the backbuffer only (no dirty mark). Callers should
// mark the affected area once via vga_mark_dirty_rect to avoid per-pixel overhead.
void vga_drawPixel_bb(int x, int y, int rr, int gg, int bb)
{
	if (!g_mbi) return;
	if (x < 0 || y < 0 || x >= (int)g_mbi->framebuffer_width || y >= (int)g_mbi->framebuffer_height) return;

	if (g_backbuffer && g_backbuffer_w >= (int)g_mbi->framebuffer_width && g_backbuffer_h >= (int)g_mbi->framebuffer_height) {
		unsigned int offset = (x + y * g_backbuffer_w) * 4;
		g_backbuffer[offset + 0] = (unsigned char)bb; // B
		g_backbuffer[offset + 1] = (unsigned char)gg; // G
		g_backbuffer[offset + 2] = (unsigned char)rr; // R
		g_backbuffer[offset + 3] = 0;                 // A (unused)
		return;
	}
	// Fallback: draw directly to framebuffer without marking dirty (overlay semantics)
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!video || pitch <= 0 || bpp < 3) return;
	unsigned char* p = video + y * pitch + x * bpp;
	p[0] = (unsigned char)bb;
	p[1] = (unsigned char)gg;
	p[2] = (unsigned char)rr;
	if (bpp >= 4) p[3] = 0xFF;
}

void vga_blit_rgb565_scaled_bb(int dst_x, int dst_y, int dst_w, int dst_h,
	const uint16_t* src, int src_w, int src_h)
{
	if (!src) return;
	if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
	vga_init_565_luts();

	int sw = g_mbi ? (int)g_mbi->framebuffer_width : 0;
	int sh = g_mbi ? (int)g_mbi->framebuffer_height : 0;
	if (sw <= 0 || sh <= 0) return;

	// Clip destination rect to screen bounds
	int x0 = dst_x;
	int y0 = dst_y;
	int x1 = dst_x + dst_w;
	int y1 = dst_y + dst_h;
	if (x1 <= 0 || y1 <= 0 || x0 >= sw || y0 >= sh) return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	int cw = x1 - x0;
	int ch = y1 - y0;
	if (cw <= 0 || ch <= 0) return;

	// Fast path: write directly into the RGBA backbuffer
	if (g_backbuffer && g_backbuffer_w >= sw && g_backbuffer_h >= sh) {
		for (int dy = 0; dy < ch; ++dy) {
			if ((dy & 0x1F) == 0) watchdog_kick("vga-blit");
			int out_y = y0 + dy;
			int rel_y = out_y - dst_y;
			int sy = (rel_y * src_h) / dst_h;
			if (sy < 0) sy = 0;
			if (sy >= src_h) sy = src_h - 1;
			uint8_t* dst_row = g_backbuffer + ((size_t)out_y * (size_t)g_backbuffer_w + (size_t)x0) * 4;
			for (int dx = 0; dx < cw; ++dx) {
				int out_x = x0 + dx;
				int rel_x = out_x - dst_x;
				int sx = (rel_x * src_w) / dst_w;
				if (sx < 0) sx = 0;
				if (sx >= src_w) sx = src_w - 1;
				uint16_t p = src[(size_t)sy * (size_t)src_w + (size_t)sx];
				uint8_t r8 = g_lut5[(p >> 11) & 0x1F];
				uint8_t g8 = g_lut6[(p >> 5) & 0x3F];
				uint8_t b8 = g_lut5[p & 0x1F];
				dst_row[dx * 4 + 0] = b8;
				dst_row[dx * 4 + 1] = g8;
				dst_row[dx * 4 + 2] = r8;
				dst_row[dx * 4 + 3] = 0;
			}
		}
		return;
	}

	// Fallback: write directly to framebuffer
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (bpp < 3) bpp = 3;
	for (int dy = 0; dy < ch; ++dy) {
		if ((dy & 0x1F) == 0) watchdog_kick("vga-blit");
		int out_y = y0 + dy;
		int rel_y = out_y - dst_y;
		int sy = (rel_y * src_h) / dst_h;
		if (sy < 0) sy = 0;
		if (sy >= src_h) sy = src_h - 1;
		unsigned char* dst_row = video + out_y * pitch + x0 * bpp;
		for (int dx = 0; dx < cw; ++dx) {
			int out_x = x0 + dx;
			int rel_x = out_x - dst_x;
			int sx = (rel_x * src_w) / dst_w;
			if (sx < 0) sx = 0;
			if (sx >= src_w) sx = src_w - 1;
			uint16_t p = src[(size_t)sy * (size_t)src_w + (size_t)sx];
			uint8_t r8 = g_lut5[(p >> 11) & 0x1F];
			uint8_t g8 = g_lut6[(p >> 5) & 0x3F];
			uint8_t b8 = g_lut5[p & 0x1F];
			dst_row[dx*bpp + 0] = b8;
			dst_row[dx*bpp + 1] = g8;
			dst_row[dx*bpp + 2] = r8;
			if (bpp >= 4) dst_row[dx*bpp + 3] = 0xFF;
		}
	}
}

void vga_blit_rgb565_bb(int dst_x, int dst_y, const uint16_t* src, int src_w, int src_h)
{
	if (!src) return;
	if (src_w <= 0 || src_h <= 0) return;
	vga_init_565_luts();

	int sw = g_mbi ? (int)g_mbi->framebuffer_width : 0;
	int sh = g_mbi ? (int)g_mbi->framebuffer_height : 0;
	if (sw <= 0 || sh <= 0) return;

	// Clip destination rect
	int x0 = dst_x;
	int y0 = dst_y;
	int x1 = dst_x + src_w;
	int y1 = dst_y + src_h;
	if (x1 <= 0 || y1 <= 0 || x0 >= sw || y0 >= sh) return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	int cw = x1 - x0;
	int ch = y1 - y0;
	if (cw <= 0 || ch <= 0) return;

	int src_x0 = x0 - dst_x;
	int src_y0 = y0 - dst_y;

	if (g_backbuffer && g_backbuffer_w >= sw && g_backbuffer_h >= sh) {
		for (int dy = 0; dy < ch; ++dy) {
			if ((dy & 0x3F) == 0) watchdog_kick("vga-blit");
			int sy = src_y0 + dy;
			const uint16_t* src_row = src + (size_t)sy * (size_t)src_w + (size_t)src_x0;
			uint8_t* dst_row = g_backbuffer + ((size_t)(y0 + dy) * (size_t)g_backbuffer_w + (size_t)x0) * 4;
			for (int dx = 0; dx < cw; ++dx) {
				uint16_t p = src_row[dx];
				dst_row[dx * 4 + 0] = g_lut5[p & 0x1F];
				dst_row[dx * 4 + 1] = g_lut6[(p >> 5) & 0x3F];
				dst_row[dx * 4 + 2] = g_lut5[(p >> 11) & 0x1F];
				dst_row[dx * 4 + 3] = 0;
			}
		}
		return;
	}

	// Fallback: write directly to framebuffer
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (bpp < 3) bpp = 3;
	for (int dy = 0; dy < ch; ++dy) {
		if ((dy & 0x3F) == 0) watchdog_kick("vga-blit");
		int sy = src_y0 + dy;
		const uint16_t* src_row = src + (size_t)sy * (size_t)src_w + (size_t)src_x0;
		unsigned char* dst_row = video + (y0 + dy) * pitch + x0 * bpp;
		for (int dx = 0; dx < cw; ++dx) {
			uint16_t p = src_row[dx];
			uint8_t r8 = g_lut5[(p >> 11) & 0x1F];
			uint8_t g8 = g_lut6[(p >> 5) & 0x3F];
			uint8_t b8 = g_lut5[p & 0x1F];
			dst_row[dx*bpp + 0] = b8;
			dst_row[dx*bpp + 1] = g8;
			dst_row[dx*bpp + 2] = r8;
			if (bpp >= 4) dst_row[dx*bpp + 3] = 0xFF;
		}
	}
}

// Blend an RGBA pixel into the backbuffer. Alpha 0..255 where 0 is transparent.
void vga_blendPixel_bb(int x, int y, int rr, int gg, int bb, int aa)
{
	if (!g_mbi) return;
	int sw = (int)g_mbi->framebuffer_width;
	int sh = (int)g_mbi->framebuffer_height;
	if (x < 0 || y < 0 || x >= sw || y >= sh) return;

	if (g_backbuffer && g_backbuffer_w >= sw && g_backbuffer_h >= sh) {
		uint8_t* p = g_backbuffer + ((size_t)y * g_backbuffer_w + x) * 4;
		// backbuffer layout: B, G, R, A
		uint8_t dst_b = p[0]; uint8_t dst_g = p[1]; uint8_t dst_r = p[2]; uint8_t dst_a = p[3];
		// Normalize alpha to 0..255
		int a = aa; if (a < 0) a = 0; if (a > 255) a = 255;
	// Composite: out = src * (a/255) + dst * (1 - a/255)
	// backbuffer layout is B, G, R so map src channels accordingly
	p[0] = (uint8_t)((bb * a + dst_b * (255 - a)) / 255);
	p[1] = (uint8_t)((gg * a + dst_g * (255 - a)) / 255);
	p[2] = (uint8_t)((rr * a + dst_r * (255 - a)) / 255);
		// keep alpha as fully opaque for now
		p[3] = 0xFF;
		vga_mark_dirty_rect(x, y, 1, 1);
		return;
	}

	// Fallback: composite directly into framebuffer
	unsigned char *video = (unsigned char *)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8; if (bpp < 3) bpp = 3;
	unsigned char* dst = video + y * pitch + x * bpp;
	// dst layout likely B G R
	int a = aa; if (a < 0) a = 0; if (a > 255) a = 255;
	uint8_t dst_b = dst[0], dst_g = dst[1], dst_r = dst[2];
	// framebuffer layout assumed B,G,R so write mapped channels
	dst[0] = (uint8_t)((bb * a + dst_b * (255 - a)) / 255);
	dst[1] = (uint8_t)((gg * a + dst_g * (255 - a)) / 255);
	dst[2] = (uint8_t)((rr * a + dst_r * (255 - a)) / 255);
	if (bpp >= 4) dst[3] = 0xFF;
}

void drawLine(int x1, int y1, int x2, int y2, int r, int g, int b)
{

	int dx = x2 - x1;      // the horizontal distance of the line
	int dy = y2 - y1;      // the vertical distance of the line
	int dyabs = dy;		   // the absolute value of the two distances
	int dxabs = dx;
	int px = 0;         // x-coordinate of location of the pixel
	int py = 0;		    // y-coordinate of location of the pixel
	int temp;		    // used in comparison

	// finding absolutes
	if (dx < 0)
		dxabs = -dx;

	if (dy < 0)
		dyabs = -dy;


	if (dxabs >= dyabs)
    {

		if (x1>x2)
        {
			temp = x2;
			x2 = x1;
			x1 = temp;
		}

		for (px = x1; px <= x2; px++) // drawing
        {
			int tempy = dy*(px - x1);
			py = y1 + tempy / dxabs;
            drawPixel(px, py, r, g, b);
		}
	}

	else 
    {

		if (y1>y2)
        {
			temp = y2;
			y2 = y1;
			y1 = temp;
		}

		for (py = y1; py <= y2; py++)
        {
			int tempx = dx*(py - y1);
			px = x1 + tempx / dyabs;
            drawPixel(px, py, r, g, b);
		}
	}
}

void clearScreen() 
{
	drawRect(0, 0, g_mbi->framebuffer_width, g_mbi->framebuffer_height, 0, 0, 0);
	width = 0;
	height = 0;
}

// Initialize a simple software backbuffer sized to the framebuffer.
void vga_init_double_buffer(void) {
	if (!g_mbi) return;
	int fbw = g_mbi->framebuffer_width;
	int fbh = g_mbi->framebuffer_height;
	if (fbw <= 0 || fbh <= 0) return;
	// If backbuffer already allocated and matches size, nothing to do
	if (g_backbuffer && g_backbuffer_w == fbw && g_backbuffer_h == fbh) return;
	// Free existing if sizes differ
	vga_free_backbuffer();
	// Try to allocate backbuffer; if allocation fails, leave g_backbuffer NULL and use direct framebuffer
	size_t bytes = (size_t)fbw * (size_t)fbh * 4;
	uint32 pages = (uint32)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
	if (pages == 0) return;

	// Low-RAM guard: skip the backbuffer when it would starve the system.
	// This avoids noisy heap "Request too large" prints and prevents frame exhaustion.
	uint32 free_frames = vmm_get_free_frames();
	if (free_frames <= pages + VGA_BACKBUFFER_MIN_FREE_FRAMES_AFTER) {
		return;
	}

	// Try predictive allocator first if available
	void* buf = NULL;
	// predictive_malloc is declared in predictive_memory.h if available
	// We'll attempt to use it via weak reference: if symbol exists, use it, otherwise fall back to malloc
	buf = vmm_kmalloc_aligned((uint32)bytes);
	if (buf) {
		g_backbuffer_alloc = VGA_BACKBUFFER_ALLOC_CONTIG_FRAMES;
		g_backbuffer_pages = pages;
	} else {
		buf = malloc(bytes);
		if (buf) {
			g_backbuffer_alloc = VGA_BACKBUFFER_ALLOC_HEAP;
			g_backbuffer_pages = 0;
		}
	}
	g_backbuffer = (unsigned char*)buf;
	if (!g_backbuffer) {
		g_backbuffer_w = 0;
		g_backbuffer_h = 0;
		g_backbuffer_alloc = VGA_BACKBUFFER_ALLOC_NONE;
		g_backbuffer_pages = 0;
		return;
	}
	g_backbuffer_w = fbw;
	g_backbuffer_h = fbh;
	// Initialize to black
	memset(g_backbuffer, 0, bytes);
}

// Blit backbuffer to real framebuffer (safe no-op if backbuffer not allocated)
static int rects_overlap_or_touch(dirty_rect_t a, dirty_rect_t b) {
	int ax1 = a.x, ay1 = a.y, ax2 = a.x + a.w, ay2 = a.y + a.h; // exclusive
	int bx1 = b.x, by1 = b.y, bx2 = b.x + b.w, by2 = b.y + b.h; // exclusive
	// Only merge when there is an actual overlap (not just touching edges)
	return !(ax2 <= bx1 || bx2 <= ax1 || ay2 <= by1 || by2 <= ay1);
}

static void rect_union_inplace(dirty_rect_t* dst, dirty_rect_t src) {
	int x1 = dst->x < src.x ? dst->x : src.x;
	int y1 = dst->y < src.y ? dst->y : src.y;
	int x2 = (dst->x + dst->w) > (src.x + src.w) ? (dst->x + dst->w) : (src.x + src.w);
	int y2 = (dst->y + dst->h) > (src.y + src.h) ? (dst->y + dst->h) : (src.y + src.h);
	dst->x = x1; dst->y = y1; dst->w = x2 - x1; dst->h = y2 - y1;
}

void vga_swap_buffers(void) {
	// Try to align copies with VBlank to reduce tearing near the top (optional)
	if (g_vsync_enabled) vga_wait_vblank();
	if (!g_mbi) return;
	if (!g_backbuffer) return;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	int fbw = g_mbi->framebuffer_width;
	int fbh = g_mbi->framebuffer_height;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!fb || pitch <= 0 || bpp < 3) return;
	// If nothing dirty, nothing to copy
	if (g_dirty_count <= 0) return;
	// If requested, collapse all dirty rects to a single bounding box for a single blit
	if (g_dirty_strategy == 1 && g_dirty_count > 1) {
		int minx = g_dirty_rects[0].x, miny = g_dirty_rects[0].y;
		int maxx = g_dirty_rects[0].x + g_dirty_rects[0].w;
		int maxy = g_dirty_rects[0].y + g_dirty_rects[0].h;
		for (int i = 1; i < g_dirty_count; ++i) {
			int rx2 = g_dirty_rects[i].x + g_dirty_rects[i].w;
			int ry2 = g_dirty_rects[i].y + g_dirty_rects[i].h;
			if (g_dirty_rects[i].x < minx) minx = g_dirty_rects[i].x;
			if (g_dirty_rects[i].y < miny) miny = g_dirty_rects[i].y;
			if (rx2 > maxx) maxx = rx2;
			if (ry2 > maxy) maxy = ry2;
		}
		g_dirty_rects[0].x = minx; g_dirty_rects[0].y = miny;
		g_dirty_rects[0].w = maxx - minx; g_dirty_rects[0].h = maxy - miny;
		g_dirty_count = 1;
	}

	// Helper to copy a span from backbuffer to fb, honoring pitch/bpp
	#define COPY_SPAN(abs_y, start_x, span_w) do { \
		if ((span_w) > 0) { \
			unsigned char* _src = g_backbuffer + ((size_t)(abs_y) * g_backbuffer_w + (start_x)) * 4; \
			unsigned char* _dst = fb + ((size_t)(abs_y) * pitch + (start_x) * bpp); \
			if (bpp == 4 && pitch == fbw * 4) { \
				memcpy(_dst, _src, (size_t)(span_w) * 4); \
			} else { \
				for (int _c = 0; _c < (span_w); ++_c) { \
					_dst[0] = _src[0]; /* B */ \
					_dst[1] = _src[1]; /* G */ \
					_dst[2] = _src[2]; /* R */ \
					if (bpp >= 4) _dst[3] = 0xFF; \
					_src += 4; \
					_dst += bpp; \
				} \
			} \
		} \
	} while (0)

	// Copy each dirty rectangle, splitting around optional exclusion rect (e.g., cursor)
	for (int rix = 0; rix < g_dirty_count; ++rix) {
		int x0 = g_dirty_rects[rix].x;
		int y0 = g_dirty_rects[rix].y;
		int x1 = x0 + g_dirty_rects[rix].w; // exclusive
		int y1 = y0 + g_dirty_rects[rix].h; // exclusive
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > fbw) x1 = fbw;
	if (y1 > fbh) y1 = fbh;
		if (x1 <= x0 || y1 <= y0) continue;
		// Copy bottom-up to reduce top-edge tearing when we miss exact vblank
		for (int abs_y = y1 - 1; abs_y >= y0; --abs_y) {
			int row_x0 = x0;
			int row_x1 = x1;
			if (g_exclude_w > 0 && g_exclude_h > 0 &&
				abs_y >= g_exclude_y && abs_y < g_exclude_y + g_exclude_h) {
				int ex0 = g_exclude_x;
				int ex1 = g_exclude_x + g_exclude_w;
				// Left segment
				int seg0_x0 = row_x0;
				int seg0_x1 = (ex0 > row_x0) ? ex0 : row_x0;
				if (seg0_x1 > row_x1) seg0_x1 = row_x1;
				COPY_SPAN(abs_y, seg0_x0, seg0_x1 - seg0_x0);
				// Right segment
				int seg1_x0 = (ex1 < row_x1) ? ex1 : row_x1;
				int seg1_x1 = row_x1;
				if (seg1_x0 < row_x0) seg1_x0 = row_x0;
				COPY_SPAN(abs_y, seg1_x0, seg1_x1 - seg1_x0);
			} else {
				COPY_SPAN(abs_y, row_x0, row_x1 - row_x0);
			}
		}
	}

	#undef COPY_SPAN

	// reset dirty rects after swap
	g_dirty_count = 0;
}

void vga_begin_frame(void) {
	// reset dirty tracking for the new frame
	g_dirty_count = 0;
}

void vga_set_swap_exclude(int x, int y, int w, int h) {
	g_exclude_x = x; g_exclude_y = y; g_exclude_w = w; g_exclude_h = h;
}

void vga_clear_swap_exclude(void) {
	g_exclude_x = -1; g_exclude_y = -1; g_exclude_w = 0; g_exclude_h = 0;
}

// Mark a pixel rectangle dirty without writing pixels (useful when
// static decorations are present in backbuffer but we want to ensure
// the area is included in the next blit).
void vga_mark_dirty_rect(int x, int y, int w, int h) {
	if (!g_backbuffer || !g_mbi) return;
	if (w <= 0 || h <= 0) return;
	// Clip to screen
	int sw = g_mbi->framebuffer_width;
	int sh = g_mbi->framebuffer_height;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;
	if (w <= 0 || h <= 0) return;

	dirty_rect_t nr = { x, y, w, h };
	// Try to merge with existing rects
	for (int i = 0; i < g_dirty_count; ++i) {
		if (rects_overlap_or_touch(g_dirty_rects[i], nr)) {
			rect_union_inplace(&g_dirty_rects[i], nr);
			// After expanding, try to merge further with others
			for (int j = 0; j < g_dirty_count; ) {
				if (j != i && rects_overlap_or_touch(g_dirty_rects[i], g_dirty_rects[j])) {
					rect_union_inplace(&g_dirty_rects[i], g_dirty_rects[j]);
					// remove g_dirty_rects[j]
					g_dirty_rects[j] = g_dirty_rects[g_dirty_count - 1];
					g_dirty_count--;
					continue;
				}
				++j;
			}
			return;
		}
	}
	// Append if space
	if (g_dirty_count < MAX_DIRTY_RECTS) {
		g_dirty_rects[g_dirty_count++] = nr;
		return;
	}
	// Fallback: union everything into a single bounding rect
	int minx = nr.x, miny = nr.y, maxx = nr.x + nr.w, maxy = nr.y + nr.h;
	for (int i = 0; i < g_dirty_count; ++i) {
		if (g_dirty_rects[i].x < minx) minx = g_dirty_rects[i].x;
		if (g_dirty_rects[i].y < miny) miny = g_dirty_rects[i].y;
		int rx2 = g_dirty_rects[i].x + g_dirty_rects[i].w;
		int ry2 = g_dirty_rects[i].y + g_dirty_rects[i].h;
		if (rx2 > maxx) maxx = rx2;
		if (ry2 > maxy) maxy = ry2;
	}
	g_dirty_rects[0].x = minx; g_dirty_rects[0].y = miny;
	g_dirty_rects[0].w = maxx - minx; g_dirty_rects[0].h = maxy - miny;
	g_dirty_count = 1;
}

// Wait for vertical blank using VGA input status register 0x3DA.
// On non-VGA or if IO isn't available, this is a fast no-op.
void vga_wait_vblank(void) {
	// Reading 0x3DA clears the flip-flop; bit 3 (0x08) is vertical retrace
	// Spin briefly to avoid long stalls. ~1ms worst case on 60Hz.
	const int max_iters = 50000; // tuned for our environment
	int it = 0;
	// Wait for retrace to end
	while (it++ < max_iters) {
		uint8 status = inportb(0x3DA);
		if (!(status & 0x08)) break;
	}
	it = 0;
	// Wait for retrace to start
	while (it++ < max_iters) {
		uint8 status = inportb(0x3DA);
		if (status & 0x08) break;
	}
}

void vga_set_vsync_enabled(int enabled) {
	g_vsync_enabled = enabled ? 1 : 0;
}

int vga_get_vsync_enabled(void) {
	return g_vsync_enabled;
}

void vga_set_dirty_strategy(int strategy) {
	g_dirty_strategy = (strategy == 1) ? 1 : 0;
}

int vga_get_dirty_strategy(void) {
	return g_dirty_strategy;
}

// Draw directly to the physical framebuffer (overlay), bypassing the backbuffer
void vga_drawPixel_fb(int x, int y, int r, int g, int b) {
	if (!g_mbi) return;
	if (x < 0 || y < 0) return;
	if (x >= (int)g_mbi->framebuffer_width || y >= (int)g_mbi->framebuffer_height) return;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!fb || pitch <= 0 || bpp < 3) return;
	unsigned char* p = fb + y * pitch + x * bpp;
	// Framebuffer uses BGR order (consistent with backbuffer writes)
	p[0] = (unsigned char)b;
	p[1] = (unsigned char)g;
	p[2] = (unsigned char)r;
	if (bpp >= 4) p[3] = 0xFF;
}

// Copy a rectangle from backbuffer to framebuffer (used to erase overlays like the mouse cursor)
void vga_blit_backbuffer_region_to_fb(int x, int y, int w, int h) {
	if (!g_mbi || !g_backbuffer) return;
	int fb_w = g_mbi->framebuffer_width;
	int fb_h = g_mbi->framebuffer_height;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fb_w) w = fb_w - x;
	if (y + h > fb_h) h = fb_h - y;
	if (w <= 0 || h <= 0) return;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!fb || pitch <= 0 || bpp < 3) return;
	if (bpp == 4 && pitch == fb_w * 4) {
		// Formats match exactly; copy each row with a single memcpy
		for (int row = 0; row < h; ++row) {
			unsigned char* src = g_backbuffer + ((size_t)(y + row) * g_backbuffer_w + x) * 4;
			unsigned char* dst = fb + ((size_t)(y + row) * pitch + x * 4);
			memcpy(dst, src, (size_t)w * 4);
		}
	} else {
		for (int row = 0; row < h; ++row) {
			unsigned char* src = g_backbuffer + (y + row) * g_backbuffer_w * 4 + x * 4;
			unsigned char* dst = fb + (y + row) * pitch + x * bpp;
			for (int col = 0; col < w; ++col) {
				// Backbuffer layout: [0]=B, [1]=G, [2]=R, [3]=A(ignored)
				dst[0] = src[0]; // B
				dst[1] = src[1]; // G
				dst[2] = src[2]; // R
				if (bpp >= 4) dst[3] = 0xFF;
				src += 4;
				dst += bpp;
			}
		}
	}
}

int vga_has_backbuffer(void) {
	return g_backbuffer != NULL;
}

// Fast overlay rectangle fill directly to framebuffer, clipped
void vga_fillRect_fb(int x, int y, int w, int h, int rr, int gg, int bb) {
	if (!g_mbi) return;
	int sw = g_mbi->framebuffer_width;
	int sh = g_mbi->framebuffer_height;
	if (w <= 0 || h <= 0) return;
	if (x >= sw || y >= sh) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;
	if (w <= 0 || h <= 0) return;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8; if (bpp < 3) bpp = 3;
	// Precompute a row in a small stack buffer for fast copy when bpp==4
	if (bpp == 4) {
		// Fill a temporary row of length w
		// Limit a small temp row to avoid large stack usage; if too big, fill per-pixel
		if (w <= 1024) {
			uint32_t rowbuf[1024];
			uint32_t packed = ((uint32_t)0xFF << 24) | ((uint32_t)(unsigned char)rr << 16) | ((uint32_t)(unsigned char)gg << 8) | (uint32_t)(unsigned char)bb;
			for (int i = 0; i < w; ++i) rowbuf[i] = packed;
			for (int yy = 0; yy < h; ++yy) {
				uint32_t* dst = (uint32_t*)(fb + (size_t)(y + yy) * pitch + x * 4);
				memcpy(dst, rowbuf, (size_t)w * 4);
			}
			return;
		}
	}
	// Generic path
	for (int yy = 0; yy < h; ++yy) {
		unsigned char* dst = fb + (size_t)(y + yy) * pitch + x * bpp;
		for (int xx = 0; xx < w; ++xx) {
			dst[0] = (unsigned char)bb;
			dst[1] = (unsigned char)gg;
			dst[2] = (unsigned char)rr;
			if (bpp >= 4) dst[3] = 0xFF;
			dst += bpp;
		}
	}
}

int vga_get_fb_bpp_bytes(void) {
	if (!g_mbi) return 0;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (bpp < 3) bpp = 3; // treat <24bpp as 24bpp for our writes
	return bpp;
}

int vga_capture_fb_region(int x, int y, int w, int h, unsigned char* out_buf, int out_buf_len) {
	if (!g_mbi || !out_buf) return -1;
	int fb_w = g_mbi->framebuffer_width;
	int fb_h = g_mbi->framebuffer_height;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = vga_get_fb_bpp_bytes();
	if (w <= 0 || h <= 0 || x >= fb_w || y >= fb_h) return -1;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fb_w) w = fb_w - x;
	if (y + h > fb_h) h = fb_h - y;
	int need = w * h * bpp;
	if (need > out_buf_len) return -1;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	for (int row = 0; row < h; ++row) {
		unsigned char* src = fb + (y + row) * pitch + x * bpp;
		memcpy(out_buf + row * w * bpp, src, (size_t)w * bpp);
	}
	return w * h * bpp;
}

int vga_restore_fb_region(int x, int y, int w, int h, const unsigned char* in_buf, int in_buf_len) {
	if (!g_mbi || !in_buf) return -1;
	int fb_w = g_mbi->framebuffer_width;
	int fb_h = g_mbi->framebuffer_height;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = vga_get_fb_bpp_bytes();
	if (w <= 0 || h <= 0 || x >= fb_w || y >= fb_h) return -1;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fb_w) w = fb_w - x;
	if (y + h > fb_h) h = fb_h - y;
	int need = w * h * bpp;
	if (need > in_buf_len) return -1;
	unsigned char* fb = (unsigned char*)(uintptr_t)g_mbi->framebuffer_addr;
	for (int row = 0; row < h; ++row) {
		unsigned char* dst = fb + (y + row) * pitch + x * bpp;
		memcpy(dst, in_buf + row * w * bpp, (size_t)w * bpp);
	}
	return w * h * bpp;
}

// Draw a NUL-terminated string at (x,y) in pixels using the existing drawText implementation.
// This temporarily adjusts the global cursor (width/height) so we don't need to duplicate the font table.
void drawTextAt(int x, int y, const char* text, int rr, int gg, int bb)
{
	if (!text) return;
	int font_handle = vga_get_system_font_handle_raw();
	int line_h = (int)vga_font_line_h_or_builtin(font_handle);
	int old_w = width;
	int old_h = height;
	int cx = x;
	int cy = y;

	for (const char* p = text; *p; ++p) {
		if (*p == '\n') {
			cx = x;
			cy += line_h;
			continue;
		}
		int adv = vga_draw_glyph8xN_at(font_handle, cx, cy, (int)(unsigned char)*p, rr, gg, bb);
		if (adv <= 0) adv = VGA_FONT_DEFAULT_ADVANCE;
		cx += adv;
	}

	// restore previous cursor
	width = old_w;
	height = old_h;
}

void drawCharAt(int x, int y, int charnum, int rr, int gg, int bb) {
	drawCharAt_font(vga_get_system_font_handle_raw(), x, y, charnum, rr, gg, bb);
}

// Start redirection
void start_shell_redirect() {
	shell_redirect_active = 1;
	shell_redirect_pos = 0;
	shell_redirect_buf[0] = '\0';
	// clear per-char redirect colour arrays
	for (int i = 0; i < SHELL_REDIRECT_BUF_SIZE; ++i) {
		shell_redirect_r[i] = 0;
		shell_redirect_g[i] = 0;
		shell_redirect_b[i] = 0;
	}
	// reset current redirect colour so new typed input falls back to vterm default
	shell_redirect_colour_r = 0;
	shell_redirect_colour_g = 0;
	shell_redirect_colour_b = 0;
	// clear any previously recorded icons for this redirect session
	shell_redirect_icon_count = 0;
}

// Stop redirection
void stop_shell_redirect() {
	shell_redirect_active = 0;
	shell_redirect_pos = 0;
	// clear redirect colour so subsequent typed input doesn't inherit last printed colour
	shell_redirect_colour_r = 0;
	shell_redirect_colour_g = 0;
	shell_redirect_colour_b = 0;
}

// Register an icon to be associated with the current output position in shell_redirect_buf.
void shell_register_redirect_icon(const char* ext) {
    if (!ext) return;
    if (shell_redirect_icon_count >= SHELL_REDIRECT_ICON_MAX) return;
    int p = shell_redirect_pos;
    shell_redirect_icons[shell_redirect_icon_count].pos = p;
    safe_strcpy(shell_redirect_icons[shell_redirect_icon_count].ext, ext, sizeof(shell_redirect_icons[shell_redirect_icon_count].ext));
    shell_redirect_icon_count++;
}

// Minimal snprintf for kernel shell (supports %s, %u, %d, %c)
int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    size_t pos = 0;
    for (const char *p = format; *p && pos + 1 < size; ++p) {
        if (*p == '%') {
            ++p;
            if (*p == 's') {
                char *s = va_arg(ap, char*);
                while (*s && pos + 1 < size) str[pos++] = *s++;
            } else if (*p == 'u') {
                unsigned int v = va_arg(ap, unsigned int);
                char buf[16];
                int i = 0;
                if (v == 0) buf[i++] = '0';
                else {
                    while (v && i < 15) { buf[i++] = '0' + (v % 10); v /= 10; }
                }
                for (int j = i-1; j >= 0 && pos + 1 < size; --j) str[pos++] = buf[j];
            } else if (*p == 'd') {
                int v = va_arg(ap, int);
                char buf[16];
                int i = 0, neg = 0;
                if (v < 0) { neg = 1; v = -v; }
                if (v == 0) buf[i++] = '0';
                else {
                    while (v && i < 15) { buf[i++] = '0' + (v % 10); v /= 10; }
                }
                if (neg && pos + 1 < size) str[pos++] = '-';
                for (int j = i-1; j >= 0 && pos + 1 < size; --j) str[pos++] = buf[j];
            } else if (*p == 'c') {
                char c = (char)va_arg(ap, int);
                str[pos++] = c;
            } else if (*p == '%') {
                str[pos++] = '%';
            }
        } else {
            str[pos++] = *p;
        }
    }
    str[pos] = '\0';
    va_end(ap);
    return pos;
}

void vga_set_colour(int nr, int ng, int nb) {
	vga_default_r = nr;
	vga_default_g = ng;
	vga_default_b = nb;
}

// Bold font - just use regular drawText with brighter colour for now
void drawText_bold(int charnum, int r, int g, int b) {
    // For now, just use regular drawText with brighter colour
    drawText(charnum, r, g, b);
}

// Italic font - just use regular drawText for now
void drawText_italic(int charnum, int r, int g, int b) {
    // For now, just use regular drawText
    drawText(charnum, r, g, b);
}

// Large font for headers - enhanced spacing and visual emphasis
void drawText_large(int charnum, int r, int g, int bb) {
    // Use regular drawText but add extra spacing and visual emphasis
	drawText(charnum, r, g, bb);
    // Add extra spacing to make headers appear larger
    width += 2; // Extra spacing between characters
}

// Markdown rendering function
void render_markdown(const char* content) {
    if (!content) return;
    
    int in_bold = 0;
    int in_italic = 0;
    int in_header = 0;
    int header_level = 0;
    int in_code = 0;
    
    for (int i = 0; content[i]; i++) {
        char c = content[i];
        
        // Handle newlines
        if (c == '\n') {
            printf("\n");
            in_header = 0;
            header_level = 0;
            continue;
        }
        
        // Handle headers (# ## ###)
        if (c == '#' && (i == 0 || content[i-1] == '\n')) {
            header_level = 0;
            while (content[i] == '#') {
                header_level++;
                i++;
            }
            if (content[i] == ' ') {
                in_header = 1;
                // Print header with different colour
                printf("%c", 200, 180, 255); // Soft lavender for headers
                continue;
            } else {
                // Not a header, go back and print normally
                i -= header_level;
                c = content[i];
            }
        }
        
        // Handle bold (**text**)
        if (c == '*' && content[i+1] == '*') {
            in_bold = !in_bold;
            i++; // Skip next *
            continue;
        }
        
        // Handle italic (*text*)
        if (c == '*' && content[i+1] != '*') {
            in_italic = !in_italic;
            continue;
        }
        
        // Handle code blocks (```)
        if (c == '`' && content[i+1] == '`' && content[i+2] == '`') {
            in_code = !in_code;
            i += 2; // Skip next two `
            printf("%c", 180, 200, 255); // Soft blue for code blocks
            continue;
        }
        
        // Handle inline code (`code`)
        if (c == '`' && content[i+1] != '`') {
            in_code = !in_code;
            printf("%c", 180, 180, 255); // Soft blue for inline code
            continue;
        }
        
        // Print the character with appropriate formatting
        if (in_header) {
            // Headers in soft lavender with large font
            drawText_large(c, 210, 170, 255);
        } else if (in_bold) {
            // Bold text in soft pink
            drawText(c, 255, 180, 180);
        } else if (in_italic) {
            // Italic text in soft gray
            drawText(c, 170, 170, 190);
        } else if (in_code) {
            // Code text in soft blue
            drawText(c, 180, 180, 255);
        } else {
            // Regular text in soft white
            drawText(c, 245, 245, 250);
        }
    }
    
    printf("\n");
}

// simple 2x2 windowing support ---
typedef struct { int x, y, w, h; } vga_window_rect_t;
static vga_window_rect_t g_win_rects[4];
static int g_active_window = -1;

static void vga_draw_window_frame(int x, int y, int w, int h, const char* title) {
    drawRect(x, y, w, 12, 40, 40, 40);
    drawRect(x, y + 12, w, h - 12, 0, 0, 0);
    if (title) {
        int old_w = width, old_h = height;
        width = x + 4;
        height = y + 2;
		for (const char* p = title; *p; ++p) drawText(*p, 220, 220, 220);
        width = old_w; height = old_h;
    }
}

void vga_windows_init_2x2(void) {
    clearScreen();
    int W = g_mbi->framebuffer_width;
    int H = g_mbi->framebuffer_height;
    int halfW = W / 2;
    int halfH = H / 2;
    g_win_rects[0] = (vga_window_rect_t){ 0, 0, halfW, halfH };
    g_win_rects[1] = (vga_window_rect_t){ halfW, 0, W - halfW, halfH };
    g_win_rects[2] = (vga_window_rect_t){ 0, halfH, halfW, H - halfH };
    g_win_rects[3] = (vga_window_rect_t){ halfW, halfH, W - halfW, H - halfH };
    for (int i = 0; i < 4; i++) {
        vga_draw_window_frame(g_win_rects[i].x, g_win_rects[i].y, g_win_rects[i].w, g_win_rects[i].h, "");
    }
    g_active_window = 0;
    width = g_win_rects[0].x;
    height = g_win_rects[0].y + 13;
}

void vga_set_active_window(int index) {
    if (index < 0 || index > 3) return;
    g_active_window = index;
    width = g_win_rects[index].x;
    height = g_win_rects[index].y + 13;
}

int vga_get_active_window(void) { return g_active_window; }

void vga_clear_window(int index) {
    if (index < 0 || index > 3) return;
    drawRect(g_win_rects[index].x + 1, g_win_rects[index].y + 13, g_win_rects[index].w - 2, g_win_rects[index].h - 14, 0, 0, 0);
}

void vga_window_set_title(int index, const char* title) {
    if (index < 0 || index > 3) return;
    vga_draw_window_frame(g_win_rects[index].x, g_win_rects[index].y, g_win_rects[index].w, g_win_rects[index].h, title);
}