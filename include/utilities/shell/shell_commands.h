#ifndef SHELL_COMMANDS_H
#define SHELL_COMMANDS_H
#include <misc/types.h>

void echo(string ch);
void help();
void ver();
void joke_spam();
void draw_cmd_handler(string ch);
void calc(string ch);
void lsata();
void drives_cmd(string ch);
void drive_cmd(string ch);
void memory_cmd(string ch);
void size(string ch);
void log_cmd(string ch);
void hexdump_cmd(string ch);
void random_cmd(string ch);
void history_cmd(string ch);
void sort_cmd(string ch);
void search_cmd(string ch);
void error_cmd(string ch);
void validate_cmd(string ch);
void process_cmd(string ch);
void portable_cmd(string ch);
void init_cmd(string ch);

// Streaming command system functions
void load_cmd(string ch);
void unload_cmd(string ch);
void status_cmd(string ch);
void clear_cmd(string ch);
void help_cmd(string ch);
void ls_cmd(string ch);
void run_cmd(string ch);

#endif 