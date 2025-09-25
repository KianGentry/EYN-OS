#include <kb.h>
#include <system.h>
#include <vga.h>
#include <multiboot.h>
#include <util.h>
#include <stdlib.h>

extern multiboot_info_t *g_mbi;

string readStr() {
    string buffstr = (string) malloc(200);
    uint8 i = 0;
    uint8 reading = 1;
    uint8 shift_pressed = 0;  // Track shift key state
    uint8 caps_lock = 0;      // Track caps lock state
    uint8 ctrl_pressed = 0;   // Track control key state
    
    // Initialize buffer
    buffstr[0] = '\0';
    
    while(reading)
    {
                if(inportb(0x64) & 0x1)
                {
                        uint8 status = inportb(0x64);
                        if (status & 0x20) {
                                // AUX (mouse) data present; let mouse handler consume it
                                continue;
                        }
                        uint8 scancode = inportb(0x60);
            
            // Handle modifier keys
            if(scancode == 42 || scancode == 54) {  // Left or Right shift press
                shift_pressed = 1;
                continue;
            }
            if(scancode == 170 || scancode == 182) {  // Left or Right shift release
                shift_pressed = 0;
                continue;
            }
            if(scancode == 58) {  // Caps Lock
                caps_lock = !caps_lock;  // Toggle caps lock state
                continue;
            }
            if(scancode == 29) {  // Left Control press
                ctrl_pressed = 1;
                continue;
            }
            if(scancode == 157) {  // Left Control release
                ctrl_pressed = 0;
                continue;
            }
            
            // Skip key release events
            if(scancode & 0x80) {
                continue;
            }
            
            // Check for Ctrl+C
            if(ctrl_pressed && scancode == 46) {  // 'c' key
                g_user_interrupt = 1;
                buffstr[0] = '\0';  // Clear the buffer
                reading = 0;        // Exit the reading loop
                printf("%c^C\n", 255, 255, 255);  // Print ^C in white
                free(buffstr);   // Clean up memory
                return buffstr;
            }
            
            // Determine if we should use uppercase (shift XOR caps lock)
            uint8 use_uppercase = shift_pressed ^ caps_lock;
            
            switch(scancode)
            {
                case 1:  // Escape
                        drawText((char)27, 255, 255, 255);  // white
                buffstr[i] = (char)27;
                i++;
                        break;
                case 2:
                        if(shift_pressed) {
                            drawText('!', 255, 255, 255);  // white
                            buffstr[i] = '!';
                        } else {
                        drawText('1', 255, 255, 255);  // white
                        buffstr[i] = '1';
                        }
                        i++;
                        break;
                case 3:
                        if(shift_pressed) {
                            drawText('@', 255, 255, 255);  // white
                            buffstr[i] = '@';
                        } else {
                        drawText('2', 255, 255, 255);  // white
                        buffstr[i] = '2';
                        }
                        i++;
                        break;
                case 4:
                        if(shift_pressed) {
                            drawText('#', 255, 255, 255);  // white
                            buffstr[i] = '#';
                        } else {
                        drawText('3', 255, 255, 255);  // white
                        buffstr[i] = '3';
                        }
                        i++;
                        break;
                case 5:
                        if(shift_pressed) {
                            drawText('$', 255, 255, 255);  // white
                            buffstr[i] = '$';
                        } else {
                        drawText('4', 255, 255, 255);  // white
                        buffstr[i] = '4';
                        }
                        i++;
                        break;
                case 6:
                        if(shift_pressed) {
                            drawText('%', 255, 255, 255);  // white
                            buffstr[i] = '%';
                        } else {
                        drawText('5', 255, 255, 255);  // white
                        buffstr[i] = '5';
                        }
                        i++;
                        break;
                case 7:
                        if(shift_pressed) {
                            drawText('^', 255, 255, 255);  // white
                            buffstr[i] = '^';
                        } else {
                        drawText('6', 255, 255, 255);  // white
                        buffstr[i] = '6';
                        }
                        i++;
                        break;
                case 8:
                        if(shift_pressed) {
                            drawText('&', 255, 255, 255);  // white
                            buffstr[i] = '&';
                        } else {
                        drawText('7', 255, 255, 255);  // white
                        buffstr[i] = '7';
                        }
                        i++;
                        break;
                case 9:
                        if(shift_pressed) {
                            drawText('*', 255, 255, 255);  // white
                            buffstr[i] = '*';
                        } else {
                        drawText('8', 255, 255, 255);  // white
                        buffstr[i] = '8';
                        }
                        i++;
                        break;
                case 10:
                        if(shift_pressed) {
                            drawText('(', 255, 255, 255);  // white
                            buffstr[i] = '(';
                        } else {
                        drawText('9', 255, 255, 255);  // white
                        buffstr[i] = '9';
                        }
                        i++;
                        break;
                case 11:
                        if(shift_pressed) {
                            drawText(')', 255, 255, 255);  // white
                            buffstr[i] = ')';
                        } else {
                        drawText('0', 255, 255, 255);  // white
                        buffstr[i] = '0';
                        }
                        i++;
                        break;
                case 12:
                        if(shift_pressed) {
                            drawText('_', 255, 255, 255);  // white
                            buffstr[i] = '_';
                        } else {
                        drawText('-', 255, 255, 255);  // white
                        buffstr[i] = '-';
                        }
                        i++;
                        break;
                case 13:
                        if(shift_pressed) {
                            drawText('+', 255, 255, 255);  // white
                            buffstr[i] = '+';
                        } else {
                        drawText('=', 255, 255, 255);  // white
                        buffstr[i] = '=';
                        }
                        i++;
                        break;
                case 14:  // Backspace
                        if (i == 0) {
                                break;
                        } else {
                                drawText('\b', 255, 255, 255);  // white
                                i--;
                                buffstr[i+1] = 0;
                                buffstr[i] = 0;
                                break;
                        }
                case 15:  // Tab
                        drawText('\t', 255, 255, 255);  // white
                        buffstr[i] = '\t';
                        i++;
                        break;
                case 16:
                        if(use_uppercase) {
                            drawText('Q', 255, 255, 255);  // white
                            buffstr[i] = 'Q';
                        } else {
                        drawText('q', 255, 255, 255);  // white
                        buffstr[i] = 'q';
                        }
                        i++;
                        break;
                case 17:
                        if(use_uppercase) {
                            drawText('W', 255, 255, 255);  // white
                            buffstr[i] = 'W';
                        } else {
                        drawText('w', 255, 255, 255);  // white
                        buffstr[i] = 'w';
                        }
                        i++;
                        break;
                case 18:
                        if(use_uppercase) {
                            drawText('E', 255, 255, 255);  // white
                            buffstr[i] = 'E';
                        } else {
                        drawText('e', 255, 255, 255);  // white
                        buffstr[i] = 'e';
                        }
                        i++;
                        break;
                case 19:
                        if(use_uppercase) {
                            drawText('R', 255, 255, 255);  // white
                            buffstr[i] = 'R';
                        } else {
                        drawText('r', 255, 255, 255);  // white
                        buffstr[i] = 'r';
                        }
                        i++;
                        break;
                case 20:
                        if(use_uppercase) {
                            drawText('T', 255, 255, 255);  // white
                            buffstr[i] = 'T';
                        } else {
                        drawText('t', 255, 255, 255);  // white
                        buffstr[i] = 't';
                        }
                        i++;
                        break;
                case 21:
                        if(use_uppercase) {
                            drawText('Y', 255, 255, 255);  // white
                            buffstr[i] = 'Y';
                        } else {
                        drawText('y', 255, 255, 255);  // white
                        buffstr[i] = 'y';
                        }
                        i++;
                        break;
                case 22:
                        if(use_uppercase) {
                            drawText('U', 255, 255, 255);  // white
                            buffstr[i] = 'U';
                        } else {
                        drawText('u', 255, 255, 255);  // white
                        buffstr[i] = 'u';
                        }
                        i++;
                        break;
                case 23:
                        if(use_uppercase) {
                            drawText('I', 255, 255, 255);  // white
                            buffstr[i] = 'I';
                        } else {
                        drawText('i', 255, 255, 255);  // white
                        buffstr[i] = 'i';
                        }
                        i++;
                        break;
                case 24:
                        if(use_uppercase) {
                            drawText('O', 255, 255, 255);  // white
                            buffstr[i] = 'O';
                        } else {
                        drawText('o', 255, 255, 255);  // white
                        buffstr[i] = 'o';
                        }
                        i++;
                        break;
                case 25:
                        if(use_uppercase) {
                            drawText('P', 255, 255, 255);  // white
                            buffstr[i] = 'P';
                        } else {
                        drawText('p', 255, 255, 255);  // white
                        buffstr[i] = 'p';
                        }
                        i++;
                        break;
                case 26:
                        if(shift_pressed) {
                            drawText('{', 255, 255, 255);  // white
                            buffstr[i] = '{';
                        } else {
                        drawText('[', 255, 255, 255);  // white
                        buffstr[i] = '[';
                        }
                        i++;
                        break;
                case 27:
                        if(shift_pressed) {
                            drawText('}', 255, 255, 255);  // white
                            buffstr[i] = '}';
                        } else {
                        drawText(']', 255, 255, 255);  // white
                        buffstr[i] = ']';
                        }
                        i++;
                        break;
                case 28:  // Enter
                        buffstr[i] = '\0';  // Properly terminate the string
                        reading = 0;
                        break;
                case 29:  // Left Control
                        // Handle control key if needed
                        break;
                case 30:
                        if(use_uppercase) {
                            drawText('A', 255, 255, 255);  // white
                            buffstr[i] = 'A';
                        } else {
                        drawText('a', 255, 255, 255);  // white
                        buffstr[i] = 'a';
                        }
                        i++;
                        break;
                case 31:
                        if(use_uppercase) {
                            drawText('S', 255, 255, 255);  // white
                            buffstr[i] = 'S';
                        } else {
                        drawText('s', 255, 255, 255);  // white
                        buffstr[i] = 's';
                        }
                        i++;
                        break;
                case 32:
                        if(use_uppercase) {
                            drawText('D', 255, 255, 255);  // white
                            buffstr[i] = 'D';
                        } else {
                        drawText('d', 255, 255, 255);  // white
                        buffstr[i] = 'd';
                        }
                        i++;
                        break;
                case 33:
                        if(use_uppercase) {
                            drawText('F', 255, 255, 255);  // white
                            buffstr[i] = 'F';
                        } else {
                        drawText('f', 255, 255, 255);  // white
                        buffstr[i] = 'f';
                        }
                        i++;
                        break;
                case 34:
                        if(use_uppercase) {
                            drawText('G', 255, 255, 255);  // white
                            buffstr[i] = 'G';
                        } else {
                        drawText('g', 255, 255, 255);  // white
                        buffstr[i] = 'g';
                        }
                        i++;
                        break;
                case 35:
                        if(use_uppercase) {
                            drawText('H', 255, 255, 255);  // white
                            buffstr[i] = 'H';
                        } else {
                        drawText('h', 255, 255, 255);  // white
                        buffstr[i] = 'h';
                        }
                        i++;
                        break;
                case 36:
                        if(use_uppercase) {
                            drawText('J', 255, 255, 255);  // white
                            buffstr[i] = 'J';
                        } else {
                        drawText('j', 255, 255, 255);  // white
                        buffstr[i] = 'j';
                        }
                        i++;
                        break;
                case 37:
                        if(use_uppercase) {
                            drawText('K', 255, 255, 255);  // white
                            buffstr[i] = 'K';
                        } else {
                        drawText('k', 255, 255, 255);  // white
                        buffstr[i] = 'k';
                        }
                        i++;
                        break;
                case 38:
                        if(use_uppercase) {
                            drawText('L', 255, 255, 255);  // white
                            buffstr[i] = 'L';
                        } else {
                        drawText('l', 255, 255, 255);  // white
                        buffstr[i] = 'l';
                        }
                        i++;
                        break;
                case 39:
                        if(shift_pressed) {
                            drawText(':', 255, 255, 255);  // white
                            buffstr[i] = ':';
                        } else {
                        drawText(';', 255, 255, 255);  // white
                        buffstr[i] = ';';
                        }
                        i++;
                        break;
                case 40:
                        if(shift_pressed) {
                            drawText('"', 255, 255, 255);  // white
                            buffstr[i] = '"';
                        } else {
                            drawText('\'', 255, 255, 255);  // white
                            buffstr[i] = '\'';
                        }
                        i++;
                        break;
                case 41:
                        if(shift_pressed) {
                            drawText('~', 255, 255, 255);  // white
                            buffstr[i] = '~';
                        } else {
                            drawText('`', 255, 255, 255);  // white
                            buffstr[i] = '`';
                        }
                        i++;
                        break;
                case 43:
                        if(shift_pressed) {
                            drawText('|', 255, 255, 255);  // white
                            buffstr[i] = '|';
                                                        } else {
                            drawText('\\', 255, 255, 255);  // white
                            buffstr[i] = '\\';
                        }
                        i++;
                        break;
                case 44:
                        if(use_uppercase) {
                            drawText('Z', 255, 255, 255);  // white
                            buffstr[i] = 'Z';
                        } else {
                        drawText('z', 255, 255, 255);  // white
                        buffstr[i] = 'z';
                        }
                        i++;
                        break;
                case 45:
                        if(use_uppercase) {
                            drawText('X', 255, 255, 255);  // white
                            buffstr[i] = 'X';
                        } else {
                        drawText('x', 255, 255, 255);  // white
                        buffstr[i] = 'x';
                        }
                        i++;
                        break;
                case 46:
                        if(use_uppercase) {
                            drawText('C', 255, 255, 255);  // white
                            buffstr[i] = 'C';
                        } else {
                        drawText('c', 255, 255, 255);  // white
                        buffstr[i] = 'c';
                        }
                        i++;
                        break;
                case 47:
                        if(use_uppercase) {
                            drawText('V', 255, 255, 255);  // white
                            buffstr[i] = 'V';
                        } else {
                        drawText('v', 255, 255, 255);  // white
                        buffstr[i] = 'v';
                        }
                        i++;
                        break;
                case 48:
                        if(use_uppercase) {
                            drawText('B', 255, 255, 255);  // white
                            buffstr[i] = 'B';
                        } else {
                        drawText('b', 255, 255, 255);  // white
                        buffstr[i] = 'b';
                        }
                        i++;
                        break;
                case 49:
                        if(use_uppercase) {
                            drawText('N', 255, 255, 255);  // white
                            buffstr[i] = 'N';
                        } else {
                        drawText('n', 255, 255, 255);  // white
                        buffstr[i] = 'n';
                        }
                        i++;
                        break;
                case 50:
                        if(use_uppercase) {
                            drawText('M', 255, 255, 255);  // white
                            buffstr[i] = 'M';
                        } else {
                        drawText('m', 255, 255, 255);  // white
                        buffstr[i] = 'm';
                        }
                        i++;
                        break;
                case 51:
                        if(shift_pressed) {
                            drawText('<', 255, 255, 255);  // white
                            buffstr[i] = '<';
                        } else {
                        drawText(',', 255, 255, 255);  // white
                        buffstr[i] = ',';
                        }
                        i++;
                        break;
                case 52:
                        if(shift_pressed) {
                            drawText('>', 255, 255, 255);  // white
                            buffstr[i] = '>';
                        } else {
                        drawText('.', 255, 255, 255);  // white
                        buffstr[i] = '.';
                        }
                        i++;
                        break;
                case 53:
                        if(shift_pressed) {
                            drawText('?', 255, 255, 255);  // white
                            buffstr[i] = '?';
                        } else {
                        drawText('/', 255, 255, 255);  // white
                        buffstr[i] = '/';
                        }
                        i++;
                        break;
                case 56:  // Right Alt
                        // Handle right alt if needed
                        break;
                case 57:  // Space
                        drawText(' ', 255, 255, 255);  // white
                        buffstr[i] = ' ';
                        i++;
                        break;
                case 69:  // Num Lock
                        // Handle num lock if needed
                        break;
                case 70:  // Scroll Lock
                        // Handle scroll lock if needed
                        break;
                case 71:  // Home
                        // Handle home key if needed
                        break;
                case 72:  // Up Arrow
                        // Handle up arrow if needed
                        break;
                case 73:  // Page Up
                        // Handle page up if needed
                        break;
                case 75:  // Left Arrow
                        // Handle left arrow if needed
                        break;
                case 77:  // Right Arrow
                        // Handle right arrow if needed
                        break;
                case 79:  // End
                        // Handle end key if needed
                        break;
                case 80:  // Down Arrow
                        // Handle down arrow if needed
                        break;
                case 81:  // Page Down
                        // Handle page down if needed
                        break;
                case 82:  // Insert
                        // Handle insert key if needed
                        break;
                case 83:  // Delete
                        // Handle delete key if needed
                        break;
            }
        }
    }
    
    // Ensure null termination
    buffstr[i] = '\0';
    
    // Return the buffer (caller is responsible for freeing)
    return buffstr;
}

void poll_keyboard_for_ctrl_c() {
    static uint8 ctrl_pressed = 0;
        if (inportb(0x64) & 0x1) {
                uint8 status = inportb(0x64);
                if (status & 0x20) {
                        return; // mouse data; ignore in keyboard code
                }
                uint8 scancode = inportb(0x60);
        if (scancode == 29) { // Ctrl press
            ctrl_pressed = 1;
        } else if (scancode == 157) { // Ctrl release
            ctrl_pressed = 0;
        } else if (ctrl_pressed && scancode == 46) { // 'c' key
            g_user_interrupt = 1;
        }
    }
}