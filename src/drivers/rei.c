#include <rei.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <multiboot.h>
#include <stdint.h>
#include <context.h>
#include <misc/sched.h>

static int rei_ctx_allow(uint32 caps, uint32 cost) {
    command_context_t* ctx = current_command_context;
    if (ctx && !cap_check(ctx->caps, caps)) return 0;
    if (ctx) {
        scheduler_account(ctx->wo, cost);
        scheduler_yield_if_needed(ctx->wo);
        if (sched_det_is_enabled()) ctx->det_seq++;
    }
    return 1;
}

// Read REI header from data
int rei_read_header(const uint8_t* data, size_t size, rei_header_t* header) {
    if (!data || !header || size < sizeof(rei_header_t)) {
        return -1;
    }
    
    // Copy header data
            memcpy(header, (char*)data, sizeof(rei_header_t));
    
    return 0;
}

// Validate REI header
int rei_validate_header(const rei_header_t* header) {
    if (!header) return -1;
    
    // Check magic number
    if (header->magic != REI_MAGIC) {
        return -1;
    }
    
    // Check dimensions
    if (header->width == 0 || header->width > REI_MAX_WIDTH ||
        header->height == 0 || header->height > REI_MAX_HEIGHT) {
        return -1;
    }
    
    // Check color depth
    if (header->depth != REI_DEPTH_MONO && 
        header->depth != REI_DEPTH_RGB && 
        header->depth != REI_DEPTH_RGBA) {
        return -1;
    }
    
    return 0;
}

// Calculate data size based on header
int rei_calculate_data_size(const rei_header_t* header) {
    if (!header) return -1;
    
    return header->width * header->height * header->depth;
}

/* Forward-declare internal RLE decompressor (static implementation below). */
static int rei_decompress_rle(const uint8_t* in, size_t in_size,
                              uint8_t* out, size_t out_size,
                              uint8_t pixel_size);

// Parse complete REI image
int rei_parse_image(const uint8_t* data, size_t size, rei_image_t* image) {
    if (!data || !image || size < sizeof(rei_header_t)) {
        return -1;
    }

    if (!rei_ctx_allow(CAP_ALLOC_MEMORY, SCHED_COST_ALLOC)) {
        return -1;
    }
    
    // Read and validate header
    if (rei_read_header(data, size, &image->header) != 0) {
        return -1;
    }
    
    if (rei_validate_header(&image->header) != 0) {
        return -1;
    }
    // Debug: print header basics once when parsing (can be silenced later)
    // printf("REI parse: %ux%u depth=%u\n", image->header.width, image->header.height, image->header.depth);
    
    // Calculate uncompressed pixel data size
    int expected_size = rei_calculate_data_size(&image->header);
    if (expected_size < 0) {
        return -1;
    }

    // Determine compression from header flags
    uint8_t comp = image->header.reserved1 & REI_COMP_MASK;

    // Allocate output buffer (always uncompressed in memory)
    image->data = (uint8_t*)malloc(expected_size);
    if (!image->data) {
        return -1;
    }

    const uint8_t* in_ptr = data + sizeof(rei_header_t);
    size_t in_size = (size > sizeof(rei_header_t)) ? (size - sizeof(rei_header_t)) : 0;

    if (comp == REI_COMP_NONE) {
        // Ensure input has enough bytes
        if (in_size < (size_t)expected_size) {
            free(image->data);
            image->data = NULL;
            return -1;
        }
        memcpy(image->data, (char*)in_ptr, expected_size);
        image->data_size = expected_size;
    } else if (comp == REI_COMP_RLE) {
        // Decompress RLE into output buffer
        int rc = rei_decompress_rle(in_ptr, in_size, image->data, expected_size, image->header.depth);
        if (rc != 0) {
            free(image->data);
            image->data = NULL;
            return -1;
        }
        image->data_size = expected_size;
    } else {
        // Unsupported compression
        free(image->data);
        image->data = NULL;
        return -1;
    }
    // Keep alpha data intact; conversion to RGB (with alpha-to-black) should be done by consumers that require it

    return 0;
}

/* Note: rei_decompress_rle is forward-declared once above and implemented below. */

// Free REI image memory
void rei_free_image(rei_image_t* image) {
    if (image && image->data) {
        free(image->data);
        image->data = NULL;
        image->data_size = 0;
    }
}

// Get pixel offset in data array
int rei_get_pixel_offset(const rei_header_t* header, int x, int y) {
    if (!header || x < 0 || x >= header->width || y < 0 || y >= header->height) {
        return -1;
    }
    
    return (y * header->width + x) * header->depth;
}

// Get pixel color as 32-bit value
uint32_t rei_get_pixel_color(const rei_image_t* image, int x, int y) {
    if (!image || !image->data) return 0;
    
    int offset = rei_get_pixel_offset(&image->header, x, y);
    if (offset < 0 || offset >= image->data_size) return 0;
    
    uint8_t* pixel = image->data + offset;
    
    switch (image->header.depth) {
        case REI_DEPTH_MONO:
            return rei_mono_to_vga(pixel[0]);
            
        case REI_DEPTH_RGB:
            return rei_rgb_to_vga(pixel[0], pixel[1], pixel[2]);
            
        case REI_DEPTH_RGBA:
            // For now, ignore alpha and use RGB
            return rei_rgb_to_vga(pixel[0], pixel[1], pixel[2]);
            
        default:
            return 0;
    }
}

// Set pixel color
void rei_set_pixel_color(rei_image_t* image, int x, int y, uint32_t color) {
    if (!image || !image->data) return;
    
    int offset = rei_get_pixel_offset(&image->header, x, y);
    if (offset < 0 || offset >= image->data_size) return;
    
    uint8_t* pixel = image->data + offset;
    
    switch (image->header.depth) {
        case REI_DEPTH_MONO:
            pixel[0] = (color & 0xFF);
            break;
            
        case REI_DEPTH_RGB:
            pixel[0] = (color >> 16) & 0xFF; // R
            pixel[1] = (color >> 8) & 0xFF;  // G
            pixel[2] = color & 0xFF;         // B
            break;
            
        case REI_DEPTH_RGBA:
            pixel[0] = (color >> 16) & 0xFF; // R
            pixel[1] = (color >> 8) & 0xFF;  // G
            pixel[2] = color & 0xFF;         // B
            pixel[3] = (color >> 24) & 0xFF; // A
            break;
    }
}

// Convert RGB to VGA color (simplified)
uint32_t rei_rgb_to_vga(uint8_t r, uint8_t g, uint8_t b) {
    // Simple RGB to VGA conversion
    // For now, use a basic mapping
    uint8_t vga_r = (r * 7) / 255;
    uint8_t vga_g = (g * 7) / 255;
    uint8_t vga_b = (b * 3) / 255;
    
    return (vga_r << 5) | (vga_g << 2) | vga_b;
}

// Convert monochrome to VGA color
uint32_t rei_mono_to_vga(uint8_t mono) {
    // Convert grayscale to VGA color
    uint8_t vga = (mono * 15) / 255;
    return vga;
}

// Display REI image at specific position
int rei_display_image(const rei_image_t* image, int x, int y) {
    if (!image || !image->data) return -1;
    
    printf("%c=== REI Image Display ===\n", 255, 255, 255);
    printf("%cSize: %dx%d pixels\n", 255, 255, 255, image->header.width, image->header.height);
    printf("%cDepth: %d bytes per pixel\n", 255, 255, 255, image->header.depth);
    printf("%cData size: %d bytes\n", 255, 255, 255, image->data_size);
    
    // Calculate center position if x,y are 0,0 (centered display)
    int display_x = x;
    int display_y = y;
    
    if (x == 0 && y == 0) {
        // Center the image on screen
        extern multiboot_info_t *g_mbi;
        if (g_mbi && g_mbi->framebuffer_width > 0 && g_mbi->framebuffer_height > 0) {
            display_x = (g_mbi->framebuffer_width - image->header.width) / 2;
            display_y = (g_mbi->framebuffer_height - image->header.height) / 2;
        }
    }
    
    printf("%cRendering at position (%d, %d)...\n", 120, 120, 255, display_x, display_y);
    
    // If shell output is being redirected (e.g., running inside a tiled vterm), don't draw pixels
    extern int shell_redirect_active;
    if (shell_redirect_active) {
        // Inform the caller via printf which will feed the redirect buffer instead of drawing
        printf("%c(REI image rendering skipped because output is redirected)\n", 200, 200, 200);
        return 0;
    }

    // Clear screen before rendering
    clearScreen();
    
    // Check if image is too large for screen and scale if needed
    extern multiboot_info_t *g_mbi;
    int scale = 1;
    if (g_mbi && (image->header.width > g_mbi->framebuffer_width || 
                   image->header.height > g_mbi->framebuffer_height)) {
        scale = 2; // Scale down by 2
        printf("%cImage too large, scaling down by factor %d\n", 120, 120, 255, scale);
    }
    
    // Compute the destination bounds and mark once as dirty
    int dst_w = (image->header.width / scale);
    if (dst_w <= 0) dst_w = image->header.width; // scale==1
    int dst_h = (image->header.height / scale);
    if (dst_h <= 0) dst_h = image->header.height;
    vga_mark_dirty_rect(display_x, display_y, dst_w, dst_h);

    // Draw each pixel to the backbuffer (or framebuffer if no backbuffer)
    for (int py = 0; py < image->header.height; py += scale) {
        for (int px = 0; px < image->header.width; px += scale) {
            uint32_t color = rei_get_pixel_color(image, px, py);
            
            // Convert VGA color back to RGB
            uint8_t r, g, b;
            if (image->header.depth == REI_DEPTH_MONO) {
                // Monochrome: convert grayscale to RGB
                uint8_t gray = color & 0xFF;
                r = g = b = gray;
            } else {
                // RGB: extract from the original pixel data
                int offset = rei_get_pixel_offset(&image->header, px, py);
                if (offset >= 0 && offset < image->data_size) {
                    uint8_t* pixel = image->data + offset;
                    if (image->header.depth == REI_DEPTH_RGB) {
                        r = pixel[0];
                        g = pixel[1];
                        b = pixel[2];
                    } else if (image->header.depth == REI_DEPTH_RGBA) {
                        r = pixel[0];
                        g = pixel[1];
                        b = pixel[2];
                        // Note: alpha is ignored for now
                    } else {
                        r = g = b = 0;
                    }
                } else {
                    r = g = b = 0;
                }
            }
            
            // Draw the pixel (with scaling)
            if (scale == 1) {
                vga_drawPixel_bb(display_x + px, display_y + py, r, g, b);
            } else {
                // Draw a scaled pixel (2x2 block)
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        vga_drawPixel_bb(display_x + px * scale + sx, display_y + py * scale + sy, r, g, b);
                    }
                }
            }
        }
    }
    
    printf("%cImage rendered successfully!\n", 120, 120, 255);
    printf("%cPress Enter to return to shell...\n", 255, 255, 255);
    
    // For now, just return immediately - the shell will handle the display
    // In a future version, we could implement a proper key wait function
    
    return 0;
}

// Display REI image centered on screen
int rei_display_image_centered(const rei_image_t* image) {
    // For now, just display at top-left
    return rei_display_image(image, 0, 0);
} 

// RLE decompression based on PackBits-like scheme at pixel granularity
// Encoding layout in the bytestream (operate on pixels of pixel_size bytes):
//  - Read signed int8 control 'n':
//      n in [0..127]  : literal run of (n+1) pixels follows
//      n in [-127..-1]: replicate next single pixel (1) for (1 - n) times
//      n == -128      : no-op (skip)
static int rei_decompress_rle(const uint8_t* in, size_t in_size,
                       uint8_t* out, size_t out_size,
                       uint8_t pixel_size) {
    if (!in || !out || pixel_size == 0) return -1;
    size_t ip = 0;
    size_t op = 0;
    while (ip < in_size && op < out_size) {
        int8_t n;
        n = (int8_t)in[ip++];
        if (n >= 0) {
            // literal run of (n+1) pixels
            size_t count = (size_t)n + 1;
            size_t bytes = count * pixel_size;
            if (ip + bytes > in_size) return -1;
            if (op + bytes > out_size) return -1;
            memcpy(out + op, in + ip, bytes);
            ip += bytes;
            op += bytes;
        } else if (n != -128) {
            // replicate next 1 pixel (1 - n) times
            size_t count = (size_t)(1 - n);
            if (ip + pixel_size > in_size) return -1;
            if (op + count * (size_t)pixel_size > out_size) return -1;
            // Copy one pixel then replicate
            const uint8_t* px = in + ip;
            ip += pixel_size;
            for (size_t i = 0; i < count; ++i) {
                memcpy(out + op, px, pixel_size);
                op += pixel_size;
            }
        } else {
            // n == -128 : no-op
        }
    }
    // Valid if we filled exactly out_size
    return (op == out_size) ? 0 : -1;
}