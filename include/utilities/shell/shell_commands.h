#ifndef SHELL_COMMANDS_H
#define SHELL_COMMANDS_H
#include <misc/types.h>
#include <utilities/shell/shell_args.h>

void echo(string ch);
void help();
void ver();
void joke_spam();
void draw_cmd_handler(const shell_args_t* args);
void calc(string ch);
void lsata();
void drives_cmd(string ch);
void drive_cmd(const shell_args_t* args);
void memory_cmd(const shell_args_t* args);
void size(const shell_args_t* args);
void log_cmd(const shell_args_t* args);
void hexdump_cmd(const shell_args_t* args);
void random_cmd(const shell_args_t* args);
void sort_cmd(const shell_args_t* args);
void search_cmd(const shell_args_t* args);
void error_cmd(const shell_args_t* args);
void validate_cmd(const shell_args_t* args);
void process_cmd(const shell_args_t* args);
void pciscan_cmd(const shell_args_t* args);
void e1000probe_cmd(const shell_args_t* args);
void e1000_cmd(const shell_args_t* args);
void ping_cmd(const shell_args_t* args);
void netstat_cmd(const shell_args_t* args);
void netcfg_cmd(const shell_args_t* args);
void portable_cmd(const shell_args_t* args);
void init_cmd(const shell_args_t* args);
void ring3_cmd(const shell_args_t* args);
void netcfg_try_autoload_quiet(uint8 drive);

// Streaming command system functions
void load_cmd(const shell_args_t* args);
void unload_cmd(const shell_args_t* args);
void status_cmd(const shell_args_t* args);
void clear_cmd(const shell_args_t* args);
void help_cmd(const shell_args_t* args);
void ls_cmd(const shell_args_t* args);
void run_cmd(const shell_args_t* args);

#endif 