#include <vga.h>
#include <multiboot.h>
#include <eynfs.h>
#include <util.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <types.h>
#include <shell.h>
#include <system.h>

extern multiboot_info_t *g_mbi;

int width, height;
int r = 255, g = 255, b = 255; // Default to white
// When non-zero, drawText operates in a minimal glyph-draw mode used by drawCharAt.
static int g_drawCharAt_mode = 0;

// Simple software backbuffer (RGBA8)
static unsigned char* g_backbuffer = NULL;
static int g_backbuffer_w = 0;
static int g_backbuffer_h = 0;
// Optional exclusion rect for swap (e.g., software cursor overlay)
static int g_exclude_x = -1, g_exclude_y = -1, g_exclude_w = 0, g_exclude_h = 0;
// Dirty rect tracking (multi-rect)
typedef struct { int x, y, w, h; } dirty_rect_t;
#define MAX_DIRTY_RECTS 128
static dirty_rect_t g_dirty_rects[MAX_DIRTY_RECTS];
static int g_dirty_count = 0;

// Shell redirection globals
int shell_redirect_active = 0;
char shell_redirect_buf[SHELL_REDIRECT_BUF_SIZE];
int shell_redirect_pos = 0;
// Color for redirected output
int shell_redirect_color_r = 255;
int shell_redirect_color_g = 255;
int shell_redirect_color_b = 255;
// Per-char redirect colors parallel to shell_redirect_buf
unsigned char shell_redirect_r[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_g[SHELL_REDIRECT_BUF_SIZE];
unsigned char shell_redirect_b[SHELL_REDIRECT_BUF_SIZE];

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
    
    // Detect available memory and set appropriate buffer size
    extern multiboot_info_t *g_mbi;
    uint32_t available_memory = 32 * 1024 * 1024; // Default assumption
    
    if (g_mbi && (g_mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        // Calculate total available memory from memory map
        uint32_t total_ram = 0;
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)g_mbi->mmap_addr;
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

void shell_log_enable() { shell_log_active = 1; }
void shell_log_disable() { shell_log_active = 0; }

void shell_log_flush() {
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
	int i, j;
	// If a backbuffer is available, draw into it; otherwise draw directly to framebuffer
	if (g_backbuffer && g_backbuffer_w >= (int)g_mbi->framebuffer_width && g_backbuffer_h >= (int)g_mbi->framebuffer_height) {
		unsigned int offset = (x + y * g_backbuffer_w) * 4;
		for (i = 0; i < h; i++) {
			for (j = 0; j < w; j++) {
				unsigned int o = offset + j * 4;
				g_backbuffer[o] = (unsigned char)b;
				g_backbuffer[o + 1] = (unsigned char)g;
				g_backbuffer[o + 2] = (unsigned char)r;
				g_backbuffer[o + 3] = 0;
			}
			offset += g_backbuffer_w * 4;
		}
		// mark region dirty
		vga_mark_dirty_rect(x, y, w, h);
		return;
	}
	// fallback to drawing directly to framebuffer
	unsigned char *video = (unsigned char *)g_mbi->framebuffer_addr;
	unsigned int offset = (x + y * g_mbi->framebuffer_width) * 4; // find location of the pixel
	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) // colouring in each line
		{
			video[offset + j * 4] = b;
			video[offset + j * 4 + 1] = g;
			video[offset + j * 4 + 2] = r;
			video[offset + j * 4 + 3] = 0;
		}
		offset += g_mbi->framebuffer_width * 4; // beginning of each line
	}
}

void drawText(int charnum, int r, int g, int b)
{
	// store hex numbers representing the pattern for characters (8 numbers per character) into an array
	// i knicked all this off of github i cant lie, i am NOT writing this much hex
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

	int i; // column number in the 8x8 pattern
	int k; // row number in the 8x8 pattern
	int w; // horizontal position of the pixel in the 8x8 pattern
	int h; // vertical position of the pixel in the 8x8 pattern

	// Minimal side-effect-free path for drawCharAt to prevent console scrolling/wrapping side effects.
	if (g_drawCharAt_mode) {
		if (charnum >= 0) {
			int x0 = width;
			int y0 = height;
			if (g_backbuffer) {
				for (k = 0; k < 8; k++) {
					int yy = y0 + k;
					if (yy < 0 || yy >= g_backbuffer_h) continue;
					unsigned char rowbits = font[charnum * 8 + k];
					for (i = 0; i < 8; i++) {
						if (rowbits & (0x01 << i)) {
							int xx = x0 + (8 - i);
							if (xx < 0 || xx >= g_backbuffer_w) continue;
							size_t o = ((size_t)yy * g_backbuffer_w + xx) * 4;
							g_backbuffer[o + 0] = (unsigned char)b;
							g_backbuffer[o + 1] = (unsigned char)g;
							g_backbuffer[o + 2] = (unsigned char)r;
							g_backbuffer[o + 3] = 0;
						}
					}
				}
				// Mark the glyph's 8x8 area dirty
				vga_mark_dirty_rect(x0, y0, 8, 8);
			} else {
				for (k = 0; k < 8; k++) {
					for (i = 0; i < 8; i++) {
						if (font[charnum * 8 + k] & (0x01 << i)) {
							drawPixel((8 - i) + x0, k + y0, r, g, b);
						}
					}
				}
			}
		}
		return;
	}
	
	// If shell output is being redirected, capture characters into the redirect buffer
	if (shell_redirect_active) {
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


	if (height == (int)(g_mbi->framebuffer_height)) 
    {
		clearScreen(); // "scrolling"
	}

	// charnum = charnum + 1;
	// Moving the cursor to the next line when we reached the end of the existing line
	if (width > (int)(g_mbi->framebuffer_width - 20))
    {
		width = 0;
		height = height + 8;
	}
	
	if (width < -2 && height > 34)
    {
		width = g_mbi->framebuffer_width - 30;
		height = height - 8;
	}
	
	if (charnum == -1)
    {
		drawRect(width, height, 16, 16, 0, 0, 0);
		width=width+16;
	}

	else if (charnum==10) // enter
    {
		drawText(-1, r, g, b);
		width = 0;
		height = height + 8;
	}

	else if (charnum== 8) // backspace
    {
        if (width > 0) {  // Only backspace if we're not at the start of a line
            width = width - 8;  // Move back one character width
            // Draw a black rectangle to erase the previous character
            drawRect(width, height, 8, 8, 0, 0, 0);
        }
    }

	else // drawing characters in 8x8
    {
		for (k = 0; k < 8; k++)
        {
			for (i = 0; i < 8; i++)
            {
				if (font[charnum * 8 + k] & (0x01 << i))
                {
					drawPixel((8 - i) + width, k + height, r, g, b);
				}
			}
		}
		width = width + 8; // move to draw the next character
	}
}

void printf(const char* format, ...) 
{
	va_list ap;
	va_start(ap, format);

	uint8 *ptr;
	static int r = 255, g = 255, b = 255; // Default to white, static to maintain state

	// Reset color to white at the start of each printf call
	r = 255;
	g = 255;
	b = 255;

	// Check if color parameters are provided
	if (format[0] == '%' && format[1] == 'c') {
		r = va_arg(ap, int);
		g = va_arg(ap, int);
		b = va_arg(ap, int);
		format += 2; // Skip the color format specifier
	}

	// Redirection logic
	if (shell_redirect_active) {
		// We'll use a local buffer to format the output
		char temp[512];
		int temp_pos = 0;
		for (ptr = (uint8*)format; *ptr != '\0' && temp_pos < 511; ptr++) {
			if (*ptr == '%') {
				ptr++;
				switch (*ptr) {
					case 's': {
						char* str = va_arg(ap, char*);
						while (*str && temp_pos < 511) {
							temp[temp_pos++] = *str++;
						}
						break;
					}
					case 'd': {
						char* num_str = int_to_string(va_arg(ap, int));
						while (*num_str && temp_pos < 511) {
							temp[temp_pos++] = *num_str++;
						}
						break;
					}
					case '%':
						temp[temp_pos++] = '%';
						break;
					case 'c':
						temp[temp_pos++] = (char)va_arg(ap, int);
						break;
				}
			} else if (*ptr == '\n') {
				temp[temp_pos++] = '\n';
			} else {
				temp[temp_pos++] = *ptr;
			}
		}
		temp[temp_pos] = '\0';
		
		// record color for redirected output so callers can render it with the same color
		shell_redirect_color_r = r;
		shell_redirect_color_g = g;
		shell_redirect_color_b = b;
		// Append to the global buffer (with bounds checking)
		int to_copy = temp_pos;
		if (shell_redirect_pos + to_copy >= SHELL_REDIRECT_BUF_SIZE - 1) {
			to_copy = SHELL_REDIRECT_BUF_SIZE - shell_redirect_pos - 1;
		}
		// store per-char color metadata for the temp buffer (for the actual copied count)
		for (int i = 0; i < to_copy; ++i) {
			shell_redirect_r[shell_redirect_pos + i] = (unsigned char)r;
			shell_redirect_g[shell_redirect_pos + i] = (unsigned char)g;
			shell_redirect_b[shell_redirect_pos + i] = (unsigned char)b;
		}
		for (int i = 0; i < to_copy; ++i) {
			shell_redirect_buf[shell_redirect_pos++] = temp[i];
		}
		shell_redirect_buf[shell_redirect_pos] = '\0';
		va_end(ap);
		return;
	}

	// Logging logic
	if (shell_log_active) {
		va_list ap_log;
		va_copy(ap_log, ap);
		// We'll use a local buffer to format the output
		char temp[512];
		int temp_pos = 0;
		const uint8* log_ptr = (const uint8*)format;
		for (; *log_ptr != '\0' && temp_pos < 511; log_ptr++) {
			if (*log_ptr == '%') {
				log_ptr++;
				switch (*log_ptr) {
					case 's': {
						char* str = va_arg(ap_log, char*);
						while (*str && temp_pos < 511) {
							temp[temp_pos++] = *str++;
						}
						break;
					}
					case 'd': {
						char* num_str = int_to_string(va_arg(ap_log, int));
						while (*num_str && temp_pos < 511) {
							temp[temp_pos++] = *num_str++;
						}
						break;
					}
					case '%':
						temp[temp_pos++] = '%';
						break;
					case 'c':
						temp[temp_pos++] = (char)va_arg(ap_log, int);
						break;
				}
			} else if (*log_ptr == '\n') {
				temp[temp_pos++] = '\n';
			} else {
				temp[temp_pos++] = *log_ptr;
			}
		}
		temp[temp_pos] = '\0';
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

	for (ptr = (uint8*)format; *ptr != '\0'; ptr++) 
    {
		if (*ptr == '%') {
			ptr++;
			switch (*ptr) {
				case 's': {
					char* str = va_arg(ap, char*);
					while (*str) {
						drawText(*str, r, g, b);
						str++;
					}
					break;
				}
				case 'd': {
					char* num_str = int_to_string(va_arg(ap, int));
					while (*num_str) {
						drawText(*num_str, r, g, b);
						num_str++;
					}
					break;
				}
				case '%':
					drawText('%', r, g, b);
					break;
				case 'c':
					drawText(va_arg(ap, int), r, g, b);
					break;
			}
		} else if (*ptr == '\n') {
			// Handle newline without drawing a character
			width = 0;
			height = height + 8;
		} else {
			drawText(*ptr, r, g, b);
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

	unsigned char *video = (unsigned char *)g_mbi->framebuffer_addr;
	unsigned int offset = (x + y * g_mbi->framebuffer_width) * 4; // finding loc of pixel
	video[offset] = (unsigned char)b;   // setting the colour of pixel; blue, green, red
	video[offset + 1] = (unsigned char)g;
	video[offset + 2] = (unsigned char)r;
	video[offset + 3] = 0;
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
	unsigned char *video = (unsigned char *)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!video || pitch <= 0 || bpp < 3) return;
	unsigned char* p = video + y * pitch + x * bpp;
	p[0] = (unsigned char)bb;
	p[1] = (unsigned char)gg;
	p[2] = (unsigned char)rr;
	if (bpp >= 4) p[3] = 0xFF;
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
	if (g_backbuffer) {
		free(g_backbuffer);
		g_backbuffer = NULL;
	}
	// Try to allocate backbuffer; if allocation fails, leave g_backbuffer NULL and use direct framebuffer
	size_t bytes = (size_t)fbw * (size_t)fbh * 4;
	// Try predictive allocator first if available
	void* buf = NULL;
	// predictive_malloc is declared in predictive_memory.h if available
	// We'll attempt to use it via weak reference: if symbol exists, use it, otherwise fall back to malloc
	buf = malloc(bytes);
	g_backbuffer = (unsigned char*)buf;
	if (!g_backbuffer) {
		g_backbuffer_w = 0;
		g_backbuffer_h = 0;
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
	// Try to align copies with VBlank to reduce tearing near the top
	vga_wait_vblank();
	if (!g_mbi) return;
	if (!g_backbuffer) return;
	unsigned char* fb = (unsigned char*)g_mbi->framebuffer_addr;
	int fbw = g_mbi->framebuffer_width;
	int fbh = g_mbi->framebuffer_height;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!fb || pitch <= 0 || bpp < 3) return;
	// If nothing dirty, nothing to copy
	if (g_dirty_count <= 0) return;
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

// Draw directly to the physical framebuffer (overlay), bypassing the backbuffer
void vga_drawPixel_fb(int x, int y, int r, int g, int b) {
	if (!g_mbi) return;
	if (x < 0 || y < 0) return;
	if (x >= (int)g_mbi->framebuffer_width || y >= (int)g_mbi->framebuffer_height) return;
	unsigned char* fb = (unsigned char*)g_mbi->framebuffer_addr;
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
	unsigned char* fb = (unsigned char*)g_mbi->framebuffer_addr;
	int pitch = g_mbi->framebuffer_pitch;
	int bpp = g_mbi->framebuffer_bpp / 8;
	if (!fb || pitch <= 0 || bpp < 3) return;
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
	unsigned char* fb = (unsigned char*)g_mbi->framebuffer_addr;
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
	unsigned char* fb = (unsigned char*)g_mbi->framebuffer_addr;
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
	int old_w = width;
	int old_h = height;
	int cx = x;
	int cy = y;

	for (const char* p = text; *p; ++p) {
		if (*p == '\n') {
			cx = x;
			cy += 8;
			continue;
		}
		// draw a single char at the pixel position with color
		drawCharAt(cx, cy, (int)(unsigned char)*p, rr, gg, bb);
		cx += 8;
	}

	// restore previous cursor
	width = old_w;
	height = old_h;
}

void drawCharAt(int x, int y, int charnum, int rr, int gg, int bb) {
	if (charnum < 0) return;
	// Use the minimal glyph-draw path in drawText without console side effects.
	int old_w = width;
	int old_h = height;
	width = x;
	height = y;
	g_drawCharAt_mode = 1;
	drawText(charnum, rr, gg, bb);
	g_drawCharAt_mode = 0;
	width = old_w;
	height = old_h;
}

// Start redirection
void start_shell_redirect() {
	shell_redirect_active = 1;
	shell_redirect_pos = 0;
	shell_redirect_buf[0] = '\0';
	// clear per-char redirect color arrays
	for (int i = 0; i < SHELL_REDIRECT_BUF_SIZE; ++i) {
		shell_redirect_r[i] = 0;
		shell_redirect_g[i] = 0;
		shell_redirect_b[i] = 0;
	}
	// reset current redirect color so new typed input falls back to vterm default
	shell_redirect_color_r = 0;
	shell_redirect_color_g = 0;
	shell_redirect_color_b = 0;
}

// Stop redirection
void stop_shell_redirect() {
	shell_redirect_active = 0;
	shell_redirect_pos = 0;
	// clear redirect color so subsequent typed input doesn't inherit last printed color
	shell_redirect_color_r = 0;
	shell_redirect_color_g = 0;
	shell_redirect_color_b = 0;
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

void vga_set_color(int nr, int ng, int nb) {
    r = nr;
    g = ng;
    b = nb;
}

// Bold font - just use regular drawText with brighter color for now
void drawText_bold(int charnum, int r, int g, int b) {
    // For now, just use regular drawText with brighter color
    drawText(charnum, r, g, b);
}

// Italic font - just use regular drawText for now
void drawText_italic(int charnum, int r, int g, int b) {
    // For now, just use regular drawText
    drawText(charnum, r, g, b);
}

// Large font for headers - enhanced spacing and visual emphasis
void drawText_large(int charnum, int r, int g, int b) {
    // Use regular drawText but add extra spacing and visual emphasis
    drawText(charnum, r, g, b);
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
                // Print header with different color
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

// --- simple 2x2 windowing support ---
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
        for (const char* p = title; *p; ++p) drawText(*p, 255, 255, 0);
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