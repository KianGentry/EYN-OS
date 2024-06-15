#pragma once
#include <stdio.h>
#include <stdint.h>

enum
{
    P_B=0,
    P_BL=1,
    P_GR=2,
    P_CY=3,
    P_RE=4,
    P_MA=5,
    P_BR=6,
    P_LG=7,
    P_DG=8,
    P_LB=9,
    P_LGR=10,
    P_LC=11,
    P_LR=12,
    P_PI=13,
    P_YE=14,
    P_W=15,
};

void print_clear();
void print_char(char character);
void print_str(char* string);
void print_set_color(uint8_t foreground, uint8_t background);
