#ifndef SCREEN_H
#define SCREEN_H
#include "types.h"

int cursorX, cursorY;
const uint8 sw, sh, sd;

void clearLine(uint8 from,uint8 to);

void updateCursor();

void clearScreen();

void scrollUp(uint8 lineNumber);

void newLineCheck();

void printfch(char c);

void printf(string ch);
void set_screen_color_from_color_code(int color_code);
void set_screen_color(int text_color,int bg_color);
void printf_colored(string ch,int text_color,int bg_color);


#endif
