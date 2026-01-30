#include <shell.h>
#include <vga.h>
#include <util.h>
#include <string.h>
#include <tui.h>
#include <hal/keyboard.h>

command_history_t g_command_history = {0};

void add_to_history(command_history_t* history, const char* command) {
    if (!history || !command || strlen(command) == 0) return;

    if (history->count > 0 && strcmp(history->commands[history->count - 1], command) == 0) return;

    if (history->count >= MAX_HISTORY_SIZE) {
        for (int i = 0; i < MAX_HISTORY_SIZE - 1; ++i) {
            strcpy(history->commands[i], history->commands[i + 1]);
        }
        history->count = MAX_HISTORY_SIZE - 1;
    }

    strcpy(history->commands[history->count], command);
    history->count++;
    history->current = history->count - 1;
}

void clear_history(command_history_t* history) {
    if (!history) return;

    for (int i = 0; i < MAX_HISTORY_SIZE; ++i) {
        history->commands[i][0] = '\0';
    }
    history->count = 0;
    history->current = -1;
}

void show_history(command_history_t* history) {
    if (!history) return;

    printf("%cCommand History (%d entries):\n", 255, 255, 255, history->count);
    for (int i = 0; i < history->count; ++i) {
        printf("%c%d: %s\n", 200, 200, 200, i + 1, history->commands[i]);
    }
}

static void line_move_left(int n) {
    while (n-- > 0) printf("\b");
}

static void line_clear(int cursor, int len) {
    line_move_left(cursor);
    for (int j = 0; j < len; ++j) printf(" ");
    line_move_left(len);
}

static void line_set(char* buff, int* len, int* cursor, const char* s) {
    line_clear(*cursor, *len);
    strncpy(buff, s, MAX_COMMAND_LENGTH - 1);
    buff[MAX_COMMAND_LENGTH - 1] = '\0';
    *len = (int)strlen(buff);
    *cursor = *len;
    printf("%s", buff);
}

string readStr_with_history(command_history_t* history) {
    static char buffstr[MAX_COMMAND_LENGTH];
    int len = 0;
    int cursor = 0;
    int history_index = -1;
    char original_input[MAX_COMMAND_LENGTH] = {0};

    buffstr[0] = '\0';

    while (1) {
        int key = tui_read_key();
        if (key == 0) continue;

        uint32 raw = (uint32)key;
        uint32 base = raw;

        if (base & HAL_KEY_FLAG_SUPER) base &= ~HAL_KEY_FLAG_SUPER;
        if ((base & 0xF000u) == HAL_KEY_FLAG_SHIFTSEL) {
            base = 0x1000u | (base & 0x0FFFu);
        }

        // Ctrl+C (abort / interrupt)
        if (base == 0x2206u || base == 3u) {
            g_user_interrupt = 1;
            buffstr[0] = '\0';
            printf("^C\n");
            return buffstr;
        }

        // Enter (do not print newline; caller does it)
        if (base == (uint32)'\n' || base == (uint32)'\r') {
            buffstr[len] = '\0';
            return buffstr;
        }

        // History navigation
        if (base == HAL_KEY_UP) {
            if (history && history->count > 0) {
                if (history_index == -1) {
                    strncpy(original_input, buffstr, MAX_COMMAND_LENGTH - 1);
                    original_input[MAX_COMMAND_LENGTH - 1] = '\0';
                    history_index = history->count - 1;
                } else if (history_index > 0) {
                    history_index--;
                }
                line_set(buffstr, &len, &cursor, history->commands[history_index]);
            }
            continue;
        }

        if (base == HAL_KEY_DOWN) {
            if (history && history->count > 0 && history_index >= 0) {
                history_index++;
                if (history_index >= history->count) {
                    history_index = -1;
                    line_set(buffstr, &len, &cursor, original_input);
                } else {
                    line_set(buffstr, &len, &cursor, history->commands[history_index]);
                }
            }
            continue;
        }

        // Cursor movement
        if (base == HAL_KEY_LEFT) {
            if (cursor > 0) {
                printf("\b");
                cursor--;
            }
            continue;
        }

        if (base == HAL_KEY_RIGHT) {
            if (cursor < len) {
                printf("%c", buffstr[cursor]);
                cursor++;
            }
            continue;
        }

        if (base == HAL_KEY_HOME) {
            line_move_left(cursor);
            cursor = 0;
            continue;
        }

        if (base == HAL_KEY_END) {
            while (cursor < len) {
                printf("%c", buffstr[cursor]);
                cursor++;
            }
            continue;
        }

        // Backspace
        if (base == (uint32)'\b' || base == 0x7Fu) {
            if (cursor > 0) {
                memmove(&buffstr[cursor - 1], &buffstr[cursor], (unsigned)(len - cursor + 1));
                cursor--;
                len--;
                printf("\b");
                printf("%s ", &buffstr[cursor]);
                line_move_left((len - cursor) + 1);
                if (history_index != -1) history_index = -1;
            }
            continue;
        }

        // Delete
        if (base == HAL_KEY_DELETE) {
            if (cursor < len) {
                memmove(&buffstr[cursor], &buffstr[cursor + 1], (unsigned)(len - cursor));
                len--;
                printf("%s ", &buffstr[cursor]);
                line_move_left((len - cursor) + 1);
                if (history_index != -1) history_index = -1;
            }
            continue;
        }

        // Printable ASCII insert
        if (base <= 0xFFu) {
            char c = (char)(uint8)base;
            if (c >= 32 && c <= 126) {
                if (len < MAX_COMMAND_LENGTH - 1) {
                    memmove(&buffstr[cursor + 1], &buffstr[cursor], (unsigned)(len - cursor + 1));
                    buffstr[cursor] = c;
                    len++;
                    printf("%s", &buffstr[cursor]);
                    cursor++;
                    line_move_left(len - cursor);
                    if (history_index != -1) history_index = -1;
                }
            }
            continue;
        }
    }
}
