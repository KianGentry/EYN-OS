#include <misc/types.h>

#include <graphics/gfx.h>
#include <hal/console.h>
#include <utilities/shell/shell_command_info.h>

#if defined(EYNOS_DISABLE_SHELL_COMMAND_REGISTRY)
#define EYNOS_REGISTER_SHELL_COMMAND(...)
#else
#define EYNOS_REGISTER_SHELL_COMMAND(...) REGISTER_SHELL_COMMAND(__VA_ARGS__)
#endif

void cmd_gfxdemo(string arg) {
    (void)arg;

    gfx_init_default();
    if (!gfx_ready()) {
        hal_console_write("gfx not ready\n");
        return;
    }

    uint32 w = gfx_screen_width();
    uint32 h = gfx_screen_height();

    if (w == 0 || h == 0) {
        gfx_clear_rgb(0, 0, 0);
        gfx_set_rgb(255, 255, 255);
        gfx_write("gfxdemo: screen size unknown\n");
        gfx_write("(backend ready, but width/height unavailable)\n");
        gfx_flush();
        return;
    }

    /* Color bars + a couple lines of text: intentionally simple + deterministic. */
    gfx_fill_rect(0, 0, (int)w, (int)(h / 3u), 120, 120, 255);
    gfx_fill_rect(0, (int)(h / 3u), (int)w, (int)(h / 3u), 120, 255, 120);
    gfx_fill_rect(0, (int)((h * 2u) / 3u), (int)w, (int)(h - ((h * 2u) / 3u)), 255, 120, 120);

    /* fb_simple renders text with an explicit background color; set it so text stays readable over the bars. */
    gfx_set_bg_rgb(120, 120, 255);
    gfx_set_rgb(0, 0, 0);
    gfx_write("gfxdemo\n");
    gfx_write("If you can read this, gfx_putc/write works.\n");

    gfx_flush();
}

EYNOS_REGISTER_SHELL_COMMAND(gfxdemo, "gfxdemo", cmd_gfxdemo, CMD_DIAGNOSTIC, "Draw a simple gfx test pattern.", "gfxdemo");
