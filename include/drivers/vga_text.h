#ifndef __VGA_TEXT_H__
#define __VGA_TEXT_H__

#include <misc/types.h>

/*
 * VGA Text Mode (80x25) Driver Header
 * 
 * Provides basic text output for TTY-only builds.
 */

void vga_text_init(void);
void vga_text_clear(void);
void vga_text_putchar(char c);
void vga_text_puts(const char *str);
void vga_text_set_color(uint8 fg, uint8 bg);
void vga_text_reset_color(void);

#endif // __VGA_TEXT_H__
