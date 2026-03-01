#ifndef FS_COMMANDS_H
#define FS_COMMANDS_H
#include <misc/types.h>
#include <stdint.h>
#include <stddef.h>
#include <utilities/shell/shell_args.h>

void ls(string input);
void cat(string ch);
void read_cmd(const shell_args_t* args);
void writefat(string ch);
void writeram(string ch);
int write_output_to_file(const char* buf, int len, const char* filename, uint8_t disk);
int append_output_to_file(const char* buf, int len, const char* filename, uint8_t disk);
int check_filesystem_integrity(uint8_t disk);
int fatfix_repair_path(uint8_t disk, const char* dirpath);
extern void* fat32_disk_img;
extern void poll_keyboard_for_ctrl_c();
extern char shell_current_path[128];
void resolve_path(const char* input, const char* cwd, char* out, size_t outsz);

#endif 