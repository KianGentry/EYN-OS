#include <tile_manager.h>
#include <tui.h>
#include <vga.h>
#include <mouse.h>
#include <util.h>
#include <misc/sched.h>
#include <system.h>
#include <eynfs.h>
#include <string.h>
#include <shell_command_info.h>

// Simple system stats GUI: shows 3 pie charts (CPU, Memory, Disk) and a sortable table below
// CPU usage is estimated from scheduler ticks vs. idle hlt count
// Memory usage is derived from heap stats in util.c
// Disk usage is read from EYNFS superblock + bitmap

typedef struct {
    int tile_idx;
    // layout cache
    int cx, cy, cw, ch; // content rect
    // sorting
    int sort_col; // 0=CPU,1=Mem,2=Disk
    int sort_dir; // 1=asc,-1=desc
    // sampling state
    uint32 last_ticks;
    uint32 last_idle_hlt;
    float cpu_percent;
    uint32 cpu_busy_hz;     // busy ticks measured in last update window
    uint32 cpu_total_hz;    // total ticks in last update window (== tick_hz ideally)
    // CPU frequency estimation (MHz) sampled over the stats window
    uint32 cpu_mhz_total;   // estimated total CPU frequency in MHz
    uint32 tsc_lo_last;     // low 32-bits of TSC at last UI sample
    uint32 tsc_tick_last;   // tick count when tsc_lo_last was captured
    // One-shot non-blocking calibration state to ensure we populate cpu_mhz_total soon after launch
    uint32 calib_start_tick;
    uint32 calib_start_tsc_lo;
    int calib_pending; // 0=none, 1=waiting for window to elapse
    int disable_tsc;    // if set, skip TSC-based MHz and show tick units
    // memory
    uint32 heap_total;
    uint32 heap_used;
    uint32 total_ram_bytes; // system total RAM from multiboot
    float mem_percent;
    // disk
    float disk_percent;
    uint32 disk_used_blocks;
    uint32 disk_total_blocks;
    // table rect for mouse hit-testing
    int table_x, table_y, table_w, table_h;
    // throttling
    uint32 frame_counter;
    uint32 last_ui_update_tick; // last time we recomputed stats
} stats_state_t;

static stats_state_t g_stats;

// Helpers to format percentages without relying on float printf (which may be unsupported)
static int clamp_pct_to_int(float pct) {
    int p = (int)(pct + 0.5f);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

static void format_pct_str(char* buf, int n, float pct) {
    int p = clamp_pct_to_int(pct);
    // %d should be supported; append literal percent sign
    snprintf(buf, (size_t)n, "%d%%", p);
}

static void format_pct_plain_str(char* buf, int n, float pct) {
    int p = clamp_pct_to_int(pct);
    snprintf(buf, (size_t)n, "%d", p);
}

// Smart size formatter: if >=10MB show integer MB; if >=1MB show one decimal MB; else show KB
static void format_size_smart(char* buf, int n, uint32 bytes) {
    const uint32 KB = 1024u;
    const uint32 MB = 1024u * 1024u;
    if (bytes >= 10u * MB) {
        uint32 mb = bytes / MB;
        snprintf(buf, (size_t)n, "%uMB", (unsigned)mb);
    } else if (bytes >= 1u * MB) {
        // one decimal without floats: floor(bytes*10/MB) => X.Y
        uint32 mb10 = (bytes / (MB / 10u)); // bytes * 10 / MB, avoiding overflow
        uint32 ip = mb10 / 10u;
        uint32 dp = mb10 % 10u;
        snprintf(buf, (size_t)n, "%u.%uMB", (unsigned)ip, (unsigned)dp);
    } else {
        uint32 kb = bytes / KB;
        snprintf(buf, (size_t)n, "%uKB", (unsigned)kb);
    }
}

// Minimal atan2 approximation (no libm). Returns angle in [0, 2*pi)
static float fast_atan2f(float y, float x) {
    float abs_y = y >= 0 ? y : -y;
    float angle;
    if (x >= 0) {
        float r = (x + abs_y) > 0 ? (x - abs_y) / (x + abs_y) : 0.0f;
        angle = 0.78539816339f - 0.78539816339f * r; // pi/4 - pi/4*r
    } else {
        float r = (abs_y - x) > 0 ? (x + abs_y) / (abs_y - x) : 0.0f;
        angle = 2.35619449019f - 0.78539816339f * r; // 3*pi/4 - pi/4*r
    }
    if (y < 0) angle = -angle;
    if (angle < 0) angle += 6.28318530718f;
    return angle;
}

static void draw_pie(int cx, int cy, int radius, float percent, int r1,int g1,int b1, int r2,int g2,int b2) {
    // Very simple filled-circle sectors using polar approximation (coarse)
    // percent in [0,100]
    float theta = (percent / 100.0f) * 6.2831853f; // 2*pi
    int rr = radius;
    for (int y = -rr; y <= rr; ++y) {
        for (int x = -rr; x <= rr; ++x) {
            if (x*x + y*y <= rr*rr) {
                float ang = fast_atan2f((float)y, (float)x); // [0,2pi)
                if (ang <= theta) drawPixel(cx + x, cy + y, r1,g1,b1);
                else drawPixel(cx + x, cy + y, r2,g2,b2);
            }
        }
    }
}

// No 64-bit division: use 32-bit cycles-per-tick (safe for <= ~4GHz and <=1s window)

// Minimal CPUID helpers (i386+). If CPUID not supported, assume no TSC.
static int cpu_has_cpuid(void) {
    int supported = 0;
    unsigned int eflags_before, eflags_after;
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %1\n\t"    // toggle ID bit
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        : "=r"(eflags_before), "=r"(eflags_after)
        :
        : "cc");
    supported = ((eflags_before ^ eflags_after) & 0x200000) != 0;
    return supported;
}

static void cpuid_eax(uint32 leaf, uint32* a, uint32* b, uint32* c, uint32* d) {
    uint32 ra=0, rb=0, rc=0, rd=0;
    __asm__ __volatile__("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(leaf), "c"(0));
    if (a) *a = ra; if (b) *b = rb; if (c) *c = rc; if (d) *d = rd;
}

static int cpu_has_tsc(void) {
    if (!cpu_has_cpuid()) return 0;
    uint32 a=0,b=0,c=0,d=0; cpuid_eax(1, &a,&b,&c,&d);
    return (d & (1u<<4)) != 0; // EDX bit 4: TSC
}

static int cpu_has_invariant_tsc(void) {
    if (!cpu_has_cpuid()) return 0;
    uint32 a=0,b=0,c=0,d=0; cpuid_eax(0x80000000, &a,&b,&c,&d);
    if (a < 0x80000007u) return 0;
    cpuid_eax(0x80000007, &a,&b,&c,&d);
    return (d & (1u<<8)) != 0; // EDX bit 8: invariant TSC
}

static void stats_sample_disk(void) {
    // Read EYNFS superblock and count free blocks similar to fsstat_cmd
    extern uint8_t g_current_drive; // physical
    eynfs_superblock_t sb;
    g_stats.disk_total_blocks = 0; g_stats.disk_used_blocks = 0; g_stats.disk_percent = 0.f;
    if (eynfs_read_superblock(g_current_drive, 2048, &sb) == 0 && sb.magic == EYNFS_MAGIC) {
        int free_blocks = 0;
        static uint8_t sector_buf[512];
        int32_t current_bitmap_sector = -1;
        // EYNFS stores block numbers relative to the filesystem base (superblock at LBA 2048).
        // Prefer bitmap block from superblock if provided; fallback to block 1.
        uint32_t bitmap_base_lba = 2048 + ((sb.free_block_map != 0) ? sb.free_block_map : 1);
        for (int i = 0; i < (int)sb.total_blocks; i++) {
            uint32_t bitmap_index = (uint32_t)i / 8; // which byte in bitmap
            uint32_t sector_offset = bitmap_index / 512; // which sector within bitmap region
            uint32_t byte_in_sector = bitmap_index % 512; // which byte within that sector
            uint32_t lba = bitmap_base_lba + sector_offset;
            if ((int32_t)lba != current_bitmap_sector) {
                if (ata_read_sector(g_current_drive, lba, sector_buf) != 0) {
                    // On read failure, break to avoid undefined behavior
                    break;
                }
                current_bitmap_sector = (int32_t)lba;
            }
            uint8_t bitmap_byte = sector_buf[byte_in_sector];
            if (!(bitmap_byte & (1 << (i % 8)))) free_blocks++;
        }
        g_stats.disk_total_blocks = sb.total_blocks;
        g_stats.disk_used_blocks = sb.total_blocks - free_blocks;
        if (sb.total_blocks) g_stats.disk_percent = (g_stats.disk_used_blocks * 100.0f) / (float)sb.total_blocks;
    }
}

static void stats_sample_memory(void) {
    extern uint32 get_heap_size(void);
    extern uint32 get_heap_used(void);
    extern uint32 get_total_ram(void);
    g_stats.heap_total = get_heap_size();
    g_stats.heap_used = get_heap_used();
    g_stats.total_ram_bytes = get_total_ram();
    if (g_stats.total_ram_bytes) {
        g_stats.mem_percent = (g_stats.heap_used * 100.0f) / (float)g_stats.total_ram_bytes;
        if (g_stats.mem_percent < 0.f) {
            g_stats.mem_percent = 0.f;
        }
        if (g_stats.mem_percent > 100.f) {
            g_stats.mem_percent = 100.f;
        }
    } else {
        g_stats.mem_percent = 0.f;
    }
}

static void stats_update_cpu(void) {
    uint32 ticks = sched_get_tick_count();
    uint32 idle = sched_get_idle_hlt_count();
    uint32 dt = ticks - g_stats.last_ticks;
    uint32 didle = idle - g_stats.last_idle_hlt;
    g_stats.last_ticks = ticks;
    g_stats.last_idle_hlt = idle;
    if (dt == 0) return;
    // Each tick corresponds to at least one HLT during sleep when idle paths run; derive busy as 1 - (didle/dt) clamped
    float idle_frac = 0.f;
    uint32 idle_ticks = (didle > dt) ? dt : didle;
    idle_frac = (float)idle_ticks / (float)dt;
    float cpu = (1.f - idle_frac) * 100.f;
    if (cpu < 0.f) {
        cpu = 0.f;
    }
    if (cpu > 100.f) {
        cpu = 100.f;
    }
    g_stats.cpu_percent = cpu;
    // keep busy/total in "Hz" units (ticks per second window)
    g_stats.cpu_busy_hz = dt - idle_ticks;
    g_stats.cpu_total_hz = dt;
}

static void stats_draw_table(int x, int y, int w, int h) {
    // Simple 3-column table headers: CPU, Memory, Disk. Show top consumers if we can; fallback to single system row
    // Draw header background
    drawRect(x, y, w, 14, 32,32,32);
    const char* h0 = "CPU %"; const char* h1 = "Memory %"; const char* h2 = "Disk %";
    drawTextAt(x+8, y+3, h0, 255,255,255);
    drawTextAt(x + w/3 + 8, y+3, h1, 255,255,255);
    drawTextAt(x + 2*w/3 + 8, y+3, h2, 255,255,255);
    // Rows (placeholder: system totals only)
    int ry = y + 18;
    char buf[32];
    format_pct_plain_str(buf, (int)sizeof(buf), g_stats.cpu_percent);
    drawTextAt(x+8, ry, buf, 200,200,0);
    format_pct_plain_str(buf, (int)sizeof(buf), g_stats.mem_percent);
    drawTextAt(x + w/3 + 8, ry, buf, 0,200,200);
    format_pct_plain_str(buf, (int)sizeof(buf), g_stats.disk_percent);
    drawTextAt(x + 2*w/3 + 8, ry, buf, 200,0,200);
}

static void stats_gui_draw(int tile_idx, int content_x, int content_y, int content_w, int content_h, void* userdata) {
    (void)userdata; g_stats.tile_idx = tile_idx;
    g_stats.cx = content_x; g_stats.cy = content_y; g_stats.cw = content_w; g_stats.ch = content_h;
    // Background
    drawRect(content_x, content_y, content_w, content_h, 16,16,16);
    // Update at most once per second to keep UI smooth
    uint32 now_ticks = sched_get_tick_count();
    uint32 tick_hz = sched_get_tick_hz(); if (tick_hz == 0) tick_hz = 50;
    if (now_ticks - g_stats.last_ui_update_tick >= tick_hz) {
        stats_update_cpu();
        stats_sample_memory();
        stats_sample_disk();
        if (!g_stats.disable_tsc) {
            // Estimate CPU MHz using TSC over this sampling window (avoid 64-bit division)
            uint32 tsc_lo_now, tsc_hi_now;
            __asm__ __volatile__("rdtsc" : "=a"(tsc_lo_now), "=d"(tsc_hi_now));
            if (g_stats.tsc_tick_last != 0) {
                uint32 dt_ticks = now_ticks - g_stats.tsc_tick_last;
                if (dt_ticks > 0) {
                    // Handle 32-bit wrap of TSC low-half
                    uint32 dcycles_lo = (tsc_lo_now >= g_stats.tsc_lo_last)
                        ? (tsc_lo_now - g_stats.tsc_lo_last)
                        : (tsc_lo_now + (0xFFFFFFFFu - g_stats.tsc_lo_last) + 1u);
                    uint32 cycles_per_sec = (dcycles_lo / dt_ticks) * tick_hz;
                    uint32 mhz_est = cycles_per_sec / 1000000u;
                    if (mhz_est > 0 && mhz_est < 100000u) g_stats.cpu_mhz_total = mhz_est;
                }
            }
            g_stats.tsc_lo_last = tsc_lo_now;
            g_stats.tsc_tick_last = now_ticks;
        }
        g_stats.last_ui_update_tick = now_ticks;
    }
    // Complete one-shot calibration if pending and at least ~1s elapsed
    if (g_stats.calib_pending && !g_stats.disable_tsc) {
        uint32 hz2 = tick_hz ? tick_hz : 50;
        uint32 dt_ticks2 = now_ticks - g_stats.calib_start_tick;
        if (dt_ticks2 >= hz2) {
            uint32 tsc_lo_now2, tsc_hi_now2; __asm__ __volatile__("rdtsc" : "=a"(tsc_lo_now2), "=d"(tsc_hi_now2));
            uint32 dcycles_lo2 = (tsc_lo_now2 >= g_stats.calib_start_tsc_lo)
                ? (tsc_lo_now2 - g_stats.calib_start_tsc_lo)
                : (tsc_lo_now2 + (0xFFFFFFFFu - g_stats.calib_start_tsc_lo) + 1u);
            uint32 cycles_per_sec2 = (dcycles_lo2 / dt_ticks2) * hz2;
            uint32 mhz_est2 = cycles_per_sec2 / 1000000u;
            if (mhz_est2 > 0 && mhz_est2 < 100000u) g_stats.cpu_mhz_total = mhz_est2;
            g_stats.calib_pending = 0;
        }
    }
    // Layout three pies across top area
    int pies_h = (content_h >= 180) ? 120 : (content_h/2);
    int pie_r = (content_w/3 - 40) / 2; if (pie_r < 20) pie_r = 20;
    int y = content_y + 20 + pie_r;
    int x1 = content_x + content_w/6; int cx1 = x1; 
    int x2 = content_x + content_w/2; int cx2 = x2; 
    int x3 = content_x + 5*content_w/6; int cx3 = x3; 
    // CPU pie
    draw_pie(cx1, y, pie_r, g_stats.cpu_percent, 220,60,60, 60,60,60);
    drawTextAt(cx1 - 20, y - pie_r - 14, "CPU", 255,255,255);
    char b[32];
    format_pct_str(b, (int)sizeof(b), g_stats.cpu_percent);
    drawTextAt(cx1 - 12, y, b, 255, 200, 200);
    // CPU detail line: show busyMHz / totalMHz (estimate). Uses TSC sampled over last UI window.
    char dline[64];
    uint32 hz = sched_get_tick_hz(); if (hz == 0) hz = 50;
    uint32 total_mhz = g_stats.cpu_mhz_total;
    uint32 busy_mhz = (total_mhz * clamp_pct_to_int(g_stats.cpu_percent)) / 100;
    if (total_mhz == 0) {
        if (g_stats.calib_pending && !g_stats.disable_tsc) {
            snprintf(dline, sizeof(dline), "estimating…");
        } else {
            // Fallback: display busy/total ticks to avoid misleading units
            uint32 disp_total = (g_stats.cpu_total_hz ? g_stats.cpu_total_hz : hz);
            snprintf(dline, sizeof(dline), "%u / %u ticks", (unsigned)g_stats.cpu_busy_hz, (unsigned)disp_total);
        }
    } else {
        snprintf(dline, sizeof(dline), "%uMHz / %uMHz", (unsigned)busy_mhz, (unsigned)total_mhz);
    }
    drawTextAt(cx1 - pie_r, y + pie_r + 6, dline, 220, 120, 120);
    // Memory pie (placeholder uses heap usage once available)
    draw_pie(cx2, y, pie_r, g_stats.mem_percent, 60,200,220, 60,60,60);
    drawTextAt(cx2 - 28, y - pie_r - 14, "Memory", 255,255,255);
    format_pct_str(b, (int)sizeof(b), g_stats.mem_percent);
    drawTextAt(cx2 - 12, y, b, 200, 255, 255);
    // Memory detail line: smart formatting (KB for <1MB, one-decimal MB for <10MB)
    // Show system RAM total, but usage is heap-used as an approximation (until full process accounting exists)
    char used_buf[24], total_buf[24];
    format_size_smart(used_buf, (int)sizeof(used_buf), g_stats.heap_used);
    format_size_smart(total_buf, (int)sizeof(total_buf), g_stats.total_ram_bytes);
    snprintf(dline, sizeof(dline), "%s / %s", used_buf, total_buf);
    drawTextAt(cx2 - pie_r, y + pie_r + 6, dline, 120, 220, 220);
    // Disk pie
    draw_pie(cx3, y, pie_r, g_stats.disk_percent, 200,60,200, 60,60,60);
    drawTextAt(cx3 - 20, y - pie_r - 14, "Disk", 255,255,255);
    format_pct_str(b, (int)sizeof(b), g_stats.disk_percent);
    drawTextAt(cx3 - 12, y, b, 255, 200, 255);
    // Disk detail line: smart formatting
    uint32 blk_sz = EYNFS_BLOCK_SIZE;
    char disk_used_buf[24], disk_total_buf[24];
    uint32 disk_used_bytes = g_stats.disk_used_blocks * blk_sz;
    uint32 disk_total_bytes = g_stats.disk_total_blocks * blk_sz;
    format_size_smart(disk_used_buf, (int)sizeof(disk_used_buf), disk_used_bytes);
    format_size_smart(disk_total_buf, (int)sizeof(disk_total_buf), disk_total_bytes);
    snprintf(dline, sizeof(dline), "%s / %s", disk_used_buf, disk_total_buf);
    drawTextAt(cx3 - pie_r, y + pie_r + 6, dline, 220, 120, 220);

    // Table below
    int table_y = y + pie_r + 20;
    if (table_y + 40 < content_y + content_h) {
        g_stats.table_x = content_x + 8; g_stats.table_y = table_y; g_stats.table_w = content_w - 16; g_stats.table_h = content_h - (table_y - content_y) - 8;
        stats_draw_table(g_stats.table_x, g_stats.table_y, g_stats.table_w, g_stats.table_h);
    }
}

static void stats_gui_key(int tile_idx, int key, void* userdata) {
    (void)userdata; (void)tile_idx;
    if ((key & 0x20FF) == 'q') { tile_unregister_gui_client(g_stats.tile_idx); return; }
}

static void stats_gui_mouse(int tile_idx, const mouse_event_t* me, void* userdata) {
    (void)userdata; (void)tile_idx;
    int left_down = (me->buttons & MOUSE_BUTTON_LEFT) != 0;
    if (!left_down) return;
    // If click in header row, change sort column
    int x = g_stats.table_x, y = g_stats.table_y, w = g_stats.table_w;
    if (me->y >= y && me->y < y + 14 && me->x >= x && me->x < x + w) {
        int col_w = w / 3;
        int which = (me->x - x) / col_w; if (which < 0) which = 0; if (which > 2) which = 2;
        if (g_stats.sort_col == which) { g_stats.sort_dir = -g_stats.sort_dir; }
        else { g_stats.sort_col = which; g_stats.sort_dir = -1; }
        tile_invalidate_gui(g_stats.tile_idx);
    }
}

static void stats_cmd(string arg) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.sort_col = 0; g_stats.sort_dir = -1;
    g_stats.last_ticks = sched_get_tick_count();
    g_stats.last_idle_hlt = sched_get_idle_hlt_count();
    // Decide TSC usability and prime baseline if enabled
    g_stats.disable_tsc = 0;
    if (!cpu_has_tsc() || !cpu_has_invariant_tsc()) {
        g_stats.disable_tsc = 1;
    }
    if (!g_stats.disable_tsc) {
        uint32 tsc_lo, tsc_hi;
        __asm__ __volatile__("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        g_stats.tsc_lo_last = tsc_lo;
        g_stats.tsc_tick_last = g_stats.last_ticks;
        // Begin one-shot calibration using ~1s window
        g_stats.calib_pending = 1;
        g_stats.calib_start_tick = g_stats.last_ticks;
        g_stats.calib_start_tsc_lo = g_stats.tsc_lo_last;
    }
    // Initial memory/disk sample
    stats_sample_memory();
    stats_sample_disk();
    g_stats.last_ui_update_tick = sched_get_tick_count();
    int tile = tile_get_focused();
    tile_set_title_status(tile, "System Stats", "stats", NULL);
    tile_register_gui_client2(tile, stats_gui_draw, stats_gui_key, stats_gui_mouse, NULL);
}

REGISTER_SHELL_COMMAND(stats_cmd_info, "stats", stats_cmd, CMD_STREAMING, "Graphical system performance monitor with CPU, memory, disk pies and sortable table", "stats");
