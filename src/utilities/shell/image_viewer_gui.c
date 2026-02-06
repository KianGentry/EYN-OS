#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <rei.h>
#include <reiv.h>
#include <eynfs.h>
#include <fs_commands.h>
#include <string.h>
#include <stdlib.h>
#include <mouse.h>
#include <hal/time.h>
#include <serial.h>
#include <watchdog.h>
#ifndef EYNFS_SUPERBLOCK_LBA
#define EYNFS_SUPERBLOCK_LBA 2048
#endif

extern uint8_t get_current_logical_drive(void);
extern char shell_current_path[128];

typedef struct {
    rei_image_t img;
    // REIV streaming video/animation support
    int is_reiv;
    reiv_header_t vh;
    uint16_t* frame565; // width*height pixels (RGB565LE)
    uint16_t* frame565_next; // next frame buffer (double-buffer)
    uint32_t frame_bytes;
    uint8_t* frame_comp; // compressed/temporary buffer (<=frame_bytes)
    uint8_t* reiv_index; // v2 index table (frame_count * sizeof(reiv_frame_entry_t))
    uint32_t reiv_index_bytes;
    uint32_t reiv_data_offset;
    uint32_t frame_index;
    uint32_t next_frame_tick;
    // Frame pacing on low-Hz timers using a rational tick accumulator:
    // ticks per frame = (hz * fps_den) / fps_num.
    // We accumulate the numerator and take integer ticks when it overflows fps_num.
    uint32_t frame_tick_accum;
    uint32_t frame_tick_hz;
    uint32_t frame_tick_fps_num;
    uint32_t frame_tick_fps_den;
    int playing;
    int loop_enabled;
    int loop_locked;
    uint32_t last_toggle_tick;
    uint32_t last_click_tick;
    uint8_t last_mouse_left;
    // Debugging / telemetry (serial)
    uint32_t dbg_last_print_tick;
    uint32_t dbg_draws;
    uint32_t dbg_advances;
    // Prefetch state for smoother playback
    uint8_t prefetch_active;
    uint8_t prefetch_ready;
    uint32_t prefetch_frame_index;
    uint32_t prefetch_file_off;
    uint32_t prefetch_total;
    uint32_t prefetch_done;
    uint8_t* prefetch_dst;
    uint8_t prefetch_dst_is_frame; // 1 => dst points into frame565_next, 0 => dst is frame_comp
    reiv_frame_entry_t prefetch_ent;
    // Cached pre-scaled RGB565 (avoids re-scaling every redraw; fixes flicker)
    uint16_t* scaled565;
    uint32_t scaled_w;
    uint32_t scaled_h;
    uint32_t scaled_src_frame_index;
    uint8_t scaled_valid;
    // filesystem handle snapshot for streaming reads
    uint8 disk;
    eynfs_superblock_t sb;
    eynfs_dir_entry_t entry;
    int content_x, content_y, content_w, content_h;
    int zoom; // 1..8
    int off_x, off_y; // pan offset in screen pixels
    int dragging; int drag_start_x, drag_start_y; int drag_off_x, drag_off_y;
    char filepath[128];
    char filename_base[128];
    int tile_idx;
    // Window mode support
    int is_window; // 0=tile, 1=window
    int window_id; // valid when is_window
    char status_left[128];
} viewer_t;

static viewer_t g_view;

static void viewer_free_resources(void) {
    if (g_view.img.data) {
        rei_free_image(&g_view.img);
    }
    if (g_view.frame565) {
        free(g_view.frame565);
        g_view.frame565 = NULL;
    }
    if (g_view.frame565_next) {
        free(g_view.frame565_next);
        g_view.frame565_next = NULL;
    }
    if (g_view.scaled565) {
        free(g_view.scaled565);
        g_view.scaled565 = NULL;
    }
    if (g_view.frame_comp) {
        free(g_view.frame_comp);
        g_view.frame_comp = NULL;
    }
    if (g_view.reiv_index) {
        free(g_view.reiv_index);
        g_view.reiv_index = NULL;
    }
    g_view.reiv_index_bytes = 0;
    g_view.reiv_data_offset = 0;
    g_view.frame_bytes = 0;
    g_view.is_reiv = 0;
    g_view.next_frame_tick = 0;
    g_view.frame_tick_accum = 0;
    g_view.frame_tick_hz = 0;
    g_view.frame_tick_fps_num = 0;
    g_view.frame_tick_fps_den = 0;
    g_view.prefetch_active = 0;
    g_view.prefetch_ready = 0;
    g_view.prefetch_frame_index = 0;
    g_view.prefetch_file_off = 0;
    g_view.prefetch_total = 0;
    g_view.prefetch_done = 0;
    g_view.prefetch_dst = NULL;
    g_view.prefetch_dst_is_frame = 0;
    memset(&g_view.prefetch_ent, 0, sizeof(g_view.prefetch_ent));
    g_view.scaled_w = 0;
    g_view.scaled_h = 0;
    g_view.scaled_src_frame_index = 0xFFFFFFFFu;
    g_view.scaled_valid = 0;
}

static void reiv_scale_rgb565_nn(uint16_t* dst, uint32_t dst_w, uint32_t dst_h, const uint16_t* src, uint32_t src_w, uint32_t src_h) {
    if (!dst || !src || !dst_w || !dst_h || !src_w || !src_h) return;
    // Nearest-neighbor scaling using 16.16 fixed point to avoid per-pixel division.
    uint32_t step_x = (src_w << 16) / dst_w;
    uint32_t step_y = (src_h << 16) / dst_h;
    uint32_t ay = 0;
    uint32_t kick_next = 32;
    for (uint32_t y = 0; y < dst_h; ++y) {
        if (y >= kick_next) { watchdog_kick("reiv-scale"); kick_next = y + 32; }
        uint32_t sy = (ay >> 16);
        if (sy >= src_h) sy = src_h - 1;
        const uint16_t* src_row = src + (size_t)sy * (size_t)src_w;
        uint16_t* dst_row = dst + (size_t)y * (size_t)dst_w;
        uint32_t ax = 0;
        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t sx = (ax >> 16);
            if (sx >= src_w) sx = src_w - 1;
            dst_row[x] = src_row[sx];
            ax += step_x;
        }
        ay += step_y;
    }
}

static const char* get_basename_local(const char* path) {
    const char* last = path; for (const char* p = path; *p; ++p) if (*p=='/') last = p+1; return last;
}

static int reiv_decode_packbits(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_len, uint32_t unit) {
    if (!in || !out || unit == 0) return -1;
    uint32_t in_pos = 0;
    uint32_t out_pos = 0;
    uint32_t kick_next = 4096;
    while (in_pos < in_len && out_pos < out_len) {
        if (out_pos >= kick_next) {
            watchdog_kick("reiv-decode");
            kick_next = out_pos + 4096;
        }
        int8_t ctl = (int8_t)in[in_pos++];
        if (ctl >= 0) {
            uint32_t count = (uint32_t)ctl + 1u;
            uint32_t bytes = count * unit;
            if (out_pos + bytes > out_len) return -1;
            if (in_pos + bytes > in_len) return -1;
            memcpy(&out[out_pos], &in[in_pos], bytes);
            in_pos += bytes;
            out_pos += bytes;
        } else if (ctl == (int8_t)0x80) {
            // no-op
            continue;
        } else {
            uint32_t count = (uint32_t)(1 - ctl);
            uint32_t bytes = count * unit;
            if (out_pos + bytes > out_len) return -1;
            if (in_pos + unit > in_len) return -1;
            for (uint32_t i = 0; i < count; ++i) {
                memcpy(&out[out_pos], &in[in_pos], unit);
                out_pos += unit;
            }
            in_pos += unit;
        }
    }
    return (out_pos == out_len) ? 0 : -1;
}

static int reiv_decode_packbits_xor_into(const uint8_t* in, uint32_t in_len, uint8_t* dst, uint32_t dst_len) {
    if (!in || !dst) return -1;
    uint32_t in_pos = 0;
    uint32_t out_pos = 0;
    uint32_t kick_next = 4096;
    while (in_pos < in_len && out_pos < dst_len) {
        if (out_pos >= kick_next) {
            watchdog_kick("reiv-decode");
            kick_next = out_pos + 4096;
        }
        int8_t ctl = (int8_t)in[in_pos++];
        if (ctl >= 0) {
            uint32_t count = (uint32_t)ctl + 1u;
            if (out_pos + count > dst_len) return -1;
            if (in_pos + count > in_len) return -1;
            for (uint32_t i = 0; i < count; ++i) {
                dst[out_pos++] ^= in[in_pos++];
            }
        } else if (ctl == (int8_t)0x80) {
            continue;
        } else {
            uint32_t count = (uint32_t)(1 - ctl);
            if (out_pos + count > dst_len) return -1;
            if (in_pos + 1u > in_len) return -1;
            uint8_t v = in[in_pos++];
            for (uint32_t i = 0; i < count; ++i) {
                dst[out_pos++] ^= v;
            }
        }
    }
    return (out_pos == dst_len) ? 0 : -1;
}

static int reiv_eynfs_read_exact(uint32_t file_off, void* dst, uint32_t len) {
    uint8_t* out = (uint8_t*)dst;
    uint32_t remaining = len;
    uint32_t off = file_off;
    // Balance watchdog safety vs overhead: too-small chunks create lots of ATA reads and slow the system.
    // 16KiB keeps reads bounded while drastically reducing call count.
    const uint32_t chunk_max = 16384;
    while (remaining) {
        uint32_t chunk = (remaining > chunk_max) ? chunk_max : remaining;
        watchdog_kick("reiv-io");
        int br = eynfs_read_file(g_view.disk, &g_view.sb, &g_view.entry, (char*)out, (int)chunk, (size_t)off);
        if (br != (int)chunk) return -1;
        out += chunk;
        off += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int reiv_eynfs_read_partial(uint32_t file_off, void* dst, uint32_t len) {
    watchdog_kick("reiv-io");
    int br = eynfs_read_file(g_view.disk, &g_view.sb, &g_view.entry, (char*)dst, (int)len, (size_t)file_off);
    return (br == (int)len) ? 0 : -1;
}

static int reiv_get_entry(uint32_t frame_index, reiv_frame_entry_t* out_ent) {
    if (!out_ent) return -1;
    if (!g_view.reiv_index || g_view.reiv_index_bytes == 0) return -1;
    uint32_t idx_off = frame_index * (uint32_t)sizeof(reiv_frame_entry_t);
    if (idx_off + sizeof(reiv_frame_entry_t) > g_view.reiv_index_bytes) return -1;
    memcpy(out_ent, g_view.reiv_index + idx_off, sizeof(*out_ent));
    return 0;
}

static void reiv_prefetch_reset(void) {
    g_view.prefetch_active = 0;
    g_view.prefetch_ready = 0;
    g_view.prefetch_total = 0;
    g_view.prefetch_done = 0;
    g_view.prefetch_file_off = 0;
    g_view.prefetch_dst = NULL;
    g_view.prefetch_dst_is_frame = 0;
    memset(&g_view.prefetch_ent, 0, sizeof(g_view.prefetch_ent));
}

static int reiv_prefetch_start(uint32_t target_frame) {
    if (!g_view.is_reiv) return -1;
    if (!g_view.frame565 || !g_view.frame565_next) return -1;
    if (target_frame >= g_view.vh.frame_count) return -1;

    // If already ready for this frame, keep it.
    if (g_view.prefetch_ready && g_view.prefetch_frame_index == target_frame) return 0;

    // If we're already streaming/decoding this frame, keep going.
    // IMPORTANT: do not reset progress; this function is called frequently (per draw).
    if (g_view.prefetch_active && g_view.prefetch_frame_index == target_frame) return 0;

    // If a different prefetch is in flight, restart.
    if (g_view.prefetch_active && g_view.prefetch_frame_index != target_frame) {
        reiv_prefetch_reset();
    }

    g_view.prefetch_frame_index = target_frame;
    g_view.prefetch_active = 1;
    g_view.prefetch_ready = 0;
    g_view.prefetch_done = 0;

    // Decide what to read and where to store it.
    if (g_view.vh.version == REIV_VERSION_V1) {
        g_view.prefetch_total = g_view.frame_bytes;
        g_view.prefetch_file_off = reiv_frame_offset_bytes(&g_view.vh, target_frame);
        g_view.prefetch_dst = (uint8_t*)g_view.frame565_next;
        g_view.prefetch_dst_is_frame = 1;
        return 0;
    }

    if (g_view.vh.version == REIV_VERSION_V2 || g_view.vh.version == REIV_VERSION_V3) {
        reiv_frame_entry_t ent;
        if (reiv_get_entry(target_frame, &ent) != 0) return -1;
        if (ent.size == 0) return -1;
        if (ent.size > g_view.frame_bytes) return -1;

        g_view.prefetch_ent = ent;
        g_view.prefetch_total = ent.size;
        g_view.prefetch_file_off = g_view.reiv_data_offset + ent.offset;

        // Raw keyframes can stream directly into the next framebuffer.
        if (!(ent.flags & REIV_FRAME_FLAG_RLE565) && !(ent.flags & REIV_FRAME_FLAG_DELTA_XOR_PREV) && ent.size == g_view.frame_bytes) {
            g_view.prefetch_dst = (uint8_t*)g_view.frame565_next;
            g_view.prefetch_dst_is_frame = 1;
            return 0;
        }

        // Otherwise, stream into compressed buffer then decode/apply.
        if (!g_view.frame_comp) return -1;
        g_view.prefetch_dst = g_view.frame_comp;
        g_view.prefetch_dst_is_frame = 0;
        return 0;
    }

    return -1;
}

static int reiv_prefetch_finish_decode(void) {
    if (!g_view.prefetch_active) return -1;
    if (g_view.prefetch_done != g_view.prefetch_total) return -1;
    uint32_t f = g_view.prefetch_frame_index;

    // If we streamed directly into the next framebuffer, we're done.
    if (g_view.prefetch_dst_is_frame) {
        g_view.prefetch_active = 0;
        g_view.prefetch_ready = 1;
        return 0;
    }

    // Decode/apply from frame_comp into frame565_next.
    const reiv_frame_entry_t ent = g_view.prefetch_ent;
    if (g_view.vh.version == REIV_VERSION_V2) {
        if (ent.flags & REIV_FRAME_FLAG_RLE565) {
            if (reiv_decode_packbits(g_view.frame_comp, ent.size, (uint8_t*)g_view.frame565_next, g_view.frame_bytes, 2u) != 0) return -1;
            g_view.prefetch_active = 0;
            g_view.prefetch_ready = 1;
            return 0;
        }
        // v2 non-RLE non-raw-direct is unexpected.
        return -1;
    }

    if (g_view.vh.version == REIV_VERSION_V3) {
        if (ent.flags & REIV_FRAME_FLAG_DELTA_XOR_PREV) {
            if (f == 0) return -1;
            memcpy((uint8_t*)g_view.frame565_next, (const uint8_t*)g_view.frame565, g_view.frame_bytes);
            uint8_t* dst = (uint8_t*)g_view.frame565_next;
            if (ent.flags & REIV_FRAME_FLAG_RLE8) {
                if (reiv_decode_packbits_xor_into(g_view.frame_comp, ent.size, dst, g_view.frame_bytes) != 0) return -1;
            } else {
                if (ent.size != g_view.frame_bytes) return -1;
                uint32_t kick_next = 4096;
                for (uint32_t i = 0; i < g_view.frame_bytes; ++i) {
                    if (i >= kick_next) {
                        watchdog_kick("reiv-decode");
                        kick_next = i + 4096;
                    }
                    dst[i] ^= g_view.frame_comp[i];
                }
            }
            g_view.prefetch_active = 0;
            g_view.prefetch_ready = 1;
            return 0;
        }

        if (ent.flags & REIV_FRAME_FLAG_RLE565) {
            if (reiv_decode_packbits(g_view.frame_comp, ent.size, (uint8_t*)g_view.frame565_next, g_view.frame_bytes, 2u) != 0) return -1;
            g_view.prefetch_active = 0;
            g_view.prefetch_ready = 1;
            return 0;
        }

        // Raw keyframes should have streamed directly.
        return -1;
    }

    return -1;
}

static void reiv_prefetch_step(uint32_t budget_bytes) {
    if (!g_view.prefetch_active) return;
    if (g_view.prefetch_ready) return;
    if (g_view.prefetch_done >= g_view.prefetch_total) return;

    // Bound the amount of I/O we do per draw to keep the UI responsive.
    // 16KiB drastically reduces eynfs_read_file() call count vs 4KiB while still bounded.
    const uint32_t chunk_max = 16384;
    uint32_t remaining = g_view.prefetch_total - g_view.prefetch_done;
    uint32_t chunk = remaining;
    if (chunk > chunk_max) chunk = chunk_max;
    if (budget_bytes && chunk > budget_bytes) chunk = budget_bytes;

    if (chunk == 0) return;
    if (reiv_eynfs_read_partial(g_view.prefetch_file_off + g_view.prefetch_done, g_view.prefetch_dst + g_view.prefetch_done, chunk) != 0) {
        // Abort prefetch; caller may fall back to synchronous load.
        reiv_prefetch_reset();
        return;
    }
    g_view.prefetch_done += chunk;
    if (g_view.prefetch_done == g_view.prefetch_total) {
        if (reiv_prefetch_finish_decode() != 0) {
            reiv_prefetch_reset();
        }
    }
}

static int reiv_stream_load_frame(uint32_t frame_index) {
    // NOTE: during load_reiv_stream() we load frame 0 before g_view.is_reiv is set.
    if (!g_view.frame565) return -1;
    if (frame_index >= g_view.vh.frame_count) return -1;

    if (g_view.vh.version == REIV_VERSION_V1) {
        uint32_t off = reiv_frame_offset_bytes(&g_view.vh, frame_index);
        return (reiv_eynfs_read_exact(off, (void*)g_view.frame565, g_view.frame_bytes) == 0) ? 0 : -1;
    }

    if (g_view.vh.version == REIV_VERSION_V2) {
        if (!g_view.reiv_index || g_view.reiv_index_bytes == 0) return -1;
        uint32_t idx_off = frame_index * (uint32_t)sizeof(reiv_frame_entry_t);
        if (idx_off + sizeof(reiv_frame_entry_t) > g_view.reiv_index_bytes) return -1;
        reiv_frame_entry_t ent;
        memcpy(&ent, g_view.reiv_index + idx_off, sizeof(ent));
        if (ent.size == 0) return -1;
        if (ent.size > g_view.frame_bytes) return -1;

        uint32_t file_off = g_view.reiv_data_offset + ent.offset;
        if (ent.flags & REIV_FRAME_FLAG_RLE565) {
            if (!g_view.frame_comp) return -1;
            if (reiv_eynfs_read_exact(file_off, (void*)g_view.frame_comp, ent.size) != 0) return -1;
            if (reiv_decode_packbits(g_view.frame_comp, ent.size, (uint8_t*)g_view.frame565, g_view.frame_bytes, 2u) != 0) return -1;
            return 0;
        }

        // Raw RGB565
        if (ent.size != g_view.frame_bytes) return -1;
        return (reiv_eynfs_read_exact(file_off, (void*)g_view.frame565, g_view.frame_bytes) == 0) ? 0 : -1;
    }

    if (g_view.vh.version == REIV_VERSION_V3) {
        if (!g_view.reiv_index || g_view.reiv_index_bytes == 0) return -1;
        uint32_t idx_off = frame_index * (uint32_t)sizeof(reiv_frame_entry_t);
        if (idx_off + sizeof(reiv_frame_entry_t) > g_view.reiv_index_bytes) return -1;
        reiv_frame_entry_t ent;
        memcpy(&ent, g_view.reiv_index + idx_off, sizeof(ent));
        if (ent.size == 0) return -1;
        if (ent.size > g_view.frame_bytes) return -1;

        uint32_t file_off = g_view.reiv_data_offset + ent.offset;

        // Delta frames require an existing previous decoded frame.
        if (ent.flags & REIV_FRAME_FLAG_DELTA_XOR_PREV) {
            if (frame_index == 0) return -1;
            if (!g_view.frame_comp) return -1;
            if (reiv_eynfs_read_exact(file_off, (void*)g_view.frame_comp, ent.size) != 0) return -1;

            // XOR delta into the existing frame buffer.
            uint8_t* dst = (uint8_t*)g_view.frame565;
            if (ent.flags & REIV_FRAME_FLAG_RLE8) {
                if (reiv_decode_packbits_xor_into(g_view.frame_comp, ent.size, dst, g_view.frame_bytes) != 0) return -1;
                return 0;
            }

            if (ent.size != g_view.frame_bytes) return -1;
            uint32_t kick_next = 4096;
            for (uint32_t i = 0; i < g_view.frame_bytes; ++i) {
                if (i >= kick_next) {
                    watchdog_kick("reiv-decode");
                    kick_next = i + 4096;
                }
                dst[i] ^= g_view.frame_comp[i];
            }
            return 0;
        }

        // Non-delta frames (keyframes)
        if (ent.flags & REIV_FRAME_FLAG_RLE565) {
            if (!g_view.frame_comp) return -1;
            if (reiv_eynfs_read_exact(file_off, (void*)g_view.frame_comp, ent.size) != 0) return -1;
            if (reiv_decode_packbits(g_view.frame_comp, ent.size, (uint8_t*)g_view.frame565, g_view.frame_bytes, 2u) != 0) return -1;
            return 0;
        }

        // Raw keyframe
        if (ent.size != g_view.frame_bytes) return -1;
        return (reiv_eynfs_read_exact(file_off, (void*)g_view.frame565, g_view.frame_bytes) == 0) ? 0 : -1;
    }

    return -1;
}

static void viewer_draw_image() {
    // Drawing a frame can take long enough to starve the WM watchdog heartbeat.
    watchdog_kick("viewer-draw");
    if (g_view.is_reiv) {
        if (!g_view.frame565) return;
        // Reserve a small control bar at the bottom of the content region
        int ctrl_h = 16;
        int vx = g_view.content_x;
        int vy = g_view.content_y;
        int vw = g_view.content_w;
        int vh = g_view.content_h - ctrl_h;
        if (vh < 1) vh = g_view.content_h;

        // Fit-to-view while preserving aspect ratio
        int src_w = (int)g_view.vh.width;
        int src_h = (int)g_view.vh.height;
        int dst_w = vw;
        int dst_h = (vw > 0) ? (src_h * vw) / src_w : 0;
        if (dst_h > vh && vh > 0) {
            dst_h = vh;
            dst_w = (src_w * vh) / src_h;
        }
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
        int dst_x = vx + (vw - dst_w) / 2;
        int dst_y = vy + (vh - dst_h) / 2;

        // Advance frame timing (tick-based)
        uint32 hz = hal_time_tick_hz();
        if (!hz) hz = 50;
        // default to ~30fps if metadata is odd
        uint32 fps_num = g_view.vh.fps_num ? g_view.vh.fps_num : 30;
        uint32 fps_den = g_view.vh.fps_den ? g_view.vh.fps_den : 1;
        // Reset accumulator if pacing parameters change.
        if (g_view.frame_tick_hz != hz || g_view.frame_tick_fps_num != fps_num || g_view.frame_tick_fps_den != fps_den) {
            g_view.frame_tick_hz = hz;
            g_view.frame_tick_fps_num = fps_num;
            g_view.frame_tick_fps_den = fps_den;
            g_view.frame_tick_accum = 0;
        }

        // Numerator for "ticks per frame" rational.
        uint32 tick_num = hz * fps_den;
        if (tick_num == 0) tick_num = 1;

        uint32 now = (uint32)hal_time_ticks();

        if (g_view.playing) {
            watchdog_kick("reiv-play");
            uint32 next = g_view.frame_index + 1;
            if (next >= g_view.vh.frame_count) {
                if (g_view.loop_enabled) next = 0;
                else { next = g_view.vh.frame_count - 1; g_view.playing = 0; }
            }

            // Prefetch/decode the upcoming frame in bounded chunks.
            // Use a larger read chunk to reduce filesystem/ATA overhead.
            if (g_view.playing) {
                uint32 budget = 8192;
                if ((int32)(now - g_view.next_frame_tick) >= 0) {
                    // Behind schedule: spend a bit more effort to catch up.
                    budget = 16384;
                } else {
                    int32 slack = (int32)(g_view.next_frame_tick - now);
                    // Roughly "half a frame" of slack: compare against floor(ticks/frame).
                    uint32 base_ticks = tick_num / fps_num;
                    if (base_ticks < 1u) base_ticks = 1u;
                    if (slack > (int32)(base_ticks / 2u)) budget = 16384;
                }
                if (reiv_prefetch_start(next) == 0) {
                    reiv_prefetch_step(budget);
                }
            }
            // On tick: advance exactly one frame and reset schedule to avoid backlog spirals.
            if (g_view.playing && (int32)(now - g_view.next_frame_tick) >= 0) {
                if (g_view.prefetch_ready && g_view.prefetch_frame_index == next) {
                    uint16_t* tmp = g_view.frame565;
                    g_view.frame565 = g_view.frame565_next;
                    g_view.frame565_next = tmp;
                    g_view.frame_index = next;
                    g_view.dbg_advances++;
                    g_view.prefetch_ready = 0;
                    g_view.scaled_valid = 0; // frame changed
                    // Schedule next frame normally.
                    // Accumulator-based fractional tick pacing.
                    g_view.frame_tick_accum += tick_num;
                    uint32 inc = g_view.frame_tick_accum / fps_num;
                    g_view.frame_tick_accum %= fps_num;
                    if (inc < 1u) inc = 1u;
                    g_view.next_frame_tick = now + inc;
                } else {
                    // Don't do heavy synchronous I/O on the tick edge (causes visible hitches and UI stalls).
                    // Instead, keep displaying the current frame and try again soon while prefetch continues.
                    // Avoid retrying every tick (busy-loop) when decoding can't keep up.
                    // This prevents a spiral where we spend all CPU in redraw+prefetch attempts.
                    // Retry soon but don't busy-loop.
                    uint32 base = tick_num / fps_num;
                    if (base < 1u) base = 1u;
                    uint32 retry = base / 4u;
                    if (retry < 1u) retry = 1u;
                    if (retry > 3u) retry = 3u;
                    g_view.next_frame_tick = now + retry;
                }

                // Start/continue prefetching the following frame when we successfully advanced.
                if (g_view.playing && g_view.frame_index == next) {
                    uint32 next2 = g_view.frame_index + 1;
                    if (next2 >= g_view.vh.frame_count) next2 = g_view.loop_enabled ? 0 : (g_view.vh.frame_count - 1);
                    if (reiv_prefetch_start(next2) == 0) {
                        reiv_prefetch_step(4096);
                    }
                }
            }

            // Continuous redraw drives playback.
        } else {
            // If playback stopped, turn off continuous redraw to avoid burning CPU.
            if (g_view.is_window) wm_set_continuous_redraw(g_view.window_id, 0);
            else tile_set_gui_continuous_redraw(g_view.tile_idx, 0);
        }

        // Always redraw to avoid flicker/tearing artifacts from partial swaps.
        // Optimize by caching a pre-scaled RGB565 frame and using a fast 1:1 RGB565 blit.
        drawRect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h, 0,0,0);

        if (dst_w == (int)g_view.vh.width && dst_h == (int)g_view.vh.height) {
            vga_mark_dirty_rect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h);
            watchdog_kick("reiv-blit");
            vga_blit_rgb565_bb(dst_x, dst_y, g_view.frame565, (int)g_view.vh.width, (int)g_view.vh.height);
        } else {
            uint32_t need_w = (uint32_t)dst_w;
            uint32_t need_h = (uint32_t)dst_h;
            uint32_t need_px = need_w * need_h;
            if (!g_view.scaled565 || g_view.scaled_w != need_w || g_view.scaled_h != need_h) {
                if (g_view.scaled565) { free(g_view.scaled565); g_view.scaled565 = NULL; }
                // Allocate scaled buffer (RGB565)
                g_view.scaled565 = (uint16_t*)malloc(need_px * 2u);
                g_view.scaled_w = need_w;
                g_view.scaled_h = need_h;
                g_view.scaled_valid = 0;
            }
            if (g_view.scaled565) {
                if (!g_view.scaled_valid || g_view.scaled_src_frame_index != g_view.frame_index) {
                    watchdog_kick("reiv-scale");
                    reiv_scale_rgb565_nn(g_view.scaled565, g_view.scaled_w, g_view.scaled_h,
                                         g_view.frame565, (uint32_t)g_view.vh.width, (uint32_t)g_view.vh.height);
                    g_view.scaled_src_frame_index = g_view.frame_index;
                    g_view.scaled_valid = 1;
                }
                vga_mark_dirty_rect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h);
                watchdog_kick("reiv-blit");
                vga_blit_rgb565_bb(dst_x, dst_y, g_view.scaled565, (int)g_view.scaled_w, (int)g_view.scaled_h);
            } else {
                // Fallback if allocation failed
                vga_mark_dirty_rect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h);
                watchdog_kick("reiv-blit");
                vga_blit_rgb565_scaled_bb(dst_x, dst_y, dst_w, dst_h, g_view.frame565, (int)g_view.vh.width, (int)g_view.vh.height);
            }
        }

        // Controls bar
        int cy = g_view.content_y + g_view.content_h - ctrl_h;
        if (cy < g_view.content_y) cy = g_view.content_y;
        drawRect(g_view.content_x, cy, g_view.content_w, ctrl_h, 24, 24, 24);
        const char* play_txt = g_view.playing ? "Pause" : "Play";
        char loop_txt[32];
        if (g_view.loop_locked) snprintf(loop_txt, sizeof(loop_txt), "Loop: On");
        else snprintf(loop_txt, sizeof(loop_txt), "Loop: %s", g_view.loop_enabled ? "On" : "Off");
        drawTextAt(g_view.content_x + 6, cy + 4, play_txt, 255, 255, 255);
        drawTextAt(g_view.content_x + 70, cy + 4, loop_txt, 200, 200, 200);
        return;
    }

    // Clear content area for non-REIV images
    drawRect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h, 0,0,0);

    if (!g_view.img.data) return;
    // Draw image with zoom and pan
    int ox = g_view.content_x + g_view.off_x;
    int oy = g_view.content_y + g_view.off_y;
    // Mark content area dirty once; we'll draw many pixels without per-pixel dirty marks
    vga_mark_dirty_rect(g_view.content_x, g_view.content_y, g_view.content_w, g_view.content_h);
    // Compute visible image range in source pixels
    int start_x = (g_view.content_x - ox) / g_view.zoom; if (start_x < 0) start_x = 0;
    int start_y = (g_view.content_y - oy) / g_view.zoom; if (start_y < 0) start_y = 0;
    int end_x = (g_view.content_x + g_view.content_w - 1 - ox) / g_view.zoom + 1; if (end_x > g_view.img.header.width) end_x = g_view.img.header.width;
    int end_y = (g_view.content_y + g_view.content_h - 1 - oy) / g_view.zoom + 1; if (end_y > g_view.img.header.height) end_y = g_view.img.header.height;
    for (int y = start_y; y < end_y; ++y) {
        for (int x = start_x; x < end_x; ++x) {
            int off = rei_get_pixel_offset(&g_view.img.header, x, y);
            if (off < 0) continue;
            uint8 r = 0, g = 0, b = 0;
            if (g_view.img.header.depth == REI_DEPTH_MONO) {
                if (off >= (int)g_view.img.data_size) continue;
                uint8 gray = g_view.img.data[off];
                r = g = b = gray;
            } else if (g_view.img.header.depth == REI_DEPTH_RGB || g_view.img.header.depth == REI_DEPTH_RGBA) {
                if (off + 2 >= (int)g_view.img.data_size) continue;
                r = g_view.img.data[off+0];
                g = g_view.img.data[off+1];
                b = g_view.img.data[off+2];
            } else {
                // Unsupported depth
                continue;
            }
            for (int zy=0; zy<g_view.zoom; ++zy) for (int zx=0; zx<g_view.zoom; ++zx) {
                int px = ox + x*g_view.zoom + zx;
                int py = oy + y*g_view.zoom + zy;
                if (px>=g_view.content_x && py>=g_view.content_y && px<g_view.content_x+g_view.content_w && py<g_view.content_y+g_view.content_h)
                    vga_drawPixel_bb(px, py, r,g,b);
            }
        }
    }
}

static void viewer_gui_draw(int tile_idx, int cx, int cy, int cw, int ch, void* ud) {
    (void)ud; g_view.tile_idx = tile_idx; g_view.content_x=cx; g_view.content_y=cy; g_view.content_w=cw; g_view.content_h=ch;
    g_view.dbg_draws++;

    // Immediate debug for first few draws (does not depend on tick cadence)
    if (g_view.is_reiv && g_view.dbg_draws <= 5) {
        uint32 now = (uint32)hal_time_ticks();
        uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
        char sbuf[220];
        int n = snprintf(sbuf, sizeof(sbuf),
                         "[REIV] draw#%u idx=%u/%u play=%d now=%u hz=%u next=%u\n",
                         (unsigned)g_view.dbg_draws,
                         (unsigned)g_view.frame_index, (unsigned)g_view.vh.frame_count,
                         g_view.playing ? 1 : 0,
                         (unsigned)now, (unsigned)hz, (unsigned)g_view.next_frame_tick);
        if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
    }

    viewer_draw_image();
    // If playing video, request continuous redraw for smooth playback
    if (g_view.is_reiv && g_view.playing) {
        if (g_view.is_window) wm_set_continuous_redraw(g_view.window_id, 1);
        else tile_set_gui_continuous_redraw(g_view.tile_idx, 1);
    }

    // Throttled serial debug to diagnose “first frame only”
    if (g_view.is_reiv) {
        uint32 now = (uint32)hal_time_ticks();
        uint32 hz = hal_time_tick_hz();
        if (!hz) hz = 50;
        if ((int32)(now - g_view.dbg_last_print_tick) >= (int32)hz) {
            g_view.dbg_last_print_tick = now;
            char sbuf[220];
            int n = snprintf(sbuf, sizeof(sbuf),
                             "[REIV] draws=%u adv=%u idx=%u/%u play=%d now=%u next=%u\n",
                             (unsigned)g_view.dbg_draws, (unsigned)g_view.dbg_advances,
                             (unsigned)g_view.frame_index, (unsigned)g_view.vh.frame_count,
                             g_view.playing ? 1 : 0, (unsigned)now, (unsigned)g_view.next_frame_tick);
            if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
        }
    }
}

static void viewer_gui_key(int tile_idx, int key, void* ud) {
    (void)ud; (void)tile_idx; int changed=0;
    if (key==0x2102) { if (g_view.zoom<8) { g_view.zoom++; changed=1; } } // Ctrl+Plus
    else if (key==0x2103) { if (g_view.zoom>1) { g_view.zoom--; changed=1; } } // Ctrl+Minus
    else if (key==' ' && g_view.is_reiv) {
        uint32 now = (uint32)hal_time_ticks();
        uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
        uint32 debounce = hz / 5; if (debounce < 2) debounce = 2; // ~200ms
        if ((int32)(now - g_view.last_toggle_tick) >= (int32)debounce) {
            g_view.last_toggle_tick = now;
            g_view.playing = !g_view.playing;
            if (g_view.playing) {
                g_view.next_frame_tick = now; // restart immediately
            }
            g_view.scaled_valid = 0;
            changed = 1;

            char sbuf[96];
            int n = snprintf(sbuf, sizeof(sbuf), "[REIV] key toggle play=%d now=%u\n", g_view.playing ? 1 : 0, (unsigned)now);
            if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
        }
    }
    else if ((key=='l' || key=='L') && g_view.is_reiv && !g_view.loop_locked) { g_view.loop_enabled = !g_view.loop_enabled; changed = 1; }
    else if (key==0x2101) { // Ctrl+Q closes
        viewer_free_resources();
        if (g_view.is_window) {
            wm_close_window(g_view.window_id);
        } else if (g_view.tile_idx>=0) {
            tile_unregister_gui_client(g_view.tile_idx);
        }
        return;
    }
    if (changed) {
        if (g_view.is_reiv) g_view.scaled_valid = 0;
        if (g_view.is_reiv) {
            if (g_view.is_window) wm_set_continuous_redraw(g_view.window_id, g_view.playing ? 1 : 0);
            else tile_set_gui_continuous_redraw(g_view.tile_idx, g_view.playing ? 1 : 0);
        }
        if (g_view.is_window) wm_invalidate_window(g_view.window_id);
        else tile_invalidate_gui(g_view.tile_idx);
    }
}

static void viewer_gui_mouse(int tile_idx, const mouse_event_t* me, void* ud) {
    (void)ud; (void)tile_idx;
    // Clickable controls for REIV
    int left_now = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    int left_edge = (left_now && !g_view.last_mouse_left);
    g_view.last_mouse_left = (uint8_t)(left_now ? 1 : 0);

    if (g_view.is_reiv && left_edge) {
        uint32 now = (uint32)hal_time_ticks();
        uint32 hz = hal_time_tick_hz(); if (!hz) hz = 50;
        uint32 debounce = hz / 5; if (debounce < 2) debounce = 2; // ~200ms
        if ((int32)(now - g_view.last_click_tick) < (int32)debounce) {
            return;
        }
        g_view.last_click_tick = now;

        int ctrl_h = 16;
        int cy = g_view.content_y + g_view.content_h - ctrl_h;
        if (me->y >= cy && me->y < cy + ctrl_h) {
            // Play/Pause hit area: first ~60px
            if (me->x >= g_view.content_x && me->x < g_view.content_x + 60) {
                g_view.playing = !g_view.playing;
                if (g_view.playing) {
                    g_view.next_frame_tick = now;
                }
                g_view.scaled_valid = 0;
            }
            // Loop hit area: after ~60px
            else if (me->x >= g_view.content_x + 60 && me->x < g_view.content_x + 160) {
                if (!g_view.loop_locked) g_view.loop_enabled = !g_view.loop_enabled;
                g_view.scaled_valid = 0;
            }
            if (g_view.is_window) {
                wm_set_continuous_redraw(g_view.window_id, g_view.playing ? 1 : 0);
                wm_invalidate_window(g_view.window_id);
            } else {
                tile_set_gui_continuous_redraw(g_view.tile_idx, g_view.playing ? 1 : 0);
                tile_invalidate_gui(g_view.tile_idx);
            }
            return;
        }
    }

    int left = left_now;
    if (left && !g_view.dragging) { g_view.dragging=1; g_view.drag_start_x=me->x; g_view.drag_start_y=me->y; g_view.drag_off_x=g_view.off_x; g_view.drag_off_y=g_view.off_y; }
    else if (!left && g_view.dragging) { g_view.dragging=0; }
    if (g_view.dragging) {
        g_view.off_x = g_view.drag_off_x + (me->x - g_view.drag_start_x);
        g_view.off_y = g_view.drag_off_y + (me->y - g_view.drag_start_y);
        if (g_view.is_window) wm_invalidate_window(g_view.window_id);
        else tile_invalidate_gui(g_view.tile_idx);
    }
}

static int load_reiv_stream(const char* path) {
    // Read header and first frame; keep entry info for streaming
    const char* fail = NULL;
    g_view.disk = get_current_logical_drive();
    if (eynfs_read_superblock(g_view.disk, EYNFS_SUPERBLOCK_LBA, &g_view.sb) != 0 || g_view.sb.magic != EYNFS_MAGIC) { fail = "superblock"; goto fail_out; }
    uint32_t pb = 0, ei = 0;
    if (eynfs_traverse_path(g_view.disk, &g_view.sb, path, &g_view.entry, &pb, &ei) != 0) { fail = "traverse"; goto fail_out; }
    if (g_view.entry.type != EYNFS_TYPE_FILE) { fail = "notfile"; goto fail_out; }

    uint8_t hdrbuf[64];
    int br = eynfs_read_file(g_view.disk, &g_view.sb, &g_view.entry, (char*)hdrbuf, (int)sizeof(reiv_header_t), 0);
    if (br != (int)sizeof(reiv_header_t)) { fail = "hdrread"; goto fail_out; }
    if (reiv_read_header(hdrbuf, sizeof(reiv_header_t), &g_view.vh) != 0) { fail = "hdrparse"; goto fail_out; }
    if (reiv_validate_header(&g_view.vh) != 0) { fail = "hdrinvalid"; goto fail_out; }

    g_view.frame_bytes = reiv_frame_size_bytes(&g_view.vh);
    if (!g_view.frame_bytes) { fail = "framesize"; goto fail_out; }

    // v2: read index table
    if (g_view.vh.version == REIV_VERSION_V2 || g_view.vh.version == REIV_VERSION_V3) {
        g_view.reiv_index_bytes = reiv_index_size_bytes(&g_view.vh);
        if (!g_view.reiv_index_bytes) { fail = "idxsize"; goto fail_out; }
        g_view.reiv_index = (uint8_t*)malloc(g_view.reiv_index_bytes);
        if (!g_view.reiv_index) { fail = "idxalloc"; goto fail_out; }
        int ibr = eynfs_read_file(g_view.disk, &g_view.sb, &g_view.entry, (char*)g_view.reiv_index, (int)g_view.reiv_index_bytes, (size_t)g_view.vh.frames_offset);
        if (ibr != (int)g_view.reiv_index_bytes) { fail = "idxread"; goto fail_out; }
        g_view.reiv_data_offset = reiv_data_offset_bytes(&g_view.vh);
        g_view.frame_comp = (uint8_t*)malloc(g_view.frame_bytes);
        if (!g_view.frame_comp) { fail = "cmpalloc"; goto fail_out; }
    }

    g_view.frame565 = (uint16_t*)malloc(g_view.frame_bytes);
    if (!g_view.frame565) { fail = "frmalloc"; goto fail_out; }

    g_view.frame565_next = (uint16_t*)malloc(g_view.frame_bytes);
    if (!g_view.frame565_next) { fail = "frmalloc2"; goto fail_out; }

    if (reiv_stream_load_frame(0) != 0) { fail = "frame0"; goto fail_out; }

    g_view.is_reiv = 1;
    g_view.frame_index = 0;
    g_view.playing = 1;
    g_view.loop_enabled = (g_view.vh.flags & REIV_FLAG_LOOP_DEFAULT) ? 1 : 0;
    g_view.loop_locked = (g_view.vh.flags & REIV_FLAG_LOOP_LOCKED) ? 1 : 0;
    // Start the first advance slightly in the future to allow prefetch of frame 1.
    {
        uint32 hz = hal_time_tick_hz();
        if (!hz) hz = 50;
        uint32 fps_num = g_view.vh.fps_num ? g_view.vh.fps_num : 30;
        uint32 fps_den = g_view.vh.fps_den ? g_view.vh.fps_den : 1;
        // Initialize accumulator-based pacing state.
        g_view.frame_tick_hz = hz;
        g_view.frame_tick_fps_num = fps_num;
        g_view.frame_tick_fps_den = fps_den;
        g_view.frame_tick_accum = 0;

        uint32 tick_num = hz * fps_den;
        if (tick_num == 0) tick_num = 1;
        uint32 inc = tick_num / fps_num;
        if (inc < 1u) inc = 1u;
        uint32 now = (uint32)hal_time_ticks();
        g_view.next_frame_tick = now + inc;
    }

    // Reset prefetch state; we'll begin prefetching on the first draw.
    reiv_prefetch_reset();

    {
        char sbuf[220];
        int n = snprintf(sbuf, sizeof(sbuf), "[REIV] loaded ver=%u %ux%u frames=%u fps=%u/%u flags=%u\n",
                         (unsigned)g_view.vh.version,
                         (unsigned)g_view.vh.width, (unsigned)g_view.vh.height,
                         (unsigned)g_view.vh.frame_count,
                         (unsigned)g_view.vh.fps_num, (unsigned)g_view.vh.fps_den,
                         (unsigned)g_view.vh.flags);
        if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
    }
    return 0;

fail_out:
    if (fail) {
        snprintf(g_view.status_left, sizeof(g_view.status_left), "REIV load failed: %s", fail);
        char sbuf[220];
        int n = snprintf(sbuf, sizeof(sbuf), "[REIV] load failed (%s) path=%s\n", fail, path);
        if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
    }
    viewer_free_resources();
    return -1;
}

static void open_viewer_gui(const char* path) {
    viewer_free_resources();
    memset(&g_view, 0, sizeof(g_view)); g_view.zoom=1; g_view.off_x=4; g_view.off_y=4;
    strncpy(g_view.filepath, path, sizeof(g_view.filepath)-1);
    const char* b = get_basename_local(path); strncpy(g_view.filename_base, b, sizeof(g_view.filename_base)-1);
    g_view.is_window = 0; g_view.window_id = -1;
    snprintf(g_view.status_left, sizeof(g_view.status_left), "Ctrl+Plus/Minus: Zoom | Space: Play/Pause | L: Loop | Ctrl+X: Close");
    // Load image file from EYNFS
    // Detect magic via small read
    {
        uint8 disk = get_current_logical_drive();
        eynfs_superblock_t sb; eynfs_dir_entry_t entry; uint32_t pb, ei;
        if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
            if (eynfs_traverse_path(disk, &sb, path, &entry, &pb, &ei) == 0 && entry.type == EYNFS_TYPE_FILE) {
                uint32_t sz = entry.size; if (sz > 64) sz = 64;
                uint8* buf = (uint8*)malloc(sz);
                if (buf) {
                    int br = eynfs_read_file(disk, &sb, &entry, buf, (int)sz, 0);
                    if (br >= 4) {
                        uint32_t magic = *(uint32_t*)buf;
                        if (magic == REIV_MAGIC) {
                            // Stream REIV instead of loading whole file
                            if (load_reiv_stream(path) != 0) {
                                snprintf(g_view.status_left, sizeof(g_view.status_left), "REIV load failed");
                                char sbuf[120];
                                int n = snprintf(sbuf, sizeof(sbuf), "[REIV] load failed path=%s\n", path);
                                if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
                            }
                        } else {
                            // Fallback: load static REI into memory
                            if (sz < 65536) {
                                // Re-read up to 64KB to cover typical REI images
                                free(buf);
                                sz = entry.size; if (sz > 65536) sz = 65536;
                                buf = (uint8*)malloc(sz);
                                if (buf) {
                                    br = eynfs_read_file(disk, &sb, &entry, buf, (int)sz, 0);
                                    if (br > 0) rei_parse_image(buf, br, &g_view.img);
                                    free(buf);
                                }
                            } else {
                                if (br > 0) rei_parse_image(buf, br, &g_view.img);
                                free(buf);
                            }
                        }
                    }
                    else free(buf);
                }
            }
        }
    }
    static char title_buf[128]; snprintf(title_buf, sizeof(title_buf), "%s - Viewer", g_view.filename_base);
    int t = tile_create_gui_tile(title_buf, g_view.status_left);
    if (t >= 0) {
        tile_register_gui_client2(t, viewer_gui_draw, viewer_gui_key, viewer_gui_mouse, NULL);
        if (g_view.is_reiv && g_view.playing) tile_set_gui_continuous_redraw(t, 1);
    }
}

static void open_viewer_window(const char* path) {
    viewer_free_resources();
    memset(&g_view, 0, sizeof(g_view)); g_view.zoom=1; g_view.off_x=4; g_view.off_y=4;
    strncpy(g_view.filepath, path, sizeof(g_view.filepath)-1);
    const char* b = get_basename_local(path); strncpy(g_view.filename_base, b, sizeof(g_view.filename_base)-1);
    g_view.is_window = 1; g_view.window_id = -1;
    snprintf(g_view.status_left, sizeof(g_view.status_left), "Ctrl+Plus/Minus: Zoom | Space: Play/Pause | L: Loop | Ctrl+X: Close");
    // Detect magic via small read
    {
        uint8 disk = get_current_logical_drive();
        eynfs_superblock_t sb; eynfs_dir_entry_t entry; uint32_t pb, ei;
        if (eynfs_read_superblock(disk, EYNFS_SUPERBLOCK_LBA, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
            if (eynfs_traverse_path(disk, &sb, path, &entry, &pb, &ei) == 0 && entry.type == EYNFS_TYPE_FILE) {
                uint32_t sz = entry.size; if (sz > 64) sz = 64;
                uint8* buf = (uint8*)malloc(sz);
                if (buf) {
                    int br = eynfs_read_file(disk, &sb, &entry, buf, (int)sz, 0);
                    if (br >= 4) {
                        uint32_t magic = *(uint32_t*)buf;
                        if (magic == REIV_MAGIC) {
                            if (load_reiv_stream(path) != 0) {
                                snprintf(g_view.status_left, sizeof(g_view.status_left), "REIV load failed");
                                char sbuf[120];
                                int n = snprintf(sbuf, sizeof(sbuf), "[REIV] load failed path=%s\n", path);
                                if (n > 0) serial_write(SERIAL_COM1, sbuf, n);
                            }
                        } else {
                            free(buf);
                            sz = entry.size; if (sz > (1u<<20)) sz = (1u<<20);
                            buf = (uint8*)malloc(sz);
                            if (buf) {
                                br = eynfs_read_file(disk, &sb, &entry, buf, (int)sz, 0);
                                if (br > 0) rei_parse_image(buf, br, &g_view.img);
                                free(buf);
                            }
                        }
                    } else {
                        free(buf);
                    }
                }
            }
        }
    }
    static char title_buf[128]; snprintf(title_buf, sizeof(title_buf), "%s - Viewer", g_view.filename_base);
    // Create a reasonable default window size and position
    int win_w = 360, win_h = 280; int win_x = 40, win_y = 40;
    int wid = wm_create_window(title_buf, win_x, win_y, win_w, win_h, g_view.status_left);
    if (wid >= 0) {
        g_view.window_id = wid;
        wm_register_gui_client2(wid, viewer_gui_draw, viewer_gui_key, viewer_gui_mouse, NULL);
        wm_set_title_status(wid, title_buf, g_view.status_left, NULL);
        wm_invalidate_window(wid);
        if (g_view.is_reiv && g_view.playing) wm_set_continuous_redraw(wid, 1);
    }
}

// Command entry points
#include <shell_command_info.h>
void view_cmd(string ch) {
    uint8 i=0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i]==' ') i++;
    if (!ch[i]) { printf("%cUsage: view <file.rei>\n", 255,255,255); return; }
    char arg[128]={0}; uint8 j=0; while (ch[i] && ch[i] != ' ' && j<127) arg[j++]=ch[i++]; arg[j]='\0';
    char abspath[128]; resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    open_viewer_gui(abspath);
}
REGISTER_SHELL_COMMAND(view_cmd_info, "view", view_cmd, CMD_STREAMING, "Open a REI image in a GUI viewer.\nUsage: view <file.rei>", "view eynos.rei");

// Window variant: open viewer in a floating window
void vieww_cmd(string ch) {
    uint8 i=0; while (ch[i] && ch[i] != ' ') i++; while (ch[i] && ch[i]==' ') i++;
    if (!ch[i]) { printf("%cUsage: vieww <file.rei>\n", 255,255,255); return; }
    char arg[128]={0}; uint8 j=0; while (ch[i] && ch[i] != ' ' && j<127) arg[j++]=ch[i++]; arg[j]='\0';
    char abspath[128]; resolve_path(arg, shell_current_path, abspath, sizeof(abspath));
    open_viewer_window(abspath);
}
REGISTER_SHELL_COMMAND(vieww_cmd_info, "vieww", vieww_cmd, CMD_STREAMING, "Open a REI image in a floating window.\nUsage: vieww <file.rei>", "vieww eynos.rei");
