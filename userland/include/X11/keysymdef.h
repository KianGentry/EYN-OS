/*
 * X11/keysymdef.h -- X11 key symbol definitions for EYN-OS.
 *
 * Only the most commonly used key symbols are defined here: Latin-1,
 * function keys, navigation keys, and modifiers.  This covers the vast
 * majority of simple X11 programs.
 */
#ifndef _X11_KEYSYMDEF_H
#define _X11_KEYSYMDEF_H

/* Miscellaneous keys */
#define XK_BackSpace        0xFF08
#define XK_Tab              0xFF09
#define XK_Linefeed         0xFF0A
#define XK_Clear            0xFF0B
#define XK_Return           0xFF0D
#define XK_Pause            0xFF13
#define XK_Scroll_Lock      0xFF14
#define XK_Sys_Req          0xFF15
#define XK_Escape           0xFF1B
#define XK_Delete           0xFFFF

/* Cursor control & motion */
#define XK_Home             0xFF50
#define XK_Left             0xFF51
#define XK_Up               0xFF52
#define XK_Right            0xFF53
#define XK_Down             0xFF54
#define XK_Prior            0xFF55   /* Page Up */
#define XK_Page_Up          0xFF55
#define XK_Next             0xFF56   /* Page Down */
#define XK_Page_Down        0xFF56
#define XK_End              0xFF57
#define XK_Begin            0xFF58

/* Function keys */
#define XK_F1               0xFFBE
#define XK_F2               0xFFBF
#define XK_F3               0xFFC0
#define XK_F4               0xFFC1
#define XK_F5               0xFFC2
#define XK_F6               0xFFC3
#define XK_F7               0xFFC4
#define XK_F8               0xFFC5
#define XK_F9               0xFFC6
#define XK_F10              0xFFC7
#define XK_F11              0xFFCA
#define XK_F12              0xFFCB

/* Modifier keys */
#define XK_Shift_L          0xFFE1
#define XK_Shift_R          0xFFE2
#define XK_Control_L        0xFFE3
#define XK_Control_R        0xFFE4
#define XK_Caps_Lock        0xFFE5
#define XK_Shift_Lock       0xFFE6
#define XK_Meta_L           0xFFE7
#define XK_Meta_R           0xFFE8
#define XK_Alt_L            0xFFE9
#define XK_Alt_R            0xFFEA
#define XK_Super_L          0xFFEB
#define XK_Super_R          0xFFEC

/* Keypad */
#define XK_KP_Space         0xFF80
#define XK_KP_Tab           0xFF89
#define XK_KP_Enter         0xFF8D
#define XK_KP_Home          0xFF95
#define XK_KP_Left          0xFF96
#define XK_KP_Up            0xFF97
#define XK_KP_Right         0xFF98
#define XK_KP_Down          0xFF99
#define XK_KP_Page_Up       0xFF9A
#define XK_KP_Page_Down     0xFF9B
#define XK_KP_End           0xFF9C
#define XK_KP_Insert        0xFF9E
#define XK_KP_Delete        0xFF9F
#define XK_KP_Multiply      0xFFAA
#define XK_KP_Add           0xFFAB
#define XK_KP_Separator     0xFFAC
#define XK_KP_Subtract      0xFFAD
#define XK_KP_Decimal       0xFFAE
#define XK_KP_Divide        0xFFAF
#define XK_KP_0             0xFFB0
#define XK_KP_1             0xFFB1
#define XK_KP_2             0xFFB2
#define XK_KP_3             0xFFB3
#define XK_KP_4             0xFFB4
#define XK_KP_5             0xFFB5
#define XK_KP_6             0xFFB6
#define XK_KP_7             0xFFB7
#define XK_KP_8             0xFFB8
#define XK_KP_9             0xFFB9
#define XK_KP_Equal         0xFFBD

/*
 * Latin-1 (ISO 8859-1) -- keysyms equal Unicode code point.
 * Only printable range 0x20..0x7E and 0xA0..0xFF.
 */
#define XK_space            0x0020
#define XK_exclam           0x0021
#define XK_quotedbl         0x0022
#define XK_numbersign       0x0023
#define XK_dollar           0x0024
#define XK_percent          0x0025
#define XK_ampersand        0x0026
#define XK_apostrophe       0x0027
#define XK_parenleft        0x0028
#define XK_parenright       0x0029
#define XK_asterisk         0x002A
#define XK_plus             0x002B
#define XK_comma            0x002C
#define XK_minus            0x002D
#define XK_period           0x002E
#define XK_slash            0x002F
#define XK_0                0x0030
#define XK_1                0x0031
#define XK_2                0x0032
#define XK_3                0x0033
#define XK_4                0x0034
#define XK_5                0x0035
#define XK_6                0x0036
#define XK_7                0x0037
#define XK_8                0x0038
#define XK_9                0x0039
#define XK_colon            0x003A
#define XK_semicolon        0x003B
#define XK_less             0x003C
#define XK_equal            0x003D
#define XK_greater          0x003E
#define XK_question         0x003F
#define XK_at               0x0040
#define XK_A                0x0041
#define XK_B                0x0042
#define XK_C                0x0043
#define XK_D                0x0044
#define XK_E                0x0045
#define XK_F                0x0046
#define XK_G                0x0047
#define XK_H                0x0048
#define XK_I                0x0049
#define XK_J                0x004A
#define XK_K                0x004B
#define XK_L                0x004C
#define XK_M                0x004D
#define XK_N                0x004E
#define XK_O                0x004F
#define XK_P                0x0050
#define XK_Q                0x0051
#define XK_R                0x0052
#define XK_S                0x0053
#define XK_T                0x0054
#define XK_U                0x0055
#define XK_V                0x0056
#define XK_W                0x0057
#define XK_X                0x0058
#define XK_Y                0x0059
#define XK_Z                0x005A
#define XK_bracketleft      0x005B
#define XK_backslash        0x005C
#define XK_bracketright     0x005D
#define XK_asciicircum      0x005E
#define XK_underscore       0x005F
#define XK_grave            0x0060
#define XK_a                0x0061
#define XK_b                0x0062
#define XK_c                0x0063
#define XK_d                0x0064
#define XK_e                0x0065
#define XK_f                0x0066
#define XK_g                0x0067
#define XK_h                0x0068
#define XK_i                0x0069
#define XK_j                0x006A
#define XK_k                0x006B
#define XK_l                0x006C
#define XK_m                0x006D
#define XK_n                0x006E
#define XK_o                0x006F
#define XK_p                0x0070
#define XK_q                0x0071
#define XK_r                0x0072
#define XK_s                0x0073
#define XK_t                0x0074
#define XK_u                0x0075
#define XK_v                0x0076
#define XK_w                0x0077
#define XK_x                0x0078
#define XK_y                0x0079
#define XK_z                0x007A
#define XK_braceleft        0x007B
#define XK_bar              0x007C
#define XK_braceright       0x007D
#define XK_asciitilde       0x007E

/* Insert */
#define XK_Insert           0xFF63

#endif /* _X11_KEYSYMDEF_H */
