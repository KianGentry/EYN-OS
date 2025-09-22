#include <shell.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <system.h>
#include <shell_command_info.h>
#include <stdint.h>

void history_cmd(string arg);

// Global command history instance
command_history_t g_command_history = {0};

// Add a command to history
void add_to_history(command_history_t* history, const char* command) {
    if (!history || !command || strlen(command) == 0) {
        return;
    }
    
    // Don't add duplicate consecutive commands
    if (history->count > 0 && strcmp(history->commands[history->count - 1], command) == 0) {
        return;
    }
    
    // Shift history if full
    if (history->count >= MAX_HISTORY_SIZE) {
        for (int i = 0; i < MAX_HISTORY_SIZE - 1; i++) {
            // Add bounds checking for array access
            if (i + 1 < MAX_HISTORY_SIZE) {
                strcpy(history->commands[i], history->commands[i + 1]);
            }
        }
        history->count = MAX_HISTORY_SIZE - 1;
    }
    
    // Add new command
    strcpy(history->commands[history->count], command);
    history->count++;
    // Set current to the index of the most-recently-added command
    history->current = history->count - 1;
}

// Clear the history
void clear_history(command_history_t* history) {
    if (!history) return;
    
    for (int i = 0; i < MAX_HISTORY_SIZE; i++) {
        history->commands[i][0] = '\0';
    }
    history->count = 0;
    history->current = -1;
}

// Show history (for debugging)
void show_history(command_history_t* history) {
    if (!history) return;
    
    printf("%cCommand History (%d entries):\n", 255, 255, 255, history->count);
    for (int i = 0; i < history->count; i++) {
        printf("%c%d: %s\n", 200, 200, 200, i + 1, history->commands[i]);
    }
}

// Enhanced readStr with history support
string readStr_with_history(command_history_t* history) {
    // Use stack allocation for faster performance
    static char buffstr[MAX_COMMAND_LENGTH];
    uint8 i = 0;
    uint8 reading = 1;
    uint8 shift_pressed = 0;
    uint8 caps_lock = 0;
    uint8 ctrl_pressed = 0;
    int history_index = -1; // -1 means not browsing history
    char original_input[MAX_COMMAND_LENGTH] = {0};
    
    // Initialize buffer
    buffstr[0] = '\0';
    
    while(reading) {
        if(inportb(0x64) & 0x1) {
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
                caps_lock = !caps_lock;
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
                buffstr[0] = '\0';
                reading = 0;
                printf("%c^C\n", 255, 255, 255);
                continue;
            }
            
            // Handle arrow keys for history navigation
            if (scancode == 72) {  // Up Arrow - go back in history
                if (history && history->count > 0) {
                    if (history_index == -1) {
                        // First time pressing up - save current input
                        strcpy(original_input, buffstr);
                        history_index = history->count - 1;
                    } else if (history_index > 0) {
                        history_index--;
                    }
                    
                    // Clear current line and load history entry
                    while (i > 0) {
                        printf("\b \b"); // Backspace and clear
                        i--;
                    }
                    strcpy(buffstr, history->commands[history_index]);
                    i = strlen(buffstr);
                    printf("%s", buffstr);
                }
                continue;
            }
            
            if (scancode == 80) {  // Down Arrow - go forward in history
                if (history && history_index >= 0) {
                    history_index++;
                    if (history_index >= history->count) {
                        // Back to original input
                        while (i > 0) {
                            printf("\b \b");
                            i--;
                        }
                        strcpy(buffstr, original_input);
                        i = strlen(buffstr);
                        printf("%s", buffstr);
                        history_index = -1;
                    } else {
                        // Load next history entry
                        while (i > 0) {
                            printf("\b \b");
                            i--;
                        }
                        strcpy(buffstr, history->commands[history_index]);
                        i = strlen(buffstr);
                        printf("%s", buffstr);
                    }
                }
                continue;
            }
            
            // Handle left/right arrows for cursor movement (basic implementation)
            if (scancode == 75) {  // Left Arrow
                if (i > 0) {
                    printf("\b");
                    i--;
                }
                continue;
            }
            
            if (scancode == 77) {  // Right Arrow
                if (i < strlen(buffstr)) {
                    printf("%c", buffstr[i]);
                    i++;
                }
                continue;
            }
            
            // Handle Enter key
            if (scancode == 28) {  // Enter
                printf("\n");
                reading = 0;
                break;
            }
            
            // Handle Backspace
            if (scancode == 14) {  // Backspace
                if (i > 0) {
                    printf("\b \b");
                    i--;
                    buffstr[i] = '\0';
                }
                continue;
            }
            
            // Determine if we should use uppercase
            uint8 use_uppercase = shift_pressed ^ caps_lock;
            
            // Handle regular character input (simplified version)
            char c = 0;
            switch(scancode) {
                case 2: c = shift_pressed ? '!' : '1'; break;
                case 3: c = shift_pressed ? '@' : '2'; break;
                case 4: c = shift_pressed ? '#' : '3'; break;
                case 5: c = shift_pressed ? '$' : '4'; break;
                case 6: c = shift_pressed ? '%' : '5'; break;
                case 7: c = shift_pressed ? '^' : '6'; break;
                case 8: c = shift_pressed ? '&' : '7'; break;
                case 9: c = shift_pressed ? '*' : '8'; break;
                case 10: c = shift_pressed ? '(' : '9'; break;
                case 11: c = shift_pressed ? ')' : '0'; break;
                case 12: c = shift_pressed ? '_' : '-'; break;
                case 13: c = shift_pressed ? '+' : '='; break;
                case 16: c = use_uppercase ? 'Q' : 'q'; break;
                case 17: c = use_uppercase ? 'W' : 'w'; break;
                case 18: c = use_uppercase ? 'E' : 'e'; break;
                case 19: c = use_uppercase ? 'R' : 'r'; break;
                case 20: c = use_uppercase ? 'T' : 't'; break;
                case 21: c = use_uppercase ? 'Y' : 'y'; break;
                case 22: c = use_uppercase ? 'U' : 'u'; break;
                case 23: c = use_uppercase ? 'I' : 'i'; break;
                case 24: c = use_uppercase ? 'O' : 'o'; break;
                case 25: c = use_uppercase ? 'P' : 'p'; break;
                case 26: c = shift_pressed ? '{' : '['; break;
                case 27: c = shift_pressed ? '}' : ']'; break;
                case 30: c = use_uppercase ? 'A' : 'a'; break;
                case 31: c = use_uppercase ? 'S' : 's'; break;
                case 32: c = use_uppercase ? 'D' : 'd'; break;
                case 33: c = use_uppercase ? 'F' : 'f'; break;
                case 34: c = use_uppercase ? 'G' : 'g'; break;
                case 35: c = use_uppercase ? 'H' : 'h'; break;
                case 36: c = use_uppercase ? 'J' : 'j'; break;
                case 37: c = use_uppercase ? 'K' : 'k'; break;
                case 38: c = use_uppercase ? 'L' : 'l'; break;
                case 39: c = shift_pressed ? ':' : ';'; break;
                case 40: c = shift_pressed ? '"' : '\''; break;
                case 41: c = shift_pressed ? '~' : '`'; break;
                case 43: c = shift_pressed ? '|' : '\\'; break;
                case 44: c = use_uppercase ? 'Z' : 'z'; break;
                case 45: c = use_uppercase ? 'X' : 'x'; break;
                case 46: c = use_uppercase ? 'C' : 'c'; break;
                case 47: c = use_uppercase ? 'V' : 'v'; break;
                case 48: c = use_uppercase ? 'B' : 'b'; break;
                case 49: c = use_uppercase ? 'N' : 'n'; break;
                case 50: c = use_uppercase ? 'M' : 'm'; break;
                case 51: c = shift_pressed ? '<' : ','; break;
                case 52: c = shift_pressed ? '>' : '.'; break;
                case 53: c = shift_pressed ? '?' : '/'; break;
                case 57: c = ' '; break;
            }
            
            if (c && i < MAX_COMMAND_LENGTH - 1) {
                drawText(c, 255, 255, 255);
                buffstr[i] = c;
                i++;
            }
        }
    }
    
    buffstr[i] = '\0';
    // Return a pointer to the static buffer
    return (string)buffstr;
}

REGISTER_SHELL_COMMAND(history, "history", history_cmd, CMD_STREAMING, "Show or clear command history.\nUsage: history [clear]\nExample: history | history clear", "history");